// agent_loop.cpp - Agent 主循环状态机实现
#include "agent_loop.hpp"

#include <string>
#include <vector>

#include "fs_path.hpp"
#include "fs_simhash.hpp"
#include "fs_utf8.hpp"
#include "provider/prompt_adapter.hpp"

namespace fs {

const char* loop_state_str(LoopState s) {
  switch (s) {
    case LoopState::kIdle: return "idle";
    case LoopState::kPlanning: return "planning";
    case LoopState::kToolCalling: return "toolCalling";
    case LoopState::kWaiting: return "waiting";
    case LoopState::kTurnDone: return "turnDone";
  }
  return "idle";
}

AgentLoop::AgentLoop(Options opts)
    : opts_(std::move(opts)), overfit_(OverfitDetector::Options()) {
  // 默认关（agent-loop.md §5）：Options 的默认构造已是 enabled=false。
  set_overfit_enabled(opts_.overfit_enabled);
}

void AgentLoop::set_overfit_enabled(bool on) {
  opts_.overfit_enabled = on;
  overfit_.set_enabled(on);
  if (on) overfit_.clear();
}

void AgentLoop::emit(const std::string& event, const Json& payload) {
  if (opts_.event) opts_.event(event, payload);
}

void AgentLoop::suspend() {
  suspend_requested_ = true;
  emit("loop.state", Json{{"state", "suspending"}, {"sid", opts_.sid}});
}

void AgentLoop::interrupt() {
  // 中止: the user wants the reply to stop NOW. The cancel predicate aborts the
  // transfer; the call site parks with the partial text (see call_model_once).
  abort_requested_ = true;
  // A suspend that was already pending must not survive the stop: the turn is
  // about to park either way.
  suspend_requested_ = false;
  emit("loop.state", Json{{"state", "suspending"}, {"sid", opts_.sid}});
}

void AgentLoop::resume() {
  suspend_requested_ = false;
  abort_requested_ = false;
  parked_ = false;
  emit("loop.state", Json{{"state", "resumed"}, {"sid", opts_.sid}});
}

// Continue a turn that a suspend parked.
//
// `resume()` only clears the request flag, which is enough while the turn is
// still running (it will simply not park). Once the loop HAS parked it is idle:
// no thread is left to notice the flag, so "恢复" did nothing and the task stayed
// stopped until the user typed something new. This re-runs the saved transcript
// as-is - no extra user message, because the user asked to continue the SAME
// task, not to say something new.
FsResult AgentLoop::resume_task() {
  if (!parked_) return FsResult::success();
  if (!saved_valid_) {
    parked_ = false;
    emit("loop.state", Json{{"state", "resumed"}, {"sid", opts_.sid}});
    return FsResult::success();
  }
  Json msgs = saved_messages_;
  parked_ = false;
  suspend_requested_ = false;
  abort_requested_ = false;
  saved_valid_ = false;
  state_ = LoopState::kPlanning;
  emit("loop.resumed", Json{{"sid", opts_.sid}, {"guidance", ""}});
  return run_loop(msgs, opts_.model, active_tools());
}

// Wake scheduling lives in AgentHost/WakeScheduler. AgentLoop's own timer flags
// and event emitters were a second, unused implementation: nothing set
// wake_scheduled_, nothing called these, and they could only duplicate/diverge
// from the real scheduler. Removed in the batch-61 audit.

// 高风险动作（agent-loop.md 3：写/删/发网络/跑终端）
bool AgentLoop::is_high_risk(const std::string& tool) const {
  return tool == "write_file" || tool == "edit_file" || tool == "remove_file" ||
         tool == "file_regex" || tool == "fetch_url" || tool == "search_web" ||
         tool.rfind("terminal_", 0) == 0;
}

// 需要用户点头的动作，按权限档区分：
//   approve     : 所有写/删/网络/终端都要问
//   permissive  : 项目内的写/删直接放行；系统级操作要问
//   full        : 不问
bool AgentLoop::needs_approval(const std::string& tool, const Json& args) const {
  if (opts_.permission == "full") return false;
  if (tool == "fetch_url" || tool == "search_web" ||
      tool.rfind("terminal_", 0) == 0) {
    return true;
  }
  bool write = tool == "write_file" || tool == "edit_file" ||
               tool == "remove_file" || tool == "file_regex";
  if (!write) return false;
  if (opts_.permission == "approve") return true;
  std::string rel = jsonutil::get<std::string>(args, "path", "");
  if (rel.empty()) return false;
  ghc::filesystem::path base = pathutil::normalize(opts_.cwd);
  if (base.empty()) return false;
  ghc::filesystem::path raw(rel);
  ghc::filesystem::path p = raw.is_absolute()
                                ? pathutil::normalize(raw)
                                : pathutil::normalize(base / rel);
  return !pathutil::is_within(p, base) && p != base;
}

// ---------------------------------------------------------------------------
// send / append
// ---------------------------------------------------------------------------
FsResult AgentLoop::send(const std::string& text) {
  // kTurnDone is a parked state, not a running one.
  if (state_ != LoopState::kIdle && state_ != LoopState::kTurnDone) {
    steering_queue_.push_back(text);
    emit("steering.queued", Json{{"len", static_cast<int>(steering_queue_.size())},
                                 {"sid", opts_.sid}});
    return FsResult::success();
  }
  // A SUSPENDED turn is continued with the new text as guidance. Starting a
  // fresh turn here would rebuild the history from disk, which does not contain
  // the tool results the turn was parked on - the paused work would silently
  // vanish ("挂起之后发消息，之前那步白跑了").
  if (parked_ && saved_valid_) return continue_task(text);
  std::string combined = text;
  if (!steering_queue_.empty()) {
    for (auto& s : steering_queue_) combined += "\n" + s;
    steering_queue_.clear();
  }
  auto_continues_ = 0;
  current_auto_round_.store(0);  // a user message is never an auto round
  return run_turn(combined);
}

FsResult AgentLoop::append(const std::string& text) {
  steering_queue_.push_back(text);
  emit("steering.queued", Json{{"len", static_cast<int>(steering_queue_.size())},
                               {"sid", opts_.sid}});
  return FsResult::success();
}

// Apply guidance that arrived while the turn was running: it becomes a user
// message before the NEXT model call in the SAME turn. Starting a new turn from
// the UI was the bug: the old turn stayed open, its tools reported into the new
// turn, and a page switch showed the wrong live transcript.
bool AgentLoop::apply_steering(Json* messages) {
  if (!messages) return false;
  std::string combined;
  if (drain_steering) {
    combined = drain_steering();
  } else {
    if (steering_queue_.empty()) return false;
    for (size_t i = 0; i < steering_queue_.size(); ++i) {
      if (!combined.empty()) combined += "\n";
      combined += steering_queue_[i];
    }
    steering_queue_.clear();
  }
  if (combined.empty()) return false;
  messages->push_back(Json{{"role", "user"},
                           {"content", AgentContext::wrap_user_turn(
                                           runtime_block(), combined)}});
  applied_steering_.push_back(combined);
  emit("steering.applied", Json{{"sid", opts_.sid}, {"text", combined}});
  return true;
}

const Json& AgentLoop::active_tools() const {
  if (opts_.context.tools.is_array() && !opts_.context.tools.empty()) {
    return opts_.context.tools;
  }
  return opts_.history.tools;
}

// ---------------------------------------------------------------------------
// runtime block (volatile state, appended to the newest user message)
// ---------------------------------------------------------------------------
std::string AgentLoop::runtime_block() {
  RuntimeState st;
  if (runtime_state) st = runtime_state();
  if (st.now_ms <= 0) st.now_ms = WallClock().now_ms();
  if (st.context_limit <= 0) st.context_limit = context_limit;
  if (st.context_tokens <= 0) st.context_tokens = last_in_tokens_;
  return AgentContext::build_runtime(opts_.context, st);
}

// ---------------------------------------------------------------------------
// one model round-trip
// ---------------------------------------------------------------------------
FsResult AgentLoop::call_model_once(const Json& messages, const Json& tools,
                                    ModelCallResult* out) {
  ModelCallRequest req;
  req.model = opts_.model;
  req.provider = opts_.provider;
  req.messages = messages;
  req.tools = tools;
  req.auto_mode = opts_.context.auto_mode;
  req.session_id = opts_.sid;
  req.thinking = opts_.thinking;
  // 中止 (the hard stop) aborts the transfer; 挂起 does NOT - it lets the round
  // finish and parks before the next model call (run_tool_group checks
  // suspend_requested_). One flag could not mean both.
  req.cancel = [this]() { return abort_requested_.load(); };

  bool streamed = false;
  auto on_delta = [this, &streamed](const std::string& piece) {
    if (piece.empty()) return;
    streamed = true;
    // Mirror the text into the live snapshot BEFORE emitting: a UI that opens
    // the session right after this event must see the same text the event
    // carried, not a version that is one delta behind.
    {
      std::lock_guard<std::mutex> lk(live_mu_);
      live_text_ += piece;
    }
    emit("stream.delta", Json{{"text", piece}, {"sid", opts_.sid}});
    maybe_persist_live();
  };
  FsResult r = call_model ? call_model(req, out, on_delta)
                          : FsResult::error(ErrCode::kInternal, "model not wired");
  if (r.ok() && out->error.empty()) {
    if (!streamed && !out->text.empty()) {
      emit("stream.delta", Json{{"text", out->text}, {"sid", opts_.sid}});
    }
    last_in_tokens_ = out->in_tokens > 0 ? out->in_tokens : last_in_tokens_;
    last_out_tokens_ = out->out_tokens;
    // Per-CALL usage, the moment it is known. The UI used to learn these numbers
    // only at turn end (one usage.session round trip AFTER the turn), so a long
    // multi-round turn showed stale context/cache figures while it was working -
    // and those are exactly the numbers you watch during a turn.
    emit("loop.usage", Json{{"sid", opts_.sid},
                            {"in", out->in_tokens},
                            {"out", out->out_tokens},
                            {"cached", out->cache_tokens},
                            {"cacheKnown", out->cache_known},
                            {"model", req.model}});
    // 防过拟合：只提示，不截断（agent-loop.md §5）。
    if (overfit_.enabled() && !out->text.empty() && overfit_.push(out->text)) {
      emit("truncate.hint", Json{{"sid", opts_.sid},
                                 {"reason", "repeated similar output"},
                                 {"text", out->text.substr(0, 200)}});
    }
  }
  return r;
}

// ---------------------------------------------------------------------------
// tool group: execute every call of one assistant reply, in order
// ---------------------------------------------------------------------------
FsResult AgentLoop::run_tool_group(const Json& calls, Json* messages) {
  for (size_t i = 0; i < calls.size(); ++i) {
    Json call = calls[i];
    if (!call.is_object()) continue;
    Json normalized = prompt::normalize_tool_call(
        call, "call_" + std::to_string(turn_started_ms_) + "_" + std::to_string(i));
    std::string name = jsonutil::get<std::string>(normalized, "name", "");
    Json args = normalized.contains("args") ? normalized["args"] : Json::object();
    std::string call_id = jsonutil::get<std::string>(normalized, "id", "");

    // A tool call whose arguments failed to parse must NOT be executed with
    // empty arguments - that is how a "write file" becomes "truncate file".
    if (jsonutil::get<bool>(normalized, "argsParseError", false)) {
      Json err = Json::object();
      err["ok"] = false;
      err["error"] = "tool arguments are not valid JSON; retry with a valid object";
      err["errCode"] = "invalid-arg";
      Json msg = Json::object();
      msg["role"] = "tool";
      msg["toolCallId"] = call_id;
      msg["name"] = name;
      msg["content"] = err.dump();
      msg["isError"] = true;
      messages->push_back(msg);
      executed_calls_.push_back(Json{{"name", name}, {"args", args}, {"ok", false},
                                     {"summary", "bad arguments"}, {"detail", err.dump(2)}});
      continue;
    }
    if (name.empty()) {
      Json msg = Json::object();
      msg["role"] = "tool";
      msg["toolCallId"] = call_id;
      msg["content"] = "{\"ok\":false,\"error\":\"empty tool name\"}";
      msg["isError"] = true;
      messages->push_back(msg);
      continue;
    }

    // ask PARKS the turn, exactly like the permission gate.
    //
    // The tool used to return "asked":true immediately, so the model believed
    // the user had answered, the turn carried on, and the choice was dropped.
    // Parking here is what makes "ask 挂起任务" true.
    //
    // SEVERAL QUESTIONS AT ONCE: a model often asks two or three things in one
    // reply (options for A, options for B). The UI presents them as "1/2", so
    // the whole run of ask calls in this group is collected here and answered
    // together - parking on the first one and re-entering the model in between
    // would make the questions arrive one modal at a time.
    if (name == "ask" && !opts_.unattended) {
      Json asks = Json::array();
      size_t last_ask = i;
      for (size_t k = i; k < calls.size(); ++k) {
        Json c = prompt::normalize_tool_call(
            calls[k], "call_" + std::to_string(turn_started_ms_) + "_ask_" +
                          std::to_string(k));
        if (jsonutil::get<std::string>(c, "name", "") != "ask") continue;
        Json a = c.contains("args") ? c["args"] : Json::object();
        std::string q = jsonutil::get<std::string>(a, "question", "");
        if (q.empty()) continue;
        Json item = Json::object();
        item["askId"] = ++ask_seq_;
        item["callId"] = jsonutil::get<std::string>(c, "id", "");
        item["question"] = q;
        item["options"] = a.contains("options") && a["options"].is_array()
                              ? a["options"] : Json::array();
        item["recommended"] = jsonutil::get_int(a, "recommended", 0);
        item["allowCustom"] = jsonutil::get<bool>(a, "allowCustom", true);
        asks.push_back(item);
        last_ask = k;
      }
      if (asks.empty()) {
        Json err = Json::object();
        err["ok"] = false;
        err["error"] = "empty question";
        Json msg = Json::object();
        msg["role"] = "tool";
        msg["toolCallId"] = call_id;
        msg["name"] = name;
        msg["content"] = err.dump();
        msg["isError"] = true;
        messages->push_back(msg);
        continue;
      }
      state_ = LoopState::kWaiting;
      gate_messages_ = *messages;
      gate_calls_ = calls;
      gate_index_ = last_ask;  // resume after the LAST collected ask
      gate_tool_msgs_ = Json::array();
      pending_tool_ = name;
      pending_args_ = args;
      pending_call_id_ = call_id;
      pending_asks_ = asks;
      saved_messages_ = *messages;
      saved_valid_ = true;
      saved_user_text_ = last_user_text_;
      Json payload = Json::object();
      payload["sid"] = opts_.sid;
      payload["asks"] = asks;
      // Backward-compatible single-question fields.
      payload["askId"] = jsonutil::get_int(asks[0], "askId", 0);
      payload["question"] = jsonutil::get<std::string>(asks[0], "question", "");
      payload["options"] = asks[0]["options"];
      payload["recommended"] = asks[0]["recommended"];
      payload["allowCustom"] = asks[0]["allowCustom"];
      emit("ask.prompt", payload);
      return FsResult::success();
    }

    if (needs_approval(name, args)) {
      if (opts_.unattended) {
        // Nobody can answer a permission prompt in a subagent (or any other
        // headless run). Refuse with a machine-readable reason instead of
        // parking the loop forever - the model can pick another path.
        Json denied = Json::object();
        denied["ok"] = false;
        denied["error"] = "permission required but this run is unattended";
        denied["errCode"] = "permission-denied";
        denied["howToProceed"] =
            "use a read-only tool, or report what the user must run themselves";
        Json msg = Json::object();
        msg["role"] = "tool";
        msg["toolCallId"] = call_id;
        msg["name"] = name;
        msg["content"] = denied.dump();
        msg["isError"] = true;
        messages->push_back(msg);
        executed_calls_.push_back(Json{{"name", name}, {"args", args}, {"ok", false},
                                       {"summary", "permission denied (unattended)"},
                                       {"detail", denied.dump(2)}});
        continue;
      }
      // Park the whole group: approving runs this call and then continues with
      // the remaining ones (they are part of the same assistant message and
      // must all be answered).
      state_ = LoopState::kWaiting;
      gate_messages_ = *messages;
      gate_calls_ = calls;
      gate_index_ = i;
      gate_tool_msgs_ = Json::array();
      pending_tool_ = name;
      pending_args_ = args;
      pending_call_id_ = call_id;
      saved_messages_ = *messages;
      saved_valid_ = true;
      saved_user_text_ = last_user_text_;
      emit("permission.prompt", Json{{"sid", opts_.sid}, {"tool", name},
                                     {"args", args}, {"callId", call_id}});
      return FsResult::success();
    }

    current_call_id_ = call_id;
    FsResult tr = execute_tool(name, args);
    if (!tr.ok()) {
      // Tool infrastructure failure (not a tool-level error): stop the turn but
      // keep the transcript so the user can continue.
      saved_messages_ = *messages;
      saved_valid_ = true;
      state_ = LoopState::kTurnDone;
      return tr;
    }
    // execute_tool appended to pending_tool_results_; drain it into messages.
    for (auto& res : pending_tool_results_) {
      Json msg = Json::object();
      msg["role"] = "tool";
      msg["toolCallId"] = jsonutil::get<std::string>(res, "id", "");
      msg["name"] = jsonutil::get<std::string>(res, "name", "");
      Json body = res.contains("result") ? res["result"] : Json::object();
      // A vision tool returns an image. The model can only SEE it if the image
      // travels as an image content block, not as a data URI buried in a JSON
      // string - so the data URI is lifted out and the text keeps only the
      // metadata (a multi-hundred-KB base64 blob in `content` would also blow
      // the context for no benefit).
      std::string data_uri;
      if (body.is_object() && body.contains("dataUri") && body["dataUri"].is_string()) {
        data_uri = body["dataUri"].get<std::string>();
        body.erase("dataUri");
      }
      // dump_safe: a tool result can carry bytes that are not UTF-8 (a legacy
      // encoded file, a terminal's output). nlohmann::json::dump() throws on
      // those, and that exception used to escape the loop and lose the turn.
      msg["content"] = body.is_string() ? body.get<std::string>() : dump_safe(body);
      // Within the turn the model may need the real payload to act on, so the
      // result is NOT projected here - but it is still bounded: one 1 MB read
      // must not be re-sent in every subsequent round of this turn. The
      // projection in HistoryBuilder is what a LATER turn sees.
      msg["content"] = utf8::truncate(msg["content"].get<std::string>(), 16000,
                                      "\n... (result truncated)");
      if (!jsonutil::get<bool>(body, "ok", true)) msg["isError"] = true;
      if (!data_uri.empty()) {
        Json parts = Json::array();
        Json text = Json::object();
        text["type"] = "text";
        text["text"] = msg["content"].get<std::string>();
        parts.push_back(text);
        Json img = Json::object();
        img["type"] = "image";
        img["dataUri"] = data_uri;
        parts.push_back(img);
        // The neutral form carries parts; prompt::adapt renders them per
        // provider (OpenAI image_url / Anthropic base64 source).
        msg["parts"] = parts;
      }
      messages->push_back(msg);
    }
    pending_tool_results_ = Json::array();

    // Round boundary: the model call and all of its tools are done. A pending
    // 挂起 (or 中止) parks HERE, so the next model call is never issued.
    if (suspend_requested_ || abort_requested_) {
      bool aborted = abort_requested_;
      suspend_requested_ = false;
      abort_requested_ = false;
      saved_messages_ = *messages;
      saved_valid_ = true;
      saved_user_text_ = last_user_text_;
      parked_ = true;
      state_ = LoopState::kIdle;
      {
        std::lock_guard<std::mutex> lk(live_mu_);
        live_running_ = false;
      }
      if (persist_live) persist_live(opts_.sid, Json{{"running", false}});
      // 中止 and 挂起 are different outcomes: 中止 must surface the persistent
      // "任务已中断 / 继续" bar, 挂起 the 恢复 control. Emitting "suspended" for
      // both made 中止 look like a suspend whenever it landed between rounds -
      // the bar then never appeared and 继续 could not be found.
      if (aborted) {
        emit("loop.interrupted", Json{{"sid", opts_.sid}, {"resumable", true},
                                      {"error", "stopped by user"}});
      } else {
        emit("loop.state", Json{{"state", "suspended"}, {"sid", opts_.sid}});
      }
      return FsResult::success();
    }
  }
  return FsResult::success();
}

// approve：权限门；批准后执行当前调用并把整组跑完，再回到模型
FsResult AgentLoop::approve(bool yes) {
  if (state_ != LoopState::kWaiting) {
    return FsResult::error(ErrCode::kBusy, "not waiting for permission");
  }
  // Nothing was gated (ask-style wait): just leave the waiting state. Re-running
  // the model here would duplicate a request that was never blocked.
  if (gate_calls_.is_array() && gate_calls_.empty() && pending_tool_.empty()) {
    state_ = LoopState::kPlanning;
    return FsResult::success();
  }
  Json messages = gate_messages_.is_array() ? gate_messages_ : saved_messages_;
  Json calls = gate_calls_.is_array() ? gate_calls_ : Json::array();
  size_t idx = gate_index_;

  auto append_result = [&messages](const std::string& call_id, const std::string& name,
                                   const Json& result, bool is_error) {
    Json msg = Json::object();
    msg["role"] = "tool";
    msg["toolCallId"] = call_id;
    msg["name"] = name;
    msg["content"] = result.is_string() ? result.get<std::string>() : dump_safe(result);
    if (is_error) msg["isError"] = true;
    messages.push_back(msg);
  };

  if (!yes) {
    // Denied: report it truthfully so the model can choose another path.
    Json denied = Json::object();
    denied["ok"] = false;
    denied["error"] = "user denied permission";
    denied["errCode"] = "permission-denied";
    append_result(pending_call_id_, pending_tool_, denied, true);
    executed_calls_.push_back(Json{{"name", pending_tool_}, {"args", pending_args_},
                                   {"ok", false}, {"summary", "denied by user"},
                                   {"detail", denied.dump(2)}});
  } else {
    current_call_id_ = pending_call_id_;
    state_ = LoopState::kToolCalling;
    FsResult tr = execute_tool(pending_tool_, pending_args_);
    for (auto& res : pending_tool_results_) {
      Json body = res.contains("result") ? res["result"] : Json::object();
      append_result(jsonutil::get<std::string>(res, "id", pending_call_id_),
                    jsonutil::get<std::string>(res, "name", pending_tool_), body,
                    !jsonutil::get<bool>(body, "ok", true));
    }
    pending_tool_results_ = Json::array();
    if (!tr.ok()) {
      saved_messages_ = messages;
      saved_valid_ = true;
      state_ = LoopState::kTurnDone;
      return tr;
    }
  }
  pending_tool_.clear();
  pending_args_ = Json::object();
  pending_call_id_.clear();

  // Run the REST of the group (calls after the one just approved). Any of them
  // may itself need approval; run_tool_group re-parks in that case.
  Json rest = Json::array();
  for (size_t i = idx + 1; i < calls.size(); ++i) rest.push_back(calls[i]);
  gate_calls_ = Json::array();
  gate_index_ = 0;
  gate_messages_ = Json::array();
  if (!rest.empty()) {
    state_ = LoopState::kToolCalling;
    FsResult r = run_tool_group(rest, &messages);
    if (!r.ok() || state_ == LoopState::kWaiting || state_ == LoopState::kIdle) {
      if (state_ == LoopState::kWaiting) {
        // Parked again on a later call in the same group: keep the transcript
        // accumulated so far with the pending group.
        gate_messages_ = messages;
      }
      return r;
    }
  }
  // Feed everything back to the model and continue the turn.
  saved_messages_ = messages;
  saved_valid_ = true;
  state_ = LoopState::kPlanning;
  return run_loop(messages, opts_.model, active_tools());
}

// ask 回答：把用户的选择作为 ask 工具的结果回填，然后继续本轮。
//
// A reply can carry several ask calls; the UI shows them as 1/2, 2/2 and reports
// each answer separately. The turn only resumes once EVERY question in the group
// has an answer - resuming after the first would leave the rest unanswered and
// the model would see a half-filled tool group.
FsResult AgentLoop::answer_ask(int ask_id, const std::string& selection) {
  if (state_ != LoopState::kWaiting || !pending_asks_.is_array() ||
      pending_asks_.empty()) {
    return FsResult::error(ErrCode::kBusy, "no pending question to answer");
  }
  // Which question is this an answer to?
  Json target = Json::object();
  for (auto& a : pending_asks_) {
    if (!a.is_object()) continue;
    int id = static_cast<int>(jsonutil::get_int(a, "askId", 0));
    if (ask_id == 0 || id == ask_id) {
      target = a;
      break;
    }
  }
  if (target.empty()) {
    // A stale answer (the user clicked an old dialog): ignore it rather than
    // resolving the CURRENT question with the wrong choice.
    return FsResult::error(ErrCode::kInvalidArg, "stale ask id");
  }
  int id = static_cast<int>(jsonutil::get_int(target, "askId", 0));
  ask_answers_[std::to_string(id)] = selection;
  emit("ask.answered", Json{{"askId", id}, {"selection", selection},
                            {"sid", opts_.sid}});

  // Not every question answered yet: stay parked, the UI shows the next one.
  int answered = 0;
  for (auto& a : pending_asks_) {
    if (!a.is_object()) continue;
    std::string k = std::to_string(jsonutil::get_int(a, "askId", 0));
    if (ask_answers_.contains(k)) ++answered;
  }
  if (answered < static_cast<int>(pending_asks_.size())) {
    return FsResult::success();
  }

  // All answered: append one tool result per ask, in call order, then resume.
  Json messages = gate_messages_.is_array() ? gate_messages_ : saved_messages_;
  for (auto& a : pending_asks_) {
    if (!a.is_object()) continue;
    std::string k = std::to_string(jsonutil::get_int(a, "askId", 0));
    std::string answer = ask_answers_.contains(k)
                             ? ask_answers_[k].get<std::string>() : std::string();
    Json result = Json::object();
    result["ok"] = true;
    result["question"] = jsonutil::get<std::string>(a, "question", "");
    result["answer"] = answer;
    Json msg = Json::object();
    msg["role"] = "tool";
    msg["toolCallId"] = jsonutil::get<std::string>(a, "callId", "");
    msg["name"] = "ask";
    msg["content"] = dump_safe(result);
    messages.push_back(msg);
    executed_calls_.push_back(
        Json{{"name", "ask"},
             {"args", Json{{"question", a["question"]}}},
             {"ok", true},
             {"summary", answer},
             {"detail", dump_safe(result, 2)}});
  }
  pending_asks_ = Json::array();
  ask_answers_ = Json::object();
  pending_tool_.clear();
  pending_args_ = Json::object();
  pending_call_id_.clear();
  gate_calls_ = Json::array();
  gate_index_ = 0;
  gate_messages_ = Json::array();
  saved_messages_ = messages;
  saved_valid_ = true;
  state_ = LoopState::kPlanning;
  return run_loop(messages, opts_.model, active_tools());
}

// 真调工具并把结果回填 pending_tool_results_
FsResult AgentLoop::execute_tool(const std::string& name, const Json& args) {
  ToolContext ctx;
  ctx.sid = opts_.sid;
  ctx.pid = opts_.pid;
  ctx.cwd = opts_.cwd;
  ctx.auto_mode = opts_.context.auto_mode;
  ctx.permission = opts_.permission;
  ctx.approved = true;  // 到达这里说明已放行（或不需要批准）
  ctx.thinking_lang = opts_.thinking_lang;
  ctx.emit = [this](const std::string& e, const Json& p) { emit(e, p); };

  emit("tool.start", Json{{"sid", opts_.sid}, {"name", name}, {"args", args},
                          {"callId", current_call_id_}});
  Json result = Json::object();
  FsResult r = invoke_tool ? invoke_tool(ctx, name, args)
                           : FsResult::error(ErrCode::kNotSupported,
                                             "tool execution not wired: " + name);
  if (r.ok()) {
    result = r.data;
    if (!result.is_object()) result = Json{{"value", result}};
    if (!result.contains("ok")) result["ok"] = true;
  } else {
    result["ok"] = false;
    result["error"] = r.message.empty() ? err_code_name(r.code) : r.message;
    result["errCode"] = err_code_name(r.code);
    if (r.denied()) result["denial"] = r.denial.reason;
  }
  pending_tool_results_.push_back(
      Json{{"name", name}, {"args", args}, {"id", current_call_id_},
           {"result", result}});

  std::string summary;
  if (!r.ok()) {
    summary = result.contains("error") && result["error"].is_string()
                  ? result["error"].get<std::string>()
                  : err_code_name(r.code);
  } else if (result.contains("summary") && result["summary"].is_string()) {
    summary = result["summary"].get<std::string>();
  } else if (result.contains("path") && result["path"].is_string()) {
    summary = result["path"].get<std::string>();
  } else if (result.contains("content") && result["content"].is_string()) {
    summary = "ok";
  } else {
    summary = "ok";
  }
  Json call = Json::object();
  call["name"] = name;
  call["args"] = args;
  call["id"] = current_call_id_;
  call["ok"] = r.ok();
  call["summary"] = summary;
  std::string body = dump_safe(result, 2);
  // Truncation must land on a character boundary: half a CJK character is
  // invalid UTF-8 and json::dump() throws when this is replayed next turn.
  body = utf8::truncate(body, 4000, "\n... (truncated)");
  call["detail"] = body;
  // A subagent call stores the id it created as a first-class field. The UI needs
  // it to offer "打开记录" on a RELOADED transcript, and recovering it by parsing
  // the (possibly truncated) detail JSON is a fallback, not a contract.
  if (name == "subagent_spawn" || name == "subagent_follow_up") {
    std::string sub_id = jsonutil::get<std::string>(result, "id", "");
    if (!sub_id.empty()) call["subagentId"] = sub_id;
  }
  executed_calls_.push_back(call);
  {
    // A tab reopened mid-turn rebuilds its work log from this snapshot; without
    // it the completed tool rows vanished when switching away and back.
    std::lock_guard<std::mutex> lk(live_mu_);
    live_tools_.push_back(call);
    while (live_tools_.size() > 200) live_tools_.erase(live_tools_.begin());
  }

  emit("tool.update", Json{{"sid", opts_.sid}, {"name", name}, {"ok", r.ok()},
                           {"summary", summary}, {"detail", body},
                           {"callId", current_call_id_}});
  return FsResult::success();
}

// ---------------------------------------------------------------------------
// turn execution
// ---------------------------------------------------------------------------
FsResult AgentLoop::run_turn(const std::string& user_text) {
  state_ = LoopState::kPlanning;
  HistoryBuilder::Options hopts = opts_.history;
  applied_steering_.clear();
  if (current_auto_round_.load() == 0) empty_auto_streak_ = 0;
  // Reuse the durable summary from a previous run/turn. Without this a reopened
  // session starts from the full transcript, which no longer fits the window
  // and gets replaced by a lossy short view.
  if (compaction_load) {
    Json st = compaction_load(opts_.sid);
    if (st.is_object()) {
      hopts.prior_summary = jsonutil::get<std::string>(st, "summary", "");
      hopts.compacted_turns = jsonutil::get_int(st, "compactedTurns", 0);
      hopts.prior_turn_id = jsonutil::get<std::string>(st, "lastTurnId", "");
      // No state at all => empty, NOT "full". Defaulting to full made the
      // short-view save guard (`prior_mode != "full"`) reject the very first
      // short view, so a long session never got a durable baseline.
      hopts.prior_mode = jsonutil::get<std::string>(st, "mode", "");
    }
  }
  // The system prefix is owned by AgentContext, which is refreshed by the host
  // before every send. If the host did not provide one (unit tests, embedded
  // use), fall back to the legacy explicit prefix.
  if (opts_.context.cwd.empty()) opts_.context.cwd = opts_.cwd;
  if (opts_.context.model.empty()) opts_.context.model = opts_.model;
  if (opts_.context.permission.empty()) opts_.context.permission = opts_.permission;
  if (opts_.context.thinking_lang.empty()) opts_.context.thinking_lang = opts_.thinking_lang;
  if (!opts_.context.tools.is_array() || opts_.context.tools.empty()) {
    // Legacy path: the host put tool definitions into history.tools.
    opts_.context.tools = hopts.tools;
  } else {
    // One source of truth for both the prompt index and the provider's tool
    // list: they used to be able to disagree (context built from the registry,
    // history.tools set once at creation), which silently hid new tools.
    hopts.tools = opts_.context.tools;
  }
  opts_.history.tools = hopts.tools;
  std::string built = AgentContext::build_system(opts_.context);
  if (!built.empty()) hopts.system_prefix = built;

  // Compaction decision inputs. The water level is the LAST request's prompt
  // tokens against the model window - a fact, not an estimate - so a session
  // compacts when the window is actually filling up rather than at an arbitrary
  // turn count.
  hopts.last_prompt_tokens = last_in_tokens_;
  hopts.context_limit = context_limit;
  if (compress_requested_) {
    hopts.force_compress = true;
    compress_requested_ = false;
  }

  Json messages = Json::array();
  Json full = Json::object();
  bool have_history = false;
  if (load_history && load_history(&full).ok()) have_history = true;
  if (have_history) {
    HistoryBuilder hb(hopts);
    // Estimate the raw conversation size first: the decision is reported to the
    // UI (a compaction the user cannot see is a surprise), and the builder then
    // reuses the same numbers to pick the cut point.
    int64_t raw = HistoryBuilder::estimate_tokens(
        full.contains("turns") ? full["turns"] : Json::array());
    HistoryBuilder::CompressDecision dec =
        HistoryBuilder::should_compress(hopts, raw);
    HistoryResult hist = hb.build_neutral(full);
    messages = hist.messages;
    if (!messages.is_array()) messages = Json::array();
    // A FULL compression is durable context: persist the summary and the prefix
    // it replaces, so the next turn/run replays summary + new turns instead of
    // the entire transcript. A SHORT view is also saved when there was no
    // durable full summary yet (an existing long session would otherwise
    // regenerate the same lossy short view on every reopen); it is never
    // allowed to overwrite a durable FULL summary.
    bool save_compaction =
        hist.compressed && hist.compacted_turns > 0 && !hist.summary.empty() &&
        (hist.mode == "full" ||
         (hist.mode == "short-view" && hopts.prior_mode != "full"));
    if (save_compaction && compaction_save) {
      Json state = Json::object();
      state["summary"] = hist.summary;
      state["compactedTurns"] = hist.compacted_turns;
      state["lastTurnId"] = hist.compacted_turn_id;
      state["mode"] = hist.mode;
      state["savedAt"] = WallClock().now_ms();
      compaction_save(opts_.sid, state);
    }
    if (hist.compressed) {
      int64_t total_turns = full.contains("turns") && full["turns"].is_array()
                                ? static_cast<int64_t>(full["turns"].size())
                                : 0;
      emit("context.compressed", Json{
              {"sid", opts_.sid},
              {"mode", hist.mode},
              {"from", hist.compressed_from},
              {"to", hist.compressed_to},
              {"droppedTurns", hist.dropped_turns},
              {"keptTurns", total_turns - hist.dropped_turns},
              {"reason", hist.mode == "short-view" ? "window-too-small"
                                                   : dec.reason},
              {"waterPct", dec.pct},
              {"promptTokens", dec.prompt_tokens},
              {"contextLimit", dec.context_limit}});
    } else if (dec.compress || hopts.force_compress) {
      // Wanted to compress but there was nothing to drop (a single huge turn).
      // Say so instead of silently doing nothing.
      emit("context.compressSkipped", Json{{"sid", opts_.sid},
                                           {"reason", dec.reason},
                                           {"turns", full.contains("turns")
                                                         ? full["turns"].size()
                                                         : 0}});
    }
  }
  if (messages.empty()) {
    Json system_msg = Json::object();
    system_msg["role"] = "system";
    system_msg["content"] = hopts.system_prefix;
    messages.push_back(system_msg);
  }
  // Replace the stored system prompt with the freshly built one: the session
  // file's first message is whatever the previous turn used, and the prefix
  // must reflect the CURRENT model/permission/tool set.
  if (!messages.empty() && jsonutil::get<std::string>(messages[0], "role", "") == "system") {
    messages[0]["content"] = hopts.system_prefix;
  }

  // Newest user message carries the volatile block. Older turns keep their
  // stored text (already unwrapped), so the cached prefix stays identical.
  std::string runtime = runtime_block();
  Json user_msg = Json::object();
  user_msg["role"] = "user";
  user_msg["content"] = AgentContext::wrap_user_turn(runtime, user_text);
  messages.push_back(user_msg);

  last_user_text_ = user_text;
  turn_started_ms_ = WallClock().now_ms();
  executed_calls_ = Json::array();
  saved_valid_ = false;
  parked_ = false;  // a fresh turn is not a continuation of a parked one
  recent_outputs_.clear();
  // Write the user's message to disk NOW, not when the answer is ready.
  //
  // The turn can run for minutes. Until it finished, the session file had no
  // record of what was asked, so closing the tab and reopening it showed the
  // transcript as it was BEFORE this turn - the question vanished while the
  // agent kept working in the background ("关掉标签页回来看不到历史").
  persist_user_turn_only(turn_started_ms_);
  user_turn_persisted_ = true;
  emit("turn.started", Json{{"sid", opts_.sid}, {"text", user_text},
                            {"startedAt", turn_started_ms_},
                            {"autoRound", current_auto_round_.load()}});
  return run_loop(messages, opts_.model, active_tools());
}

// A turn must never disappear because something threw.
//
// The whole point of this wrapper: a tool result that is not valid UTF-8 used to
// make nlohmann::json::dump() throw inside run_loop, the exception unwound all
// the way to the backend call, and NOTHING was persisted - the user's question
// stayed on disk with no answer, no tool log, no error. "我前面讲的所有内容都
// 必须保存" means the failure path has to write too.
FsResult AgentLoop::run_loop(Json messages, const std::string& model, const Json& tools) {
  // Auto-continue used to recurse (`return run_loop(new_messages, ...)`), so an
  // unbounded auto mode eventually overflowed the stack. Iterate instead: each
  // round returns with `auto_pending_` set, and this loop starts the next one
  // with the same (conservative) exception handling.
  Json cur_messages = std::move(messages);
  std::string cur_model = model;
  Json cur_tools = tools;
  for (;;) {
    auto_pending_ = false;
    FsResult r;
    try {
      r = run_loop_impl(std::move(cur_messages), cur_model, cur_tools);
    } catch (const std::exception& e) {
      return fail_turn(std::string("internal error: ") + e.what());
    } catch (...) {
      return fail_turn("internal error");
    }
    if (!auto_pending_) return r;
    cur_messages = std::move(auto_pending_messages_);
    cur_model = auto_pending_model_;
    cur_tools = std::move(auto_pending_tools_);
  }
}

FsResult AgentLoop::fail_turn(const std::string& error) {
  // A parked turn is not a failure: it was saved and the UI offers 恢复.
  if (parked_) return FsResult::error(ErrCode::kInternal, error);
  std::string partial;
  {
    std::lock_guard<std::mutex> lk(live_mu_);
    partial = live_text_;
    live_running_ = false;
  }
  if (partial.empty() && last_text_.empty()) {
    // Nothing streamed yet; the message list may still hold a narration.
    for (size_t i = 0; i < interim_texts_.size(); ++i) {
      if (interim_texts_[i].is_string()) partial += interim_texts_[i].get<std::string>();
    }
  }
  // The persist call itself must not escape: this function runs from a catch
  // block, and an I/O failure here would re-throw out of the handler and lose
  // even the notification. A turn that cannot be written still has to be REPORTED.
  try {
    persist_failed_turn(error, partial);
  } catch (...) {
  }
  state_ = LoopState::kTurnDone;
  {
    std::lock_guard<std::mutex> lk(live_mu_);
    live_running_ = false;
  }
  try {
    if (persist_live) persist_live(opts_.sid, Json{{"running", false}});
  } catch (...) {
  }
  emit("loop.interrupted", Json{{"sid", opts_.sid}, {"resumable", false},
                                {"error", error}, {"saved", true}});
  return FsResult::error(ErrCode::kInternal, error);
}

// Write the assistant half of a turn that did not finish.
//
// `error` is stored on the turn so a reload can say WHY it stopped; the partial
// text and every tool call that actually ran are kept, because they are the work
// the user watched happen. executed_calls_/interim_texts_ are cleared afterwards:
// the turn is closed, the next one starts with an empty log.
bool AgentLoop::persist_failed_turn(const std::string& error,
                                    const std::string& partial_text) {
  if (!persist_turn) return false;
  bool has_tools = executed_calls_.is_array() && !executed_calls_.empty();
  bool has_interim = interim_texts_.is_array() && !interim_texts_.empty();
  if (partial_text.empty() && !has_tools && !has_interim && error.empty()) return false;
  int64_t now = WallClock().now_ms();
  if (!user_turn_persisted_) {
    persist_user_turn_only(turn_started_ms_ > 0 ? turn_started_ms_ : now);
    user_turn_persisted_ = true;
  }
  Json asst = Json::object();
  asst["role"] = "assistant";
  asst["streamSummary"] = partial_text;
  asst["toolCalls"] = executed_calls_;
  if (has_interim) asst["interim"] = interim_texts_;
  if (has_interim && !interim_after_.empty()) {
    // Where each note belongs relative to the tool calls (see interim_after_).
    Json afters = Json::array();
    for (size_t i = 0; i < interim_after_.size(); ++i) afters.push_back(interim_after_[i]);
    asst["interimAfter"] = afters;
  }
  asst["startedAt"] = turn_started_ms_ > 0 ? turn_started_ms_ : now;
  asst["ts"] = now;
  asst["error"] = error;
  // A failed turn is finished, not streaming: without this the reloaded
  // transcript showed a permanently "running" answer.
  asst["failed"] = true;
  int ar = current_auto_round_.load();
  if (ar > 0) asst["autoRound"] = ar;
  persist_turn(asst);
  executed_calls_ = Json::array();
  interim_texts_ = Json::array();
  interim_after_.clear();
  user_turn_persisted_ = false;
  return true;
}

FsResult AgentLoop::run_loop_impl(Json messages, const std::string& model, const Json& tools) {
  state_ = LoopState::kPlanning;
  {
    // Publish the turn as "live" so a reopened tab can render the in-flight
    // answer instead of showing nothing until it lands.
    std::lock_guard<std::mutex> lk(live_mu_);
    live_running_ = true;
    live_text_.clear();
    live_interim_ = Json::array();
    live_interim_after_.clear();
    live_tools_ = Json::array();
    live_started_ms_ = turn_started_ms_;
  }
  ModelCallResult result;
  // Guidance queued before this turn (or while it was starting) rides on the
  // first model call too.
  apply_steering(&messages);
  FsResult r = call_model_once(messages, tools, &result);
  // 中止 during the reply: the transfer was aborted on purpose, so this is a
  // PARK, not a failure. Keep what the model already said as context (otherwise
  // the resumed turn re-answers from scratch) and let the UI show 恢复.
  //
  // NOTE: this is abort_requested_, NOT suspend_requested_. 挂起 lets the round
  // finish and parks before the next model call instead.
  if (abort_requested_.load()) {
    abort_requested_ = false;
    if (!result.text.empty()) {
      messages.push_back(Json{{"role", "assistant"},
                              {"content", result.text + "\n\n[stopped by user]"}});
    }
    saved_messages_ = messages;
    saved_valid_ = true;
    saved_user_text_ = last_user_text_;
    parked_ = true;
    suspend_requested_ = false;
    state_ = LoopState::kIdle;
    {
      std::lock_guard<std::mutex> lk(live_mu_);
      live_running_ = false;
    }
    if (persist_live) persist_live(opts_.sid, Json{{"running", false}});
    emit("loop.state", Json{{"state", "suspended"}, {"sid", opts_.sid}});
    return FsResult::success();
  }
  if (!r.ok() || !result.error.empty()) {
    // The user turn is already on disk (written when the turn started). The
    // ASSISTANT half is persisted here too: whatever was streamed before the
    // failure, plus every tool call that ran. Without this the transcript on
    // reload showed the question and NOTHING else - "重新打开发现真没了".
    saved_messages_ = messages;
    saved_valid_ = true;
    saved_user_text_ = last_user_text_;
    std::string why = result.error.empty() ? r.message : result.error;
    persist_failed_turn(why.empty() ? "provider error" : why, result.text);
    {
      std::lock_guard<std::mutex> lk(live_mu_);
      live_running_ = false;
    }
    if (persist_live) persist_live(opts_.sid, Json{{"running", false}});
    emit("loop.interrupted", Json{{"sid", opts_.sid}, {"resumable", true},
                                  {"error", why}});
    state_ = LoopState::kTurnDone;
    return r;
  }

  int rounds = 0;
  int repeat_count = 0;
  std::string last_signature;
  std::string stop_note;   // set when the tool loop had to be cut short
  std::string stop_code;   // machine-readable reason ("round-limit"|"repeat")
  bool zh = opts_.context.lang == ContextLang::kZh;
  while (true) {
    if (result.tool_calls.empty()) {
      // Guidance that arrived while the final answer streamed deserves one more
      // model call in the same turn; it must not be turned into a new
      // conversation with the old turn left open.
      if (apply_steering(&messages)) {
        state_ = LoopState::kPlanning;
        ModelCallResult next;
        FsResult nr = call_model_once(messages, tools, &next);
        if (!nr.ok() || !next.error.empty()) {
          saved_messages_ = messages;
          saved_valid_ = true;
          std::string why = next.error.empty() ? nr.message : next.error;
          persist_failed_turn(why.empty() ? "provider error" : why, result.text);
          {
            std::lock_guard<std::mutex> lk(live_mu_);
            live_running_ = false;
          }
          if (persist_live) persist_live(opts_.sid, Json{{"running", false}});
          emit("loop.interrupted", Json{{"sid", opts_.sid}, {"resumable", true},
                                        {"error", why}});
          state_ = LoopState::kTurnDone;
          return nr;
        }
        result = next;
        continue;
      }
      break;
    }
    if (++rounds > opts_.max_tool_rounds) {
      // A safety stop, not a design feature. Instead of just stopping, the turn
      // ENDS normally and a NEW auto round starts with an explicit instruction
      // to review the work so far - a long task should not be cut in half by a
      // counter. The notice is worded as coming from the TOOL, not the user, so
      // the model does not mistake it for the user asking to wrap up.
      stop_code = "round-limit";
      stop_note = zh
          ? "本轮工具调用达到上限（" + std::to_string(opts_.max_tool_rounds) +
                " 次），已自动开新一轮。请先审视已完成的工作与当前进度，再决定下一步。"
          : "this turn reached the tool-call limit (" +
                std::to_string(opts_.max_tool_rounds) +
                "); a new round was started automatically. Review what you have "
                "done and where the task stands before continuing.";
      break;
    }
    state_ = LoopState::kToolCalling;

    // Same call repeated verbatim: the model is stuck (usually because it never
    // saw a result). Stop the loop and end the turn with an explanation - an
    // error return would leave the UI thinking the send itself failed.
    Json calls = result.tool_calls;
    std::string signature = calls.dump();
    if (signature == last_signature) {
      ++repeat_count;
    } else {
      repeat_count = 0;
      last_signature = signature;
    }
    if (repeat_count >= opts_.max_repeat_calls) {
      stop_code = "repeat";
      stop_note = zh
          ? "模型连续 " + std::to_string(repeat_count + 1) +
                " 次发出同一个工具调用而没有进展，已暂停本轮。"
          : "the model issued the same tool call " +
                std::to_string(repeat_count + 1) +
                " times without progress; the turn was paused.";
      break;
    }

    // The assistant message first (it owns the tool_calls), then one tool message
    // per call, in order.
    //
    // Its TEXT is the round's narration ("let me look at the config first"), not
    // the answer: the turn's answer is whatever the LAST round says. It is emitted
    // separately as `assistant.interim` so the UI can file it under the work log
    // instead of gluing it onto the answer - and so it survives a reload (only the
    // final text was stored before, so the narration was lost).
    if (!result.text.empty()) {
      // `tools` is the number of tool calls ALREADY EXECUTED when this narration
      // arrived. The UI needs it to place the note in the stream: a round can
      // call several tools, and "before the tool with the same index as the
      // note" put a later thought above tools that really came earlier.
      emit("assistant.interim", Json{{"sid", opts_.sid}, {"text", result.text},
                                     {"round", rounds},
                                     {"tools", static_cast<int>(executed_calls_.size())}});
      interim_texts_.push_back(result.text);
      interim_after_.push_back(static_cast<int>(executed_calls_.size()));
      // The LIVE snapshot needs the same split: this round's text is narration,
      // not the answer, and the next round starts a new answer. Clear live_text_
      // so a tab reopened mid-turn does not show all prior narration as the
      // reply. Keep the narration in the live work log instead.
      {
        std::lock_guard<std::mutex> lk(live_mu_);
        live_interim_.push_back(result.text);
        live_interim_after_.push_back(static_cast<int>(executed_calls_.size()));
        live_text_.clear();
        while (live_interim_.size() > 40) {
          live_interim_.erase(live_interim_.begin());
          if (!live_interim_after_.empty()) live_interim_after_.erase(
              live_interim_after_.begin());
        }
      }
      // Bound it: a model that narrates every round of a very long turn would
      // otherwise grow this (and the turn it is persisted with) without limit.
      // The last few narrations are what a reader actually wants.
      while (interim_texts_.size() > 40) {
        interim_texts_.erase(interim_texts_.begin());
        if (!interim_after_.empty()) interim_after_.erase(interim_after_.begin());
      }
    }
    Json asst = Json::object();
    asst["role"] = "assistant";
    asst["content"] = result.text;
    Json neutral_calls = Json::array();
    for (size_t i = 0; i < calls.size(); ++i) {
      neutral_calls.push_back(prompt::normalize_tool_call(
          calls[i], "call_" + std::to_string(turn_started_ms_) + "_" +
                        std::to_string(rounds) + "_" + std::to_string(i)));
    }
    asst["toolCalls"] = neutral_calls;
    messages.push_back(asst);

    FsResult gr = run_tool_group(neutral_calls, &messages);
    if (!gr.ok()) return gr;
    if (state_ == LoopState::kWaiting) {
      // Parked on approval: saved_messages_ already holds the transcript.
      return FsResult::success();
    }
    if (state_ == LoopState::kIdle) return FsResult::success();  // suspended

    // User guidance that arrived during the tool work is carried into the next
    // model call of THIS turn.
    apply_steering(&messages);

    ModelCallResult next;
    FsResult nr = call_model_once(messages, tools, &next);
    if (!nr.ok() || !next.error.empty()) {
      saved_messages_ = messages;
      saved_valid_ = true;
      std::string why = next.error.empty() ? nr.message : next.error;
      // Same contract as the first round: the work done so far is saved (tool
      // calls included), not only the user's question.
      persist_failed_turn(why.empty() ? "provider error" : why, result.text);
      {
        std::lock_guard<std::mutex> lk(live_mu_);
        live_running_ = false;
      }
      if (persist_live) persist_live(opts_.sid, Json{{"running", false}});
      emit("loop.interrupted", Json{{"sid", opts_.sid}, {"resumable", true},
                                    {"error", why}});
      state_ = LoopState::kTurnDone;
      return nr;
    }
    result = next;
  }

  // ---- turn finished ----
  last_text_ = result.text;
  last_messages_ = messages;
  bool auto_continue = false;
  std::string auto_why;
  if (!stop_note.empty()) {
    // The turn ends normally (the user's send succeeded) but says why it was
    // cut short, and the unexecuted calls are dropped rather than replayed.
    // `loop.paused` (not `loop.interrupted`) because the turn produced real
    // output: showing the "interrupted, continue?" bar for a limit that the user
    // never asked for read like a crash. The notice goes into the answer itself.
    emit("loop.paused", Json{{"sid", opts_.sid}, {"reason", stop_code},
                             {"note", stop_note}, {"rounds", rounds}});
    if (!result.text.empty()) result.text += "\n\n";
    result.text += "[" + stop_note + "]";
    result.tool_calls = Json::array();
  }
  int64_t ended_ms = WallClock().now_ms();
  // Was THIS turn an auto-continue round? The nudge text is stored as the user
  // turn's promptRaw, so without a marker a reloaded transcript showed the
  // system's "Continue." instruction as if the USER had typed it - and the auto
  // round was glued onto the previous exchange with no divider.
  int this_auto_round = current_auto_round_.load();
  Json user_turn = Json::object();
  user_turn["role"] = "user";
  user_turn["promptRaw"] = last_user_text_;
  user_turn["ts"] = turn_started_ms_ > 0 ? turn_started_ms_ : ended_ms;
  if (this_auto_round > 0) {
    user_turn["autoRound"] = this_auto_round;
    user_turn["system"] = true;  // not typed by the user: render as a marker
  }
  Json asst_turn = Json::object();
  asst_turn["role"] = "assistant";
  asst_turn["streamSummary"] = result.text;
  asst_turn["toolCalls"] = executed_calls_;
  // The between-tools narration: kept with the turn so the work log can render it
  // after a reload. Without it the model's own commentary on what it was doing
  // simply disappeared when the session was reopened.
  if (interim_texts_.is_array() && !interim_texts_.empty()) {
    asst_turn["interim"] = interim_texts_;
    if (!interim_after_.empty()) {
      Json afters = Json::array();
      for (size_t i = 0; i < interim_after_.size(); ++i) afters.push_back(interim_after_[i]);
      asst_turn["interimAfter"] = afters;
    }
  }
  // Guidance injected mid-turn is part of the conversation; persist it so a
  // replay does not silently lose what the user told the model.
  if (!applied_steering_.empty()) {
    Json g = Json::array();
    for (size_t i = 0; i < applied_steering_.size(); ++i) g.push_back(applied_steering_[i]);
    asst_turn["guidance"] = g;
  }
  asst_turn["startedAt"] = turn_started_ms_ > 0 ? turn_started_ms_ : ended_ms;
  asst_turn["ts"] = ended_ms;
  if (this_auto_round > 0) asst_turn["autoRound"] = this_auto_round;
  Json usage = Json::object();
  usage["in"] = last_in_tokens_;
  usage["out"] = last_out_tokens_;
  asst_turn["usage"] = usage;
  // Did this turn actually DO anything? An auto round that ends without a single
  // tool call has nothing to continue from - continuing is how auto mode spun
  // five "模型输出当前没有新的指令，到此结束" rounds in a row.
  bool turn_had_tools = executed_calls_.is_array() && !executed_calls_.empty();
  executed_calls_ = Json::array();
  interim_texts_ = Json::array();
  interim_after_.clear();
  if (persist_turn) {
    // The user turn was already written when the turn STARTED (see run_turn);
    // writing it again here would duplicate it in the transcript.
    if (!user_turn_persisted_) persist_turn(user_turn);
    persist_turn(asst_turn);
  }
  user_turn_persisted_ = false;
  emit("turn.done", Json{{"sid", opts_.sid}, {"summary", result.text},
                         {"startedAt", turn_started_ms_}, {"endedAt", ended_ms},
                         {"usage", usage}, {"autoRound", this_auto_round}});
  state_ = LoopState::kTurnDone;
  saved_valid_ = false;
  {
    // The answer is on disk now, so the live snapshot must stop serving it:
    // otherwise a reopened tab would show the streamed copy AND the stored turn.
    std::lock_guard<std::mutex> lk(live_mu_);
    live_running_ = false;
    live_text_.clear();
    live_interim_ = Json::array();
    live_interim_after_.clear();
    live_tools_ = Json::array();
  }
  // Clear the crash-safe copy too: the turn landed, so it must not be replayed as
  // a "crashed" turn on the next open.
  if (persist_live) persist_live(opts_.sid, Json{{"running", false}});

  // A cut-short turn (tool-round limit) DOES continue automatically: the point
  // of the limit is to break up a long run, not to end the task. The new round
  // is told to review progress first, so the model does not lose the thread.
  if (stop_note.empty()) {
    auto_continue = should_auto_continue(&auto_why);
  } else if (stop_code == "round-limit") {
    auto_continue = true;
    auto_why = zh ? "工具调用达到上限，开新一轮继续" : "tool-call limit reached; continuing";
  }
  // EMPTY-ROUND GUARD: an automatic round that executed no tool call produced
  // nothing to build on (the model said "到此结束" but a plan document was still
  // active). One such round is a signal to stop, not to ask again - BUT only
  // when no checklist item is still open. An open checklist is an explicit
  // instruction to keep going ("清单没清空，自动轮却不继续" was this guard
  // suppressing a todo-driven continuation after a text-only round).
  if (auto_continue && this_auto_round > 0 && !turn_had_tools) {
    ++empty_auto_streak_;
    bool open_todos = false;
    if (runtime_state) {
      RuntimeState st = runtime_state();
      if (st.todos.is_array()) {
        for (auto& t : st.todos) {
          if (t.is_object() && !jsonutil::get<bool>(t, "done", false)) {
            open_todos = true;
            break;
          }
        }
      }
    }
    // Two consecutive empty auto rounds mean the model is not making progress,
    // even if a checklist is still open. Continuing forever is how an apparently
    // "finished" task kept waking and re-rendering until the system committed
    // out (the stored session had dozens of SYSTEM-continue turns with empty
    // answers). A single empty round still continues when todos are open.
    if (!open_todos || empty_auto_streak_ >= 2) {
      auto_continue = false;
      auto_why.clear();
      emit("auto.halted", Json{{"sid", opts_.sid},
                               {"reason", zh ? "自动轮连续没有执行任何工具，已停止"
                                             : "auto rounds ran no tools; stopped"},
                               {"rounds", auto_continues_},
                               {"emptyRounds", empty_auto_streak_}});
    }
  } else if (turn_had_tools) {
    empty_auto_streak_ = 0;
  }

  // The model's emergency brake. It is consumed once: the current turn finishes
  // normally, but no further auto round starts. Ordinary uncertainty must be
  // parked in plan/todo instead of calling this.
  if (auto_stop_requested_.exchange(false)) {
    auto_continue = false;
    emit("auto.halted", Json{{"sid", opts_.sid},
                             {"reason", zh ? "模型请求强制停止自动轮"
                                           : "model requested auto stop"},
                             {"rounds", auto_continues_}});
  }

  // ---- auto mode: continue while the plan is still open ----
  if (auto_continue) {
    if (opts_.max_auto_continues <= 0 ||
        auto_continues_ < opts_.max_auto_continues) {
      ++auto_continues_;
      state_ = LoopState::kIdle;
      // Announce the round BEFORE it starts. Without this the next round's text
      // simply appeared under the previous answer with no marker, and the session
      // list showed nothing while it ran - the "稀里糊涂" report.
      emit("auto.next", Json{{"sid", opts_.sid}, {"reason", auto_why},
                             {"round", auto_continues_},
                             {"maxRounds", opts_.max_auto_continues}});
      // Continue on the same conversation. The model's OWN last answer must be
      // in the history first: without it the continuation request looks like
      // "user said: continue" out of nowhere and the model repeats the work.
      Json msgs = messages;
      if (!result.text.empty()) {
        Json asst = Json::object();
        asst["role"] = "assistant";
        asst["content"] = result.text;
        msgs.push_back(asst);
      }
      std::string runtime = runtime_block();
      // The nudge must NOT read like a user instruction. It is a TOOL notice:
      // the previous round hit the tool-call cap, which says nothing about the
      // user's wishes. Worded as "SYSTEM:" so the model can tell it apart from
      // both the user and its own reasoning, and told to REVIEW first - a bare
      // "continue" makes it barrel on and often declare the task finished.
      std::string nudge;
      if (stop_code == "round-limit") {
        nudge = opts_.context.lang == ContextLang::kZh
                    ? "SYSTEM: 上一轮因工具调用次数达到上限而被系统自动切分（不是用户要求停止，"
                      "也不代表任务已完成）。请先审视已完成的工作与当前进度，再继续推进；"
                      "只有在确实全部完成时才收尾并给出总结。"
                    : "SYSTEM: the previous round was split automatically because it "
                      "reached the tool-call limit. This is NOT a user request to "
                      "stop and does NOT mean the task is done. Review what you have "
                      "completed and where the task stands, then continue; only wrap "
                      "up if everything really is finished.";
      } else {
        nudge = opts_.context.lang == ContextLang::kZh
                    ? "SYSTEM: 继续。按当前规划推进下一步；全部完成就收尾并给出总结。"
                    : "SYSTEM: continue. Take the next open step in the plan; if "
                      "everything is done, wrap up with a summary.";
      }
      Json user_msg = Json::object();
      user_msg["role"] = "user";
      user_msg["content"] = AgentContext::wrap_user_turn(runtime, nudge);
      msgs.push_back(user_msg);
      // The nudge is the stored prompt for THIS round; mark it as a system round
      // so a reload does not present "Continue." as something the user typed.
      last_user_text_ = nudge;
      current_auto_round_.store(auto_continues_);
      turn_started_ms_ = WallClock().now_ms();
      // Hand the next round to the run_loop() trampoline instead of recursing.
      auto_pending_ = true;
      auto_pending_messages_ = msgs;
      auto_pending_model_ = opts_.model;
      auto_pending_tools_ = tools;
      return FsResult::success();
    }
    emit("auto.halted", Json{{"sid", opts_.sid},
                             {"reason", "auto-continue limit reached"},
                             {"rounds", auto_continues_}});
  }
  current_auto_round_.store(0);
  auto_continues_ = 0;
  return FsResult::success();
}

// 自动模式：未完成的工作还在就继续（有上限，防止无人值守地烧钱）
bool AgentLoop::should_auto_continue(std::string* why) {
  if (!opts_.context.auto_mode) return false;
  if (!runtime_state) return false;
  RuntimeState st = runtime_state();
  // LAYER 1 - the task list (todo): the per-turn process. Open items mean the
  // current work is not finished, so the next round has something to do.
  int open_todos = 0;
  if (st.todos.is_array()) {
    for (auto& t : st.todos) {
      if (t.is_object() && !jsonutil::get<bool>(t, "done", false)) ++open_todos;
    }
  }
  if (open_todos > 0) {
    if (why) *why = std::to_string(open_todos) + " unfinished checklist item(s)";
    return true;
  }
  // LAYER 2 - the plan DOCUMENT. An active document means the whole job is not
  // done and the model still owes the next task list. It is NOT a competing view
  // of the checklist: no priority, no "newer replaces the other" - both exist at
  // the same time by design.
  if (st.plan.is_object() && jsonutil::get<bool>(st.plan, "exists", false)) {
    std::string status = jsonutil::get<std::string>(st.plan, "status", "");
    std::string content = jsonutil::get<std::string>(st.plan, "content", "");
    if (!content.empty() && status != "done") {
      if (why) *why = "plan document is still active";
      return true;
    }
  }
  return false;
}

// Persist the user's message on its own, the moment the turn starts.
//
// A turn can run for minutes; until it finished, the session file had no record
// of what was asked, so closing the tab and reopening it showed the transcript as
// it was BEFORE this turn. Writing here means the question survives a reload, a
// crash, or a turn that never produces an answer.
void AgentLoop::persist_user_turn_only(int64_t now_ms) {
  if (!persist_turn) return;
  if (last_user_text_.empty()) return;  // nothing typed (e.g. an auto round)
  Json user_turn = Json::object();
  user_turn["role"] = "user";
  user_turn["promptRaw"] = last_user_text_;
  user_turn["ts"] = turn_started_ms_ > 0 ? turn_started_ms_ : now_ms;
  int ar = current_auto_round_.load();
  if (ar > 0) {
    user_turn["autoRound"] = ar;
    user_turn["system"] = true;
  }
  persist_turn(user_turn);
}

// Live snapshot for a UI that (re)opens a running session.
Json AgentLoop::live_state() const {
  std::lock_guard<std::mutex> lk(live_mu_);
  Json j = Json::object();
  j["running"] = live_running_;
  j["text"] = live_text_;
  j["startedAt"] = live_started_ms_;
  j["autoRound"] = current_auto_round_.load();
  j["tools"] = live_tools_;
  // Completed-round narration belongs in the reopened tab's WORK LOG, not in
  // its answer. The UI merges these arrays into ChatMsg::interim.
  j["interim"] = live_interim_;
  Json afters = Json::array();
  for (size_t i = 0; i < live_interim_after_.size(); ++i) {
    afters.push_back(live_interim_after_[i]);
  }
  j["interimAfter"] = afters;
  return j;
}

// Persist the in-flight turn, throttled.
//
// The turn is appended to the session only when it FINISHES, so a crash (the
// webview crash the user hit) lost the whole answer. The live snapshot existed,
// but only in memory - which is exactly what a crash takes with it. Writing it
// to disk every couple of seconds means a crashed session still has the user's
// question and whatever the model had produced.
//
// Throttled because a fast model emits hundreds of deltas per second and each
// write is a file write; 2s is far below "the user notices the loss" and far
// above "the disk notices the load".
void AgentLoop::maybe_persist_live() {
  if (!persist_live) return;
  int64_t now = WallClock().now_ms();
  if (now - last_live_persist_ms_ < 2000) return;
  last_live_persist_ms_ = now;
  Json snap = live_state();
  // The user's question must survive too: it is the half the UI cannot rebuild.
  snap["userText"] = last_user_text_;
  snap["ts"] = turn_started_ms_;
  snap["autoRound"] = current_auto_round_.load();
  persist_live(opts_.sid, snap);
}

// addon2 任务健壮性：打断后续接
FsResult AgentLoop::continue_task(const std::string& guidance) {
  if (!saved_valid_) {
    if (guidance.empty()) {
      // Nothing saved and nothing typed: there is nothing to continue. The UI's
      // 继续 button sends an empty guidance on purpose - it used to reach send("")
      // and look dead.
      return FsResult::error(ErrCode::kInvalidArg, "nothing to continue");
    }
    return send(guidance);
  }
  Json msgs = saved_messages_;
  if (!guidance.empty()) {
    // Guidance (the user's own words) becomes the next user message; an EMPTY
    // guidance continues the SAME task without putting an empty question in
    // front of the model.
    Json g = Json::object();
    g["role"] = "user";
    g["content"] = guidance;
    msgs.push_back(g);
    last_user_text_ = guidance;
  }
  turn_started_ms_ = WallClock().now_ms();
  saved_valid_ = false;
  // The turn is running again: leaving these set would keep loop_state()
  // reporting "suspended" for a task that is visibly working.
  parked_ = false;
  suspend_requested_ = false;
  abort_requested_ = false;
  state_ = LoopState::kPlanning;
  emit("loop.resumed", Json{{"sid", opts_.sid}, {"guidance", guidance}});
  return run_loop(msgs, opts_.model, active_tools());
}

}  // namespace fs
