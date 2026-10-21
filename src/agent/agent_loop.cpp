// agent_loop.cpp - Agent 主循环状态机实现
#include "agent_loop.hpp"

#include <string>
#include <vector>

#include "fs_path.hpp"
#include "fs_simhash.hpp"

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

AgentLoop::AgentLoop(Options opts) : opts_(std::move(opts)) {}

void AgentLoop::emit(const std::string& event, const Json& payload) {
  if (opts_.event) opts_.event(event, payload);
}

// send：用户输入进入 planning（手动模式；自动模式 plan 未闭合时由 finish_turn 触发 autoNext）
FsResult AgentLoop::send(const std::string& text) {
  // kTurnDone is a PARKED state, not a running one: the previous turn finished,
  // whether it succeeded or was interrupted. Treating it as busy (the old
  // `state_ != kIdle` test) meant that after one failed turn every later message
  // went into the steering queue and was never sent - the session looked
  // permanently interrupted and could not be typed into again.
  if (state_ != LoopState::kIdle && state_ != LoopState::kTurnDone) {
    // 正在运行：进入引导队列（addon1.md 当前传递）
    steering_queue_.push_back(text);
    emit("steering.queued", Json{{"len", (int)steering_queue_.size()}});
    return FsResult::success();
  }
  // 合并引导队列（当前队列内容 >1 时合并输入）
  std::string combined = text;
  if (!steering_queue_.empty()) {
    combined = text;
    for (auto& s : steering_queue_) combined += "\n" + s;
    steering_queue_.clear();
  }
  state_ = LoopState::kPlanning;
  FsResult r = run_turn(combined);
  return r;
}

// append：追加输入（进入引导队列）
FsResult AgentLoop::append(const std::string& text) {
  steering_queue_.push_back(text);
  emit("steering.queued", Json{{"len", (int)steering_queue_.size()}});
  return FsResult::success();
}

// 挂起：置位，当前 tool 跑完后不再发新调用
void AgentLoop::suspend() {
  suspend_requested_ = true;
  emit("loop.state", Json{{"state", "suspending"}});
}

void AgentLoop::resume() {
  suspend_requested_ = false;
  emit("loop.state", Json{{"state", "resumed"}});
}

void AgentLoop::cancel_wake() {
  wake_scheduled_ = false;
  wake_at_ms_ = 0;
  emit("wake.cancelled", Json::object());
}

FsResult AgentLoop::on_subagent_done(const std::string& subagent_id,
                                     const std::string& summary) {
  // 自动模式：子代理完成唤醒父（agent-loop.md 2）
  emit("subagent.done", Json{{"id", subagent_id}, {"summary", summary}});
  if (state_ == LoopState::kIdle) {
    // 唤醒继续（P3 v1：仅发事件；真正自动续跑由 FsApi 层驱动）
    return FsResult::success();
  }
  return FsResult::success();
}

FsResult AgentLoop::on_terminal_exit(const std::string& tid, int exit_code) {
  emit("terminal.exit", Json{{"tid", tid}, {"exitCode", exit_code}});
  return FsResult::success();
}

FsResult AgentLoop::on_timer() {
  if (wake_scheduled_ && wake_at_ms_ > 0) {
    emit("wake.fired", Json{{"source", "timer"}});
    wake_scheduled_ = false;
    wake_at_ms_ = 0;
  }
  return FsResult::success();
}

// 高风险动作（agent-loop.md 3：写/删/发网络/跑终端）
bool AgentLoop::is_high_risk(const std::string& tool) const {
  return tool.rfind("file.write", 0) == 0 || tool.rfind("file.diffWrite", 0) == 0 ||
         tool.rfind("file.rm", 0) == 0 || tool.rfind("network.", 0) == 0 ||
         tool.rfind("terminal.", 0) == 0;
}

