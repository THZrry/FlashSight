// provider_client.cpp - 按形态构造请求 + 解析响应（网络走 NetClient，可桩）
#include "provider_client.hpp"

#include <chrono>

namespace fs {

namespace {

// Build the error string for a non-2xx response.
//
// The status code alone is not actionable: every provider explains the problem
// in the body ({"error":{"message":"tools[3].function.name: string does not
// match pattern..."}}), and the UI shows this string to the user. Keeping a
// readable excerpt turns "http 400" into "http 400: tools[3]... invalid name",
// which is the difference between a guess and a fix.
std::string http_error_text(const HttpResponse& resp) {
  std::string text = "http " + std::to_string(resp.status);
  std::string body = resp.body;
  if (body.empty()) return text;
  // Prefer the provider's structured message when there is one.
  try {
    Json j = Json::parse(body);
    if (j.is_object()) {
      if (j.contains("error")) {
        const Json& e = j["error"];
        if (e.is_object()) {
          std::string m = jsonutil::get<std::string>(e, "message", "");
          if (!m.empty()) body = m;
        } else if (e.is_string()) {
          body = e.get<std::string>();
        }
      } else if (j.contains("message") && j["message"].is_string()) {
        body = j["message"].get<std::string>();
      }
    }
  } catch (...) {
    // Not JSON (a proxy's HTML error page): use the raw body.
  }
  // One line, bounded: the toast is not a log viewer.
  std::string flat;
  for (char c : body) flat.push_back((c == '\n' || c == '\r' || c == '\t') ? ' ' : c);
  size_t start = flat.find_first_not_of(' ');
  if (start != std::string::npos) flat = flat.substr(start);
  const size_t kMax = 400;
  if (flat.size() > kMax) flat = flat.substr(0, kMax) + "…";
  if (flat.empty()) return text;
  return text + ": " + flat;
}

}  // namespace

std::string mask_secret(const std::string& s) {
  if (s.size() <= 8) return std::string(s.size(), '*');
  return s.substr(0, 4) + "****" + s.substr(s.size() - 4);
}

ProviderClient::ProviderClient(const ProviderConn& conn, const NetClient* net)
    : conn_(conn), net_(net) {}

FsResult ProviderClient::chat(const ChatRequest& req, ChatResponse* out,
                              const StreamDeltaFn& on_delta) const {
  if (!out) return FsResult::error(ErrCode::kInvalidArg, "null response");
  auto t0 = std::chrono::steady_clock::now();
  FsResult r;
  switch (conn_.kind) {
    case ApiKind::kOpenAIChat:
    case ApiKind::kCustom:
    case ApiKind::kPlan:  // legacy value: same chat shape, session header set
      r = chat_openai_chat(req, out, on_delta);
      break;
    case ApiKind::kOpenAIResponse:
      r = chat_openai_response(req, out, on_delta);
      break;
    case ApiKind::kAnthropic:
      r = chat_anthropic(req, out, on_delta);
      break;
    default:
      r = FsResult::error(ErrCode::kNotSupported, "unknown api kind");
      break;
  }
  out->latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();
  return r;
}

// ----- OpenAI Chat Completions -----
// 自定义头的值里可以写 {{SESSION}}，替换成当前会话 id。这样 opencode 系的
// 会话亲和头和 Azure 的 api-key 走同一条路径：都是"模板预填的一条普通头"，
// 没有"某种供应商才有的隐藏开关"。
namespace {
std::string subst_session(const std::string& v, const std::string& sid) {
  std::string out = v;
  const std::string k = "{{SESSION}}";
  size_t p = 0;
  while ((p = out.find(k, p)) != std::string::npos) {
    out.replace(p, k.size(), sid);
    p += sid.size();
  }
  return out;
}

// ---- SSE (text/event-stream) line splitter -------------------------------
// The body arrives in arbitrary read-sized chunks, so a `data:` line can be cut
// in half. Buffer until a newline, and keep the remainder for the next chunk.
class SseReader {
 public:
  // Returns each complete `data:` payload (the JSON after "data: ").
  void feed(const char* data, size_t len, std::vector<std::string>* out) {
    buf_.append(data, len);
    size_t start = 0;
    for (;;) {
      size_t nl = buf_.find('\n', start);
      if (nl == std::string::npos) break;
      std::string line = buf_.substr(start, nl - start);
      start = nl + 1;
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.compare(0, 5, "data:") != 0) continue;   // event:/id:/comments
      std::string payload = line.substr(5);
      if (!payload.empty() && payload[0] == ' ') payload.erase(0, 1);
      out->push_back(payload);
    }
    buf_.erase(0, start);
  }

