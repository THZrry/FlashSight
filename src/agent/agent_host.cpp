// agent_host.cpp - AgentHost implementation (part 1: setup + send/suspend)
#include "agent_host.hpp"

#include "ask_tool.hpp"
#include "file_tools.hpp"
#include "mcp_tools.hpp"
#include "net_tools.hpp"
#include "summarize_tool.hpp"
#include "term_tools.hpp"
#include "vision_tools.hpp"

namespace fs {

AgentHost::AgentHost(AgentHostDeps deps) : deps_(std::move(deps)), wakes_(deps_.clock) {
  subagents_.set_done_fn([this](const SubagentRecord& r) {
    emit("subagent.done", r.to_json());
  });
  wakes_.set_fire_fn([this](const PendingWake& w) {
    Json p = Json::object();
    p["sid"] = w.sid;
    p["source"] = wake_source_str(w.source);
    p["ref"] = w.ref;
    emit("wake.fired", p);
  });
}

void AgentHost::emit(const std::string& event, const Json& payload) {
  // session.keypoint：收集要点，persist_turn 时写入当前 turn（单会话持久记忆）
  if (event == "session.keypoint" && payload.contains("points") &&
      payload["points"].is_array()) {
    // sid 需由调用方在 payload 带上；ToolContext.emit 闭包可注入
    std::string sid = jsonutil::get<std::string>(payload, "sid", "");
    if (!sid.empty()) {
      for (auto& kp : payload["points"]) {
        if (kp.is_string()) pending_keypoints_[sid].push_back(kp);
      }
    }
  }
  if (deps_.event) deps_.event(event, payload);
}

void AgentHost::register_builtin_tools(McpRegistry* mcp) {
  register_file_tools(&tools_);
  register_term_tools(&tools_);
  register_net_tools(&tools_);
  register_vision_tools(&tools_);
  register_mcp_tools(&tools_, mcp);
  register_ask_tool(&tools_);
  register_summarize_tool(&tools_);
}

AgentLoop& AgentHost::loop_for(const std::string& sid, bool auto_mode) {
  auto it = loops_.find(sid);
  if (it != loops_.end()) return *it->second;
  AgentLoop::Options opts;
  opts.sid = sid;
  opts.event = [this](const std::string& e, const Json& p) { emit(e, p); };
  (void)auto_mode;
  auto loop = std::unique_ptr<AgentLoop>(new AgentLoop(std::move(opts)));
  // 模型调用走宿主注入的 call_model
  loop->call_model = [this](const ModelCallRequest& req, ModelCallResult* out) -> FsResult {
    if (!deps_.call_model) {
      return FsResult::error(ErrCode::kNotSupported, "model not wired");
    }
    return deps_.call_model(req, out);
  };
  AgentLoop& ref = *loop;
  loops_[sid] = std::move(loop);
  return ref;
}

ToolContext AgentHost::make_ctx(const std::string& sid, bool auto_mode) {
  ToolContext ctx;
  ctx.sid = sid;
  ctx.auto_mode = auto_mode;
  // emit 闭包自动注入 sid（session.keypoint 等需要归属会话）
  ctx.emit = [this, sid](const std::string& e, const Json& p) {
    Json pp = p.is_object() ? p : Json::object();
    if (!pp.contains("sid")) pp["sid"] = sid;
    emit(e, pp);
  };
  ctx.invoke_tool = [this, sid, auto_mode](const std::string& name,
                                          const Json& args) -> FsResult {
    ToolContext inner = make_ctx(sid, auto_mode);
    return tools_.invoke(inner, name, args);
  };
  // terminal/net 回调由 FsApi 层注入（v1 留空，工具返回 not-wired）
  return ctx;
}

FsResult AgentHost::send(const std::string& sid, const std::string& text,
                         const Json& attachments, bool auto_mode) {
  (void)attachments;
  AgentLoop& loop = loop_for(sid, auto_mode);
  return loop.send(text);
}

FsResult AgentHost::append(const std::string& sid, const std::string& text) {
  AgentLoop& loop = loop_for(sid, false);
  return loop.append(text);
}

FsResult AgentHost::suspend(const std::string& sid) {
  auto it = loops_.find(sid);
  if (it == loops_.end()) return FsResult::success();
  it->second->suspend();
  return FsResult::success();
}

FsResult AgentHost::resume(const std::string& sid) {
  auto it = loops_.find(sid);
  if (it == loops_.end()) return FsResult::success();
  it->second->resume();
  return FsResult::success();
}

FsResult AgentHost::cancel_wake(const std::string& sid) {
  wakes_.cancel(sid);
  auto it = loops_.find(sid);
  if (it != loops_.end()) it->second->cancel_wake();
  emit("wake.cancelled", Json{{"sid", sid}});
  return FsResult::success();
}

FsResult AgentHost::tool_invoke(const std::string& sid, const std::string& name,
                                const Json& args, bool auto_mode) {
  // 自动模式：ask 已从工具列表移除
  if (auto_mode && name == "ask") {
    return FsResult::error(ErrCode::kBusy, "ask not available in auto mode");
  }
  ToolContext ctx = make_ctx(sid, auto_mode);
  FsResult r = tools_.invoke(ctx, name, args);
  // 落库 tool 调用记录（由调用方拼 turn；此处发 tool.update 事件）
  emit("tool.update", Json{{"sid", sid}, {"name", name}, {"ok", r.ok()}});
  return r;
}

FsResult AgentHost::ask_answer(const std::string& sid, int ask_id,
                               const std::string& selection) {
  AgentLoop& loop = loop_for(sid, false);
  return loop.answer_ask(ask_id, selection);
}

FsResult AgentHost::subagent_spawn(const std::string& sid, const std::string& kind,
                                   const std::string& task, const std::string& model,
                                   int depth, int64_t timeout_ms, std::string* out_id) {
  return subagents_.spawn(sid, kind, task, model, "", depth, timeout_ms, out_id);
}

FsResult AgentHost::subagent_cancel(const std::string& id) {
  return subagents_.cancel(id);
}

FsResult AgentHost::subagent_list(const std::string& sid) {
  Json arr = Json::array();
  for (auto& r : subagents_.list_for_sid(sid)) arr.push_back(r.to_json());
  return FsResult::success(arr);
}

FsResult AgentHost::on_terminal_exit(const std::string& sid, const std::string& tid,
                                     int exit_code) {
  // 后台终端退出：自动模式唤醒，手动模式徽标（事件统一发 terminal.exit + wake.fired）
  emit("terminal.exit", Json{{"sid", sid}, {"tid", tid}, {"exitCode", exit_code}});
  wakes_.schedule(sid, WakeSource::kTerminalExit, 0, tid);
  auto due = wakes_.due(0);
  return FsResult::success();
}

FsResult AgentHost::on_timer_tick() {
  int64_t now = deps_.clock ? deps_.clock->now_ms() : 0;
  auto due = wakes_.due(now);
  return FsResult::success(Json{{"fired", (int)due.size()}});
}

FsResult AgentHost::on_subagent_done(const std::string& id, const std::string& summary) {
  FsResult r = subagents_.complete_with_summary(id, summary, Json::object());
  if (!r.ok()) return r;
  const SubagentRecord* rec = subagents_.find(id);
  if (rec) {
    wakes_.schedule(rec->parent_sid, WakeSource::kSubagentDone, 0, id);
    wakes_.due(0);
  }
  return FsResult::success();
}

FsResult AgentHost::steering_push(const std::string& sid, const std::string& kind,
                                  const std::string& text) {
  SteeringKind k = (kind == "round-end") ? SteeringKind::kRoundEnd : SteeringKind::kImmediate;
  int id = steering_[sid].push(k, text);
  emit("steering.queued", Json{{"sid", sid}, {"id", id}, {"kind", kind}});
  return FsResult::success(Json{{"id", id}});
}

FsResult AgentHost::steering_edit(const std::string& sid, int id, const std::string& text) {
  bool ok = steering_[sid].edit(id, text);
  return ok ? FsResult::success()
            : FsResult::error(ErrCode::kNotFound, "steering item not found");
}

FsResult AgentHost::steering_erase(const std::string& sid, int id) {
  bool ok = steering_[sid].erase(id);
  return ok ? FsResult::success()
            : FsResult::error(ErrCode::kNotFound, "steering item not found");
}

FsResult AgentHost::steering_reorder(const std::string& sid, const std::vector<int>& order) {
  bool ok = steering_[sid].reorder(order);
  return ok ? FsResult::success()
            : FsResult::error(ErrCode::kInvalidArg, "bad reorder");
}

FsResult AgentHost::steering_list(const std::string& sid) {
  return FsResult::success(steering_[sid].to_json());
}

FsResult AgentHost::persist_turn(const std::string& sid, const Json& turn) {
  if (!deps_.append_turn) {
    return FsResult::error(ErrCode::kInternal, "persist not wired");
  }
  // 附上待落库的要点（session.summarize 经 session.keypoint 事件收集）
  Json t = turn;
  auto it = pending_keypoints_.find(sid);
  if (it != pending_keypoints_.end() && !it->second.empty()) {
    Json kps = Json::array();
    if (t.contains("keyPoints") && t["keyPoints"].is_array()) kps = t["keyPoints"];
    for (auto& kp : it->second) kps.push_back(kp);
    t["keyPoints"] = kps;
    it->second.clear();
  }
  return deps_.append_turn(sid, t);
}

}  // namespace fs
