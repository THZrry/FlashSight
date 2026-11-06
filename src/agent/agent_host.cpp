// agent_host.cpp - AgentHost implementation (part 1: setup + send/suspend)
#include "agent_host.hpp"

#include <set>

#include "ask_tool.hpp"
#include "check_dir_tools.hpp"
#include "file_tools.hpp"
#include "lsp_tools.hpp"
#include "mcp_tools.hpp"
#include "net_tools.hpp"
#include "orchestration_tools.hpp"
#include "regex_tools.hpp"
#include "skill_registry.hpp"
#include "summarize_tool.hpp"
#include "term_tools.hpp"
#include "todo_tools.hpp"
#include "vision_tools.hpp"
#include "common/fs_i18n.hpp"

namespace fs {

namespace {
// Set while a subagent's own loop is executing on this thread. reap_subagent_threads
// must not run here: a nested spawn would try to join an ancestor (or itself).
thread_local bool g_in_subagent_thread = false;

std::string platform_name() {
#if defined(_WIN32)
  return "windows";
#elif defined(__APPLE__)
  return "macos";
#else
  return "linux";
#endif
}

// Tools a read-only agent may use. A research agent that can write is a
// research agent that will "fix" what it was asked to inspect.
//
// file_regex is NOT here: it rewrites files (that is its whole purpose), so a
// read-only agent must not have it. It was in this list before the rename and
// would have let a "read-only" research subagent edit the tree.
bool read_only_tool(const std::string& n) {
  static const char* kAllowed[] = {
      "read_file", "list_dir", "find_files", "check_dir",
      "todo_add", "todo_list", "todo_check", "todo_clear",
      "fetch_url", "search_web", "load_skill", "list_skills",
      "lsp", "summarize_session", "ask"};
  for (const char* a : kAllowed) {
    if (n == a) return true;
  }
  return false;
}

bool preset_read_only(const std::string& kind) {
  for (auto& p : builtin_preset_agents()) {
    if (p.name == kind) return p.read_only;
  }
  return false;
}
}  // namespace

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
    // A wake that only tells the UI "something fired" is a dead end: the
    // terminal-exit / subagent-done / model-timer event is the reason a parked
    // task should resume, and nothing else consumes wake.fired. Start (or
    // steer) that turn here.
    start_wake_continuation(w);
  });
}

AgentHost::~AgentHost() {
  // Worker threads capture `this`; they must be joined before any member is
  // destroyed. A detached thread here would be a use-after-free.
  wait_wake_threads();
  wait_subagents();
}

void AgentHost::wait_wake_threads() {
  std::vector<WakeThread> jobs;
  {
    std::lock_guard<std::mutex> lk(wake_mu_);
    jobs.swap(wake_threads_);
  }
  for (size_t i = 0; i < jobs.size(); ++i) {
    if (jobs[i].thread.joinable()) jobs[i].thread.join();
  }
  std::lock_guard<std::mutex> lk(wake_mu_);
  wake_inflight_.clear();
}

void AgentHost::reap_wake_threads() {
  std::vector<WakeThread> finished;
  {
    std::lock_guard<std::mutex> lk(wake_mu_);
    for (std::vector<WakeThread>::iterator it = wake_threads_.begin();
         it != wake_threads_.end();) {
      if (it->done && it->done->load()) {
        finished.push_back(std::move(*it));
        it = wake_threads_.erase(it);
      } else {
        ++it;
      }
    }
  }
  for (size_t i = 0; i < finished.size(); ++i) {
    if (finished[i].thread.joinable()) finished[i].thread.join();
  }
}

void AgentHost::start_wake_continuation(const PendingWake& w) {
  if (w.sid.empty()) return;
  std::string note;
  switch (w.source) {
    case WakeSource::kTimer:
      note = "SYSTEM: 定时唤醒已到";
      if (!w.ref.empty()) note += "（备注：" + w.ref + "）";
      note += "。请先看规划/清单，按下一步继续；全部完成就收尾并给出总结。";
      break;
    case WakeSource::kTerminalExit:
      note = "SYSTEM: 后台终端 " + w.ref +
             " 已退出。先用 terminal_poll 读取它的输出，再按规划继续；全部完成就"
             "收尾并给出总结。";
      break;
    case WakeSource::kSubagentDone:
      note = "SYSTEM: 子代理 " + w.ref +
             " 已完成。读取它的摘要，按规划继续；全部完成就收尾并给出总结。";
      break;
    default:
      note = "SYSTEM: 唤醒事件已到。按当前规划继续。";
      break;
  }
  {
    std::lock_guard<std::recursive_mutex> host_lock(lock());
    std::map<std::string, std::unique_ptr<AgentLoop> >::iterator it = loops_.find(w.sid);
    if (it == loops_.end() || !it->second) return;  // nothing to continue
    LoopState st = it->second->state();
    bool busy = st != LoopState::kIdle && st != LoopState::kTurnDone;
    if (!busy) {
      std::lock_guard<std::mutex> wl(wake_mu_);
      if (!wake_inflight_.insert(w.sid).second) busy = true;
    }
    if (busy) {
      // The task is already working: this wake becomes guidance for the next
      // round rather than a second concurrent turn.
      steering_[w.sid].push(SteeringKind::kRoundEnd, note);
      emit("steering.queued", Json{{"sid", w.sid},
                                   {"kind", "round-end"},
                                   {"text", note}});
      return;
    }
  }
  std::shared_ptr<std::atomic<bool> > done(new std::atomic<bool>(false));
  std::string sid = w.sid;
  std::lock_guard<std::mutex> wl(wake_mu_);
  WakeThread job;
  job.done = done;
  job.thread = std::thread([this, sid, note, done]() {
    continue_task(sid, note);
    {
      std::lock_guard<std::mutex> lk(wake_mu_);
      wake_inflight_.erase(sid);
    }
    done->store(true);
  });
  wake_threads_.push_back(std::move(job));
}

void AgentHost::wait_subagents() {
  std::vector<SubagentThread> threads;
  {
    std::lock_guard<std::mutex> lock(subagent_mu_);
    threads.swap(subagent_threads_);
  }
  for (auto& t : threads) {
    if (t.thread.joinable()) t.thread.join();
  }
}

// Reap finished subagent threads.
//
// A std::thread that has run to completion but was never joined still holds its
// OS thread handle and its STACK (megabytes of reserved address space per thread
// on Windows) until join() is called. `subagent_threads_` was only ever swapped
// out in wait_subagents(), which runs at destruction - so every finished
// subagent's stack stayed alive for the life of the process, and the vector grew
// by one entry per spawn.
//
// *** PRECONDITION: THE CALLER MUST NOT HOLD THE HOST LOCK. ***
//
// This function JOINS threads, and a subagent's last acts (todo tick + emit) take
// the host lock. Calling it while holding that lock deadlocks the moment a
// subagent finishes: the joiner waits for the thread, the thread waits for the
// lock the joiner is holding. That is precisely the freeze the user saw "a few
// seconds after spawning a subagent" - the periodic tick fired, reaped, and
// hung the whole app. Every call site therefore calls this OUTSIDE its lock.
//
// The manager's record status is the truth for "this thread is done", so the
// entry carries the subagent id and the finished ones are joined and dropped
// here.
void AgentHost::reap_subagent_threads() {
  // Never reap from a thread that IS a subagent: a nested spawn would otherwise
  // try to join its own ancestor (or itself, once it finishes) and deadlock.
  if (g_in_subagent_thread) return;
  std::vector<SubagentThread> done;
  {
    std::lock_guard<std::recursive_mutex> host_lock(lock());
    std::lock_guard<std::mutex> lock(subagent_mu_);
    std::vector<SubagentThread> keep;
    keep.reserve(subagent_threads_.size());
    for (size_t i = 0; i < subagent_threads_.size(); ++i) {
      SubagentThread& st = subagent_threads_[i];
      const SubagentRecord* rec = subagents_.find(st.id);
      bool finished = !rec || rec->status != SubagentStatus::kRunning;
      if (finished) {
        done.push_back(std::move(st));
      } else {
        keep.push_back(std::move(st));
      }
    }
    subagent_threads_.swap(keep);
  }
  // Join with NO lock held (see the precondition above).
  for (auto& t : done) {
    if (t.thread.joinable()) t.thread.join();
  }
}