 private:
  std::string buf_;
};

// Streaming tool calls arrive as fragments: {index, id?, function{name?,
// arguments?}} where only the FIRST fragment carries id/name and the arguments
// are concatenated across fragments. Merge them into the same shape as a
// non-streaming response so the rest of the loop cannot tell the difference.
void merge_tool_call_deltas(const Json& deltas, Json* acc) {
  if (!acc) return;
  if (!acc->is_array()) *acc = Json::array();
  for (size_t i = 0; i < deltas.size(); ++i) {
    const Json& d = deltas[i];
    if (!d.is_object()) continue;
    int64_t idx = jsonutil::get_int(d, "index", -1);
    if (idx < 0) idx = static_cast<int64_t>(acc->size());
    while (static_cast<int64_t>(acc->size()) <= idx) acc->push_back(Json::object());
    Json& slot = (*acc)[static_cast<size_t>(idx)];
    if (!slot.is_object()) slot = Json::object();
    std::string id = jsonutil::get<std::string>(d, "id", "");
    if (!id.empty()) slot["id"] = id;
    std::string type = jsonutil::get<std::string>(d, "type", "");
    if (!type.empty()) slot["type"] = type;
    if (d.contains("function") && d["function"].is_object()) {
      const Json& f = d["function"];
      if (!slot.contains("function") || !slot["function"].is_object()) {
        slot["function"] = Json::object();
      }
      std::string name = jsonutil::get<std::string>(f, "name", "");
      if (!name.empty()) slot["function"]["name"] = name;
      std::string args = jsonutil::get<std::string>(f, "arguments", "");
      if (!args.empty()) {
        std::string prev = jsonutil::get<std::string>(slot["function"],
                                                      "arguments", "");
        slot["function"]["arguments"] = prev + args;
      }
    }
  }
}
}  // namespace

// The composer's fallback ladder is disable/enable, because a model with no
// configured levels has no known vocabulary. No provider accepts those words
// verbatim: OpenAI wants minimal/low/medium/high, Anthropic has no named levels
// at all, and anything else treats an unknown effort as a 400. Normalise here,
// at the single place that talks to the wire:
//   disable -> omit the field entirely (off means "no reasoning hint")
//   enable  -> a middling named level, the safest thing a reasoning model accepts
// It returns "" when the field should be omitted.
std::string wire_thinking(const std::string& v) {
  if (v.empty()) return "";
  if (v == "disable" || v == "disabled" || v == "off" || v == "none") return "";
  if (v == "enable" || v == "enabled" || v == "on") return "medium";
  return v;
}