// 需要用户点头的动作，按权限档区分（用户明确要求的口径）：
//   approve     : 所有写/删/网络/终端都要问
//   permissive  : 项目内的写/删直接放行；**系统级操作**（项目外的文件读写、
//                 起终端、发网络）要问
//   full        : 不问
// "项目内"的判定看工具参数里的 path：相对路径就在项目里，绝对路径且不在
// 项目根下就是系统操作。
bool AgentLoop::needs_approval(const std::string& tool, const Json& args) const {
  if (opts_.permission == "full") return false;
  if (tool.rfind("network.", 0) == 0 || tool.rfind("terminal.", 0) == 0) {
    return true;  // 系统级：与档位无关（full 已提前返回）
  }
  bool write = tool.rfind("file.write", 0) == 0 ||
               tool.rfind("file.diffWrite", 0) == 0 ||
               tool.rfind("file.rm", 0) == 0;
  if (!write) return false;
  if (opts_.permission == "approve") return true;
  // permissive：只对项目外（系统）路径询问。Resolve the path the same way the
  // file tools do and compare it with the project root - checking
  // `is_absolute()` alone misses rooted-but-drive-less paths and relative paths
  // that walk out with "..".
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

// 自动模式不确定上下文：高风险降级 deferred（agent-loop.md 3）
bool AgentLoop::should_deferred(const std::string& tool) const {
  // v1：自动模式下高风险工具且无用户确认标记 → deferred
  // P3 简单实现：自动模式下写/删/网络/终端都走权限门（approve 档语义）
  // 详细"不确定上下文"判定留联调。
  return is_high_risk(tool);
}

// approve：权限门（approve 档）；批准后从断点续跑（addon2：打断后续接）
FsResult AgentLoop::approve(bool yes) {
  if (state_ != LoopState::kWaiting) {
    return FsResult::error(ErrCode::kBusy, "not waiting for permission");
  }
  if (pending_tool_.empty()) {
    state_ = LoopState::kPlanning;
    return FsResult::success();
  }
  if (!yes) {
    // 拒绝：回填错误结果给模型，如实告知（不静默）
    Json result = Json::object();
    result["ok"] = false;
    result["error"] = "user denied permission";
    result["errCode"] = "permission-denied";
    pending_tool_results_.push_back(
        Json{{"name", pending_tool_}, {"args", pending_args_}, {"result", result}});
  } else {
    // 批准：真调工具，结果回填
    state_ = LoopState::kToolCalling;
    current_call_id_ = pending_call_id_;
    FsResult tr = execute_tool(pending_tool_, pending_args_);
    if (!tr.ok()) return tr;
  }
  pending_tool_.clear();
  pending_args_ = Json::object();
  // 从断点续跑：把本次 tool 结果追加到保存的 messages 再喂模型
  if (saved_valid_) {
    Json msgs = saved_messages_;
    Json tool_msg = Json::object();
    tool_msg["role"] = "user";
    Json content = Json::array();
    for (auto& tr2 : pending_tool_results_) {
      content.push_back(Json{{"type", "tool_result"},
                             {"tool_call_id", jsonutil::get<std::string>(tr2, "id", "")},
                             {"name", tr2["name"]},
                             {"content", tr2["result"].dump()}});
    }
    tool_msg["content"] = content;
    msgs.push_back(tool_msg);
    pending_tool_results_.clear();
    saved_valid_ = false;
    state_ = LoopState::kPlanning;
    return run_loop(msgs, opts_.model, opts_.history.tools);
  }
  state_ = LoopState::kToolCalling;
  return FsResult::success();
}

// ask 回答
FsResult AgentLoop::answer_ask(int ask_id, const std::string& selection) {
  emit("ask.answered", Json{{"askId", ask_id}, {"selection", selection}});
  state_ = LoopState::kPlanning;
  return FsResult::success();
}

// 处理一个工具调用（兼容扁平 {name,args} 与 OpenAI 原生 {function:{name,arguments}}）
FsResult AgentLoop::handle_tool_call(const Json& call) {
  std::string name = jsonutil::get<std::string>(call, "name", "");
  Json args = call.contains("args") ? call["args"] : Json::object();
  std::string call_id = jsonutil::get<std::string>(call, "id", "");
  if (name.empty() && call.contains("function") && call["function"].is_object()) {
    const Json& fn = call["function"];
    name = jsonutil::get<std::string>(fn, "name", "");
    if (fn.contains("arguments")) {
      const Json& a = fn["arguments"];
      if (a.is_string()) {
        try {
          args = Json::parse(a.get<std::string>());
        } catch (...) {
          args = Json::object();
          args["_raw"] = a.get<std::string>();
        }
      } else if (a.is_object()) {
        args = a;
      }
    }
  }
  if (name.empty()) return FsResult::error(ErrCode::kInvalidArg, "empty tool name");

  // 自动模式：ask 已被移除；高风险降级 deferred（agent-loop.md 3）
  bool auto_mode = jsonutil::get<bool>(opts_.history.env, "autoMode", false);
  bool ask = needs_approval(name, args);
  if (auto_mode && ask) {
    // 自动模式高风险：deferred，等待用户（手动模式才等批准；自动模式下发 permission.prompt 事件）
    state_ = LoopState::kWaiting;
    pending_tool_ = name;
    pending_args_ = args;
    pending_call_id_ = call_id;
    emit("permission.prompt", Json{{"tool", name}, {"args", args}});
    return FsResult::success();
  }
  // 手动模式：按档位决定要不要等批准（approve 全问；permissive 只问系统操作）
  if (!auto_mode && ask) {
    state_ = LoopState::kWaiting;
    pending_tool_ = name;
    pending_args_ = args;
    pending_call_id_ = call_id;
    emit("permission.prompt", Json{{"tool", name}, {"args", args}});
    return FsResult::success();
  }
  // 非高风险：直接执行（通过 invoke_tool 真调工具）
  current_call_id_ = call_id;
  return execute_tool(name, args);
}

// 真调工具并把结果回填 pending_tool_results_（addon2：工具必须真正执行）
FsResult AgentLoop::execute_tool(const std::string& name, const Json& args) {
  ToolContext ctx;
  ctx.sid = opts_.sid;
  ctx.pid = opts_.pid;
  ctx.cwd = opts_.cwd;
  ctx.auto_mode = jsonutil::get<bool>(opts_.history.env, "autoMode", false);
  ctx.permission = opts_.permission;
  // This call reached execute_tool either because it needed no approval or
  // because the user just approved it - either way the permission gate must not
  // ask again (a second refusal after "allow" is worse than never asking).
  ctx.approved = true;
  ctx.thinking_lang = opts_.thinking_lang;
  ctx.emit = [this](const std::string& e, const Json& p) { emit(e, p); };
  // The UI shows a tool the moment it STARTS, not when it finishes: a slow tool
  // (a build, a fetch) otherwise leaves the transcript blank for seconds and the
  // turn looks stuck. `tool.start` carries the args so the block can already
  // say WHAT is running.
  emit("tool.start", Json{{"name", name}, {"args", args}});
  // 进程（todo）/invoke_tool 由 Host 注入的 invoke_tool 转发时补齐
  Json result = Json::object();
  FsResult r = invoke_tool ? invoke_tool(ctx, name, args)
                           : FsResult::error(ErrCode::kNotSupported,
                                             "tool execution not wired: " + name);
  if (r.ok()) {
    result = r.data;
  } else {
    // 错误结果回填给模型（拒绝/失败都如实给，不静默）
    result["ok"] = false;
    result["error"] = r.message.empty() ? err_code_name(r.code) : r.message;
    result["errCode"] = err_code_name(r.code);
    if (r.denied()) result["denial"] = r.denial.reason;
  }
  pending_tool_results_.push_back(
      Json{{"name", name}, {"args", args}, {"id", current_call_id_},
           {"result", result}});

  // One line that describes the OUTCOME, shown on the collapsed row. The full
  // result goes into the expandable body; dumping the whole thing on the header
  // made every row as wide as a file.
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
  } else if (result.is_object() && result.size() == 0) {
    summary = "ok";
  } else {
    summary = "ok";
  }
  Json call = Json::object();
  call["name"] = name;
  call["args"] = args;
  call["ok"] = r.ok();
  call["summary"] = summary;
  // The body is the tool's own payload, trimmed: a 2 MB file read would be
  // written into the session file and re-parsed on every reload.
  std::string body = result.dump(2);
  if (body.size() > 4000) body = body.substr(0, 4000) + "\n... (truncated)";
  call["detail"] = body;
  executed_calls_.push_back(call);

  emit("tool.update", Json{{"name", name}, {"ok", r.ok()},
                           {"summary", summary}, {"detail", body}});
  return FsResult::success();
}