void AgentHost::emit(const std::string& event, const Json& payload) {
  std::lock_guard<std::recursive_mutex> host_lock(lock());
  // session.keypoint：收集要点，persist_turn 时写入当前 turn（单会话持久记忆）
  if (event == "session.keypoint" && payload.contains("points") &&
      payload["points"].is_array()) {
    // sid 需由调用方在 payload 带上；ToolContext.emit 闭包可注入
    std::string sid = jsonutil::get<std::string>(payload, "sid", "");
    if (!sid.empty()) {
      // Subagent worker threads emit into the same map as the main thread.
      std::lock_guard<std::mutex> lock(keypoint_mu_);
      for (auto& kp : payload["points"]) {
        if (kp.is_string()) pending_keypoints_[sid].push_back(kp);
      }
    }
  }
  if (deps_.event) deps_.event(event, payload);
}

void AgentHost::register_builtin_tools(McpRegistry* mcp) {
  register_file_tools(&tools_);
  register_check_dir_tools(&tools_);
  register_term_tools(&tools_);
  register_net_tools(&tools_);
  register_vision_tools(&tools_);
  register_mcp_tools(&tools_, mcp);
  register_ask_tool(&tools_);
  register_regex_tools(&tools_);
  register_todo_tools(&tools_);
  register_lsp_tools(&tools_);
  register_skill_tools(&tools_, &skills_);
  register_summarize_tool(&tools_);
  register_orchestration_tools(&tools_);
}

AgentLoop& AgentHost::loop_for(const std::string& sid, bool auto_mode,
                               const std::string& permission) {
  auto it = loops_.find(sid);
  if (it != loops_.end()) {
    it->second->set_permission(permission);
    it->second->set_auto_mode(auto_mode);
    return *it->second;
  }
  AgentLoop::Options opts;
  opts.sid = sid;
  opts.permission = permission;
  if (deps_.load_full) {
    Json full;
    if (deps_.load_full(sid, &full).ok()) {
      if (full.contains("meta") && full["meta"].is_object()) {
        opts.cwd = jsonutil::get<std::string>(full["meta"], "cwd", "");
        opts.pid = jsonutil::get<std::string>(full["meta"], "pid", "");
      }
      Json ss = full.value("sessionSettings", Json::object());
      opts.model = jsonutil::get<std::string>(ss, "model", "");
      opts.thinking = jsonutil::get<std::string>(ss, "thinking", "");
    }
  }
  // Compaction settings. Only TWO knobs remain, and both mean what they say:
  // the water-level threshold and how much stays verbatim when it fires.
  // (recent-turns and token-budget were removed: they folded history on every
  // turn regardless of the water level.)
  if (deps_.setting) {
    Json cp = deps_.setting(opts.pid, "history.compressAtPct", Json(80));
    Json kt = deps_.setting(opts.pid, "history.keepTurns", Json(8));
    if (cp.is_number()) opts.history.compress_at_pct = static_cast<int>(cp.get<int64_t>());
    if (kt.is_number()) opts.history.keep_turns = static_cast<int>(kt.get<int64_t>());
  }
  opts.event = [this](const std::string& e, const Json& p) { emit(e, p); };
  auto loop = std::unique_ptr<AgentLoop>(new AgentLoop(std::move(opts)));
  loop->call_model = [this](const ModelCallRequest& req, ModelCallResult* out,
                            const std::function<void(const std::string&)>& on_delta)
      -> FsResult {
    if (!deps_.call_model) {
      return FsResult::error(ErrCode::kNotSupported, "model not wired");
    }
    return deps_.call_model(req, out, on_delta);
  };
  std::string hist_sid = sid;
  loop->load_history = [this, hist_sid](Json* out) -> FsResult {
    if (!deps_.load_full) return FsResult::error(ErrCode::kNotSupported, "no history");
    return deps_.load_full(hist_sid, out);
  };
  loop->persist_turn = [this, hist_sid](const Json& turn) {
    persist_turn(hist_sid, turn);
    // Title upkeep runs after a turn lands: the placeholder appears on the first
    // turn, and the model summary only once there is something to summarise.
    if (deps_.title_upkeep) deps_.title_upkeep(hist_sid);
  };
  // Crash-safe snapshot of the turn in flight (see AgentLoop::maybe_persist_live).
  loop->persist_live = [this](const std::string& sid, const Json& live) {
    if (deps_.write_live) deps_.write_live(sid, live);
  };
  loop->invoke_tool = [this](const ToolContext& in, const std::string& name,
                             const Json& args) -> FsResult {
    // make_ctx reads session_ctx_ and the tool registry, both of which subagent
    // threads mutate; take the host lock for the SETUP only. The tool itself runs
    // unlocked: a slow tool must not block suspend, panel loads or a second
    // subagent.
    ToolContext ctx;
    {
      std::lock_guard<std::recursive_mutex> host_lock(lock());
      ctx = make_ctx(in.sid, in.auto_mode);
      ctx.permission = in.permission;
      ctx.cwd = in.cwd;
      ctx.pid = in.pid;
      ctx.approved = in.approved;
      ctx.thinking_lang = in.thinking_lang;
    }
    return tools_.invoke(ctx, name, args);
  };
  loop->runtime_state = [this, sid]() { return collect_runtime(sid); };
  AgentLoop& ref = *loop;
  loops_[sid] = std::move(loop);
  return ref;
}

// ---------------------------------------------------------------------------
// context construction
// ---------------------------------------------------------------------------
ContextInput AgentHost::build_context(const std::string& sid, bool auto_mode,
                                      const std::string& permission,
                                      const std::string& model,
                                      const std::string& thinking) {
  ContextInput in;
  in.auto_mode = auto_mode;
  in.permission = permission;
  in.model = model;
  in.thinking = thinking;
  in.agent_name = "FlashSight";
  in.sid = sid;

  // Thinking language: only when the user explicitly set zh/en.
  std::string tl = thinking_lang_;
  if (tl.empty() || tl == "auto") tl = "";
  in.thinking_lang = tl;
  // Output language: the settings page has offered this selector all along, but
  // nothing read the key, so picking a language changed nothing on the wire. It
  // is a real instruction now (empty = the model decides, which is the default).
  if (deps_.setting) {
    Json ol = deps_.setting("", "model.outputLang", Json(""));
    if (ol.is_string()) {
      std::string o = ol.get<std::string>();
      if (o == "zh" || o == "en") in.output_lang = o;
    }
  }
  if (tl == "zh") in.lang = ContextLang::kZh;
  else if (tl == "en") in.lang = ContextLang::kEn;
  else {
    // Follow the UI language when no thinking language is forced: a Chinese
    // user should not get an English system prompt.
    in.lang = i18n::effective_lang() == i18n::Lang::kEn ? ContextLang::kEn
                                                        : ContextLang::kZh;
  }

  // Tools: the registry is the single source of truth. `ask` is withheld in
  // auto mode (agent-loop.md §3) and listed as disabled instead.
  in.tools = tools_.neutral_defs();
  if (auto_mode) {
    Json filtered = Json::array();
    for (auto& t : in.tools) {
      if (jsonutil::get<std::string>(t, "name", "") == "ask") continue;
      filtered.push_back(t);
    }
    in.tools = filtered;
    in.allow_ask = false;
    in.disabled_tools.push_back("ask");
  }
  in.skills = skills_.list_json(parse_think_lang(tl.empty() ? "auto" : tl));

  // Session + project facts FIRST: the project id is what scopes every setting
  // lookup below (project value overrides the global one).
  std::string pid;
  Json full;
  if (deps_.load_full && deps_.load_full(sid, &full).ok()) {
    if (full.contains("meta") && full["meta"].is_object()) {
      in.cwd = jsonutil::get<std::string>(full["meta"], "cwd", "");
      in.session_title = jsonutil::get<std::string>(full["meta"], "title", "");
      pid = jsonutil::get<std::string>(full["meta"], "pid", "");
    }
  }
  in.pid = pid;
  if (deps_.setting) {
    Json subs = deps_.setting(pid, "subagents.maxParallel", Json(4));
    Json dep = deps_.setting(pid, "subagents.maxDepth", Json(3));
    if (subs.is_number()) in.max_subagent_parallel = static_cast<int>(subs.get<int64_t>());
    if (dep.is_number()) in.max_subagent_depth = static_cast<int>(dep.get<int64_t>());
    Json mem = deps_.setting(pid, "model.longMemory", Json::object());
    if (mem.is_object()) in.memory = mem;
  }
  in.os = platform_name();
  if (deps_.git_summary && !in.cwd.empty()) {
    Json g = deps_.git_summary(in.cwd);
    if (g.is_object()) in.git = g;
  }
  if (deps_.project_instructions && !in.cwd.empty()) {
    auto cit = project_ctx_.find(in.cwd);
    if (cit == project_ctx_.end()) {
      ProjectCtx pc;
      pc.instructions = deps_.project_instructions(in.cwd);
      if (deps_.instructions_source) pc.source = deps_.instructions_source(in.cwd);
      cit = project_ctx_.insert(std::make_pair(in.cwd, pc)).first;
    }
    in.project_instructions = cit->second.instructions;
    in.instructions_source = cit->second.source;
  }
  if (deps_.setting) {
    Json tools_map = deps_.setting(pid, "environment.cliTools", Json::object());
    if (tools_map.is_object()) in.cli_tools = tools_map;
    Json vars = deps_.setting(pid, "environment.vars", Json::object());
    if (vars.is_object()) {
      for (auto it2 = vars.begin(); it2 != vars.end(); ++it2) {
        in.env_names.push_back(it2.key());  // names only: values may be secrets
      }
    }
  }
  return in;
}