FsResult ProviderClient::chat_openai_chat(const ChatRequest& req, ChatResponse* out,
                                          const StreamDeltaFn& on_delta) const {
  Json body = Json::object();
  body["model"] = req.model;
  body["messages"] = req.messages;
  if (!req.tools.is_null() && !req.tools.empty()) body["tools"] = req.tools;
  // Reasoning effort: only sent when the caller picked a level. Sending it to a
  // model that has no levels is a 400, so an empty value must mean "omit".
  std::string effort = wire_thinking(req.thinking);
  if (!effort.empty()) body["reasoning_effort"] = effort;
  body["temperature"] = req.temperature;
  body["max_tokens"] = req.max_tokens;
  body["stream"] = req.stream;
  if (req.stream) {
    // Without this a streamed reply carries no usage block, so every streamed
    // call recorded 0 prompt tokens - which is what made the context water
    // level disappear (-1%) after the first streamed turn. Supported by
    // [OI]-compatible endpoints (DeepSeek included).
    body["stream_options"] = Json{{"include_usage", true}};
  }
  // 供应商特有的请求体字段（浅合并；不覆盖上面构造好的标准字段，除非用户
  // 明确写了同名字段——那正是"自定义 body"的意义）
  if (conn_.body_override.is_object()) {
    for (auto it = conn_.body_override.begin(); it != conn_.body_override.end(); ++it) {
      body[it.key()] = it.value();
    }
  }

  HttpRequest http;
  http.method = "POST";
  http.url = conn_.base_url + "/chat/completions";
  http.headers["Content-Type"] = "application/json";
  http.headers["Authorization"] = "Bearer " + conn_.api_key;
  // 会话亲和头：这是供应商的一个开关（opencode 系端点需要），不是请求形态
  if (conn_.send_session) {
    if (!req.session_id.empty()) {
      http.headers["x-opencode-session"] = req.session_id;
      http.headers["x-opencode-session-id"] = req.session_id;
    }
    http.headers["x-opencode-client"] = "flashsight";
  }
  for (auto& kv : conn_.custom_headers) {
    http.headers[kv.first] = subst_session(kv.second, req.session_id);
  }
  http.body = dump_safe(body);  // bad bytes in history must not abort the request
  http.timeout_ms = 120000;

  HttpResponse resp;
  FsResult nr;
  // Some servers ignore "stream": true and answer with one complete JSON body.
  // The SSE reader sees no "data:" lines at all in that case, so fall back to
  // parsing the body as a normal response - otherwise the reply silently
  // vanished (the tests caught exactly this).
  bool saw_sse = false;
  if (req.stream) {
    // Real streaming: each SSE delta is parsed as it arrives and handed to
    // on_delta immediately, so the UI renders while the model is still talking.
    SseReader reader;
    nr = net_->request_stream(http, [&](const char* data, size_t len) {
      // 停止生成: drop the connection instead of reading an answer nobody wants.
      // Returning false aborts the transfer (NetClient::request_stream contract).
      if (req.cancel && req.cancel()) return false;
      std::vector<std::string> payloads;
      reader.feed(data, len, &payloads);
      if (!payloads.empty()) saw_sse = true;
      for (size_t i = 0; i < payloads.size(); ++i) {
        if (payloads[i] == "[DONE]") continue;
        try {
          Json d = Json::parse(payloads[i]);
          Json choices = d.contains("choices") && d["choices"].is_array()
                             ? d["choices"] : Json::array();
          if (choices.empty()) {
            // A usage-only tail chunk (stream_options.include_usage, or a
            // provider that appends it after [DONE]). This is the ONLY place a
            // streamed reply reports prompt tokens, so it feeds the context
            // water level.
            if (d.contains("usage") && d["usage"].is_object()) {
              const Json& u = d["usage"];
              int64_t pin = jsonutil::get_int(u, "prompt_tokens", 0);
              int64_t pout = jsonutil::get_int(u, "completion_tokens", 0);
              if (pin > 0) out->in_tokens = pin;
              if (pout > 0) out->out_tokens = pout;
              int64_t pcache = jsonutil::get_int(u, "prompt_tokens_cached", 0);
              if (pcache > 0) out->cache_tokens = pcache;
            }
            continue;
          }
          const Json& ch = choices[0];
          // Some endpoints attach usage to the final content chunk instead.
          if (d.contains("usage") && d["usage"].is_object()) {
            const Json& u = d["usage"];
            int64_t pin = jsonutil::get_int(u, "prompt_tokens", 0);
            int64_t pout = jsonutil::get_int(u, "completion_tokens", 0);
            if (pin > 0) out->in_tokens = pin;
            if (pout > 0) out->out_tokens = pout;
          }
          Json delta = ch.contains("delta") && ch["delta"].is_object()
                           ? ch["delta"] : Json::object();
          std::string piece = jsonutil::get<std::string>(delta, "content", "");
          if (!piece.empty()) {
            out->text += piece;
            if (on_delta) on_delta(piece);
          }
          // Tool calls also arrive incrementally (index + partial arguments).
          // They are accumulated but NOT forwarded: the UI shows them once the
          // turn ends, and a half-built JSON argument is not renderable.
          if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
            merge_tool_call_deltas(delta["tool_calls"], &out->tool_calls);
          }
        } catch (...) {
          // A malformed delta must not kill the stream.
        }
      }
      return true;
    }, &resp);
    if (!nr.ok()) {
      // A transfer WE aborted is not a network failure: report it as a
      // cancellation so the loop can park the turn instead of calling it broken.
      if (req.cancel && req.cancel()) {
        return FsResult::error(ErrCode::kCancelled, "stopped by user");
      }
      return nr;
    }
  } else {
    nr = net_->request(http, &resp);
  }
  if (!nr.ok()) return nr;
  out->http_status = resp.status;
  if (resp.status == 402) {
    out->error = "quota exhausted (402 balance-zero)";
    return FsResult::error(ErrCode::kRemote, out->error);
  }
  if (resp.status == 429) {
    out->error = "rate limited (429)";
    return FsResult::error(ErrCode::kRemote, out->error);
  }
  if (resp.status != 200) {
    // Keep the server's own message. "http 400" alone is undiagnosable: the
    // reason (an illegal tool name, an unsupported field, a bad model id) is
    // only ever in the response body, and dropping it turned a one-line fix
    // into a guessing game.
    out->error = http_error_text(resp);
    return FsResult::error(ErrCode::kRemote, out->error);
  }
  // Streaming already accumulated the text - unless the server ignored
  // "stream": true and sent one plain JSON body, in which case saw_sse is false
  // and the normal parse below is the only way to get the reply.
  if (req.stream && saw_sse) return FsResult::success();
  try {
    Json j = Json::parse(resp.body);
    // text 累积
    Json choices = j.contains("choices") && j["choices"].is_array()
                       ? j["choices"] : Json::array();
    if (!choices.empty()) {
      Json msg = choices[0].contains("message") ? choices[0]["message"] : Json::object();
      out->text = jsonutil::get<std::string>(msg, "content", "");
      // tool_calls（P3 解析执行）
      if (msg.contains("tool_calls") && msg["tool_calls"].is_array()) {
        out->tool_calls = msg["tool_calls"];
      }
    }
    // usage
    Json u = j.contains("usage") && j["usage"].is_object() ? j["usage"] : Json::object();
    out->in_tokens = jsonutil::get_int(u, "prompt_tokens", 0);
    out->out_tokens = jsonutil::get_int(u, "completion_tokens", 0);
    out->cache_tokens = jsonutil::get_int(u, "prompt_tokens_cached", 0);
    if (out->cache_tokens == 0) {
      // [OI] spells the same number two ways; DeepSeek and OpenRouter use the
      // nested form, and missing it made the cache-hit rate read 0%.
      if (u.contains("prompt_cache_hit_tokens")) {
        out->cache_tokens = jsonutil::get_int(u, "prompt_cache_hit_tokens", 0);
      } else if (u.contains("prompt_tokens_details") &&
                 u["prompt_tokens_details"].is_object()) {
        out->cache_tokens =
            jsonutil::get_int(u["prompt_tokens_details"], "cached_tokens", 0);
      }
    }
    // Cost. Only what the ENDPOINT reports is shown: a number computed from
    // hard-coded list prices looks exactly like a bill, and reconciling it
    // against a real invoice is a losing game (prices change, discounts exist,
    // cache billing differs per provider). So:
    //   * a `cost` field (opencode/Zen style) or `usage.cost` (OpenRouter),
    //   * otherwise quota / unknown - never a guess.
    double reported = -1.0;
    std::string cost_str = jsonutil::get<std::string>(j, "cost", "");
    if (!cost_str.empty()) {
      if (cost_str == "0" || cost_str == "0.0" || cost_str == "0.00000000") {
        out->cost_kind = "quota";
        out->cost = 0.0;
        reported = 0.0;
      } else {
        try {
          reported = std::stod(cost_str);
        } catch (...) {
          reported = -1.0;
        }
      }
    }
    if (reported < 0 && u.contains("cost") && u["cost"].is_number()) {
      reported = u["cost"].get<double>();
    }
    if (reported >= 0) {
      out->cost = reported;
      if (out->cost_kind != "quota") out->cost_kind = "money";
    } else if (conn_.has_quota) {
      out->cost_kind = conn_.quota_kind;
      out->cost = conn_.quota_value;
    } else {
      out->cost_kind = "unknown";
      out->cost = 0.0;
    }
    // The server ignored "stream": true and sent one complete body. Push it as
    // a single delta so a streaming consumer still renders it.
    if (req.stream && on_delta && !out->text.empty()) on_delta(out->text);
    return FsResult::success();
  } catch (...) {
    out->error = "response parse failed";
    return FsResult::error(ErrCode::kParse, out->error);
  }
}