// 执行一轮（planning -> toolCalling -> ... -> turnDone）
// addon2：工具结果回填后再喂模型，循环直到模型不再要工具（完整 agent 循环）
FsResult AgentLoop::run_turn(const std::string& user_text) {
  HistoryBuilder::Options hopts = opts_.history;
  // The conversation so far, then the new user message. Building only
  // [system, user] (the old behaviour) made every turn stateless: the model
  // could not see anything said before, so follow-ups like "what did I just
  // ask?" were answered as if the session had just started.
  Json messages = Json::array();
  Json full = Json::object();
  bool have_history = false;
  if (load_history && load_history(&full).ok()) have_history = true;
  if (have_history) {
    HistoryResult hist = HistoryBuilder(hopts).build(full);
    messages = hist.messages;
    if (!messages.is_array()) messages = Json::array();
  }
  if (messages.empty()) {
    Json system_msg = Json::object();
    system_msg["role"] = "system";
    system_msg["content"] = hopts.system_prefix;
    messages.push_back(system_msg);
  }
  Json user_msg = Json::object();
  user_msg["role"] = "user";
  user_msg["content"] = user_text;
  messages.push_back(user_msg);
  last_user_text_ = user_text;
  turn_started_ms_ = WallClock().now_ms();  // this turn's clock starts here
  executed_calls_ = Json::array();          // fresh turn, fresh tool log
  saved_valid_ = false;  // 新一轮开始：清掉旧保存点
  return run_loop(messages, opts_.model, hopts.tools);
}