RuntimeState AgentHost::collect_runtime(const std::string& sid) {
  // *** MUST take the host lock. ***
  //
  // This runs on the TURN thread (AgentLoop::runtime_block), while subagent
  // threads concurrently insert into `subagents_.records_` and `todos_`. Reading
  // those std::maps without the lock is a data race: it can corrupt the tree
  // (crash) or spin inside a rebalanced node (hang). It only became likely with
  // TWO subagents, because that is when one thread mutates while another turn
  // reads - which matches "freezes only after spawning a second subagent".
  std::lock_guard<std::recursive_mutex> host_lock(lock());
  RuntimeState st;
  st.now_ms = deps_.clock ? deps_.clock->now_ms() : WallClock().now_ms();
  if (deps_.plan_get) {
    Json p = deps_.plan_get(sid);
    if (p.is_object()) st.plan = p;
  }
  hydrate_todos(sid);
  st.todos = todos_.to_json(sid);
  for (auto& r : subagents_.list_for_sid(sid)) {
    if (r.status != SubagentStatus::kRunning) continue;  // finished ones already reported
    Json j = Json::object();
    j["id"] = r.id;
    j["kind"] = r.kind;
    j["status"] = subagent_status_str(r.status);
    st.subagents.push_back(j);
  }
  return st;
}