// ----- OpenAI Response API -----
FsResult ProviderClient::chat_openai_response(const ChatRequest& req, ChatResponse* out,
                                              const StreamDeltaFn& on_delta) const {
  Json body = Json::object();
  body["model"] = req.model;
  body["input"] = req.messages;  // Response API 用 input 数组
  if (!req.tools.is_null() && !req.tools.empty()) body["tools"] = req.tools;
  // The Responses API names the same knob `reasoning.effort`, as a nested object.
  // It was simply missing here, so picking a thinking level on an
  // OpenAI-Responses provider did nothing.
  std::string effort = wire_thinking(req.thinking);
  if (!effort.empty()) body["reasoning"] = Json{{"effort", effort}};
  body["max_output_tokens"] = req.max_tokens;
  body["stream"] = req.stream;

  HttpRequest http;
  http.method = "POST";
  http.url = conn_.base_url + "/responses";
  http.headers["Content-Type"] = "application/json";
  http.headers["Authorization"] = "Bearer " + conn_.api_key;
  for (auto& kv : conn_.custom_headers) {
    http.headers[kv.first] = subst_session(kv.second, req.session_id);
  }
  http.body = dump_safe(body);  // bad bytes in history must not abort the request
  http.timeout_ms = 120000;

  HttpResponse resp;
  FsResult nr = net_->request(http, &resp);
  if (!nr.ok()) return nr;
  out->http_status = resp.status;
  if (resp.status != 200) {
    out->error = http_error_text(resp);
    return FsResult::error(ErrCode::kRemote, out->error);
  }
  try {
    Json j = Json::parse(resp.body);
    // Response API 输出在 output[].content[].text
    Json output = j.contains("output") && j["output"].is_array()
                      ? j["output"] : Json::array();
    std::string text;
    for (auto& item : output) {
      if (!item.is_object()) continue;
      Json content = item.contains("content") && item["content"].is_array()
                         ? item["content"] : Json::array();
      for (auto& c : content) {
        if (c.is_object()) {
          std::string t = jsonutil::get<std::string>(c, "text", "");
          if (!t.empty()) text += t;
        }
      }
    }
    out->text = text;
    Json u = j.contains("usage") && j["usage"].is_object() ? j["usage"] : Json::object();
    out->in_tokens = jsonutil::get_int(u, "input_tokens", 0);
    out->out_tokens = jsonutil::get_int(u, "output_tokens", 0);
    out->cache_tokens = jsonutil::get_int(u, "cached_input_tokens", 0);
    out->cost_kind = conn_.has_quota ? conn_.quota_kind : "unknown";
    out->cost = conn_.has_quota ? conn_.quota_value : 0.0;
    if (req.stream && on_delta && !out->text.empty()) on_delta(out->text);
    return FsResult::success();
  } catch (...) {
    out->error = "response parse failed";
    return FsResult::error(ErrCode::kParse, out->error);
  }
}