// 从给定 messages 续跑（工具循环 + 回填）；中断/失败时保存 messages 供继续
FsResult AgentLoop::run_loop(Json messages, const std::string& model, const Json& tools) {
  ModelCallRequest req;
  req.model = model;
  req.messages = messages;
  req.tools = tools;
  req.auto_mode = jsonutil::get<bool>(opts_.history.env, "autoMode", false);
  req.session_id = opts_.sid;  // x-opencode-session 透传（套餐连接）

  // 挂起中：发完当前调用后不再发新调用（v1：run_loop 结束后检查）
  //
  // Deltas are emitted AS THEY ARRIVE, not after the call returns: the whole
  // point of streaming is that the transcript fills in while the model is still
  // talking. `streamed` tracks whether anything was pushed, so a provider that
  // answers in one piece (no SSE) still gets its text emitted exactly once.
  if (turn_started_ms_ <= 0) turn_started_ms_ = WallClock().now_ms();
  bool streamed = false;
  auto on_delta = [this, &streamed](const std::string& piece) {
    if (piece.empty()) return;
    streamed = true;
    emit("stream.delta", Json{{"text", piece}});
  };
  ModelCallResult result;
  FsResult r = call_model ? call_model(req, &result, on_delta)
                          : FsResult::error(ErrCode::kInternal, "no model");
  if (!r.ok() || !result.error.empty()) {
    // 失败：保存断点，允许"打断后续接"（addon2）
    saved_messages_ = req.messages;
    saved_valid_ = true;
    emit("loop.interrupted", Json{{"resumable", true},
                                  {"error", result.error.empty() ? r.message : result.error}});
    Json turn = Json::object();
    turn["role"] = "assistant";
    turn["error"] = result.error.empty() ? r.message : result.error;
    state_ = LoopState::kTurnDone;
    return r;
  }
  // 非流式（或流式没吐任何东西）：整段补发一次，UI 才不会一片空白
  if (!streamed && !result.text.empty()) {
    emit("stream.delta", Json{{"text", result.text}});
  }
  // 工具调用：执行 + 回填结果 + 再喂模型，直到无 tool_calls
  int max_iters = 20;  // 防止死循环
  while (!result.tool_calls.empty() && max_iters-- > 0) {
    state_ = LoopState::kToolCalling;
    // 记录 assistant 的 tool_calls 消息（协议要求回填 tool 结果前先有对应的 assistant 消息）
    Json asst = Json::object();
    asst["role"] = "assistant";
    asst["content"] = result.text;
    asst["tool_calls"] = result.tool_calls;
    req.messages.push_back(asst);
    for (auto& c : result.tool_calls) {
      FsResult tr = handle_tool_call(c);
      if (!tr.ok()) return tr;
      if (state_ == LoopState::kWaiting) {
        // 等批准：保存断点，批准后由 continue_task 续跑
        saved_messages_ = req.messages;
        saved_valid_ = true;
        return FsResult::success();
      }
      if (suspend_requested_) {
        suspend_requested_ = false;
        saved_messages_ = req.messages;
        saved_valid_ = true;
        state_ = LoopState::kIdle;
        emit("loop.state", Json{{"state", "suspended"}});
        return FsResult::success();
      }
    }
    // 把工具结果喂回模型（tool result 作为 user 角色消息，标准 tool 回填格式）
    Json tool_msg = Json::object();
    tool_msg["role"] = "user";
    Json content = Json::array();
    for (auto& tr : pending_tool_results_) {
      content.push_back(Json{{"type", "tool_result"},
                             {"tool_call_id", jsonutil::get<std::string>(tr, "id", "")},
                             {"name", tr["name"]},
                             {"content", tr["result"].dump()}});
    }
    tool_msg["content"] = content;
    req.messages.push_back(tool_msg);
    pending_tool_results_.clear();
    ModelCallResult r2;
    bool streamed2 = false;
    auto on_delta2 = [this, &streamed2](const std::string& piece) {
      if (piece.empty()) return;
      streamed2 = true;
      emit("stream.delta", Json{{"text", piece}});
    };
    FsResult r2r = call_model ? call_model(req, &r2, on_delta2)
                              : FsResult::error(ErrCode::kInternal, "no model");
    if (!r2r.ok() || !r2.error.empty()) {
      saved_messages_ = req.messages;
      saved_valid_ = true;
      emit("loop.interrupted", Json{{"resumable", true},
                                    {"error", r2.error.empty() ? r2r.message : r2.error}});
      Json turn = Json::object();
      turn["role"] = "assistant";
      turn["error"] = r2.error.empty() ? r2r.message : r2.error;
      state_ = LoopState::kTurnDone;
      return r2r;
    }
    result = r2;
    if (!streamed2 && !result.text.empty()) {
      emit("stream.delta", Json{{"text", result.text}});
    }
  }
  // 落库：user 轮 + assistant 轮都要写。此前只发 turn.done 事件、从不落盘，
  // 于是下一轮 load_history 读到的 turns 永远是空的 —— 多轮对话看起来像每句
  // 都是新会话的第一句。
  //
  // Timestamps are written HERE, from the loop's own clock: append_turn used to
  // stamp both turns at write time, which is after the model already answered -
  // so a reloaded turn showed a duration of 0.0s no matter how long it took.
  int64_t ended_ms = WallClock().now_ms();
  Json user_turn = Json::object();
  user_turn["role"] = "user";
  user_turn["promptRaw"] = last_user_text_;
  user_turn["ts"] = turn_started_ms_ > 0 ? turn_started_ms_ : ended_ms;
  Json asst_turn = Json::object();
  asst_turn["role"] = "assistant";
  asst_turn["streamSummary"] = result.text;
  asst_turn["toolCalls"] = executed_calls_.size() > 0 ? executed_calls_
                                                      : result.tool_calls;
  executed_calls_ = Json::array();
  asst_turn["startedAt"] = turn_started_ms_ > 0 ? turn_started_ms_ : ended_ms;
  asst_turn["ts"] = ended_ms;
  if (persist_turn) {
    persist_turn(user_turn);
    persist_turn(asst_turn);
  }
  emit("turn.done", Json{{"summary", result.text},
                         {"startedAt", turn_started_ms_},
                         {"endedAt", ended_ms}});
  state_ = LoopState::kTurnDone;
  return FsResult::success();
}

// addon2 任务健壮性：打断后续接。从保存点追加引导文本并续跑。
FsResult AgentLoop::continue_task(const std::string& guidance) {
  if (!saved_valid_) {
    // 无保存点：把引导当普通新输入处理（不静默失败）
    return send(guidance);
  }
  if (state_ == LoopState::kWaiting) {
    // 等待批准中：先按批准语义继续，再把引导追加到断点
    state_ = LoopState::kPlanning;
  }
  Json msgs = saved_messages_;
  Json g = Json::object();
  g["role"] = "user";
  g["content"] = guidance;
  msgs.push_back(g);
  last_user_text_ = guidance;
  turn_started_ms_ = WallClock().now_ms();
  saved_valid_ = false;
  state_ = LoopState::kPlanning;
  emit("loop.resumed", Json{{"guidance", guidance}});
  return run_loop(msgs, opts_.model, opts_.history.tools);
}



FsResult AgentLoop::finish_turn(const Json& turn) {
  emit("turn.done", turn);
  return FsResult::success();
}

}  // namespace fs