ToolContext AgentHost::make_ctx(const std::string& sid, bool auto_mode) {
  ToolContext ctx;
  ctx.sid = sid;
  ctx.auto_mode = auto_mode;
  ctx.thinking_lang = thinking_lang_;
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
  // 进程（todo）：owner = 会话 sid（子代理上下文由 spawn 时另建 ctx 覆盖 owner）
  ctx.todo_op = [this, sid](const Json& args, Json* out) -> FsResult {
    FsResult r = todo_op(sid, args);
    if (r.ok() && out) *out = r.data;
    return r;
  };
  // 终端/网络回调由 FsApi 注入（reproc / NetClient）
  // 终端回调包一层：注入会话 sid（注册表按会话归属终端；工具层不感知）
  ctx.terminal_spawn = [this, sid](const Json& a, Json* out) -> FsResult {
    if (!deps_.terminal_spawn) {
      return FsResult::error(ErrCode::kNotSupported, "terminal spawn not wired");
    }
    Json args = a;
    if (!args.contains("sid")) args["sid"] = sid;
    return deps_.terminal_spawn(args, out);
  };
  ctx.terminal_poll = deps_.terminal_poll;
  ctx.terminal_kill = deps_.terminal_kill;
  ctx.net_fetch = deps_.net_fetch;
  // Search is its own callback (own request shape + parsing), not a marker
  // smuggled through net_fetch. The project id comes from the session's context
  // snapshot so a per-project search endpoint is honoured.
  std::string ctx_pid;
  {
    auto it = session_ctx_.find(sid);
    if (it != session_ctx_.end()) ctx_pid = it->second.pid;
  }
  ctx.web_search = [this, ctx_pid](const Json& a, Json* out) -> FsResult {
    if (!deps_.web_search) {
      return FsResult::error(ErrCode::kNotSupported,
                             "web search is not configured (Settings > Model > Search)");
    }
    Json req = a.is_object() ? a : Json::object();
    if (!req.contains("pid") && !ctx_pid.empty()) req["pid"] = ctx_pid;
    return deps_.web_search(req, out);
  };
  // Screen capture is a user-facing feature switch (Settings > Computer
  // control). Absent settings mean "allowed", so tests and the embedded build
  // behave like before.
  if (deps_.setting) {
    ctx.computer_enabled = [this, ctx_pid]() -> bool {
      Json v = deps_.setting(ctx_pid, "computer.enabled", Json(true));
      return v.is_boolean() ? v.get<bool>() : true;
    };
  }
  ctx.deny_check = deps_.deny_check;
  // Orchestration tools: plan / subagent / workflow. The tool layer only
  // forwards; the stores live here.
  ctx.plan_op = [this, sid](const Json& args, Json* out) -> FsResult {
    std::string op = jsonutil::get<std::string>(args, "op", "");
    if (op == "update") {
      if (!deps_.plan_update) {
        return FsResult::error(ErrCode::kNotSupported, "plan store not wired");
      }
      // The plan is a DOCUMENT. `content` replaces it; `status` is the one
      // machine-readable fact (active|done) that auto mode follows.
      Json plan = Json::object();
      plan["content"] = jsonutil::get<std::string>(args, "content", "");
      plan["status"] = jsonutil::get<std::string>(args, "status", "");
      FsResult r = deps_.plan_update(sid, plan);
      if (r.ok()) emit("plan.update", Json{{"sid", sid}, {"plan", r.data}});
      if (out) *out = r.data;
      return r;
    }
    if (!deps_.plan_get) return FsResult::error(ErrCode::kNotSupported, "plan store not wired");
    Json cur = deps_.plan_get(sid);
    if (out) *out = cur;
    return FsResult::success(cur);
  };
  // Timer wakes: the model schedules "come back in N seconds" and ENDS its
  // turn. Sleeping inside terminal_exec was the old workaround; it pinned a turn
  // worker (and, before the lock fix, the whole terminal manager) for the wait.
  ctx.wake_op = [this, sid](const Json& args, Json* out) -> FsResult {
    std::string op = jsonutil::get<std::string>(args, "op", "schedule");
    if (op == "cancel") {
      bool had = wakes_.cancel(sid);
      emit("wake.cancelled", Json{{"sid", sid}});
      if (out) *out = Json{{"cancelled", had}};
      return FsResult::success(Json{{"cancelled", had}});
    }
    if (op == "list") {
      std::vector<PendingWake> list = wakes_.pending_for_sid(sid);
      int64_t now = deps_.clock ? deps_.clock->now_ms() : WallClock().now_ms();
      Json arr = Json::array();
      for (size_t i = 0; i < list.size(); ++i) {
        arr.push_back(Json{{"source", wake_source_str(list[i].source)},
                           {"at", list[i].at_ms},
                           {"inMs", list[i].at_ms > 0 ? list[i].at_ms - now : 0},
                           {"ref", list[i].ref}});
      }
      if (out) *out = Json{{"wakes", arr}};
      return FsResult::success(Json{{"wakes", arr}});
    }
    int64_t delay = jsonutil::get_int(args, "delayMs", 0);
    if (delay <= 0) {
      return FsResult::error(ErrCode::kInvalidArg, "delayMs must be > 0");
    }
    // A year-long timer is a typo, not intent; cap it so a bad argument cannot
    // park the task beyond the process lifetime.
    const int64_t kMax = 7LL * 24 * 3600 * 1000;
    if (delay > kMax) delay = kMax;
    std::string note = jsonutil::get<std::string>(args, "note", "");
    int64_t at = (deps_.clock ? deps_.clock->now_ms() : WallClock().now_ms()) + delay;
    wakes_.schedule(sid, WakeSource::kTimer, at, note);
    emit("wake.scheduled", Json{{"sid", sid}, {"at", at}, {"delayMs", delay},
                                {"note", note}});
    Json res = Json{{"at", at}, {"delayMs", delay}, {"note", note}};
    if (out) *out = res;
    return FsResult::success(res);
  };
  ctx.subagent_op = [this, sid](const Json& args, Json* out) -> FsResult {
    std::string op = jsonutil::get<std::string>(args, "op", "");
    if (op == "spawn") {
      std::string kind = jsonutil::get<std::string>(args, "kind", "research");
      std::string task = jsonutil::get<std::string>(args, "task", "");
      if (task.empty()) return FsResult::error(ErrCode::kInvalidArg, "empty task");
      std::string model = jsonutil::get<std::string>(args, "model", "");
      int64_t timeout = jsonutil::get_int(args, "timeoutMs", 0);
      std::string step = jsonutil::get<std::string>(args, "step", "");
      int64_t todo_id = jsonutil::get_int(args, "todoId", 0);
      // `wait` was documented but never read: the call returned "running" no
      // matter what, so "spawn one step, get its answer, then spawn the next"
      // was impossible from the model's side (it had to guess with follow_up).
      bool wait = jsonutil::get<bool>(args, "wait", false);
      // complete-step IS the current task advancing: it must block and return
      // its summary. A non-blocking default forced the model to poll/follow-up,
      // which it rarely did - so it kept doing the steps inline, which is
      // exactly the context cost delegation exists to avoid.
      if (kind == "complete-step") wait = true;
      std::string id;
      FsResult r = subagent_spawn(sid, kind, task, model, 1, timeout, &id);
      if (!r.ok()) return r;
      // A step-scoped subagent: remember which checklist item it implements, so
      // the panel can show "step 3/7" and the item is ticked when it reports
      // back. The link lives here (host state), not in the prompt.
      if (!step.empty() || todo_id > 0) {
        std::lock_guard<std::recursive_mutex> host_lock(lock());
        subagent_steps_[id] = Json{{"step", step}, {"todoId", todo_id}};
        Json p = Json{{"sid", sid}, {"id", id}, {"step", step}, {"todoId", todo_id},
                      {"kind", kind}};
        emit("subagent.step", p);
      }
      if (wait) {
        // Block until this subagent reports. Poll the record rather than joining
        // the thread: the caller is the model's turn thread, and the thread's
        // completion path takes the host lock - joining here would be the same
        // deadlock that froze the tick.
        const int64_t kPollMs = 50;
        int64_t waited = 0;
        std::string summary;
        bool done = false;
        for (;;) {
          {
            std::lock_guard<std::recursive_mutex> host_lock(lock());
            const SubagentRecord* rec = subagents_.find(id);
            if (rec && rec->status != SubagentStatus::kRunning) {
              summary = rec->summary;
              done = true;
            }
          }
          if (done) break;
          if (timeout > 0 && waited >= timeout) break;
          std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
          waited += kPollMs;
        }
        Json res = Json{{"id", id}, {"kind", kind},
                        {"status", done ? "done" : "running"}};
        if (!summary.empty()) res["summary"] = summary;
        if (!step.empty()) res["step"] = step;
        if (out) *out = res;
        return FsResult::success(res);
      }
      Json res = Json{{"id", id}, {"kind", kind}, {"status", "running"}};
      if (!step.empty()) res["step"] = step;
      if (out) *out = res;
      return FsResult::success(res);
    }
    if (op == "list") {
      FsResult r = subagent_list(sid);
      if (out) *out = r.data;
      return r;
    }
    std::string id = jsonutil::get<std::string>(args, "id", "");
    if (op == "cancel") return subagent_cancel(id);
    if (op == "steer") {
      return subagent_steer(id, jsonutil::get<std::string>(args, "text", ""));
    }
    if (op == "followUp") {
      // A finished subagent can be continued in its own context. `steer` refuses
      // it (nothing is running), so this is the path the model uses to say
      // "now also do X there" without spawning an unrelated second agent.
      FsResult r = subagent_follow_up(id, jsonutil::get<std::string>(args, "text", ""));
      if (out && r.ok()) *out = r.data;
      return r;
    }
    return FsResult::error(ErrCode::kInvalidArg, "unknown subagent op: " + op);
  };
  ctx.workflow_op = [this, sid](const Json& args, Json* out) -> FsResult {
    std::string op = jsonutil::get<std::string>(args, "op", "");
    if (!deps_.workflow_op) {
      return FsResult::error(ErrCode::kNotSupported, "workflow store not wired");
    }
    Json req = args;
    req["sid"] = sid;
    return deps_.workflow_op(req, out);
  };
  return ctx;
}

// 进程（todo）操作：owner 为会话 sid 或子代理 id（addon2：子代理独立进程树）
FsResult AgentHost::todo_op(const std::string& owner, const Json& args) {
  std::lock_guard<std::recursive_mutex> host_lock(lock());
  hydrate_todos(owner);
  std::string op = jsonutil::get<std::string>(args, "op", "");
  int64_t now = deps_.clock ? deps_.clock->now_ms() : 0;
  if (op == "add") {
    std::string task = jsonutil::get<std::string>(args, "task", "");
    if (task.empty()) return FsResult::error(ErrCode::kInvalidArg, "empty task");
    int id = todos_.add(owner, task);
    // A checklist does NOT touch the plan: they are different layers now. The
    // plan is the document that says what the whole job is; the list is this
    // round's tasks derived from it. (Batch 49 cleared the plan here, which was
    // the "same thing, newer wins" confusion - the user rejected it.)
    save_todos(owner);
    emit("todo.updated", Json{{"owner", owner}, {"items", todos_.to_json(owner)}});
    return FsResult::success(Json{{"id", id}, {"items", todos_.to_json(owner)}});
  }
  if (op == "list") {
    return FsResult::success(
        Json{{"items", todos_.to_json(owner)}, {"pending", todos_.pending_count(owner)}});
  }
  if (op == "check") {
    int id = static_cast<int>(jsonutil::get_int(args, "id", 0));
    bool done = jsonutil::get<bool>(args, "done", true);
    if (!todos_.check(owner, id, done, now)) {
      return FsResult::error(ErrCode::kNotFound, "todo item not found");
    }
    // A completed list STAYS: the backend keeps the task list for the whole
    // task, and the next round starts from it (all-done is still useful context
    // and the user can see what was done). It is only removed by `erase`/`clear`.
    save_todos(owner);
    emit("todo.updated", Json{{"owner", owner}, {"items", todos_.to_json(owner)}});
    return FsResult::success(Json{{"items", todos_.to_json(owner)},
                                  {"pending", todos_.pending_count(owner)}});
  }
  if (op == "erase") {
    int id = static_cast<int>(jsonutil::get_int(args, "id", 0));
    if (!todos_.erase(owner, id)) {
      return FsResult::error(ErrCode::kNotFound, "todo item not found");
    }
    // Erasing changes the list too - without this event the panel kept showing
    // the removed row until something else refreshed it.
    save_todos(owner);
    emit("todo.updated", Json{{"owner", owner}, {"items", todos_.to_json(owner)}});
    return FsResult::success(Json{{"items", todos_.to_json(owner)}});
  }
  if (op == "clear") {
    todos_.clear(owner);
    save_todos(owner);
    emit("todo.updated", Json{{"owner", owner}, {"items", Json::array()}});
    return FsResult::success(Json{{"items", Json::array()}});
  }
  return FsResult::error(ErrCode::kInvalidArg, "unknown todo op: " + op);
}