// ----- Anthropic Messages -----
FsResult ProviderClient::chat_anthropic(const ChatRequest& req, ChatResponse* out,
                                        const StreamDeltaFn& on_delta) const {
  Json body = Json::object();
  body["model"] = req.model;
  body["messages"] = req.messages;  // role/user/assistant；system 单独字段
  // Anthropic requires `system` at the top level, never as a message. It used to
  // be dropped entirely, so the model ran with no instructions at all.
  if (!req.system.empty()) body["system"] = req.system;
  if (!req.thinking.empty()) {
    // Extended thinking: budget must be below max_tokens and above the API
    // minimum. Levels map to budgets because Anthropic has no named levels.
    // disable/off mean "omit the field" (see wire_thinking).
    std::string lvl = wire_thinking(req.thinking);
    if (lvl.empty()) {
      // reasoning explicitly disabled: send nothing at all
    } else {
      int64_t budget = 4096;  // enable/medium and anything unknown
      if (lvl == "minimal") budget = 1024;
      else if (lvl == "low") budget = 2048;
      else if (lvl == "high") budget = 8192;
      else if (lvl == "max" || lvl == "xhigh" || lvl == "extra-high") budget = 16384;
      if (req.max_tokens > 0 && budget >= req.max_tokens) budget = req.max_tokens / 2;
      if (budget > 0) {
        body["thinking"] = Json{{"type", "enabled"}, {"budget_tokens", budget}};
      }
    }
  }
  body["max_tokens"] = req.max_tokens;
  if (!req.tools.is_null() && !req.tools.empty()) body["tools"] = req.tools;
  body["stream"] = req.stream;

  HttpRequest http;
  http.method = "POST";
  http.url = conn_.base_url + "/messages";
  http.headers["Content-Type"] = "application/json";
  http.headers["x-api-key"] = conn_.api_key;
  http.headers["anthropic-version"] = "2023-06-01";
  for (auto& kv : conn_.custom_headers) {
    http.headers[kv.first] = subst_session(kv.second, req.session_id);
  }
  http.body = dump_safe(body);  // bad bytes in history must not abort the request
  http.timeout_ms = 120000;

  HttpResponse resp;
  FsResult nr;
  bool saw_sse = false;
  if (req.stream) {
    // Anthropic's SSE events: content_block_delta carries {delta:{type:
    // text_delta, text}}; message_delta carries the final usage numbers.
    SseReader reader;
    nr = net_->request_stream(http, [&](const char* data, size_t len) {
      // 停止生成: same abort contract as the OpenAI shape above.
      if (req.cancel && req.cancel()) return false;
      std::vector<std::string> payloads;
      reader.feed(data, len, &payloads);
      if (!payloads.empty()) saw_sse = true;
      for (size_t i = 0; i < payloads.size(); ++i) {
        if (payloads[i] == "[DONE]") continue;
        try {
          Json d = Json::parse(payloads[i]);
          std::string type = jsonutil::get<std::string>(d, "type", "");
          if (type == "content_block_delta" && d.contains("delta") &&
              d["delta"].is_object()) {
            const Json& dl = d["delta"];
            if (jsonutil::get<std::string>(dl, "type", "") == "text_delta") {
              std::string piece = jsonutil::get<std::string>(dl, "text", "");
              if (!piece.empty()) {
                out->text += piece;
                if (on_delta) on_delta(piece);
              }
            }
          } else if (type == "message_start" && d.contains("message") &&
                     d["message"].is_object()) {
            const Json& m = d["message"];
            if (m.contains("usage") && m["usage"].is_object()) {
              out->in_tokens = jsonutil::get_int(m["usage"], "input_tokens", 0);
            }
          } else if (type == "message_delta" && d.contains("usage") &&
                     d["usage"].is_object()) {
            out->out_tokens = jsonutil::get_int(d["usage"], "output_tokens",
                                                out->out_tokens);
          } else if (type == "error" && d.contains("error") &&
                     d["error"].is_object()) {
            out->error = jsonutil::get<std::string>(d["error"], "message",
                                                    "stream error");
          }
        } catch (...) {
          // A malformed event must not kill the stream.
        }
      }
      return true;
    }, &resp);
    if (!nr.ok()) {
      // A transfer WE aborted is not a network failure: report it as a
      // cancellation so the loop can park the turn instead of calling it broken.
      if (req.cancel && req.cancel()) {
        return FsResult::error(ErrCode::kCancelled, "stopped by user");
      }
      return nr;
    }
  } else {
    nr = net_->request(http, &resp);
  }
  if (!nr.ok()) return nr;
  out->http_status = resp.status;
  if (resp.status != 200) {
    out->error = http_error_text(resp);
    return FsResult::error(ErrCode::kRemote, out->error);
  }
  if (!out->error.empty()) return FsResult::error(ErrCode::kRemote, out->error);
  // Same fallback as the OpenAI path: a server that ignores "stream": true sends
  // one plain JSON body, and the SSE parser saw nothing.
  if (req.stream && saw_sse) return FsResult::success();
  try {
    Json j = Json::parse(resp.body);
    Json content = j.contains("content") && j["content"].is_array()
                       ? j["content"] : Json::array();
    std::string text;
    for (auto& c : content) {
      if (c.is_object() && jsonutil::get<std::string>(c, "type", "") == "text") {
        text += jsonutil::get<std::string>(c, "text", "");
      }
    }
    out->text = text;
    Json u = j.contains("usage") && j["usage"].is_object() ? j["usage"] : Json::object();
    out->in_tokens = jsonutil::get_int(u, "input_tokens", 0);
    out->out_tokens = jsonutil::get_int(u, "output_tokens", 0);
    out->cache_tokens = jsonutil::get_int(u, "cache_read_input_tokens", 0);
    out->cost_kind = conn_.has_quota ? conn_.quota_kind : "unknown";
    out->cost = conn_.has_quota ? conn_.quota_value : 0.0;
    if (req.stream && on_delta && !out->text.empty()) on_delta(out->text);
    return FsResult::success();
  } catch (...) {
    out->error = "response parse failed";
    return FsResult::error(ErrCode::kParse, out->error);
  }
}

