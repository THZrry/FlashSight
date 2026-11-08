// history_builder.cpp - HistoryBuilder implementation
#include "history_builder.hpp"

#include <string>
#include <vector>

#include "provider/prompt_adapter.hpp"
#include "fs_utf8.hpp"

namespace fs {

int64_t HistoryBuilder::estimate_tokens(const std::string& s) {
  int64_t ascii = 0, cjk = 0;
  for (unsigned char c : s) {
    if (c >= 0x80) ++cjk;
    else ++ascii;
  }
  // 英文约 4 字符/token，CJK 约 1 字符/token（保守：宁可高估）
  return ascii / 4 + cjk + 1;
}

int64_t HistoryBuilder::estimate_tokens(const Json& j) {
  return estimate_tokens(j.dump());
}

namespace {
Json compact_value(const Json& v, int depth);  // defined below

// Auto-continue rounds that produced nothing are noise, not conversation. A
// real session accumulated 49 `SYSTEM: continue` turns with empty assistant
// answers; replaying them wasted the window and pushed the meaningful history
// out. They stay in full.json but never enter the model context.
bool is_auto_nudge(const Json& t) {
  if (!t.is_object() || jsonutil::get<std::string>(t, "role", "") != "user") {
    return false;
  }
  if (jsonutil::get_int(t, "autoRound", 0) <= 0) return false;
  std::string text = jsonutil::get<std::string>(t, "promptRaw", "");
  if (text.empty()) text = jsonutil::get<std::string>(t, "content", "");
  return text.compare(0, 7, "SYSTEM:") == 0;
}

bool is_empty_auto_answer(const Json& t) {
  if (!t.is_object() || jsonutil::get<std::string>(t, "role", "") != "assistant") {
    return false;
  }
  if (jsonutil::get_int(t, "autoRound", 0) <= 0) return false;
  if (!jsonutil::get<std::string>(t, "streamSummary", "").empty()) return false;
  if (!jsonutil::get<std::string>(t, "content", "").empty()) return false;
  return !t.contains("toolCalls") || !t["toolCalls"].is_array() ||
         t["toolCalls"].empty();
}

// Tool ARGUMENTS are replayed on every later turn. The fresh result may be
// complete (the model needs it while it is working), but the historical copy
// must not re-send whole file contents / diff bodies: that is what made a long
// session cost hundreds of thousands of prompt tokens. This keeps the routing
// facts and the ranges; the bulky payload stays in full.json and is re-readable
// with a tool call when actually needed.
Json project_tool_args(const std::string& name, const Json& in) {
  Json out = Json::object();
  if (!in.is_object()) return out;
  auto str = [&](const char* k, size_t cap) {
    if (!in.contains(k) || !in[k].is_string()) return;
    out[k] = utf8::truncate(in[k].get<std::string>(), cap, "…");
  };
  auto num = [&](const char* k) {
    if (in.contains(k) && in[k].is_number()) out[k] = in[k];
  };
  auto boolean = [&](const char* k) {
    if (in.contains(k) && in[k].is_boolean()) out[k] = in[k];
  };
  if (name == "write_file") {
    str("path", 400);
    num("bytes");
    // content is the whole file; it is in full.json and in the file itself.
  } else if (name == "edit_file") {
    str("path", 400);
    str("lineEnding", 20);
    if (in.contains("edits") && in["edits"].is_array()) {
      Json ranges = Json::array();
      const Json& es = in["edits"];
      for (size_t i = 0; i < es.size() && i < 50; ++i) {
        if (!es[i].is_object()) continue;
        ranges.push_back(Json{
            {"start", es[i].value("start", Json(0))},
            {"end", es[i].value("end", Json(0))},
            {"newBytes", static_cast<int64_t>(
                 jsonutil::get<std::string>(es[i], "newText", "").size())}});
      }
      out["edits"] = Json{{"count", static_cast<int>(es.size())}, {"ranges", ranges}};
    }
  } else if (name == "read_file") {
    str("path", 400);
    num("offset");
    num("limit");
  } else if (name == "terminal_exec" || name == "terminal_spawn") {
    str("command", 1200);
    str("shell", 80);
    num("timeoutMs");
    boolean("background");
    boolean("wakeOnExit");
  } else if (name == "plan_update") {
    // The document lives at .flashsight/plans/<sid>.md; replaying its whole body
    // every turn is pure cost. Status is the machine-readable fact.
    str("status", 20);
  } else if (name == "todo_add" || name == "todo_check" || name == "todo_uncheck" ||
             name == "todo_erase") {
    num("id");
    str("task", 200);
    boolean("done");
  } else if (name == "subagent_spawn") {
    str("kind", 60);
    str("step", 200);
    str("task", 600);
    num("todoId");
  } else if (name == "subagent_follow_up" || name == "subagent_steer") {
    str("id", 80);
    str("text", 500);
  } else if (name == "fetch_url") {
    str("url", 800);
    str("method", 12);
    num("timeoutMs");
  } else if (name == "search_web") {
    str("query", 500);
    num("count");
  } else if (name == "ask") {
    str("question", 800);
  } else {
    // Unknown/MCP tool: preserve the shape, bound every string.
    Json c = compact_value(in, 0);
    if (c.is_object()) out = c;
  }
  return out;
}
}  // namespace

// One stored turn becomes one or more neutral messages.
//
// A persisted assistant turn carries what the model SAID and what the tools
// RETURNED (executed_calls_). Both have to come back, otherwise the model sees
// its own request with no answer on the next turn and repeats it. Tool results
// are emitted as role:"tool" messages - the neutral form - and prompt::adapt()
// renders them as tool messages (OpenAI) or tool_result blocks (Anthropic).
void HistoryBuilder::turn_to_neutral(const Json& turn, Json* out) {
  std::string role = jsonutil::get<std::string>(turn, "role", "user");
  if (role == "user") {
    std::string text = jsonutil::get<std::string>(turn, "promptRaw", "");
    if (text.empty()) text = jsonutil::get<std::string>(turn, "streamSummary", "");
    if (text.empty()) return;  // a pure tool-result turn adds nothing on its own
    out->push_back(Json{{"role", "user"}, {"content", text}});
    // Anthropic-era turns stored tool results on the user turn; replay them.
    Json results = turn.contains("toolResults") && turn["toolResults"].is_array()
                       ? turn["toolResults"] : Json::array();
    for (auto& r : results) {
      if (!r.is_object()) continue;
      Json msg = Json::object();
      msg["role"] = "tool";
      msg["toolCallId"] = jsonutil::get<std::string>(r, "toolUseId", "");
      msg["content"] = r.contains("result")
                           ? (r["result"].is_string() ? r["result"].get<std::string>()
                                                      : dump_safe(r["result"]))
                           : "";
      if (msg["toolCallId"].get<std::string>().empty()) continue;
      out->push_back(msg);
    }
    return;
  }
  if (role != "assistant") return;

  // Mid-turn user guidance was applied to the next model call of this turn. On
  // replay it must be a user message again, not vanish from the transcript.
  if (turn.contains("guidance") && turn["guidance"].is_array()) {
    for (auto& g : turn["guidance"]) {
      if (!g.is_string()) continue;
      std::string text = g.get<std::string>();
      if (!text.empty()) out->push_back(Json{{"role", "user"}, {"content", text}});
    }
  }

  std::string text = jsonutil::get<std::string>(turn, "streamSummary", "");
  Json calls = Json::array();
  Json tool_msgs = Json::array();
  Json stored = turn.contains("toolCalls") && turn["toolCalls"].is_array()
                    ? turn["toolCalls"] : Json::array();
  int seq = 0;
  for (auto& c : stored) {
    if (!c.is_object()) continue;
    std::string name = jsonutil::get<std::string>(c, "name", "");
    if (name.empty()) continue;
    Json n = prompt::normalize_tool_call(c, "call_" + std::to_string(seq++));
    // History replay gets a bounded argument copy; the tool result below is
    // projected too. Full fidelity stays in full.json.
    if (n.contains("args")) n["args"] = project_tool_args(name, n["args"]);
    calls.push_back(n);
    // The result is the payload the tool produced. A stored turn may carry it as
    // `result` (structured, older turns) or `detail` (the display text the panel
    // shows). The display text is pretty-printed and can be 4000 characters of
    // before/after diff; replaying it verbatim every turn is what filled up a
    // long session's window. `project_tool_result` keeps the fields that let the
    // model continue and samples the payload.
    std::string name_for_proj = name;
    Json structured = c.contains("result") && c["result"].is_object() ? c["result"]
                                                                     : Json::object();
    std::string detail;
    if (c.contains("detail") && c["detail"].is_string()) {
      detail = c["detail"].get<std::string>();
    }
    std::string body;
    if (!structured.empty()) {
      body = HistoryBuilder::project_tool_result(name_for_proj, structured, detail);
    } else if (!detail.empty()) {
      body = HistoryBuilder::project_tool_result(name_for_proj, Json::object(), detail);
    } else if (c.contains("summary") && c["summary"].is_string()) {
      body = c["summary"].get<std::string>();
    }
    if (body.empty()) body = "{}";
    // A backstop for unknown tools that echoed a big blob: the projection bounds
    // known shapes, this bounds the rest.
    body = utf8::truncate(body, 4000, "\n... (truncated when replayed)");
    Json msg = Json::object();
    msg["role"] = "tool";
    msg["toolCallId"] = jsonutil::get<std::string>(n, "id", "");
    msg["name"] = name;
    msg["content"] = body;
    if (jsonutil::get<bool>(c, "ok", true) == false) msg["isError"] = true;
    tool_msgs.push_back(msg);
  }
  if (text.empty() && calls.empty()) {
    // A failed turn with only an error string: keep the error visible.
    std::string err = jsonutil::get<std::string>(turn, "error", "");
    if (err.empty()) return;
    text = "[" + err + "]";
  }
  Json msg = Json::object();
  msg["role"] = "assistant";
  msg["content"] = text;
  if (!calls.empty()) msg["toolCalls"] = calls;
  // The assistant message must come BEFORE its tool results.
  out->push_back(msg);
  for (auto& tm : tool_msgs) out->push_back(tm);
}

std::string HistoryBuilder::fold_summary(const Json& turns, size_t from, size_t to,
                                         int max_points) {
  return make_fold(turns, from, to, "", max_points);
}

namespace {

// A JSON value cut down to something that can be replayed on every turn: long
// strings truncated on a character boundary, long arrays sampled.
Json compact_value(const Json& v, int depth) {
  if (depth > 4) return Json("…");
  if (v.is_string()) {
    const std::string& s = v.get<std::string>();
    return Json(utf8::truncate(s, 400, "…"));
  }
  if (v.is_array()) {
    Json out = Json::array();
    size_t n = v.size() > 12 ? 12 : v.size();
    for (size_t i = 0; i < n; ++i) out.push_back(compact_value(v[i], depth + 1));
    if (v.size() > n) out.push_back(Json("… +" + std::to_string(v.size() - n) + " more"));
    return out;
  }
  if (v.is_object()) {
    Json out = Json::object();
    for (auto it = v.begin(); it != v.end(); ++it) {
      out[it.key()] = compact_value(it.value(), depth + 1);
    }
    return out;
  }
  return v;
}

int array_size(const Json& j, const char* key) {
  if (!j.is_object() || !j.contains(key) || !j[key].is_array()) return -1;
  return static_cast<int>(j[key].size());
}

// The tail of a text blob: for terminal output and build logs the END carries
// the signal (the error is at the bottom); the beginning is boilerplate.
std::string tail_of(const std::string& s, size_t max_bytes) {
  if (s.size() <= max_bytes) return s;
  size_t cut = s.size() - max_bytes;
  while (cut < s.size() && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) ++cut;
  return "…" + s.substr(cut);
}

}  // namespace

std::string HistoryBuilder::project_tool_result(const std::string& name,
                                                const Json& result,
                                                const std::string& detail) {
  Json r = result.is_object() ? result : Json::object();
  bool parsed_detail = false;
  if (r.empty() && !detail.empty()) {
    // Older turns only stored the display text. It is pretty-printed JSON in
    // practice, so try to recover the structure.
    try {
      Json parsed = Json::parse(detail);
      if (parsed.is_object()) {
        r = parsed;
        parsed_detail = true;
      }
    } catch (...) {
    }
  }
  Json out = Json::object();
  out["ok"] = jsonutil::get<bool>(r, "ok", true);
  if (r.empty() && !detail.empty()) {
    // Neither a structure nor JSON: keep a bounded copy of the text. Dropping it
    // entirely would erase the result from the model's view of the turn.
    out["output"] = utf8::truncate(detail, 900, "\n… (truncated when replayed)");
    return out.dump();
  }
  (void)parsed_detail;
  auto copy = [&](const char* k) {
    if (r.contains(k)) out[k] = compact_value(r[k], 1);
  };

  if (name == "read_file") {
    copy("path"); copy("startLine"); copy("endLine"); copy("totalLines");
    copy("hash"); copy("eol");
    std::string content = jsonutil::get<std::string>(r, "content", "");
    if (!content.empty()) {
      // Enough to recognise the file. `hash` is what makes a stale re-use fail
      // loudly instead of corrupting the file, so the model can re-read safely.
      out["content"] = utf8::truncate(
          content, 400, "… (call read_file again for the full text)");
    }
  } else if (name == "edit_file" || name == "write_file") {
    copy("path"); copy("totalLines"); copy("hash");
    copy("summary"); copy("bytes");
    int n = array_size(r, "changes");
    if (n >= 0) {
      // before/after text is the bulk of an edit result and only useful while
      // reading the diff; the line ranges are what "continue" needs.
      Json ranges = Json::array();
      const Json& ch = r["changes"];
      for (size_t i = 0; i < ch.size() && i < 20; ++i) {
        if (!ch[i].is_object()) continue;
        ranges.push_back(Json{{"start", ch[i].value("start", Json(0))},
                              {"end", ch[i].value("end", Json(0))}});
      }
      out["changes"] = Json{{"count", n}, {"ranges", ranges}};
    }
  } else if (name == "list_dir" || name == "find_files") {
    copy("path");
    int n = array_size(r, "entries");
    if (n >= 0) {
      out["entries"] = n;
      std::string names;
      const Json& arr = r["entries"];
      for (size_t i = 0; i < arr.size() && i < 60; ++i) {
        std::string nm = jsonutil::get<std::string>(arr[i], "name", "");
        if (nm.empty()) continue;
        if (!names.empty()) names += ",";
        names += nm;
      }
      if (!names.empty()) out["names"] = utf8::truncate(names, 900, "…");
    }
    n = array_size(r, "results");
    if (n >= 0) {
      out["results"] = n;
      Json sample = Json::array();
      const Json& arr = r["results"];
      for (size_t i = 0; i < arr.size() && i < 25; ++i) {
        sample.push_back(compact_value(arr[i], 1));
      }
      out["sample"] = sample;
    }
    if (r.contains("names") && r["names"].is_string() && !out.contains("names")) {
      out["names"] = utf8::truncate(r["names"].get<std::string>(), 900, "…");
    }
  } else if (name == "terminal" || name == "terminal_run" || name == "exec" ||
             name == "run_command" || name == "shell") {
    copy("exitCode"); copy("tid"); copy("status"); copy("timedOut");
    std::string text = jsonutil::get<std::string>(r, "output", "");
    if (text.empty()) text = jsonutil::get<std::string>(r, "text", "");
    if (text.empty()) text = jsonutil::get<std::string>(r, "stdout", "");
    if (!text.empty()) out["output"] = tail_of(text, 1200);
    std::string err = jsonutil::get<std::string>(r, "stderr", "");
    if (!err.empty()) out["stderr"] = tail_of(err, 600);
  } else if (name == "screenshot" || name == "read_image") {
    copy("width"); copy("height"); copy("format"); copy("path"); copy("backend");
    copy("bytes");
    // The image travels as a content block when it is fresh; replaying a data
    // URI into every later request would be pure cost.
  } else if (name == "search_web") {
    copy("provider"); copy("query");
    int n = array_size(r, "hits");
    if (n >= 0) {
      out["hits"] = n;
      Json sample = Json::array();
      const Json& arr = r["hits"];
      for (size_t i = 0; i < arr.size() && i < 6; ++i) {
        if (!arr[i].is_object()) continue;
        sample.push_back(Json{{"title", jsonutil::get<std::string>(arr[i], "title", "")},
                              {"url", jsonutil::get<std::string>(arr[i], "url", "")}});
      }
      out["sample"] = sample;
    }
  } else if (name == "fetch_url") {
    copy("status"); copy("url"); copy("contentType");
    std::string body = jsonutil::get<std::string>(r, "text", "");
    if (!body.empty()) out["text"] = utf8::truncate(body, 1200, "\n… (truncated)");
  } else if (name == "ask") {
    copy("question"); copy("answer");
  } else if (name.rfind("todo_", 0) == 0) {
    copy("id"); copy("pending");
    int n = array_size(r, "items");
    if (n >= 0) {
      Json items = Json::array();
      const Json& arr = r["items"];
      for (size_t i = 0; i < arr.size() && i < 20; ++i) {
        if (!arr[i].is_object()) continue;
        items.push_back(Json{
            {"id", arr[i].value("id", Json(0))},
            {"task", utf8::truncate(jsonutil::get<std::string>(arr[i], "task", ""), 120,
                                    "…")},
            {"done", arr[i].value("done", Json(false))}});
      }
      out["items"] = items;
    }
  } else if (name == "plan_update" || name == "plan_get") {
    // The plan is a DOCUMENT. Replaying its body is cost; status/path are the
    // facts that let the model continue (see plan_get when it needs the text).
    copy("status"); copy("exists"); copy("path");
  } else if (name.rfind("subagent_", 0) == 0) {
    copy("id"); copy("kind"); copy("status"); copy("step");
  } else {
    // Unknown (MCP / plugin) tool: keep its structure, bounded.
    Json c = compact_value(r, 0);
    if (c.is_object()) {
      for (auto it = c.begin(); it != c.end(); ++it) out[it.key()] = it.value();
    }
  }
  if (jsonutil::get<bool>(r, "ok", true) == false) {
    // A failure's reason IS the useful part; never drop it.
    copy("error"); copy("errCode"); copy("howToProceed"); copy("denial");
  }
  std::string s = dump_safe(out);
  if (s.size() > 2400) s = utf8::truncate(s, 2400, "\n… (result summary truncated)");
  return s;
}

std::string HistoryBuilder::make_fold(const Json& turns, size_t from, size_t to,
                                      const std::string& prior, int max_points,
                                      size_t max_digest_bytes) {
  std::string points;
  std::string digest;
  int collected = 0;
  // A fold must never erase the conversation: keyPoints are optional (they only
  // exist when the model called the keypoint tool), so a summary made only of
  // them was often just "[Earlier conversation was compacted]" and the model
  // lost everything. Always add a per-turn digest: user prompts and assistant
  // answers, one line each, so FULL compression still carries the thread.
  const size_t kMaxDigestBytes =
      max_digest_bytes > 512 ? max_digest_bytes : 512;
  for (size_t i = from; i < to && i < turns.size(); ++i) {
    const Json& t = turns[i];
    if (!t.is_object()) continue;
    if (is_auto_nudge(t) || is_empty_auto_answer(t)) continue;
    // A forgotten turn is dropped from context, not summarised.
    if (jsonutil::get<bool>(t, "forgotten", false)) continue;
    if (t.contains("keyPoints") && t["keyPoints"].is_array()) {
      for (auto& kp : t["keyPoints"]) {
        if (!kp.is_string() || collected >= max_points) continue;
        points += "- " + kp.get<std::string>() + "\n";
        ++collected;
      }
    }
    if (digest.size() >= kMaxDigestBytes) continue;
    std::string role = jsonutil::get<std::string>(t, "role", "");
    std::string text;
    if (role == "user") {
      text = jsonutil::get<std::string>(t, "promptRaw", "");
      if (text.empty()) text = jsonutil::get<std::string>(t, "content", "");
    } else {
      text = jsonutil::get<std::string>(t, "streamSummary", "");
      if (text.empty()) text = jsonutil::get<std::string>(t, "content", "");
    }
    // The model's narration is process, not the answer; keep the answer line.
    if (text.empty()) continue;
    text = utf8::truncate(text, 260, "…");
    for (size_t k = 0; k < text.size(); ++k) {
      if (text[k] == '\n' || text[k] == '\r') text[k] = ' ';
    }
    std::string line = (role == "user" ? "U: " : "A: ") + text;
    if (role != "user" && t.contains("toolCalls") && t["toolCalls"].is_array()) {
      std::string names;
      for (auto& c : t["toolCalls"]) {
        std::string n = jsonutil::get<std::string>(c, "name", "");
        if (n.empty()) continue;
        if (!names.empty()) names += ", ";
        names += n;
      }
      if (!names.empty()) line += " [tools: " + utf8::truncate(names, 120, "…") + "]";
    }
    digest += line + "\n";
  }
  std::string out;
  if (prior.empty()) {
    out = "[Earlier conversation was compacted]\n";
  } else if (prior.rfind("[Earlier conversation was compacted]", 0) == 0) {
    // The durable summary already carries the header; do not nest a second one
    // on every later compaction.
    out = prior + "\n";
  } else {
    out = "[Earlier conversation was compacted]\n" + prior + "\n";
  }
  if (!points.empty()) out += "[Key points]\n" + points;
  if (!digest.empty()) out += "[Turn digest]\n" + digest;
  return out;
}

Json HistoryBuilder::build_system() const {
  Json sys = Json::object();
  sys["role"] = "system";
  std::string text = opts_.system_prefix;
  if (opts_.env.is_object() && !opts_.env.empty()) {
    text += "\n\n[环境]\n" + opts_.env.dump();
  }
  if (opts_.tools.is_array() && !opts_.tools.empty()) {
    text += "\n\n[工具定义]\n" + opts_.tools.dump();
  }
  if (opts_.mcp.is_object() && !opts_.mcp.empty()) {
    text += "\n\n[Skills 与 MCP]\n" + opts_.mcp.dump();
  }
  if (opts_.disabled_system_tools.is_array() && !opts_.disabled_system_tools.empty()) {
    text += "\n\n[禁用的系统工具]\n" + opts_.disabled_system_tools.dump();
  }
  sys["content"] = text;
  return sys;
}

// Should we compact this turn?
//
// The water level is the ONLY automatic trigger: "how full is the model's window
// right now" is a fact the provider reported, and compacting earlier than that
// throws away history the model could have used.
//
// There is deliberately NO token budget here. A token cap is a second, invisible
// threshold the user cannot reason about (it fires on a long session regardless of
// how full the window is), and providers handle a long context themselves. The
// only other trigger is an explicit "compact now".
HistoryBuilder::CompressDecision HistoryBuilder::should_compress(
    const Options& opts, int64_t turns_tokens) {
  (void)turns_tokens;
  CompressDecision d;
  d.prompt_tokens = opts.last_prompt_tokens;
  d.context_limit = opts.context_limit;
  if (opts.context_limit > 0 && opts.last_prompt_tokens > 0) {
    d.pct = static_cast<int>(opts.last_prompt_tokens * 100 / opts.context_limit);
  }
  if (opts.force_compress) {
    d.compress = true;
    d.reason = "forced";
    return d;
  }
  if (opts.compress_at_pct > 0 && opts.context_limit > 0 &&
      opts.last_prompt_tokens > 0) {
    // Threshold on the LAST request: if the previous call already used 80% of
    // the window, this call must compact before it goes out - not after it
    // fails.
    if (d.pct >= opts.compress_at_pct) {
      d.compress = true;
      d.reason = "water-level";
      return d;
    }
  }
  return d;
}

HistoryResult HistoryBuilder::build_neutral(const Json& full) const {
  HistoryResult out;
  Json messages = Json::array();
  messages.push_back(build_system());
  // Seed messages: an earlier conversation this run CONTINUES (a subagent
  // follow-up). They sit between the system prompt and everything from `full`,
  // so the model reads them as its own prior turns.
  if (opts_.seed_messages.is_array()) {
    for (size_t i = 0; i < opts_.seed_messages.size(); ++i) {
      const Json& m = opts_.seed_messages[i];
      if (!m.is_object()) continue;
      std::string role = jsonutil::get<std::string>(m, "role", "");
      if (role.empty() || role == "system") continue;  // the prefix is ours
      messages.push_back(m);
    }
  }
  if (!full.contains("turns") || !full["turns"].is_array()) {
    out.messages = messages;
    return out;
  }
  const Json& turns = full["turns"];
  size_t total = turns.size();
  // NO recent-turns window. The water level (or an explicit "compact now") is
  // the only trigger; the API/provider deals with a long context.

  // ---- durable compaction state -------------------------------------------
  // A previous run may have folded a leading prefix of turns into a summary.
  // Reusing it is not an optimisation: without it a reopened session replays
  // the whole transcript, exceeds the window and regenerates a lossy short
  // view, losing early information the summary already held.
  std::string prior = opts_.prior_summary;
  size_t start = 0;
  bool prior_ok = false;
  // Full transcript size, used to decide whether a persisted SHORT VIEW is
  // still necessary (the user may have switched to a bigger-window model).
  //
  // Count the messages the REPLAY will actually contain, not the raw turn JSON:
  // a stored turn keeps the complete tool result/interim text, while replay
  // projects it down to a bounded summary. Using raw JSON here overestimated a
  // 180k-token session as several hundred thousand, so a window that was nowhere
  // near full produced a short view and threw the early conversation away.
  auto replay_tokens = [this](const Json& arr, size_t from, size_t to) -> int64_t {
    int64_t sum = 0;
    for (size_t i = from; i < to && i < arr.size(); ++i) {
      if (!arr[i].is_object()) continue;
      if (is_auto_nudge(arr[i]) || is_empty_auto_answer(arr[i])) continue;
      if (jsonutil::get<bool>(arr[i], "forgotten", false)) continue;
      Json group = Json::array();
      turn_to_neutral(arr[i], &group);
      sum += estimate_tokens(group);
    }
    return sum;
  };
  int64_t raw_tokens_all = replay_tokens(turns, 0, total);
  if (!prior.empty() && opts_.compacted_turns > 0 &&
      opts_.compacted_turns <= static_cast<int64_t>(total)) {
    size_t c = static_cast<size_t>(opts_.compacted_turns);
    bool ok = true;
    if (!opts_.prior_turn_id.empty()) {
      ok = c > 0 && turns[c - 1].is_object() &&
           jsonutil::get<std::string>(turns[c - 1], "turnId", "") ==
               opts_.prior_turn_id;
    }
    // A forgotten turn inside the compacted prefix means the summary still
    // contains text the user asked to drop: discard it and rebuild.
    for (size_t i = 0; ok && i < c; ++i) {
      if (!turns[i].is_object() ||
          jsonutil::get<bool>(turns[i], "forgotten", false)) {
        ok = false;
      }
    }
    // A short view is a fallback for a tight window. If the FULL transcript
    // fits the current window again (model switched back), replay all of it.
    if (ok && opts_.prior_mode == "short-view" && opts_.context_limit > 0 &&
        raw_tokens_all <= opts_.context_limit) {
      ok = false;
    }
    if (ok) {
      start = c;
      prior_ok = true;
    }
  }
  if (!prior_ok) {
    prior.clear();
    start = 0;
  }

  // Effective history = the durable summary + turns not yet folded. Using the
  // FULL transcript here was the "reopen at 16%, send says over window" bug:
  // the turns the summary already replaced were counted again. The token count
  // comes from the projected replay (see replay_tokens), not from the stored JSON.
  int64_t prior_tokens = prior.empty() ? 0 : estimate_tokens(prior);
  int64_t raw_tokens = raw_tokens_all;
  int64_t effective_tokens = prior_tokens + replay_tokens(turns, start, total);
  CompressDecision dec = should_compress(opts_, raw_tokens);
  bool window_short = false;
  if (opts_.context_limit > 0) {
    int64_t need = effective_tokens > opts_.last_prompt_tokens
                       ? effective_tokens
                       : opts_.last_prompt_tokens;
    if (need > opts_.context_limit) window_short = true;
  }
  const bool fold = opts_.force_compress || dec.compress || window_short;
  if (fold) {
    size_t keep = 0;  // full compression: no turn stays verbatim
    if (window_short) {
      // A short view is a fallback, not "keep one turn": retaining the user's
      // configured keepTurns (8 by default) is what stops a 1M-window session
      // from collapsing to 9% of context. The fold loop below shrinks it only
      // when the projected summary + suffix really cannot fit.
      keep = opts_.keep_turns > 0 ? static_cast<size_t>(opts_.keep_turns) : 8;
      if (keep >= total) keep = total > 1 ? total - 1 : total;
    }
    size_t fold_to = total > keep ? total - keep : start;
    if (fold_to < start) fold_to = start;
    // Nothing new to fold: every stored turn is already represented by the
    // durable summary. Reuse it instead of reporting a compression that did
    // not change anything.
    if (!(prior_ok && fold_to == start)) {
      // A short view is used precisely when the window is tight, so its summary
      // must be small too; full compression can carry a richer digest.
      size_t digest_cap = window_short ? 6000 : 32000;
      std::string fold = make_fold(turns, start, fold_to, prior,
                                   opts_.max_fold_points, digest_cap);
      // Honor keepTurns when it fits, but never emit a short view that the
      // window still cannot hold: fold one more turn at a time until the
      // projected summary + suffix is inside the limit. Measuring the actual
      // fold (not a guessed 7k) is what lets a 4k window keep 4 tiny turns
      // instead of collapsing to one.
      if (window_short && opts_.context_limit > 0) {
        while (fold_to + 1 < total) {
          int64_t cand = estimate_tokens(fold) +
                         replay_tokens(turns, fold_to, total);
          if (cand <= opts_.context_limit) break;
          ++fold_to;
          fold = make_fold(turns, start, fold_to, prior,
                           opts_.max_fold_points, digest_cap);
        }
      }
      Json summary = Json::object();
      summary["role"] = "user";
      summary["content"] = fold;
      Json with_fold = messages;
      with_fold.push_back(summary);
      int64_t used = estimate_tokens(fold);
      for (size_t i = fold_to; i < total; ++i) {
        const Json& t = turns[i];
        if (!t.is_object()) continue;
        if (is_auto_nudge(t) || is_empty_auto_answer(t)) continue;
        if (jsonutil::get<bool>(t, "forgotten", false)) continue;
        Json group = Json::array();
        turn_to_neutral(t, &group);
        used += estimate_tokens(group);
        for (auto& m : group) with_fold.push_back(m);
      }
      out.messages = with_fold;
      out.est_tokens = used;
      out.compressed = true;
      out.mode = window_short ? "short-view" : "full";
      out.compressed_from = static_cast<int64_t>(start);
      out.compressed_to = static_cast<int64_t>(fold_to);
      out.dropped_turns = static_cast<int64_t>(fold_to);
      out.summary = fold;
      out.compacted_turns = static_cast<int64_t>(fold_to);
      if (fold_to > 0 && turns[fold_to - 1].is_object()) {
        out.compacted_turn_id =
            jsonutil::get<std::string>(turns[fold_to - 1], "turnId", "");
      }
      return out;
    }
  }
  // Normal replay: the durable summary (if any) followed by every turn it does
  // not cover. This is the path a reopened session takes when the summary makes
  // the prompt fit; it must NOT be reported as a new compression.
  if (prior_ok) {
    Json summary = Json::object();
    summary["role"] = "user";
    summary["content"] = prior;
    messages.push_back(summary);
  }
  int64_t used = prior_tokens;
  for (size_t i = start; i < total; ++i) {
    const Json& t = turns[i];
    if (!t.is_object()) continue;
    if (is_auto_nudge(t) || is_empty_auto_answer(t)) continue;
    if (jsonutil::get<bool>(t, "forgotten", false)) continue;
    Json group = Json::array();
    turn_to_neutral(t, &group);
    used += estimate_tokens(group);
    for (size_t k = 0; k < group.size(); ++k) messages.push_back(group[k]);
  }
  out.est_tokens = used;
  out.messages = messages;
  return out;
}

HistoryResult HistoryBuilder::build(const Json& full) const {
  HistoryResult r = build_neutral(full);
  r.messages = prompt::adapt(ApiKind::kOpenAIChat, r.messages, Json::array()).messages;
  return r;
}

HistoryResult HistoryBuilder::build_anthropic(const Json& full) const {
  HistoryResult r = build_neutral(full);
  prompt::Adapted a = prompt::adapt(ApiKind::kAnthropic, r.messages, Json::array());
  // Legacy shape: {system, messages} in one object. The system text comes from
  // the adapter (it is the message the adapter consumed), not from re-reading
  // the neutral array.
  Json out = Json::object();
  out["system"] = a.system.is_string() ? a.system.get<std::string>() : std::string();
  out["messages"] = a.messages;
  r.messages = out;
  return r;
}

}  // namespace fs