void AgentHost::hydrate_todos(const std::string& owner) {
  std::lock_guard<std::recursive_mutex> host_lock(lock());
  if (owner.empty() || todos_hydrated_.count(owner)) return;
  // Mark first: an owner with no saved list must not re-read on every call.
  todos_hydrated_.insert(owner);
  if (!deps_.todo_load) return;
  Json items = deps_.todo_load(owner);
  if (items.is_array() && !items.empty()) todos_.restore(owner, items);
}

void AgentHost::save_todos(const std::string& owner) {
  if (deps_.todo_save) deps_.todo_save(owner, todos_.to_json(owner));
}

int AgentHost::todo_pending(const std::string& owner) const {
  std::lock_guard<std::recursive_mutex> host_lock(lock());
  return todos_.pending_count(owner);
}

FsResult AgentHost::request_compress(const std::string& sid) {
  std::lock_guard<std::recursive_mutex> host_lock(lock());
  AgentLoop& loop = loop_for(sid, false);
  loop.request_compress();
  emit("context.compressRequested", Json{{"sid", sid}});
  return FsResult::success();
}

FsResult AgentHost::context_status(const std::string& sid) {
  std::lock_guard<std::recursive_mutex> host_lock(lock());
  int64_t limit = 0;
  int64_t prompt = 0;
  int pct = 0;
  int at = 80;
  auto it = loops_.find(sid);
  if (it != loops_.end() && it->second) {
    limit = it->second->context_limit;
    prompt = it->second->last_prompt_tokens();
  }
  if (deps_.setting) {
    Json v = deps_.setting("", "history.compressAtPct", Json(80));
    if (v.is_number()) at = static_cast<int>(v.get<int64_t>());
  }
  if (limit > 0 && prompt > 0) {
    pct = static_cast<int>(prompt * 100 / limit);
    if (pct < 0) pct = 0;
  }
  return FsResult::success(Json{{"sid", sid},
                                {"promptTokens", prompt},
                                {"contextLimit", limit},
                                {"pct", pct},
                                {"compressAtPct", at},
                                {"shouldCompress", at > 0 && pct >= at}});
}

FsResult AgentHost::send(const std::string& sid, const std::string& text,
                         const Json& attachments, bool auto_mode,
                         const std::string& permission,
                         const std::string& model,
                         const std::string& thinking,
                         const std::string& provider) {
  (void)attachments;
  // Setup under the lock (loop lookup, context rebuild), then RELEASE it for
  // the turn itself. Holding it across loop.send() would block every other
  // call - suspend, approve, panel loads - for the whole model round trip,
  // which is the freeze this design exists to prevent. The turn's own
  // callbacks (persist_turn, invoke_tool) take the same recursive lock.
  AgentLoop* loop = nullptr;
  ContextInput ctx;
  {
    std::lock_guard<std::recursive_mutex> host_lock(lock());
    loop = &loop_for(sid, auto_mode, permission);
    if (!model.empty()) loop->set_model(model);
    // Always set it (empty clears): switching from a provider-qualified pick to a
    // plain one must not keep routing through the previous provider.
    loop->set_provider(provider);
    if (!thinking.empty()) loop->set_thinking(thinking);
    loop->set_permission(permission);
    loop->set_auto_mode(auto_mode);
    loop->set_thinking_lang(thinking_lang_);
    if (deps_.setting) {
      Json of = deps_.setting("", "session.overfitEnabled", Json(false));
      loop->set_overfit_enabled(of.is_boolean() ? of.get<bool>() : false);
    }
    if (thinking_lang_ == "auto") {
      // "auto" is not a language the model can be told to think in: resolve it
      // against the UI locale so the prompt either says a language or nothing.
      loop->set_thinking_lang("");
    }
    // Rebuild the context on EVERY send. Model, permission, tool enablement,
    // project instructions and git state can all change between turns, and the
    // prefix has to describe the request that is about to go out.
    //
    // The model comes from the LOOP (set_model above), not from the previous
    // context: on a brand-new loop the context is still empty, so reading the
    // model out of it left `ctx.model` - and every subagent seeded from
    // `session_ctx_` - with an empty name. An empty name makes provider
    // routing fall back to "the first enabled connection", which is how a
    // DeepSeek subagent ended up posted at an Anthropic endpoint (403).
    ctx = build_context(sid, auto_mode, permission, loop->model(),
                        loop->context().thinking);
    if (!ctx.cwd.empty()) loop->set_cwd(ctx.cwd);
    loop->set_context(ctx);
    session_ctx_[sid] = ctx;  // subagents inherit this session's environment
    // History thresholds are SETTINGS. They used to be read once at loop
    // creation, so changing the compaction threshold or keep-turns in Settings
    // did nothing until the app restarted - "the options have no effect".
    if (deps_.setting) {
      HistoryBuilder::Options hopts = loop->history_options();
      Json cp = deps_.setting(ctx.pid, "history.compressAtPct", Json(80));
      Json kt = deps_.setting(ctx.pid, "history.keepTurns", Json(8));
      if (cp.is_number()) hopts.compress_at_pct = static_cast<int>(cp.get<int64_t>());
      if (kt.is_number()) hopts.keep_turns = static_cast<int>(kt.get<int64_t>());
      loop->set_history_options(hopts);
    }
    if (deps_.context_limit) loop->context_limit = deps_.context_limit(ctx.model);
    // The water level is the LAST request's prompt tokens, which lives in the
    // usage log (it survives a restart).
    if (deps_.last_prompt_tokens) {
      loop->set_last_prompt_tokens(deps_.last_prompt_tokens(sid));
    }
    loop->set_tools(ctx.tools);
  }
  return loop->send(text);
}

FsResult AgentHost::append(const std::string& sid, const std::string& text) {
  std::lock_guard<std::recursive_mutex> host_lock(lock());
  AgentLoop& loop = loop_for(sid, false);
  return loop.append(text);
}

// addon2 任务健壮性：打断后续接（给原任务发引导，从断点续跑）
FsResult AgentHost::continue_task(const std::string& sid, const std::string& guidance) {
  // Same rule as send(): look the loop up under the lock, then RUN the turn
  // outside it. Holding the host lock across a whole continuation blocked every
  // other call (todos_for, is_running, session.list enrichment) for its entire
  // duration - the UI then looked frozen and its checklist read stale.
  AgentLoop* loop = nullptr;
  {
    std::lock_guard<std::recursive_mutex> host_lock(lock());
    auto it = loops_.find(sid);
    if (it == loops_.end()) {
      return FsResult::error(ErrCode::kNotFound, "no loop for session: " + sid);
    }
    loop = it->second.get();
  }
  return loop->continue_task(guidance);
}

void AgentHost::set_session_model(const std::string& sid, const std::string& model,
                                  const std::string& provider) {
  if (model.empty()) return;
  std::lock_guard<std::recursive_mutex> host_lock(lock());
  auto it = loops_.find(sid);
  if (it != loops_.end() && it->second) {
    it->second->set_model(model);
    it->second->set_provider(provider);
  }
}

// addon2：子代理引导（子代理没有轮的概念，只需当前引导）
FsResult AgentHost::subagent_steer(const std::string& id, const std::string& text) {
  std::lock_guard<std::recursive_mutex> host_lock(lock());
  FsResult r = subagents_.steer(id, text);
  if (r.ok()) emit("subagent.steered", Json{{"id", id}});
  return r;
}