// ----- testConnection：轻量探活（发一条最小 chat 或 /models）-----
FsResult ProviderClient::test_connection(ChatResponse* out) const {
  ChatRequest req;
  req.model = "gpt-4o-mini";  // 探活用最小模型；实际由调用方覆盖
  Json msg = Json::object();
  msg["role"] = "user";
  msg["content"] = "ping";
  req.messages = Json::array({msg});
  req.max_tokens = 8;
  req.stream = false;
  return chat(req, out, nullptr);
}

void ProviderClient::parse_openai_chat_response(const Json& body, ChatResponse* out) {
  Json choices = body.contains("choices") && body["choices"].is_array()
                     ? body["choices"] : Json::array();
  if (!choices.empty()) {
    Json msg = choices[0].contains("message") ? choices[0]["message"] : Json::object();
    out->text = jsonutil::get<std::string>(msg, "content", "");
    if (msg.contains("tool_calls") && msg["tool_calls"].is_array()) {
      out->tool_calls = msg["tool_calls"];
    }
  }
}

void ProviderClient::parse_anthropic_response(const Json& body, ChatResponse* out) {
  Json content = body.contains("content") && body["content"].is_array()
                     ? body["content"] : Json::array();
  std::string text;
  for (auto& c : content) {
    if (c.is_object() && jsonutil::get<std::string>(c, "type", "") == "text") {
      text += jsonutil::get<std::string>(c, "text", "");
    }
  }
  out->text = text;
}

}  // namespace fs