// 权限门批准/拒绝（approve 档）
FsResult AgentHost::approve(const std::string& sid, bool yes) {
  std::lock_guard<std::recursive_mutex> host_lock(lock());
  auto it = loops_.find(sid);
  if (it == loops_.end()) {
    return FsResult::error(ErrCode::kNotFound, "no loop for session: " + sid);
  }
  return it->second->approve(yes);
}

FsResult AgentHost::suspend(const std::string& sid) {
  std::lock_guard<std::recursive_mutex> host_lock(lock());
  auto it = loops_.find(sid);
  if (it == loops_.end()) return FsResult::success();
  it->second->suspend();
  return FsResult::success();
}

FsResult AgentHost::interrupt(const std::string& sid) {
  std::lock_guard<std::recursive_mutex> host_lock(lock());
  auto it = loops_.find(sid);
  if (it == loops_.end()) return FsResult::success();
  it->second->interrupt();
  return FsResult::success();
}

FsResult AgentHost::resume(const std::string& sid) {
  // Snapshot the loop under the lock, then RUN outside it: continuing a parked
  // turn is a whole model round trip, and holding the host lock across it is the
  // deadlock this design exists to prevent (see send()).
  AgentLoop* loop = nullptr;
  bool parked = false;
  {
    std::lock_guard<std::recursive_mutex> host_lock(lock());
    auto it = loops_.find(sid);
    if (it == loops_.end()) return FsResult::success();
    loop = it->second.get();
    parked = loop->parked();
    // A request that has not parked yet only needs the flag cleared.
    if (!parked) {
      loop->resume();
      return FsResult::success();
    }
  }
  return loop->resume_task();
}

FsResult AgentHost::cancel_wake(const std::string& sid) {
  std::lock_guard<std::recursive_mutex> host_lock(lock());
  wakes_.cancel(sid);
  auto it = loops_.find(sid);
  if (it != loops_.end()) it->second->cancel_wake();
  emit("wake.cancelled", Json{{"sid", sid}});
  return FsResult::success();
}

FsResult AgentHost::tool_invoke(const std::string& sid, const std::string& name,
                                const Json& args, bool auto_mode,
                                const std::string& permission) {
  std::lock_guard<std::recursive_mutex> host_lock(lock());
  // 自动模式：ask 已从工具列表移除
  if (auto_mode && name == "ask") {
    return FsResult::error(ErrCode::kBusy, "ask not available in auto mode");
  }
  ToolContext ctx = make_ctx(sid, auto_mode);
  ctx.permission = permission;
  FsResult r = tools_.invoke(ctx, name, args);
  // 落库 tool 调用记录（由调用方拼 turn；此处发 tool.update 事件）
  emit("tool.update", Json{{"sid", sid}, {"name", name}, {"ok", r.ok()}});
  return r;
}

FsResult AgentHost::ask_answer(const std::string& sid, int ask_id,
                               const std::string& selection) {
  std::lock_guard<std::recursive_mutex> host_lock(lock());
  AgentLoop& loop = loop_for(sid, false);
  return loop.answer_ask(ask_id, selection);
}

FsResult AgentHost::subagent_spawn(const std::string& sid, const std::string& kind,
                                   const std::string& task, const std::string& model,
                                   int depth, int64_t timeout_ms, std::string* out_id) {
  // Reap first: finished threads hold their stacks until joined, and the vector
  // below is appended to on every spawn.
  reap_subagent_threads();
  std::lock_guard<std::recursive_mutex> host_lock(lock());
  std::string id;
  FsResult r = subagents_.spawn(sid, kind, task, model, "", depth, timeout_ms, &id);
  if (!r.ok()) return r;
  if (out_id) *out_id = id;
  emit("subagent.started", Json{{"sid", sid}, {"id", id}, {"kind", kind},
                                {"task", task}});
  std::string parent = sid;
  std::string sub_id = id;
  std::string sub_kind = kind;
  std::string sub_task = task;
  std::string sub_model = model;
  int sub_depth = depth;
  Json req = Json::object();
  req["id"] = id;
  req["kind"] = kind;
  req["task"] = task;
  req["model"] = model;
  req["depth"] = depth;  // Snapshot the parent's environment now: a subagent inherits the workspace,
  // model and permission - never the parent's CONVERSATION (that is the whole
  // point of delegating).
  ContextInput seed;
  {
    std::lock_guard<std::mutex> l(subagent_mu_);
    auto it = session_ctx_.find(sid);
    if (it != session_ctx_.end()) seed = it->second;
  }
  // The seed also carries the parent session id, which is what lets the
  // subagent's events be attributed to the parent for the UI.
  seed.sid = sid;
  // The thread is created WITHOUT holding subagent_mu_: its body takes that same
  // mutex early (to read its step record), so holding it across the spawn is a
  // race against the new thread's first lock acquisition.
  SubagentThread st;
  st.id = id;
  st.thread = std::thread([this, req, seed, parent, sub_id, sub_kind, sub_task,
                           sub_model, sub_depth]() {
    g_in_subagent_thread = true;
    Json out = Json::object();
    FsResult rr = run_subagent(req, seed, &out);
    std::string summary = rr.ok()
        ? jsonutil::get<std::string>(out, "summary", "")
        : (rr.message.empty() ? "subagent failed" : rr.message);
    if (summary.empty()) summary = "subagent finished without a summary";
    Json full = out.contains("full") ? out["full"] : Json::object();
    // The completion path mutates host state (subagent records, the step map) and
    // runs a todo_op, all of which the UI thread reads concurrently. Doing it
    // without the lock was a data race on std::map - it can corrupt the tree or
    // crash, and it is what made the freeze intermittent rather than every time.
    Json step_snapshot = Json::object();
    Json record_json = Json::object();
    {
      std::lock_guard<std::recursive_mutex> host_lock(lock());
      subagents_.complete_with_summary(sub_id, summary, full);
      // Snapshot the finished record while holding the lock, then persist it
      // OUTSIDE the lock: this is file IO, and holding the host lock across it
      // would block every other agent call (the same rule that fixed the
      // follow-up deadlock).
      if (const SubagentRecord* rec = subagents_.find(sub_id)) {
        record_json = rec->to_json_full();
      }
      // A step subagent ticks its own checklist item: the model does not have to
      // remember to do it, and the progress bar cannot drift from reality.
      {
        std::lock_guard<std::mutex> l(subagent_mu_);
        auto it = subagent_steps_.find(sub_id);
        if (it != subagent_steps_.end()) {
          step_snapshot = it->second;
          subagent_steps_.erase(it);
        }
      }
      int64_t todo_id = jsonutil::get_int(step_snapshot, "todoId", 0);
      if (todo_id > 0) {
        Json targs = Json{{"op", "check"}, {"id", todo_id}, {"done", true}};
        todo_op(parent, targs);
      }
    }
    Json payload = Json{{"sid", parent}, {"id", sub_id}, {"kind", sub_kind},
                        {"summary", summary}};
    if (!step_snapshot.empty()) payload["step"] = step_snapshot;
    // Persist AFTER releasing the lock: this is file IO, and holding the host
    // lock across it would block every other agent call.
    if (deps_.write_subagent && !record_json.empty()) {
      deps_.write_subagent(parent, sub_id, record_json);
    }
    emit("subagent.done", payload);
  });
  {
    std::lock_guard<std::mutex> lock(subagent_mu_);
    subagent_threads_.push_back(std::move(st));
  }
  return FsResult::success(Json{{"id", id}});
}

// Run one subagent to completion on the calling (worker) thread.
//
// A subagent is a full agent loop with:
//   * its own context (role=subagent, the task, no session history),
//   * a restricted tool set (read-only presets cannot write at all),
//   * permission never higher than the parent's,
//   * no persistence: its transcript lives in the record, not in the session.
// The parent only ever sees `summary`, which is what keeps the main context
// small - the entire point of having subagents.
FsResult AgentHost::run_subagent(const Json& req, const ContextInput& seed, Json* out) {
  std::string id = jsonutil::get<std::string>(req, "id", "");
  std::string kind = jsonutil::get<std::string>(req, "kind", "research");
  std::string task = jsonutil::get<std::string>(req, "task", "");
  std::string model = jsonutil::get<std::string>(req, "model", "");
  int depth = static_cast<int>(jsonutil::get_int(req, "depth", 1));

  ContextInput ctx = seed;  // the parent's environment as the base
  ctx.role = "subagent";
  ctx.subagent_kind = kind;
  ctx.subagent_task = task;
  ctx.subagent_step = jsonutil::get<std::string>(req, "step", "");
  // A subagent's events belong to the parent session (it has no session of its
  // own); seed.sid is the parent id.
  ctx.sid = seed.sid;
  ctx.auto_mode = true;  // a subagent runs to completion without user prompts
  ctx.allow_ask = false;
  ctx.allow_workflow = false;  // no nested pipelines
  ctx.disabled_tools.push_back("ask");
  ctx.model = model.empty() ? seed.model : model;
  if (ctx.cwd.empty()) ctx.cwd = seed.cwd;

  bool read_only = preset_read_only(kind);
  Json tools = Json::array();
  for (auto& t : tools_.neutral_defs()) {
    std::string n = jsonutil::get<std::string>(t, "name", "");
    if (read_only && !read_only_tool(n)) continue;
    if (n == "ask") continue;
    tools.push_back(t);
  }
  ctx.tools = tools;
  // Test hook: the tool set a subagent actually receives. A read-only preset
  // must not be able to write, and that is only checkable from here.
  if (subagent_tool_probe_) subagent_tool_probe_(tools);

  AgentLoop::Options opts;
  opts.sid = id;
  opts.cwd = ctx.cwd;
  opts.model = ctx.model;
  // A subagent's permission never exceeds its parent's. A read-only preset is
  // clamped further: it may not write at all, regardless of the parent.
  opts.permission = read_only ? "permissive" : seed.permission;
  opts.unattended = true;  // no user to answer a permission prompt
  opts.context = ctx;
  opts.history.tools = tools;
  // No session history: the task (plus any prior transcript below) IS the context.
  // Setting a turns window here was redundant and is gone with the field.
  // A FOLLOW-UP run carries the previous conversation so the subagent keeps its
  // context ("same area, more work"). It is replayed as history, not pasted into
  // the task text, so the model sees its own earlier turns and the tool results
  // they produced.
  Json prior = req.contains("priorTranscript") && req["priorTranscript"].is_array()
                   ? req["priorTranscript"]
                   : Json::array();
  if (!prior.empty()) {
    opts.history.seed_messages = prior;
  }
  // Subagent events go out tagged with the PARENT session + the subagent id.
  // The main chat uses this to show the work in progress (it is the user's
  // window into what the agent is doing) WITHOUT the transcript entering the
  // main conversation's history - that separation is the point of delegating.
  opts.event = [this, id, seed, kind]() {
    std::string parent = seed.sid;
    return [this, id, parent, kind](const std::string& e, const Json& p) {
      Json tagged = p.is_object() ? p : Json::object();
      tagged["sid"] = parent;
      tagged["subagentId"] = id;
      // The KIND travels with every event: the UI decides from it whether the
      // work belongs inline in the conversation (a complete-step is the current
      // task advancing) or only in the sidebar (research/explore are side work
      // the user does not need in the transcript).
      tagged["subagentKind"] = kind;
      tagged["fromSubagent"] = true;
      emit(e, tagged);
    };
  }();
  AgentLoop loop(std::move(opts));
  loop.call_model = [this](const ModelCallRequest& r, ModelCallResult* o,
                           const std::function<void(const std::string&)>& d) {
    if (!deps_.call_model) return FsResult::error(ErrCode::kNotSupported, "model not wired");
    return deps_.call_model(r, o, d);
  };
  loop.invoke_tool = [this, id](const ToolContext& in, const std::string& name,
                                const Json& args) -> FsResult {
    ToolContext tctx = make_ctx(id, true);
    tctx.cwd = in.cwd;
    tctx.pid = in.pid;
    tctx.permission = in.permission;
    tctx.approved = true;
    tctx.thinking_lang = in.thinking_lang;
    // A subagent's own todo tree is keyed by its id (make_ctx uses the id as
    // sid, so todo_op already lands on a separate owner).
    return tools_.invoke(tctx, name, args);
  };
  // The task is the whole conversation; no history, no persistence.
  FsResult r = loop.send(task);
  if (!r.ok()) return r;
  // The loop's final text is the summary. A subagent that ends on a tool call
  // has nothing to report, so say so instead of returning an empty string.
  std::string summary = loop.last_text();
  if (summary.empty()) summary = "subagent finished without producing a summary";
  Json full = Json::object();
  full["transcript"] = loop.last_messages();
  if (out) {
    (*out)["summary"] = summary;
    (*out)["full"] = full;
    (*out)["depth"] = depth;
  }
  return FsResult::success();
}

FsResult AgentHost::subagent_cancel(const std::string& id) {
  std::lock_guard<std::recursive_mutex> host_lock(lock());
  return subagents_.cancel(id);
}

// Continue a finished subagent with more work.
//
// The user's observation was exact: the model CAN keep guiding a subagent after
// it reported back, but nothing exposed it - `steer` rejects a non-running
// record and there was no "run it again with this instruction" path. This is
// that path, and it is synchronous so the caller (the model via
// subagent_follow_up, or the UI) gets the new summary back.
//
// The new run is seeded with the PREVIOUS transcript: the point of a follow-up
// is "same context, more work", so throwing the transcript away would make it
// indistinguishable from a fresh spawn with a longer task string.
FsResult AgentHost::subagent_follow_up(const std::string& id, const std::string& text) {
  if (text.empty()) {
    return FsResult::error(ErrCode::kInvalidArg, "follow-up text is empty");
  }
  // *** Do NOT hold the host lock across the run. ***
  //
  // This used to take the lock and then run a whole agent loop inside it. That
  // loop emits events (needs the lock) and may spawn a nested subagent whose
  // worker thread also needs it - so with a second subagent alive the app locked
  // up hard, exactly as reported ("spawn 第二个子代理才会出现"). Snapshot what the
  // run needs under the lock, release it, then run.
  std::string kind;
  std::string parent;
  int depth = 1;
  Json prior;
  ContextInput seed;
  {
    std::lock_guard<std::recursive_mutex> host_lock(lock());
    const SubagentRecord* rec = subagents_.find(id);
    if (!rec) return FsResult::error(ErrCode::kNotFound, "subagent not found: " + id);
    // Snapshot BEFORE reopening (which clears the summary).
    kind = rec->kind;
    parent = rec->parent_sid;
    depth = rec->depth;
    prior = subagents_.transcript_for(id);
    {
      std::lock_guard<std::mutex> l(subagent_mu_);
      auto it = session_ctx_.find(parent);
      if (it != session_ctx_.end()) seed = it->second;
    }
    seed.sid = parent;
    FsResult ro = subagents_.reopen_for_follow_up(id);
    if (!ro.ok()) return ro;
  }
  emit("subagent.started", Json{{"sid", parent}, {"id", id}, {"kind", kind},
                                {"task", text}, {"followUp", true}});

  Json req = Json::object();
  req["id"] = id;
  req["kind"] = kind;
  req["task"] = text;
  req["depth"] = depth;
  req["priorTranscript"] = prior;
  Json out = Json::object();
  // Run inline on the calling thread, with NO host lock held (see above).
  bool prev_flag = g_in_subagent_thread;
  g_in_subagent_thread = true;
  FsResult rr = run_subagent(req, seed, &out);
  g_in_subagent_thread = prev_flag;
  std::string summary = rr.ok() ? jsonutil::get<std::string>(out, "summary", "")
                                : (rr.message.empty() ? "subagent failed" : rr.message);
  if (summary.empty()) summary = "subagent finished without a summary";
  Json full = out.contains("full") ? out["full"] : Json::object();
  {
    std::lock_guard<std::recursive_mutex> host_lock(lock());
    subagents_.complete_with_summary(id, summary, full);
  }
  emit("subagent.done", Json{{"sid", parent}, {"id", id}, {"kind", kind},
                             {"summary", summary}, {"followUp", true}});
  if (!rr.ok()) return rr;
  return FsResult::success(Json{{"id", id}, {"summary", summary}});
}

FsResult AgentHost::subagent_list(const std::string& sid) {
  std::lock_guard<std::recursive_mutex> host_lock(lock());
  Json arr = Json::array();
  // to_json() (NOT to_json_full): a list must stay small. Inlining each agent's
  // transcript - which repeats the whole system prompt per message - made a
  // 3-agent list exceed the tool-output cap and the last agent vanished.
  for (auto& r : subagents_.list_for_sid(sid)) arr.push_back(r.to_json());
  return FsResult::success(arr);
}

// One agent's full record, transcript included: what the detail tab renders.
FsResult AgentHost::subagent_detail(const std::string& id) {
  std::lock_guard<std::recursive_mutex> host_lock(lock());
  const SubagentRecord* rec = subagents_.find(id);
  if (!rec) return FsResult::error(ErrCode::kNotFound, "subagent not found: " + id);
  return FsResult::success(rec->to_json_full());
}

FsResult AgentHost::on_terminal_exit(const std::string& sid, const std::string& tid,
                                     int exit_code) {
  (void)exit_code;
  // terminal.exit itself is emitted by FsApi::terminal_tick (which owns the
  // process registry); this only turns a wakeOnExit terminal's exit into an
  // actual continuation. The old code emitted terminal.exit here TOO and was
  // never called from anywhere - wakeOnExit was a feature that did nothing.
  std::lock_guard<std::recursive_mutex> host_lock(lock());
  wakes_.schedule(sid, WakeSource::kTerminalExit, 0, tid);
  wakes_.due(0);
  return FsResult::success();
}

FsResult AgentHost::on_timer_tick() {
  // Reap FIRST, and therefore before taking the lock: reap joins worker
  // threads, whose final act is an emit that needs the host lock. Holding it
  // across the join deadlocked the app a few seconds after a subagent finished
  // (the tick is what made it look like a hang rather than a slow call).
  reap_wake_threads();
  reap_subagent_threads();
  std::lock_guard<std::recursive_mutex> host_lock(lock());
  // Keep the record table bounded: each finished record holds a whole transcript
  // and each message in it carries a copy of the system prompt. Without this the
  // process grows by megabytes per session and eventually dies - the "looks like
  // a memory leak, then the program exits by itself" report.
  //
  // Pruned per SESSION, not globally, and only finished records: a running agent
  // must never be dropped, and one busy session must not evict another's history.
  {
    std::set<std::string> sids;
    for (auto& kv : loops_) {
      if (kv.second) sids.insert(kv.first);
    }
    for (auto& sid : sids) subagents_.prune_finished(sid);
  }
  int64_t now = deps_.clock ? deps_.clock->now_ms() : 0;
  auto due = wakes_.due(now);
  return FsResult::success(Json{{"fired", (int)due.size()}});
}

bool AgentHost::is_running(const std::string& sid) const {
  std::lock_guard<std::recursive_mutex> host_lock(lock());
  auto it = loops_.find(sid);
  if (it == loops_.end() || !it->second) return false;
  // kIdle/kTurnDone mean the loop is parked between turns; anything else is a
  // turn in flight (planning, calling a tool, or waiting on the user).
  LoopState s = it->second->state();
  return s != LoopState::kIdle && s != LoopState::kTurnDone;
}

std::string AgentHost::loop_state(const std::string& sid) const {
  std::lock_guard<std::recursive_mutex> host_lock(lock());
  auto it = loops_.find(sid);
  if (it == loops_.end() || !it->second) return "idle";
  // A suspend request outranks the coarse state: from the user's point of view
  // the task IS suspended the moment they pressed the button, even if the loop
  // is still winding down the current tool. Reporting "toolCalling" would make
  // the button flip back to "挂起" right after they pressed it.
  //
  // A PARKED turn reports the same thing: it already returned from run_loop, so
  // its state is idle - without this the button flipped back to 挂起 the moment
  // the loop actually parked, and the paused task looked like it had finished.
  if (it->second->suspend_requested() || it->second->parked()) return "suspended";
  return loop_state_str(it->second->state());
}

Json AgentHost::todos_for(const std::string& sid) {
  std::lock_guard<std::recursive_mutex> host_lock(lock());
  hydrate_todos(sid);
  return todos_.to_json(sid);
}

FsResult AgentHost::live_turn(const std::string& sid) {
  std::lock_guard<std::recursive_mutex> host_lock(lock());
  auto it = loops_.find(sid);
  if (it == loops_.end() || !it->second) {
    return FsResult::success(Json{{"running", false}});
  }
  return FsResult::success(it->second->live_state());
}

int AgentHost::auto_round(const std::string& sid) const {  std::lock_guard<std::recursive_mutex> host_lock(lock());
  auto it = loops_.find(sid);
  if (it == loops_.end() || !it->second) return 0;
  return it->second->auto_round();
}

bool AgentHost::any_running() const {
  std::lock_guard<std::recursive_mutex> host_lock(lock());
  for (auto& kv : loops_) {
    if (!kv.second) continue;
    LoopState s = kv.second->state();
    if (s != LoopState::kIdle && s != LoopState::kTurnDone) return true;
  }
  return false;
}

FsResult AgentHost::on_subagent_done(const std::string& id, const std::string& summary) {
  std::lock_guard<std::recursive_mutex> host_lock(lock());
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
  std::lock_guard<std::recursive_mutex> host_lock(lock());
  SteeringKind k = (kind == "round-end") ? SteeringKind::kRoundEnd : SteeringKind::kImmediate;
  int id = steering_[sid].push(k, text);
  emit("steering.queued", Json{{"sid", sid}, {"id", id}, {"kind", kind}});
  return FsResult::success(Json{{"id", id}});
}

FsResult AgentHost::steering_edit(const std::string& sid, int id, const std::string& text) {
  std::lock_guard<std::recursive_mutex> host_lock(lock());
  bool ok = steering_[sid].edit(id, text);
  return ok ? FsResult::success()
            : FsResult::error(ErrCode::kNotFound, "steering item not found");
}

FsResult AgentHost::steering_erase(const std::string& sid, int id) {
  std::lock_guard<std::recursive_mutex> host_lock(lock());
  bool ok = steering_[sid].erase(id);
  return ok ? FsResult::success()
            : FsResult::error(ErrCode::kNotFound, "steering item not found");
}

FsResult AgentHost::steering_reorder(const std::string& sid, const std::vector<int>& order) {
  std::lock_guard<std::recursive_mutex> host_lock(lock());
  bool ok = steering_[sid].reorder(order);
  return ok ? FsResult::success()
            : FsResult::error(ErrCode::kInvalidArg, "bad reorder");
}

FsResult AgentHost::steering_list(const std::string& sid) {
  std::lock_guard<std::recursive_mutex> host_lock(lock());
  return FsResult::success(steering_[sid].to_json());
}

FsResult AgentHost::persist_turn(const std::string& sid, const Json& turn) {
  std::lock_guard<std::recursive_mutex> host_lock(lock());
  if (!deps_.append_turn) {
    return FsResult::error(ErrCode::kInternal, "persist not wired");
  }
  // 附上待落库的要点（summarize_session 经 session.keypoint 事件收集）
  Json t = turn;
  {
    std::lock_guard<std::mutex> lock(keypoint_mu_);
    auto it = pending_keypoints_.find(sid);
    if (it != pending_keypoints_.end() && !it->second.empty()) {
      Json kps = Json::array();
      if (t.contains("keyPoints") && t["keyPoints"].is_array()) kps = t["keyPoints"];
      for (auto& kp : it->second) kps.push_back(kp);
      t["keyPoints"] = kps;
      it->second.clear();
    }
  }
  return deps_.append_turn(sid, t);
}

}  // namespace fs
