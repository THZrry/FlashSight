// fs_api.cpp - FsApi unified entry implementation (P1 scope)
#include "fs_api.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <thread>

#include "core/folder_picker.hpp"
#include "fs_git.hpp"
#include "fs_image.hpp"
#include "fs_path.hpp"
#include "fs_utf8.hpp"
#include "provider/balance_probe.hpp"
#include "provider/prompt_adapter.hpp"
#include "tools/search_backend.hpp"
#include "tools/skill_registry.hpp"

namespace fs {

FsApi::FsApi(const ghc::filesystem::path& root_dir, const Clock* clock, Random* rng)
    : root_(root_dir.is_absolute() ? root_dir : pathutil::normalize(root_dir)),
      clock_(clock ? clock : &default_clock_),
      default_rng_(0),
      rng_(rng ? rng : &default_rng_) {
  ghc::filesystem::create_directories(root_);
  ghc::filesystem::create_directories(root_ / "usage");
  settings_.reset(new SettingsStore(root_, ghc::filesystem::path(), true));
  proj_.reset(new ProjectManager(root_, settings_.get()));
  proj_->load();
  sessions_.reset(new SessionManager(proj_.get(), clock_, rng_));
  usage_.reset(new UsageStore(root_ / "usage" / "calls.jsonl"));
  locks_.reset(new LockArbiter(clock_));
  terminals_.reset(new TerminalRegistry(clock_));
  terminal_mgr_.reset(new TerminalManager(terminals_.get(), clock_));
  // Every drained byte goes straight to the transcript (see the sink's docs).
  terminal_mgr_->set_output_sink([this](const std::string& tid,
                                        const std::string& text) {
    append_terminal_log(tid, text);
  });
  // Where each terminal's transcript lives, resolved AT CREATE TIME - a
  // foreground command drains all of its output before spawn() returns, so a
  // path assigned afterwards never sees a byte.
  terminal_mgr_->set_log_path_fn([this](const std::string& sid,
                                        const std::string& tid) -> std::string {
    try {
      std::string path = sessions_->make_store(sid).terminal_log_path(sid, tid).string();
      if (!path.empty()) {
        std::lock_guard<std::mutex> lk(term_log_mu_);
        term_log_paths_[tid] = path;
      }
      return path;
    } catch (...) {
      return std::string();
    }
  });
  deny_list_.reset(new DenyList());
  plans_.reset(new PlanStore());
  {
    // The plan DOCUMENT lives in the project, next to the session data:
    // <project>/.flashsight/plans/<sid>.md. The store is path-injected because
    // only FsApi knows which project a session belongs to.
    SessionManager* sm = sessions_.get();
    plans_->set_path_fn([sm](const std::string& sid) {
      // Plans live WITH the session (multiple, numbered), not in one
      // project-wide file per sid. current.md is only the base path; PlanStore
      // writes plan-001.md, plan-002.md, ... next to it.
      return sm->project_store_of(sid).session_dir(sid) / "plans" / "current.md";
    });
  }
  workflows_.reset(new WorkflowStore(clock_));
  // P2: provider 层
  providers_.reset(new ProviderStore(root_ / "providers.json"));
  model_table_.reset(new ModelTable());
  model_table_->set_preset(builtin_models_preset());
  HttpRequest sample;
  net_.reset(new HttplibNetClient(sample));
  // P3: agent 宿主
  mcp_registry_.reset(new McpRegistry());
  AgentHostDeps deps;
  deps.clock = clock_;
  SessionManager* sm = sessions_.get();
  ProviderStore* ps = providers_.get();
  ModelTable* mt = model_table_.get();
  HttplibNetClient* net = net_.get();
  UsageStore* us = usage_.get();
  deps.load_full = [sm](const std::string& sid, Json* out) -> FsResult {
    return sm->load_full(sid, out);
  };
  deps.append_turn = [sm](const std::string& sid, const Json& turn) -> FsResult {
    return sm->append_turn(sid, turn);
  };
  deps.write_patch = [sm](const std::string& sid, const Json& patch) -> FsResult {
    Json tid = patch.contains("turnId") ? patch["turnId"] : Json("");
    return sm->make_store(sid).write_patch(
        sid, tid.is_string() ? tid.get<std::string>() : "", patch);
  };
  const Clock* clk = clock_;
  deps.call_model = [ps, mt, net, us, clk](
                        const ModelCallRequest& req, ModelCallResult* out,
                        const std::function<void(const std::string&)>& on_delta)
      -> FsResult {
    if (!out) return FsResult::error(ErrCode::kInvalidArg, "null result");
    // 按模型名找 provider。**不能**只取第一个启用的：多供应商时那会把
    // "deepseek-chat" 发到 Anthropic 端点上（用户配了两家就必然踩到），
    // 返回一个与模型无关的 403/404。先按"哪个供应商的模型表里有这个模型"
    // 匹配，认不出来才回退到第一个启用的连接。
    std::vector<ProviderConn> conns = ps->list();
    ProviderConn conn;
    bool found = false;
    ModelEntry picked;  // the model's own settings (retry) when it was matched
    // The PROVIDER the user picked wins. Two providers can serve the same model
    // id; matching by name alone always routed to the first one, so picking the
    // second provider's identical model silently used the first.
    if (!req.provider.empty()) {
      for (size_t i = 0; i < conns.size(); ++i) {
        if (conns[i].id != req.provider) continue;
        if (!conns[i].enabled) break;
        conn = conns[i];
        found = true;
        std::vector<ModelEntry> models = mt->list_models(conn.id);
        for (size_t k = 0; k < models.size(); ++k) {
          if (models[k].id == req.model) { picked = models[k]; break; }
        }
        break;
      }
    }
    if (!found && !req.model.empty()) {
      for (auto& c : conns) {
        if (!c.enabled) continue;
        std::vector<ModelEntry> models = mt->list_models(c.id);
        for (size_t i = 0; i < models.size(); ++i) {
          if (models[i].id == req.model) {
            conn = c;
            picked = models[i];
            found = true;
            break;
          }
        }
        if (found) break;
      }
    }
    if (!found) {
      for (auto& c : conns) {
        if (c.enabled) {
          conn = c;
          found = true;
          break;
        }
      }
    }
    if (!found) {
      // 无 provider：回显模式（单测/无 key 时可跑通 Loop）
      out->text = "(no provider configured; echo)";
      out->error = "";
      return FsResult::success();
    }
    ChatRequest creq;
    creq.model = req.model.empty() ? "gpt-4o-mini" : req.model;
    // The loop speaks the provider-neutral format; the wire shape is decided
    // here, where the connection (and therefore the API kind) is known. This is
    // what makes one transcript replayable against any provider.
    prompt::Adapted adapted = prompt::adapt(conn.kind, req.messages, req.tools);
    creq.messages = adapted.messages;
    creq.tools = adapted.tools;
    // Anthropic keeps the system prompt in its own field.
    if (!adapted.system.is_null() && conn.kind == ApiKind::kAnthropic) {
      creq.system = adapted.system.is_string() ? adapted.system.get<std::string>() : "";
    }
    if (!req.thinking.empty()) creq.thinking = req.thinking;
    // Streaming whenever the UI will consume deltas. On the desktop the delta
    // callback fires on the caller's thread (in-process), and on the server the
    // SSE bytes arrive on this request's thread - either way the UI can render
    // while the model is still talking instead of waiting for the whole reply.
    creq.stream = static_cast<bool>(on_delta);
    creq.session_id = req.session_id;  // kPlan: x-opencode-session 亲和 id
    creq.cancel = req.cancel;          // 停止生成 -> abort the SSE transfer
    ProviderRuntime runtime(conn, net, us, clk);
    // PER-MODEL retry settings (Settings > model > advanced). 0 = the global
    // default. Without this the fields were written and never read: a flaky
    // relay behaved the same for a model configured for 5 retries and one for 0.
    if (found) {
      if (picked.retry.max > 0) runtime.retry_policy().max_retries = picked.retry.max;
      if (picked.retry.backoff_ms > 0) {
        runtime.retry_policy().backoff_ms = picked.retry.backoff_ms;
      }
    }
    ChatResponse cresp;
    FsResult r = runtime.call(creq, &cresp, on_delta);
    if (!r.ok()) {
      out->error = cresp.error.empty() ? r.message : cresp.error;
      return r;
    }
    out->text = cresp.text;
    out->tool_calls = cresp.tool_calls;
    out->in_tokens = cresp.in_tokens;
    out->out_tokens = cresp.out_tokens;
    out->cache_tokens = cresp.cache_tokens;
    out->cache_known = cresp.cache_known;
    out->error = "";
    return FsResult::success();
  };
  call_model_fn_ = deps.call_model;  // for one-shot calls (AI title)
  deps.event = [this](const std::string& e, const Json& p) {
    // 子代理完成 -> 推进工作流（addon2）；同时转发给 UI 订阅者
    if (e == "subagent.done") {
      std::string sub_id = jsonutil::get<std::string>(p, "id", "");
      if (!sub_id.empty()) workflow_on_subagent_done(sub_id);
    }
    emit_event(e, p);
  };
  // 黑白名单判定（permissions.files.blacklist/whitelist；黑 > 白 > 默认档）
  DenyList* dl = deny_list_.get();
  deps.deny_check = [dl](const std::string& abs_path, std::string* reason) -> bool {
    PathMatch m = dl->match(ghc::filesystem::path(abs_path));
    if (m.denied) {
      if (reason) *reason = "denied by blacklist: " + m.matched;
      return false;
    }
    return true;  // 白名单命中或未命中：由权限档决定（此处不额外拒绝）
  };
  // 终端回调（reproc 经 TerminalManager；工具层只调回调）
  TerminalManager* tm = terminal_mgr_.get();
  // Through spawn_terminal, NOT the manager directly: that shortcut skipped
  // the project-root cwd default, so model commands ran in the app's own
  // directory (git log showed THIS repo instead of the user's project).
  deps.terminal_spawn = [this](const Json& a, Json* out) -> FsResult {
    return spawn_terminal(a, out);
  };
  deps.terminal_poll = [tm](const Json& a, Json* out) -> FsResult {
    return tm->poll(a, out);
  };
  deps.terminal_kill = [tm](const Json& a, Json* out) -> FsResult {
    return tm->kill(a, out);
  };
  // 网络回调（fetch_url / search_web 经 NetClient；宿主负责证书/超时/大小）
  deps.net_fetch = [net](const Json& a, Json* out) -> FsResult {
    HttpRequest req;
    req.method = jsonutil::get<std::string>(a, "method", "GET");
    req.url = jsonutil::get<std::string>(a, "url", "");
    req.timeout_ms = static_cast<int>(jsonutil::get_int(a, "timeoutMs", 30000));
    req.size_cap = jsonutil::get_int(a, "sizeCap", 0);
    req.verify_cert = jsonutil::get<bool>(a, "verifyCert", true);
    if (a.contains("headers") && a["headers"].is_object()) {
      for (auto it = a["headers"].begin(); it != a["headers"].end(); ++it) {
        if (it.value().is_string()) req.headers[it.key()] = it.value().get<std::string>();
      }
    }
    if (a.contains("body") && a["body"].is_string()) {
      req.body = a["body"].get<std::string>();
    }
    if (req.url.empty()) {
      return FsResult::error(ErrCode::kInvalidArg, "empty url");
    }
    HttpResponse resp;
    FsResult r = net->request(req, &resp);
    if (!r.ok()) return r;
    if (resp.status == 0) {
      return FsResult::error(ErrCode::kNetwork,
                             resp.error.empty() ? "network error" : resp.error);
    }
    Json o = Json::object();
    o["status"] = resp.status;
    o["body"] = resp.body;
    if (out) *out = o;
    return FsResult::success(o);
  };
  // ----- Web 搜索（search_web 工具）-----
  // The search API shapes live in tools/search_backend.cpp; this callback only
  // supplies the configured endpoint and does the HTTP round trip through the
  // same NetClient the fetch tool uses (proxy/cert settings apply).
  FsApi* self_search = this;
  deps.web_search = [self_search, net](const Json& a, Json* out) -> FsResult {
    std::string pid = jsonutil::get<std::string>(a, "pid", "");
    auto setting = [self_search, &pid](const char* key, const Json& def) -> Json {
      if (!pid.empty()) {
        std::string path = self_search->path_for(pid);
        if (!path.empty()) return self_search->store_for(pid).get_value(key, def);
      }
      return self_search->settings_->get_value(key, def);
    };
    search::Config cfg = search::config_from_settings(
        setting(kKeySearchProvider, Json("")),
        setting(kKeySearchEndpoint, Json("")),
        setting(kKeySearchApiKey, Json("")),
        setting(kKeySearchMaxResults, Json(5)));
    std::string query = jsonutil::get<std::string>(a, "query", "");
    int count = static_cast<int>(jsonutil::get_int(a, "count", cfg.max_results));
    search::HttpCall call = search::build_request(cfg, query, count);
    if (!call.ok) {
      return FsResult::error(ErrCode::kNotSupported, call.error);
    }
    HttpRequest req;
    req.method = call.method;
    req.url = call.url;
    req.timeout_ms = 30000;
    for (auto it = call.headers.begin(); it != call.headers.end(); ++it) {
      if (it.value().is_string()) req.headers[it.key()] = it.value().get<std::string>();
    }
    req.body = call.body;
    HttpResponse resp;
    FsResult nr = net->request(req, &resp);
    if (!nr.ok()) return nr;
    search::Parsed parsed = search::parse_response(cfg, resp.status, resp.body);
    if (!parsed.ok) {
      return FsResult::error(ErrCode::kRemote, parsed.error);
    }
    Json hits = Json::array();
    for (size_t i = 0; i < parsed.hits.size(); ++i) {
      Json h = Json::object();
      h["title"] = parsed.hits[i].title;
      h["url"] = parsed.hits[i].url;
      h["snippet"] = parsed.hits[i].snippet;
      hits.push_back(h);
    }
    Json o = Json::object();
    o["provider"] = cfg.provider;
    o["query"] = query;
    o["hits"] = hits;
    o["count"] = static_cast<int64_t>(parsed.hits.size());
    if (out) *out = o;
    return FsResult::success(o);
  };
  // ----- context sources (AgentContext) -----
  // Settings: project override > global > built-in default. The context layer
  // must not know about SettingsStore, so the lookup is injected here.
  FsApi* self = this;
  deps.setting = [self](const std::string& pid, const std::string& key,
                        const Json& def) -> Json {
    // Project value wins over the global one; an unknown pid falls back to the
    // global store (that is what `store_for` does for an empty path).
    if (!pid.empty()) {
      std::string path = self->path_for(pid);
      if (!path.empty()) return self->store_for(pid).get_value(key, def);
    }
    return self->settings_->get_value(key, def);
  };
  // Project instructions: the conventions a repo carries. AGENTS.md is the
  // cross-tool name; .flashsight/instructions.md is the project-local one.
  deps.project_instructions = [](const std::string& cwd) -> std::string {
    if (cwd.empty()) return std::string();
    const char* names[] = {"AGENTS.md", ".flashsight/instructions.md",
                           "CLAUDE.md", ".cursorrules"};
    for (const char* n : names) {
      ghc::filesystem::path p = ghc::filesystem::path(cwd) / n;
      std::error_code ec;
      if (!ghc::filesystem::is_regular_file(p, ec)) continue;
      std::ifstream ifs(p.string(), std::ios::binary);
      if (!ifs) continue;
      std::stringstream ss;
      ss << ifs.rdbuf();
      // Decode first: an AGENTS.md saved by a Chinese editor is GBK, and its
      // bytes go into the SYSTEM PROMPT. Invalid UTF-8 there throws when the
      // provider request is serialized, so the turn dies before it starts.
      std::string text = utf8::decode_to_utf8(ss.str());
      // Cap it: a 200 KB instructions file would be re-sent every turn and
      // would dwarf the actual conversation. Cut on a character boundary.
      text = utf8::truncate(text, 16000, "\n... (truncated)");
      return text;
    }
    return std::string();
  };
  deps.instructions_source = [](const std::string& cwd) -> std::string {
    if (cwd.empty()) return std::string();
    const char* names[] = {"AGENTS.md", ".flashsight/instructions.md",
                           "CLAUDE.md", ".cursorrules"};
    for (const char* n : names) {
      std::error_code ec;
      if (ghc::filesystem::is_regular_file(ghc::filesystem::path(cwd) / n, ec)) {
        return std::string(n);
      }
    }
    return std::string();
  };
  // Git summary: branch/dirty/ahead/behind. Cheap enough to refresh per turn
  // (one `git status`), and it is what makes "commit your work" reliable.
  deps.git_summary = [](const std::string& cwd) -> Json {
    if (cwd.empty() || !git::is_repo(cwd)) return Json::object();
    git::Status st = git::status(cwd);
    if (!st.ok || !st.is_repo) return Json::object();
    Json j = Json::object();
    j["branch"] = st.branch;
    j["dirty"] = !st.clean;
    j["ahead"] = st.ahead;
    j["behind"] = st.behind;
    if (!st.operation.empty()) j["operation"] = st.operation;
    return j;
  };
  // Context window for the water level: the model table is the only source of
  // the endpoint-reported context length.
  ModelTable* mt2 = model_table_.get();
  ProviderStore* ps2 = providers_.get();
  deps.context_limit = [mt2, ps2](const std::string& provider,
                                  const std::string& model) -> int64_t {
    if (model.empty()) return 0;
    // The SESSION's provider wins. Iterating every provider and returning the
    // first model-id match let an unrelated provider that serves the same id win
    // - e.g. A6API/deepseek-v4.1-flash (1M) read the window of a 128k reseller
    // entry, so the water level and the short-view fallback were both wrong.
    if (!provider.empty()) {
      ModelEntry e = mt2->resolve(provider, model);
      if (e.id == model && e.caps.context_len > 0) return e.caps.context_len;
      return 0;  // provider chosen but model unknown: do not borrow another
    }
    for (auto& c : ps2->list()) {
      ModelEntry e = mt2->resolve(c.id, model);
      if (e.id == model && e.caps.context_len > 0) return e.caps.context_len;
    }
    return 0;
  };
  PlanStore* plans2 = plans_.get();
  deps.plan_get = [plans2](const std::string& sid) -> Json { return plans2->get(sid); };
  // Automatic session titles (placeholder first, model summary later). Runs on
  // the turn's worker thread, after the turn is on disk.
  FsApi* self_title = this;
  deps.title_upkeep = [self_title](const std::string& sid) {
    self_title->maybe_upgrade_title(sid);
  };
  // Water level across restarts: the last request's prompt tokens for this
  // session is a fact already recorded in the usage log.
  UsageStore* us2 = usage_.get();
  deps.last_prompt_tokens = [us2](const std::string& sid) -> int64_t {
    if (sid.empty()) return 0;
    UsageFilter f;
    f.sid = sid;
    UsageSummary sum = us2->query(f);
    return sum.has_last ? sum.last.in : 0;
  };
  deps.plan_update = [plans2](const std::string& sid, const Json& plan) -> FsResult {
    // The document is written to disk; `plan` carries
    // {content, status, turnId, newPlan}.
    std::string content = jsonutil::get<std::string>(plan, "content", "");
    std::string status = jsonutil::get<std::string>(plan, "status", "");
    std::string turn_id = jsonutil::get<std::string>(plan, "turnId", "");
    bool new_plan = jsonutil::get<bool>(plan, "newPlan", false);
    return plans2->update(sid, content, status, turn_id, new_plan);
  };
  // The task list lives with the session (todos.json), not only in the host's
  // memory: reopening a session - or the app - must start from the checklist the
  // task already had, including finished items.
  SessionManager* sm_todo = sessions_.get();
  deps.todo_load = [sm_todo](const std::string& owner) -> Json {
    if (owner.empty()) return Json::array();
    SessionFileStore st = sm_todo->make_store(owner);
    if (!st.exists(owner)) return Json::array();  // subagent owner: not a session
    std::ifstream in(st.todos_path(owner).string(), std::ios::binary);
    if (!in) return Json::array();
    try {
      Json j = Json::parse(in);
      return j.is_array() ? j : Json::array();
    } catch (...) {
      return Json::array();
    }
  };
  deps.todo_save = [sm_todo](const std::string& owner, const Json& items) {
    if (owner.empty()) return;
    SessionFileStore st = sm_todo->make_store(owner);
    if (!st.exists(owner)) return;  // subagent owner: not a session
    std::string err;
    jsonutil::write_atomic(st.todos_path(owner).string(), items, &err);
  };
  // Durable compaction state. This is conversation context, not a cache: a
  // reopened session must start from the summary the previous run compressed to,
  // not re-send every stored turn and fall back to a lossy short view.
  deps.compaction_load = [sm_todo](const std::string& sid) -> Json {
    if (sid.empty()) return Json::object();
    SessionFileStore st = sm_todo->make_store(sid);
    if (!st.exists(sid)) return Json::object();
    std::ifstream in(st.compaction_path(sid).string(), std::ios::binary);
    if (!in) return Json::object();
    try {
      Json j = Json::parse(in);
      return j.is_object() ? j : Json::object();
    } catch (...) {
      return Json::object();
    }
  };
  deps.compaction_save = [sm_todo](const std::string& sid, const Json& state) {
    if (sid.empty()) return;
    SessionFileStore st = sm_todo->make_store(sid);
    if (!st.exists(sid)) return;
    std::string err;
    jsonutil::write_atomic(st.compaction_path(sid).string(), state, &err);
  };
  // Crash-safe live snapshot. Stored as runtime.json's `live` field: runtime.json
  // is explicitly "cache, may be deleted", which is exactly the right lifetime for
  // an in-flight turn (it is cleared when the turn lands).
  SessionManager* sm_live = sessions_.get();
  deps.write_live = [sm_live](const std::string& sid, const Json& live) {
    sm_live->write_live_snapshot(sid, live);
  };
  // A finished subagent's record is written next to the session, not only kept in
  // memory: the work-log row that spawned it lives in the transcript forever, so
  // its "打开记录" button has to keep working after a restart.
  deps.write_subagent = [sm_live](const std::string& sid, const std::string& id,
                                  const Json& rec) {
    sm_live->write_subagent_record(sid, id, rec);
  };
  WorkflowStore* wf = workflows_.get();
  deps.workflow_op = [wf, this](const Json& req, Json* out) -> FsResult {
    std::string op = jsonutil::get<std::string>(req, "op", "");
    std::string sid = jsonutil::get<std::string>(req, "sid", "");
    if (op == "list") {
      Json arr = Json::array();
      Json defs = wf->list_defs();
      for (auto& d : defs) arr.push_back(d);
      for (auto* run : wf->runs_for_sid(sid)) {
        if (run) arr.push_back(run->to_json());
      }
      if (out) *out = Json{{"items", arr}};
      return FsResult::success(Json{{"items", arr}});
    }
    if (op == "propose") {
      // Model-planned workflow: one implementation, shared with the API route.
      return workflow_propose(sid, req);
    }
    if (op == "start") {
      std::string name = jsonutil::get<std::string>(req, "name", "");
      const WorkflowDef* def = wf->find_def(name);
      if (!def) return FsResult::error(ErrCode::kNotFound, "workflow not found: " + name);
      WorkflowRun* run = wf->create_run(*def, sid);
      if (!run) return FsResult::error(ErrCode::kInternal, "create_run failed");
      emit_event("workflow_started", Json{{"runId", run->id}, {"sid", sid},
                                          {"name", name}});
      start_next_pending_step(run);
      Json j = run->to_json();
      if (out) *out = j;
      return FsResult::success(j);
    }
    return FsResult::error(ErrCode::kInvalidArg, "unknown workflow op: " + op);
  };
  agent_host_.reset(new AgentHost(std::move(deps)));
  agent_host_->register_builtin_tools(mcp_registry_.get());
  // addon2：思考语言设置（工具 skill 中英双语）注入宿主
  Json tl = settings_->get_value(kKeyThinkingLang, Json("auto"));
  agent_host_->set_thinking_lang(tl.is_string() ? tl.get<std::string>() : "auto");
  // 用户 skill：从设置恢复（内置 skill 是代码，不在这里）
  agent_host_->skills().load_user(settings_->get_value(kKeySkills, Json::array()));
  // MCP/外部工具注册表：从设置恢复（重启后保留）
  Json mcp = settings_->get_value(kKeyMcpEntries, Json::array());
  if (mcp.is_array()) {
    // One-time migration: a saved table from before the split can hold SKILL
    // directories under the MCP page (that is the "gsd appears in MCP" report).
    // Those are moved to the skill registry here - importing each dir - and the
    // MCP table is rewritten without them.
    bool moved = false;
    for (size_t i = 0; i < mcp.size(); ++i) {
      if (!McpRegistry::is_skill_dir_entry(mcp[i])) continue;
      std::string dir = jsonutil::get<std::string>(mcp[i], "dir", "");
      if (!dir.empty()) agent_host_->skills().import_dir(dir);
      moved = true;
    }
    mcp_registry_->load(mcp);  // drops the skill-dir entries
    if (moved) {
      settings_->set_global(kKeySkills, agent_host_->skills().user_json());
      settings_->set_global(kKeyMcpEntries, mcp_registry_->dump());
      settings_->save_global();
    }
  }
  // 逐模型设置：从设置恢复用户覆盖（启用状态/能力/档位/重试/图标）
  model_table_->load_user_models(settings_->get_value(kKeyProviderModels, Json::object()));
  // 以及上一次从 API 取回的模型列表（上下文长度等事实的唯一来源）
  model_table_->load_api_models(
      settings_->get_value(kKeyProviderApiModels, Json::object()));
  apply_proxy_settings();  // 代理设置立即生效
  // 文件黑白名单：从设置装载（黑 > 白 > 三档默认）
  {
    Json bl = settings_->get_value(kKeyPermFileBlacklist, Json::array());
    Json wl = settings_->get_value(kKeyPermFileWhitelist, Json::array());
    std::vector<std::string> b, w;
    for (size_t i = 0; bl.is_array() && i < bl.size(); ++i) {
      if (bl[i].is_string()) b.push_back(bl[i].get<std::string>());
    }
    for (size_t i = 0; wl.is_array() && i < wl.size(); ++i) {
      if (wl[i].is_string()) w.push_back(wl[i].get<std::string>());
    }
    deny_list_->set_blacklist(b);
    deny_list_->set_whitelist(w);
  }
  // addon2：内置示例工作流
  for (auto& d : builtin_workflows()) workflows_->define(d.to_json());
  // 终端后台轮询：新增输出/退出直接推事件，UI 不再手动"轮询"
  start_terminal_poller();
}

void FsApi::apply_proxy_settings() {
  std::string host = settings_->get_value(kKeyNetworkProxyHost, Json("")).is_string()
                         ? settings_->get_value(kKeyNetworkProxyHost, Json("")).get<std::string>()
                         : "";
  Json pv = settings_->get_value(kKeyNetworkProxyPort, Json(0));
  int port = pv.is_number() ? static_cast<int>(pv.get<int64_t>()) : 0;
  if (net_) net_->set_default_proxy(host, port);
}

void FsApi::apply_server_settings() {
  if (!ui_server_restart_) return;   // no server in this process (tests)
  // Before the server has started (i.e. this is being called from main() while
  // applying command-line overrides), there is no listener to rebind: the start
  // path will use these values directly. Triggering restart() here created a
  // worker that bound the port, and then start() bound it again.
  if (ui_server_status_) {
    Json st = ui_server_status_();
    if (!jsonutil::get<bool>(st, "running", false)) return;
  }
  std::string host = settings_->get_value(kKeyServerHost, Json("127.0.0.1")).is_string()
                         ? settings_->get_value(kKeyServerHost, Json("127.0.0.1"))
                               .get<std::string>()
                         : "127.0.0.1";
  if (host.empty()) host = "127.0.0.1";
  Json pv = settings_->get_value(kKeyServerPort, Json(0));
  int port = pv.is_number() ? static_cast<int>(pv.get<int64_t>()) : 0;
  std::string pw = settings_->get_value(kKeyServerPassword, Json("")).is_string()
                       ? settings_->get_value(kKeyServerPassword, Json(""))
                             .get<std::string>()
                       : "";
  ui_server_restart_(port, host, pw);
}

FsResult FsApi::server_status() {
  if (!ui_server_status_) {
    return FsResult::success(Json{{"running", false},
                                  {"host", ""},
                                  {"port", 0},
                                  {"password", false},
                                  {"url", ""}});
  }
  return FsResult::success(ui_server_status_());
}

// The page asks these before mounting. The HTTP layer already enforces the
// token; these two only exist so the UI knows whether to draw the gate.
FsResult FsApi::server_auth_required() {
  return FsResult::success(Json{{"required", false}, {"authed", true}});
}

FsResult FsApi::server_login(const std::string& password) {
  (void)password;
  return FsResult::success(Json{{"token", ""}});
}

// 事件扇出：给所有监听者（本地 UI + HTTP API 服务）；无监听者则丢弃（后端不依赖 UI）
void FsApi::emit_event(const std::string& event, const Json& payload) {
  for (std::map<int, std::function<void(const std::string&, const Json&)> >::iterator it =
           event_listeners_.begin();
       it != event_listeners_.end(); ++it) {
    if (it->second) it->second(event, payload);
  }
}

FsApi::~FsApi() {
  stop_terminal_poller();
}

// ----- project.* -----
FsResult FsApi::project_list() { return proj_->list_projects(); }

// A project with no session cannot appear in the sidebar (the rail lists SESSIONS
// per project), so opening a folder looked like "the project went nowhere". Give
// a brand-new project one empty session; a project that already has sessions is
// left alone.
void FsApi::ensure_project_session(const std::string& pid, const std::string& path) {
  if (pid.empty()) return;
  FsResult r = sessions_->list_sessions(pid);
  if (r.ok() && r.data.is_array() && !r.data.empty()) return;
  sessions_->new_session(pid, path);
}

FsResult FsApi::project_open(const std::string& path) {
  FsResult r = proj_->open_project(path);
  if (r.ok()) {
    ensure_project_session(jsonutil::get<std::string>(r.data, "pid", ""),
                           jsonutil::get<std::string>(r.data, "path", ""));
  }
  return r;
}
FsResult FsApi::project_create(const std::string& path) {
  FsResult r = proj_->create_project(path);
  if (r.ok()) {
    ensure_project_session(jsonutil::get<std::string>(r.data, "pid", ""),
                           jsonutil::get<std::string>(r.data, "path", ""));
  }
  return r;
}
FsResult FsApi::project_remove(const std::string& pid) { return proj_->remove_project(pid); }
FsResult FsApi::project_get(const std::string& pid) { return proj_->get_project(pid); }
FsResult FsApi::project_pick_folder() {
  std::string path;
  if (pick_folder_dialog(&path)) {
    return FsResult::success(Json{{"path", path}});
  }
  return FsResult::success(Json{{"cancelled", true}});
}
// 选文件（不是选目录）：驱动路径、CLI 工具路径这些字段需要的是可执行文件本身，
// 用目录选择框是选不中的。
FsResult FsApi::pick_file(const std::string& filterDesc, const std::string& filterSpec) {
  std::string path;
  if (pick_file_dialog(&path, filterDesc, filterSpec)) {
    return FsResult::success(Json{{"path", path}});
  }
  return FsResult::success(Json{{"cancelled", true}});
}
FsResult FsApi::project_resolve_drop(const std::string& name) {
  if (name.empty()) {
    return FsResult::error(ErrCode::kInvalidArg, "empty folder name");
  }
  // already registered? reuse the path
  nlohmann::json all = proj_->list_projects().data;
  std::string lower = name;
  for (size_t i = 0; i < lower.size(); ++i) {
    lower[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(lower[i])));
  }
  if (all.is_array()) {
    for (size_t i = 0; i < all.size(); ++i) {
      std::string path = jsonutil::get<std::string>(all[i], "path", "");
      ghc::filesystem::path p(path);
      std::string fname = p.filename().string();
      for (size_t k = 0; k < fname.size(); ++k) {
        fname[k] = static_cast<char>(std::tolower(static_cast<unsigned char>(fname[k])));
      }
      if (fname == lower) return FsResult::success(Json{{"path", path}});
    }
  }
  std::string found = find_folder_by_name(default_search_roots(), name, 6);
  if (found.empty()) {
    return FsResult::success(Json{{"notFound", true}, {"name", name}});
  }
  return FsResult::success(Json{{"path", found}});
}
FsResult FsApi::project_reveal(const std::string& pid) {
  std::string path = path_for(pid);
  if (path.empty()) {
    return FsResult::error(ErrCode::kNotFound, "project not found: " + pid);
  }
  if (!reveal_in_file_manager(path)) {
    return FsResult::error(ErrCode::kIo, "cannot open file manager");
  }
  return FsResult::success();
}

// Open a markdown link from the transcript.
//
// A chat link is MODEL OUTPUT, so it is treated as untrusted:
//   * http(s) goes to the system browser (never into this window - navigating
//     away would destroy the session view),
//   * anything else (file://, a custom protocol, javascript:) is refused rather
//     than handed to the OS handler,
//   * a project-relative path opens the side-panel preview instead, which is what
//     a link to a source file should do.
FsResult FsApi::open_link(const std::string& sid, const std::string& href) {
  if (href.empty()) return FsResult::error(ErrCode::kInvalidArg, "empty link");
  if (href.compare(0, 7, "http://") == 0 || href.compare(0, 8, "https://") == 0) {
    if (!open_url_in_browser(href)) {
      return FsResult::error(ErrCode::kIo, "cannot open the browser");
    }
    return FsResult::success(Json{{"opened", "browser"}, {"href", href}});
  }
  if (href.compare(0, 7, "file://") == 0 || href.find("://") != std::string::npos) {
    return FsResult::error(ErrCode::kInvalidArg,
                           "only http(s) links and project files can be opened");
  }
  // A file inside the project: report the path so the UI can open its preview
  // panel. The UI owns tabs, so the backend does not open one itself.
  std::string cwd;
  if (!sid.empty()) {
    Json full;
    if (sessions_->load_full(sid, &full).ok() && full.contains("meta")) {
      cwd = jsonutil::get<std::string>(full["meta"], "cwd", "");
    }
  }
  std::string rel = href;
  // "#L12" / "#L12-L20" line anchors are stripped: the preview opens the file,
  // and a line number is not part of the path.
  size_t hash = rel.find('#');
  if (hash != std::string::npos) rel = rel.substr(0, hash);
  if (rel.compare(0, 2, "./") == 0) rel = rel.substr(2);
  if (!cwd.empty()) {
    ghc::filesystem::path base = pathutil::normalize(cwd);
    ghc::filesystem::path p = pathutil::normalize(base / rel);
    std::error_code ec;
    if (pathutil::is_within(p, base) && ghc::filesystem::is_regular_file(p, ec)) {
      return FsResult::success(
          Json{{"opened", "file"}, {"path", rel}, {"abs", p.string()}});
    }
  }
  return FsResult::error(ErrCode::kNotFound, "link target not found: " + href);
}

namespace {
// resolve a project-relative path, refusing anything that escapes the root
bool safe_project_path(const std::string& root, const std::string& rel,
                       ghc::filesystem::path* out) {
  if (root.empty()) return false;
  ghc::filesystem::path base = pathutil::normalize(root);
  ghc::filesystem::path p = rel.empty() || rel == "."
                                ? base
                                : pathutil::normalize(base / rel);
  std::string bs = base.string(), ps = p.string();
  if (ps.size() < bs.size()) return false;
  for (size_t i = 0; i < bs.size(); ++i) {
    char a = bs[i], b = ps[i];
    if (a == '\\') a = '/';
    if (b == '\\') b = '/';
    if (a != b) return false;
  }
  // require the next char to be a separator or end (no ".."-style prefix hits)
  if (ps.size() > bs.size() && ps[bs.size()] != '/' && ps[bs.size()] != '\\') return false;
  if (out) *out = p;
  return true;
}
}  // namespace

// List one directory for the composer's path completion, in either of two modes:
//
//   * `abs` relative to the PROJECT ROOT (the normal "/" case: the user typed
//     "/src/ne"), and
//   * an ABSOLUTE path ("./" or "/" outside the project) - allowed only when the
//     session's permission level lets the agent read outside the project, so the
//     hint list can never reveal directories the agent itself may not touch.
//
// Returns {dir, prefix, entries:[{name,isDir}]} where `dir`/`prefix` are the
// resolved pieces the UI should keep typing against.
FsResult FsApi::complete_path(const std::string& sid, const std::string& typed) {
  std::string cwd;
  std::string perm;
  {
    Json full;
    if (!sid.empty() && sessions_->load_full(sid, &full).ok()) {
      if (full.contains("meta")) {
        cwd = jsonutil::get<std::string>(full["meta"], "cwd", "");
      }
      Json ss = full.contains("sessionSettings") ? full["sessionSettings"]
                                                 : Json::object();
      perm = jsonutil::get<std::string>(ss, "filePerm", "permissive");
    }
  }
  bool outside_ok = (perm == "full" || perm == "permissive");
  // Split the TYPED text, not the resolved absolute path.
  //
  // The old code resolved first and then split the RESULT on its last separator,
  // which for "./src/ui/" is the one before "ui" - so it listed src/ filtered by
  // the word "ui" instead of opening ui/. Any path below the first level
  // therefore never matched ("文件提示依旧不支持子文件夹（无匹配）").
  //
  // A trailing separator means "list this directory"; anything after the last
  // separator is the partial name to filter by. The returned dir/prefix are the
  // TYPED pieces, so the UI keeps working against what the user sees.
  size_t sep = typed.find_last_of("/\\");
  std::string dir_part = sep == std::string::npos ? std::string() : typed.substr(0, sep + 1);
  std::string frag = sep == std::string::npos ? typed : typed.substr(sep + 1);
  auto empty_answer = [&](bool blocked) {
    Json j = Json{{"dir", dir_part}, {"prefix", frag}, {"entries", Json::array()}};
    if (blocked) j["blocked"] = true;
    return FsResult::success(j);
  };
  if (dir_part.empty()) {
    if (cwd.empty()) return empty_answer(false);
  } else if (dir_part.compare(0, 2, "./") == 0 || dir_part.compare(0, 2, ".\\") == 0) {
    if (!outside_ok) return empty_answer(true);
  } else if (dir_part[0] == '/' || dir_part[0] == '\\') {
    if (!outside_ok) return empty_answer(true);
  } else if (cwd.empty()) {
    return empty_answer(false);
  }
  // Resolve the DIRECTORY the typed text points at.
  ghc::filesystem::path dir;
  if (dir_part.empty()) {
    dir = pathutil::normalize(ghc::filesystem::path(cwd));
  } else if (dir_part.compare(0, 2, "./") == 0 ||
             dir_part.compare(0, 2, ".\\") == 0) {
    // "./x" means "next to the project root" in the user's head.
    dir = pathutil::normalize(ghc::filesystem::path(cwd) / dir_part.substr(2));
  } else if (dir_part[0] == '/' || dir_part[0] == '\\') {
    dir = pathutil::normalize(dir_part);
  } else {
    dir = pathutil::normalize(ghc::filesystem::path(cwd) / dir_part);
  }
  std::error_code ec;
  Json arr = Json::array();
  if (ghc::filesystem::is_directory(dir, ec)) {
    for (auto& e : ghc::filesystem::directory_iterator(dir, ec)) {
      std::string name = e.path().filename().string();
      if (name.empty() || name[0] == '.') continue;  // hidden noise
      if (name.size() < frag.size()) continue;
      bool match = true;
      for (size_t i = 0; i < frag.size(); ++i) {
        char a = name[i], c = frag[i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (a != c) { match = false; break; }
      }
      if (!match) continue;
      std::error_code ec2;
      Json j = Json::object();
      j["name"] = name;
      j["isDir"] = ghc::filesystem::is_directory(e.path(), ec2);
      arr.push_back(j);
      if (arr.size() >= 60) break;
    }
  }
  return FsResult::success(Json{{"dir", dir_part}, {"prefix", frag}, {"entries", arr}});
}

FsResult FsApi::project_files(const std::string& pid, const std::string& rel) {
  std::string root = path_for(pid);
  ghc::filesystem::path dir;
  if (!safe_project_path(root, rel, &dir)) {
    return FsResult::error(ErrCode::kInvalidArg, "path escapes project: " + rel);
  }
  std::error_code ec;
  if (!ghc::filesystem::is_directory(dir, ec)) {
    return FsResult::error(ErrCode::kNotFound, "dir not found: " + rel);
  }
  Json arr = Json::array();
  // A single directory with hundreds of thousands of entries (a log dump, an
  // unpacked archive) must not become a JSON array AND a DOM subtree of that
  // size. The cap is generous for real source trees and the reply says when it
  // cut the list, so the user knows what they are not seeing.
  const size_t kMaxEntries = 2000;
  bool truncated = false;
  for (auto& e : ghc::filesystem::directory_iterator(dir, ec)) {
    if (arr.size() >= kMaxEntries) { truncated = true; break; }
    std::string name = e.path().filename().string();
    bool isdir = ghc::filesystem::is_directory(e.path(), ec);
    std::error_code ec2;
    int64_t size = isdir ? 0
                         : static_cast<int64_t>(ghc::filesystem::file_size(e.path(), ec2));
    Json j = Json::object();
    j["name"] = name;
    j["isDir"] = isdir;
    j["size"] = size;
    arr.push_back(j);
  }
  // directories first, then files, each alphabetical
  std::sort(arr.begin(), arr.end(), [](const Json& a, const Json& b) {
    bool ad = jsonutil::get<bool>(a, "isDir", false);
    bool bd = jsonutil::get<bool>(b, "isDir", false);
    if (ad != bd) return ad;
    return jsonutil::get<std::string>(a, "name", "") <
           jsonutil::get<std::string>(b, "name", "");
  });
  return FsResult::success(
      Json{{"entries", arr}, {"path", rel}, {"truncated", truncated}});
}

namespace {

// A preview tab cannot render raw bytes. NULs are the standard tell (the file
// tools use the same guard); UTF-16 TEXT is the one NUL-carrying case that is
// really text, and its NULs sit on ONE parity of the byte pairs.
bool preview_is_binary(const std::string& raw) {
  size_t n = std::min(raw.size(), static_cast<size_t>(4096));
  size_t nul = 0, even_nul = 0, odd_nul = 0;
  for (size_t i = 0; i < n; ++i) {
    if (raw[i] != '\0') continue;
    ++nul;
    if ((i & 1) == 0) ++even_nul; else ++odd_nul;
  }
  if (nul == 0) return false;
  size_t pairs = n / 2;
  if (pairs >= 8 && (even_nul >= pairs * 3 / 4 || odd_nul >= pairs * 3 / 4)) {
    return false;
  }
  return true;
}

// Image types the content sniffer cannot see: SVG is XML text, ICO is a
// container, and AVIF has no stb decoder. Matched by extension only.
const char* image_mime_for_name(const std::string& rel) {
  size_t dot = rel.find_last_of('.');
  if (dot == std::string::npos) return nullptr;
  std::string ext = rel.substr(dot + 1);
  for (size_t i = 0; i < ext.size(); ++i) {
    ext[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(ext[i])));
  }
  if (ext == "svg") return "image/svg+xml";
  if (ext == "ico") return "image/x-icon";
  if (ext == "avif") return "image/avif";
  return nullptr;
}

}  // namespace

FsResult FsApi::project_read_file(const std::string& pid, const std::string& rel) {
  std::string root = path_for(pid);
  ghc::filesystem::path file;
  if (!safe_project_path(root, rel, &file)) {
    return FsResult::error(ErrCode::kInvalidArg, "path escapes project: " + rel);
  }
  std::error_code ec;
  if (!ghc::filesystem::is_regular_file(file, ec)) {
    return FsResult::error(ErrCode::kNotFound, "file not found: " + rel);
  }
  auto sz = ghc::filesystem::file_size(file, ec);
  // A hard cap before reading: reading a 2 GB file to see it is not a text is
  // exactly the kind of "just open it" that hangs the app.
  if (!ec && sz > 32LL * 1024 * 1024) {
    return FsResult::error(ErrCode::kInvalidArg, "file too large to preview (>32MB)");
  }
  std::ifstream ifs(file.string(), std::ios::binary);
  if (!ifs) return FsResult::error(ErrCode::kIo, "cannot open file: " + rel);
  std::stringstream ss;
  ss << ifs.rdbuf();
  std::string bytes = ss.str();
  // 1) An image is CONTENT, not text, whatever its extension says. The sniffer
  //    only needs the header, so a PNG called ".dat" still previews.
  fs::image::Format fmt = fs::image::sniff(bytes);
  const char* name_mime = image_mime_for_name(rel);
  bool by_name = name_mime && !preview_is_binary(bytes);
  if (fmt != fs::image::Format::kUnknown || by_name) {
    if (bytes.size() > 12LL * 1024 * 1024) {
      return FsResult::error(ErrCode::kInvalidArg,
                             "image too large to preview (>12MB)");
    }
    std::string mime = fmt != fs::image::Format::kUnknown
                           ? fs::image::mime_of(fmt)
                           : std::string(name_mime);
    return FsResult::success(Json{
        {"kind", "image"},
        {"mime", mime},
        {"dataUri", "data:" + mime + ";base64," + fs::image::base64(bytes)},
        {"size", static_cast<int64_t>(bytes.size())}});
  }
  // 2) Binary (an executable, an object file, an archive): say so. Opening it
  //    as text produced pages of mojibake and, on a bad decode, an entire
  //    unreadable preview - the user could not even tell what the file WAS.
  if (preview_is_binary(bytes)) {
    return FsResult::success(Json{{"kind", "binary"},
                                  {"size", static_cast<int64_t>(bytes.size())}});
  }
  if (!ec && sz > 512 * 1024) {
    return FsResult::error(ErrCode::kInvalidArg, "file too large to preview (>512KB)");
  }
  // 3) Text. The preview pane renders this text; raw GBK bytes would make the
  //    whole reply unserializable (the side panel then showed "加载失败" for a
  //    perfectly ordinary file).
  return FsResult::success(Json{{"content", utf8::decode_to_utf8(bytes)},
                                {"kind", "text"},
                                {"size", static_cast<int64_t>(bytes.size())}});
}

// 保存聊天附件：写进 <project>/.flashsight/uploads/。放在项目里（而不是临时
// 目录）是为了让模型能像读别的文件一样按路径读它——附件的意义就是"模型能看"。
// 同名不覆盖：加 -1/-2 后缀，否则第二次上传会静默盖掉第一次的内容。
FsResult FsApi::project_save_upload(const std::string& pid, const std::string& name,
                                    const std::string& bytes) {
  std::string root = path_for(pid);
  if (root.empty()) {
    return FsResult::error(ErrCode::kNotFound, "project not found: " + pid);
  }
  // 只取基名：浏览器给的 filename 可能带路径（也防目录穿越）
  std::string base = name;
  size_t cut = base.find_last_of("/\\");
  if (cut != std::string::npos) base = base.substr(cut + 1);
  if (base.empty() || base == "." || base == "..") base = "upload";
  // 去掉会破坏路径的字符（Windows 保留字符）
  for (size_t i = 0; i < base.size(); ++i) {
    char c = base[i];
    if (c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' ||
        c == '|' || c < 0x20) {
      base[i] = '_';
    }
  }
  ghc::filesystem::path dir =
      ghc::filesystem::path(root) / ".flashsight" / "uploads";
  std::error_code ec;
  ghc::filesystem::create_directories(dir, ec);
  if (ec) {
    return FsResult::error(ErrCode::kIo, "cannot create uploads dir: " + ec.message());
  }
  std::string stem = base, ext;
  size_t dot = base.find_last_of('.');
  if (dot != std::string::npos && dot > 0) {
    stem = base.substr(0, dot);
    ext = base.substr(dot);
  }
  ghc::filesystem::path target = dir / base;
  for (int n = 1; ghc::filesystem::exists(target, ec) && n < 1000; ++n) {
    target = dir / (stem + "-" + std::to_string(n) + ext);
  }
  std::ofstream ofs(target.string(), std::ios::binary);
  if (!ofs) {
    return FsResult::error(ErrCode::kIo, "cannot write " + target.string());
  }
  ofs.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  ofs.close();
  if (!ofs) {
    return FsResult::error(ErrCode::kIo, "write failed: " + target.string());
  }
  std::string rel = ".flashsight/uploads/" + target.filename().string();
  Json o = Json::object();
  o["name"] = target.filename().string();
  o["path"] = target.string();
  o["rel"] = rel;
  o["size"] = static_cast<int64_t>(bytes.size());
  return FsResult::success(o);
}

// ----- session.* -----
FsResult FsApi::session_new(const std::string& pid, const std::string& cwd) {
  return sessions_->new_session(pid, cwd);
}
FsResult FsApi::session_open(const std::string& sid) { return sessions_->open_session(sid); }
FsResult FsApi::session_close(const std::string& sid) { return sessions_->close_session(sid); }
FsResult FsApi::session_fork(const std::string& sid, const std::string& turnId) {
  return sessions_->fork(sid, turnId);
}
FsResult FsApi::session_rollback(const std::string& sid, const std::string& turnId) {
  return sessions_->rollback(sid, turnId, nullptr);
}
// Drop the turn and everything after it from the transcript, WITHOUT reverting
// file patches. This is what "编辑并重发最后一条" needs: the user wants a do-over
// of the message, not to undo the edits that message already produced.
FsResult FsApi::session_trim_history(const std::string& sid,
                                     const std::string& turnId) {
  return sessions_->trim_history(sid, turnId);
}
FsResult FsApi::session_forget(const std::string& sid, const std::string& scope) {
  return sessions_->forget(sid, scope);
}
FsResult FsApi::session_list(const std::string& pid) {
  FsResult r = sessions_->list_sessions(pid);
  if (!r.ok() || !r.data.is_array()) return r;
  // Enrich each row with the two things the project card shows and the session
  // files do not carry: when it last changed, and whether it is busy right now.
  // Both are derived here (file mtime + the host's live loops) rather than
  // stored, so they cannot go stale on disk.
  Json out = Json::array();
  for (size_t i = 0; i < r.data.size(); ++i) {
    Json m = r.data[i];
    if (!m.is_object()) continue;
    std::string sid = jsonutil::get<std::string>(m, "sid", "");
    if (sid.empty()) continue;
    m["running"] = agent_host_ && agent_host_->is_running(sid);
    // "busy" is not enough for the status dot: the UI distinguishes running,
    // waiting-for-approval, failed and done. Loop state is live; lastError is
    // filled by list_sessions, which already reads each full.json.
    m["loopState"] = agent_host_ ? agent_host_->loop_state(sid) : "idle";
    // An auto-continue round is background work: the session must show the
    // purple dot even when no user message triggered it.
    int auto_round = agent_host_ ? agent_host_->auto_round(sid) : 0;
    if (auto_round > 0) m["autoRound"] = auto_round;
    // Background activity: a terminal that is still running (and that the user
    // has NOT closed the tab for) means work can still appear in this session
    // even though no turn is in flight. The status dot uses this to show a
    // distinct "background" colour instead of "idle".
    int64_t live_terms = 0;
    if (terminals_) {
      for (auto& te : terminals_->list_for_session(sid)) {
        bool user_visible =
            te.kind == TermKind::kUser ||
            ((te.kind == TermKind::kModelBg ||
              te.kind == TermKind::kModelFg) &&
             !te.label.empty());
        if (user_visible && te.status == TermStatus::kRunning) ++live_terms;
      }
    }
    m["terminalCount"] = live_terms;
    int64_t mt = sessions_->full_mtime_ms(sid);
    if (mt > 0) m["updatedAt"] = mt;
    out.push_back(m);
  }
  // Most-recently-updated first. list_sessions() returns sids alphabetically
  // (a directory scan); the sidebar showed that raw order, so a session that
  // just finished a turn stayed buried between old ones.
  std::vector<Json> rows(out.begin(), out.end());
  std::sort(rows.begin(), rows.end(), [](const Json& a, const Json& b) {
    int64_t ta = jsonutil::get_int(a, "updatedAt", 0);
    int64_t tb = jsonutil::get_int(b, "updatedAt", 0);
    if (ta != tb) return ta > tb;
    return jsonutil::get<std::string>(a, "sid", "") <
           jsonutil::get<std::string>(b, "sid", "");
  });
  Json sorted = Json::array();
  for (size_t i = 0; i < rows.size(); ++i) sorted.push_back(rows[i]);
  return FsResult::success(sorted);
}
FsResult FsApi::session_delete(const std::string& sid) {
  return sessions_->delete_session(sid);
}
FsResult FsApi::session_mark_seen(const std::string& sid, int64_t ms) {
  return sessions_->mark_seen(sid, ms);
}
FsResult FsApi::session_rename(const std::string& sid, const std::string& title) {
  // A rename from the UI is a USER decision: mark it so the automatic title
  // upkeep never overwrites it.
  return sessions_->rename_session_by_user(sid, title);
}

// Ask the model for a short title.
//
// The transcript is condensed to the first user messages (a title only needs the
// gist, and sending a whole session would cost more than the title is worth).
// The summary-model preference wins; otherwise the session's own model is used.
FsResult FsApi::session_ai_title(const std::string& sid) {
  Json full;
  FsResult r = sessions_->load_full(sid, &full);
  if (!r.ok()) return r;
  const Json& turns = full.contains("turns") && full["turns"].is_array()
                          ? full["turns"] : Json::array();
  std::string excerpt;
  int taken = 0;
  for (size_t i = 0; i < turns.size() && taken < 6; ++i) {
    if (jsonutil::get<std::string>(turns[i], "role", "") != "user") continue;
    std::string text = jsonutil::get<std::string>(turns[i], "promptRaw", "");
    if (text.empty()) continue;
    excerpt += "- " + utf8::truncate(text, 400) + "\n";
    ++taken;
  }
  if (excerpt.empty()) {
    return FsResult::error(ErrCode::kInvalidArg, "no user messages to summarise");
  }
  Json ss = full.value("sessionSettings", Json::object());
  std::string model = jsonutil::get<std::string>(ss, "model", "");
  std::string provider = jsonutil::get<std::string>(ss, "provider", "");
  bool model_overridden = false;
  if (settings_) {
    // The settings UI stores this as the dotted key "model.pref.summary.model"
    // (a plain string), while this code read an OBJECT at "model.pref.summary".
    // The two never met, so "摘要模型" in settings had no effect at all. Accept
    // both shapes: the flat key the UI writes, and the object form.
    Json flat = settings_->get_value(kKeyModelPrefSummary, Json::object());
    std::string m;
    if (flat.is_string()) {
      m = flat.get<std::string>();
    } else if (flat.is_object()) {
      m = jsonutil::get<std::string>(flat, "model", "");
    }
    if (!m.empty()) {
      model_overridden = m != model;
      model = m;
    }
  }
  if (model.empty()) {
    return FsResult::error(ErrCode::kInvalidArg, "no model selected");
  }
  ModelCallRequest req;
  req.model = model;
  // The title request is a model call like any other: it must carry the same
  // provider identity as the session. Leaving it empty routed a selected
  // A6API/deepseek-v4.1-flash title call to the first provider that knew a
  // similar name (HappyAPI), producing a 503 "selfdeploy/..." while the main
  // turn correctly used A6. A different summary model is resolved by name, but
  // the session's provider still wins when it serves that model.
  req.provider = provider;
  if (model_overridden && providers_ && model_table_) {
    auto serves = [this, &model](const std::string& pid) {
      std::vector<ModelEntry> models = model_table_->list_models(pid);
      for (size_t i = 0; i < models.size(); ++i) {
        if (models[i].id == model) return true;
      }
      return false;
    };
    if (!req.provider.empty() && serves(req.provider)) {
      // keep the session provider
    } else {
      req.provider.clear();
      for (auto& c : providers_->list()) {
        if (c.enabled && serves(c.id)) {
          req.provider = c.id;
          break;
        }
      }
    }
  }
  req.messages = Json::array({
      Json{{"role", "system"},
           {"content", "You name coding sessions. Reply with ONLY the title: one "
                       "line, at most 6 words, no quotes, no trailing period, in "
                       "the language the user writes in."}},
      Json{{"role", "user"},
           {"content", "User messages in this session:\n" + excerpt}},
  });
  ModelCallResult out;
  FsResult cr = call_model_fn_
                    ? call_model_fn_(req, &out, nullptr)
                    : FsResult::error(ErrCode::kNotSupported, "model not wired");
  if (!cr.ok() || !out.error.empty()) {
    return FsResult::error(cr.code == ErrCode::kOk ? ErrCode::kRemote : cr.code,
                           out.error.empty() ? cr.message : out.error);
  }
  // One line, trimmed, bounded: a model that answers with a paragraph must not
  // become the tab label.
  std::string title = out.text;
  size_t nl = title.find_first_of("\r\n");
  if (nl != std::string::npos) title = title.substr(0, nl);
  title = utf8::truncate(title, 120);
  // Strip surrounding quotes the model often adds despite being told not to.
  while (!title.empty() && (title.front() == '"' || title.front() == '\'' ||
                            title.front() == '「' || title.front() == ' ')) {
    title.erase(title.begin());
  }
  while (!title.empty() && (title.back() == '"' || title.back() == '\'' ||
                            title.back() == '」' || title.back() == ' ' ||
                            title.back() == '。' || title.back() == '.')) {
    title.pop_back();
  }
  if (title.empty()) {
    return FsResult::error(ErrCode::kParse, "model returned an empty title");
  }
  FsResult rr = sessions_->rename_session(sid, title);
  if (!rr.ok()) return rr;
  return FsResult::success(Json{{"sid", sid}, {"title", title}});
}

// Automatic title upkeep.
//
// The rule from the design doc: a placeholder first (the first sentence of the
// user's message, so the tab is never blank), and only once the session has real
// content (3+ turns, or one turn with many tool calls) ask the model for a
// proper summary. That keeps the common case free (no extra request) and spends
// a call only when there is something worth summarising.
void FsApi::maybe_upgrade_title(const std::string& sid) {
  if (!sessions_ || sid.empty()) return;
  Json full;
  if (!sessions_->load_full(sid, &full).ok()) return;
  Json meta = full.value("meta", Json::object());
  // A title the user typed (or accepted from AI rename) is final: overwriting it
  // would silently undo an explicit action.
  if (jsonutil::get<bool>(meta, "titleUser", false)) return;
  const Json& turns = full.contains("turns") && full["turns"].is_array()
                          ? full["turns"] : Json::array();
  if (turns.empty()) return;

  std::string first_user;
  for (auto& t : turns) {
    if (jsonutil::get<std::string>(t, "role", "") != "user") continue;
    first_user = jsonutil::get<std::string>(t, "promptRaw", "");
    if (!first_user.empty()) break;
  }
  if (first_user.empty()) return;

  std::string current = jsonutil::get<std::string>(meta, "title", "");
  bool placeholder = current.empty() || current.compare(0, 6, "会话 ") == 0 ||
                     current.compare(0, 5, "Chat ") == 0;

  // Stage 1: placeholder = first sentence, truncated. No model call.
  if (placeholder) {
    std::string t = first_user;
    size_t cut = t.find_first_of("\r\n");
    if (cut != std::string::npos) t = t.substr(0, cut);
    t = utf8::truncate(t, 40, "…");
    while (!t.empty() && t.back() == ' ') t.pop_back();
    if (!t.empty()) sessions_->rename_session(sid, t);
  }

  // Stage 2: enough content for a real title? 3+ turns, or the first turn made
  // 10+ tool calls (one long autonomous run says more than its prompt).
  int64_t tool_calls = 0;
  for (auto& t : turns) {
    if (jsonutil::get<std::string>(t, "role", "") != "assistant") continue;
    if (t.contains("toolCalls") && t["toolCalls"].is_array()) {
      tool_calls += static_cast<int64_t>(t["toolCalls"].size());
    }
  }
  if (turns.size() < 3 && tool_calls < 10) return;
  // Only once: mark the summary title so this does not run every turn.
  if (jsonutil::get<bool>(meta, "titleSummarized", false)) return;
  FsResult r = session_ai_title(sid);
  if (r.ok()) {
    Json patch = Json::object();
    patch["titleSummarized"] = true;
    sessions_->set_meta_flag(sid, patch);
  }
}
FsResult FsApi::session_set_model(const std::string& sid, const std::string& model) {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  FsResult r = sessions_->set_model(sid, model);
  if (r.ok()) {
    sessions_->append_system_op(sid, "setModel", Json{{"model", model}});
    // A session can be MID-TURN (auto rounds) when the user changes the model.
    // Updating only the stored settings left the live AgentLoop on the old model
    // and, worse, the old provider for every following auto round.
    Json full;
    if (agent_host_ && sessions_->load_full(sid, &full).ok()) {
      Json ss = full.value("sessionSettings", Json::object());
      agent_host_->set_session_model(
          sid, model, jsonutil::get<std::string>(ss, "provider", ""));
    }
  }
  return r;
}

FsResult FsApi::session_set_model_provider(const std::string& sid,
                                           const std::string& model,
                                           const std::string& provider) {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  FsResult rm = sessions_->set_model(sid, model);
  if (!rm.ok()) return rm;
  FsResult rp = sessions_->set_provider(sid, provider);
  if (!rp.ok()) return rp;
  sessions_->append_system_op(sid, "setModel", Json{{"model", model}});
  sessions_->append_system_op(sid, "setProvider", Json{{"provider", provider}});
  if (agent_host_) agent_host_->set_session_model(sid, model, provider);
  return FsResult::success();
}
FsResult FsApi::session_set_thinking(const std::string& sid, const std::string& thinking) {
  return sessions_->set_thinking(sid, thinking);
}
FsResult FsApi::session_set_provider(const std::string& sid,
                                     const std::string& provider) {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  FsResult r = sessions_->set_provider(sid, provider);
  if (r.ok()) {
    sessions_->append_system_op(sid, "setProvider", Json{{"provider", provider}});
    Json full;
    if (agent_host_ && sessions_->load_full(sid, &full).ok()) {
      Json ss = full.value("sessionSettings", Json::object());
      std::string model = jsonutil::get<std::string>(ss, "model", "");
      if (!model.empty()) agent_host_->set_session_model(sid, model, provider);
    }
  }
  return r;
}
FsResult FsApi::session_set_permission(const std::string& sid, const std::string& perm) {
  // 低→高需确认（permissions.md §1）：approve→permissive/full、permissive→full 都要令牌
  Json full;
  FsResult r = sessions_->load_full(sid, &full);
  if (!r.ok()) return r;
  std::string cur = jsonutil::get<std::string>(full["sessionSettings"], "filePerm", "permissive");
  auto rank = [](const std::string& s) {
    return s == "full" ? 2 : s == "permissive" ? 1 : 0;
  };
  if (rank(perm) > rank(cur)) {
    return FsResult::deny("confirm-required",
                          "permission upgrade requires explicit confirm token");
  }
  r = sessions_->set_permission(sid, perm);
  if (r.ok()) sessions_->append_system_op(sid, "setPermission", Json{{"filePerm", perm}});
  return r;
}
FsResult FsApi::session_set_permission_confirm(const std::string& sid, const std::string& perm,
                                               const std::string& token) {
  FsResult r = sessions_->set_permission_confirm(sid, perm, token);
  if (r.ok()) sessions_->append_system_op(sid, "setPermission", Json{{"filePerm", perm}});
  return r;
}
FsResult FsApi::session_request_permission_token(const std::string& sid,
                                                 const std::string& newPerm) {
  return FsResult::success(
      Json{{"token", sessions_->request_permission_change_token(sid, newPerm)}});
}
FsResult FsApi::session_set_auto_mode(const std::string& sid, bool autoMode) {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  FsResult r = sessions_->set_auto_mode(sid, autoMode);
  if (r.ok() && agent_host_) agent_host_->set_session_auto_mode(sid, autoMode);
  return r;
}
FsResult FsApi::session_set_enter_send(const std::string& sid, const std::string& enterSend) {
  return sessions_->set_enter_send(sid, enterSend);
}
FsResult FsApi::session_refresh_config(const std::string& sid) {
  return sessions_->refresh_config(sid);
}
FsResult FsApi::session_append_turn(const std::string& sid, const Json& turn) {
  return sessions_->append_turn(sid, turn);
}
FsResult FsApi::session_get(const std::string& sid) {
  FsResult r = sessions_->get_session(sid);
  // 自愈：项目注册表被清掉/重建后，老会话 meta.pid 会悬空（文件面板、
  // 终端 cwd 都要用它）。按 meta.cwd 找回当前注册的 pid 再返回。
  if (r.ok() && r.data.is_object() && r.data.contains("meta") &&
      r.data["meta"].is_object()) {
    std::string pid = jsonutil::get<std::string>(r.data["meta"], "pid", "");
    if (!pid.empty() && path_for(pid).empty()) {
      std::string cwd = jsonutil::get<std::string>(r.data["meta"], "cwd", "");
      std::string fresh = cwd.empty() ? "" : proj_->pid_by_path(cwd);
      if (!fresh.empty()) r.data["meta"]["pid"] = fresh;
    }
  }
  return r;
}

// Everything except the transcript. Kept separate so opening a long session does
// not transfer (and then discard) hundreds of turns just to read the model name.
FsResult FsApi::session_info(const std::string& sid) {
  Json full;
  FsResult r = sessions_->load_full(sid, &full);
  if (!r.ok()) return r;
  Json out = Json::object();
  if (full.contains("meta")) out["meta"] = full["meta"];
  if (full.contains("sessionSettings")) out["sessionSettings"] = full["sessionSettings"];
  out["systemOps"] = full.contains("systemOps") && full["systemOps"].is_array()
                         ? full["systemOps"]
                         : Json::array();
  out["turnCount"] = full.contains("turns") && full["turns"].is_array()
                         ? static_cast<int>(full["turns"].size())
                         : 0;
  // Same self-healing as session_get: a dangling pid is recovered from meta.cwd.
  if (out.contains("meta") && out["meta"].is_object()) {
    std::string pid = jsonutil::get<std::string>(out["meta"], "pid", "");
    if (!pid.empty() && path_for(pid).empty()) {
      std::string cwd = jsonutil::get<std::string>(out["meta"], "cwd", "");
      std::string fresh = cwd.empty() ? "" : proj_->pid_by_path(cwd);
      if (!fresh.empty()) out["meta"]["pid"] = fresh;
    }
  }
  return FsResult::success(out);
}

// Paginated history. Turns are stored oldest-first; the UI pages BACKWARDS from
// the end (the latest turn is what you open a session for).
//
// `before` is an exclusive upper bound into the array; -1 means "from the end".
// The slice is returned with its absolute start index so the client can keep
// prepending without guessing.
FsResult FsApi::session_turns(const std::string& sid, int before, int limit) {
  Json full;
  FsResult r = sessions_->load_full(sid, &full);
  if (!r.ok()) return r;
  const Json& turns = full.contains("turns") && full["turns"].is_array()
                          ? full["turns"]
                          : Json::array();
  int total = static_cast<int>(turns.size());
  if (limit <= 0) limit = 30;
  int end = (before < 0 || before > total) ? total : before;
  int start = end - limit;
  if (start < 0) start = 0;
  Json slice = Json::array();
  for (int i = start; i < end; ++i) slice.push_back(turns[i]);
  return FsResult::success(Json{{"turns", slice},
                                {"start", start},
                                {"total", total},
                                {"hasMore", start > 0}});
}

// ----- P3: send/append/suspend/resume/cancelWake -----
//
// session.send BLOCKS for the whole turn (provider round trip, then every tool
// call, then the next round). That is fine and intentional: the desktop UI runs
// it on a worker thread (NativeBackend), so the window never blocks, and the
// HTTP path already runs it on a request thread. Making it fire-and-forget here
// would move the threading into the core, where it would have to be undone for
// every test and for the synchronous callers.
//
// The lock: this method does NOT take the shared mutex across the turn (that
// would serialize every other call for minutes). AgentHost::send takes it only
// around the short setup, and the turn's own callbacks (persist_turn, tool
// execution) take it individually.
FsResult FsApi::session_send(const std::string& sid, const std::string& text,
                             const Json& attachments, const std::string& model,
                             const std::string& thinking,
                             const std::string& provider) {
  Json ss;
  {
    // Short prelude under the lock: persist the composer's model/thinking and
    // read the session settings. The turn itself runs WITHOUT it (see below).
    std::lock_guard<std::recursive_mutex> lk(mu_);
    if (!model.empty()) sessions_->set_model(sid, model);
    // The provider is part of the model's identity (same id on two providers):
    // persist it so the next turn routes to the same connection.
    if (!provider.empty()) sessions_->set_provider(sid, provider);
    if (!thinking.empty()) sessions_->set_thinking(sid, thinking);
    Json full;
    FsResult r = sessions_->load_full(sid, &full);
    if (!r.ok()) return r;
    ss = full.value("sessionSettings", Json::object());
  }
  bool auto_mode = jsonutil::get<bool>(ss, "autoMode", false);
  std::string perm = jsonutil::get<std::string>(ss, "filePerm", "permissive");
  std::string sess_model = jsonutil::get<std::string>(ss, "model", "");
  std::string sess_provider = jsonutil::get<std::string>(ss, "provider", "");
  // AgentHost::send takes the lock around its own setup and releases it for the
  // model call; the turn's callbacks re-take it one at a time.
  return agent_host_->send(sid, text, attachments, auto_mode, perm, sess_model,
                           jsonutil::get<std::string>(ss, "thinking", ""),
                           sess_provider);
}

FsResult FsApi::session_append(const std::string& sid, const std::string& text) {
  return agent_host_->append(sid, text);
}

FsResult FsApi::session_suspend(const std::string& sid) {
  return agent_host_->suspend(sid);
}

FsResult FsApi::session_interrupt(const std::string& sid) {
  return agent_host_->interrupt(sid);
}

FsResult FsApi::session_resume(const std::string& sid) {
  // Same rule as 继续: a resumed turn uses the model the session has NOW.
  {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    Json full;
    if (sessions_->load_full(sid, &full).ok()) {
      Json ss = full.value("sessionSettings", Json::object());
      std::string model = jsonutil::get<std::string>(ss, "model", "");
      std::string provider = jsonutil::get<std::string>(ss, "provider", "");
      if (!model.empty()) agent_host_->set_session_model(sid, model, provider);
    }
  }
  return agent_host_->resume(sid);
}

FsResult FsApi::session_cancel_wake(const std::string& sid) {
  return agent_host_->cancel_wake(sid);
}

// 定时任务：让自动模式在指定时间后自己再跑一轮。这是"模型自设唤醒"的用户
// 入口——调度器早就有了（WakeScheduler），但没有任何接口能建一个定时唤醒，
// 所以此前只能靠终端退出/子代理完成这类事件触发。
FsResult FsApi::session_schedule(const std::string& sid, int64_t delay_ms,
                                 const std::string& note) {
  if (!agent_host_) return FsResult::error(ErrCode::kInternal, "no agent host");
  if (sid.empty()) return FsResult::error(ErrCode::kInvalidArg, "no session");
  if (delay_ms <= 0) return FsResult::error(ErrCode::kInvalidArg, "delay must be > 0");
  int64_t at = (clock_ ? clock_->now_ms() : WallClock().now_ms()) + delay_ms;
  agent_host_->wakes().schedule(sid, WakeSource::kTimer, at, note);
  return FsResult::success(Json{{"at", at}, {"delayMs", delay_ms}});
}

// 当前会话的待唤醒（定时任务 + 事件唤醒）。UI 用它显示"还有多久"和取消按钮。
FsResult FsApi::session_pending_wakes(const std::string& sid) {
  Json arr = Json::array();
  if (agent_host_) {
    int64_t now = clock_ ? clock_->now_ms() : WallClock().now_ms();
    std::vector<PendingWake> list = agent_host_->wakes().pending_for_sid(sid);
    for (size_t i = 0; i < list.size(); ++i) {
      Json o = Json::object();
      o["source"] = wake_source_str(list[i].source);
      o["at"] = list[i].at_ms;
      o["inMs"] = list[i].at_ms > 0 ? list[i].at_ms - now : 0;
      o["ref"] = list[i].ref;
      arr.push_back(o);
    }
  }
  return FsResult::success(Json{{"wakes", arr}});
}

FsResult FsApi::session_loop_state(const std::string& sid) {
  std::string state = agent_host_ ? agent_host_->loop_state(sid) : "idle";
  bool compacted = false;
  if (!sid.empty()) {
    std::error_code ec;
    compacted = ghc::filesystem::is_regular_file(
        sessions_->make_store(sid).compaction_path(sid), ec);
  }
  return FsResult::success(
      Json{{"sid", sid}, {"state", state}, {"compacted", compacted}});
}

FsResult FsApi::session_todos(const std::string& sid) {
  Json items = agent_host_ ? agent_host_->todos_for(sid) : Json::array();
  return FsResult::success(Json{{"sid", sid}, {"items", items}});
}

// addon2 任务健壮性：打断后续接
FsResult FsApi::session_continue(const std::string& sid, const std::string& guidance,
                                 bool suppress_queue_once) {
  // A continuation is a NEW turn: pick up the model the session is set to now.
  // Continuing with the model captured at send time ignored a model switch made
  // while the task was parked.
  {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    Json full;
    if (sessions_->load_full(sid, &full).ok()) {
      Json ss = full.value("sessionSettings", Json::object());
      std::string model = jsonutil::get<std::string>(ss, "model", "");
      std::string provider = jsonutil::get<std::string>(ss, "provider", "");
      if (!model.empty()) agent_host_->set_session_model(sid, model, provider);
    }
  }
  // AgentHost::continue_task releases the host lock for the turn, like send().
  return agent_host_->continue_task(sid, guidance, suppress_queue_once);
}

// approve 档权限门
FsResult FsApi::session_approve(const std::string& sid, bool yes) {
  return agent_host_->approve(sid, yes);
}

// 上下文压缩：用户主动触发（或 UI 在水位提示上点"压缩"）
FsResult FsApi::session_compress(const std::string& sid) {
  if (!agent_host_) return FsResult::error(ErrCode::kInternal, "no agent host");
  FsResult r = agent_host_->request_compress(sid);
  if (r.ok()) sessions_->append_system_op(sid, "compress", Json{{"by", "user"}});
  return r;
}

// 上下文水位：promptTokens / contextLimit / pct / compressAtPct / shouldCompress
FsResult FsApi::session_context(const std::string& sid) {
  if (!agent_host_) return FsResult::error(ErrCode::kInternal, "no agent host");
  return agent_host_->context_status(sid);
}

// The in-flight turn. The session file only holds finished turns, so a tab
// reopened while a turn is running had nothing to render - the answer kept
// streaming into a closed view. This serves the in-memory copy.
FsResult FsApi::session_live_turn(const std::string& sid) {
  Json in_mem = Json::object();
  if (agent_host_) {
    FsResult r = agent_host_->live_turn(sid);
    if (r.ok() && r.data.is_object()) in_mem = r.data;
  }
  bool running = jsonutil::get<bool>(in_mem, "running", false);
  if (running) return FsResult::success(in_mem);
  // Nothing is running in memory. There may still be a CRASHED turn on disk: a
  // snapshot written while the turn was streaming but never cleared because the
  // process died. Handing it back lets the reopened session show what was lost
  // instead of silently dropping it - which is what the user saw after the crash.
  Json crashed = sessions_->read_live_snapshot(sid);
  if (crashed.is_object() && !crashed.empty() &&
      jsonutil::get<bool>(crashed, "running", false)) {
    Json out = crashed;
    out["running"] = false;   // the process is gone: it is not running now
    out["crashed"] = true;    // the UI labels it "interrupted, can continue"
    return FsResult::success(out);
  }
  return FsResult::success(Json{{"running", false}});
}

// ----- P3: tool.* / ask / subagent / steering -----
FsResult FsApi::tool_invoke(const std::string& sid, const std::string& name,
                            const Json& args) {
  Json full;
  FsResult r = sessions_->load_full(sid, &full);
  if (!r.ok()) return r;
  Json ss = full.value("sessionSettings", Json::object());
  bool auto_mode = jsonutil::get<bool>(ss, "autoMode", false);
  std::string perm = jsonutil::get<std::string>(ss, "filePerm", "permissive");
  return agent_host_->tool_invoke(sid, name, args, auto_mode, perm);
}

FsResult FsApi::ask_answer(const std::string& sid, int ask_id,
                           const std::string& selection) {
  return agent_host_->ask_answer(sid, ask_id, selection);
}

FsResult FsApi::subagent_spawn(const std::string& sid, const std::string& kind,
                               const std::string& task, const std::string& model,
                               int depth, int64_t timeout_ms) {
  std::string id;
  FsResult r = agent_host_->subagent_spawn(sid, kind, task, model, depth,
                                           timeout_ms, &id);
  if (!r.ok()) return r;
  return FsResult::success(Json{{"id", id}});
}

FsResult FsApi::subagent_cancel(const std::string& id) {
  return agent_host_->subagent_cancel(id);
}

FsResult FsApi::subagent_list(const std::string& sid) {
  return agent_host_->subagent_list(sid);
}

FsResult FsApi::subagent_detail(const std::string& id) {
  return agent_host_->subagent_detail(id);
}

// `sid` is the session the record belongs to. A record is per-session (ids are
// per-session counters), so a bare id cannot locate the file after a restart.
// The UI knows the session - the work-log row it clicked is in that transcript -
// and passes it here.
FsResult FsApi::subagent_detail_in(const std::string& sid, const std::string& id) {
  FsResult r = agent_host_->subagent_detail(id);
  if (r.ok()) return r;
  // The in-memory record is gone (the process restarted, or the record was
  // pruned). A finished subagent's transcript is part of the session's history -
  // the work-log row that created it is still there - so fall back to the copy
  // saved next to the session. Without this every "打开记录" button in an older
  // transcript pointed at nothing.
  if (!sid.empty()) {
    Json rec = sessions_->read_subagent_record(sid, id);
    if (rec.is_object() && !rec.empty()) return FsResult::success(rec);
  }
  return r;
}

FsResult FsApi::subagent_follow_up(const std::string& id, const std::string& text) {
  return agent_host_->subagent_follow_up(id, text);
}

// addon2：子代理引导（子代理无轮概念，只需当前引导）
FsResult FsApi::subagent_steer(const std::string& id, const std::string& text) {
  return agent_host_->subagent_steer(id, text);
}

FsResult FsApi::steering_push(const std::string& sid, const std::string& kind,
                              const std::string& text) {
  return agent_host_->steering_push(sid, kind, text);
}

FsResult FsApi::steering_list(const std::string& sid) {
  return agent_host_->steering_list(sid);
}
FsResult FsApi::steering_edit(const std::string& sid, int id, const std::string& text) {
  return agent_host_->steering_edit(sid, id, text);
}
FsResult FsApi::steering_erase(const std::string& sid, int id) {
  return agent_host_->steering_erase(sid, id);
}
FsResult FsApi::steering_take(const std::string& sid, int id) {
  return agent_host_->steering_take(sid, id);
}
FsResult FsApi::steering_reorder(const std::string& sid, const std::vector<int>& order) {
  return agent_host_->steering_reorder(sid, order);
}

// ----- skill.*（addon2：中英双语 skill，按需加载）-----
FsResult FsApi::skill_list() {
  return FsResult::success(
      agent_host_->skills().list_json(parse_think_lang(agent_host_->thinking_lang())));
}

FsResult FsApi::skill_load(const std::string& name) {
  return agent_host_->skills().load(name,
                                    parse_think_lang(agent_host_->thinking_lang()));
}

// 从目录导入 skill（该目录下每个含 SKILL.md 的子目录 / 每个 .md 文件算一条）。
// 导入后写回设置：注册表本身是内存里的，重启要从设置恢复。
FsResult FsApi::skill_import(const std::string& dir) {
  if (dir.empty()) return FsResult::error(ErrCode::kInvalidArg, "dir required");
  int n = agent_host_->skills().import_dir(dir);
  settings_->set_global(kKeySkills, agent_host_->skills().user_json());
  settings_->save_global();
  return FsResult::success(Json{{"added", n}, {"skills", agent_host_->skills().list_json(
      parse_think_lang(agent_host_->thinking_lang()))}});
}

// 新建一条 skill：正文就是用户写的说明。
FsResult FsApi::skill_create(const std::string& name, const std::string& title,
                             const std::string& description, const std::string& body) {
  if (name.empty()) return FsResult::error(ErrCode::kInvalidArg, "name required");
  Skill s;
  s.name = name;
  s.title_zh = title.empty() ? name : title;
  s.title_en = s.title_zh;
  s.desc_zh = description;
  s.desc_en = description;
  s.body_zh = body;
  s.body_en = body;
  s.loadable = true;
  if (!agent_host_->skills().add(s)) {
    return FsResult::error(ErrCode::kInvalidArg, "cannot add skill");
  }
  settings_->set_global(kKeySkills, agent_host_->skills().user_json());
  settings_->save_global();
  return FsResult::success(Json{{"name", name}});
}

FsResult FsApi::skill_set_enabled(const std::string& name, bool enabled) {
  if (!agent_host_->skills().set_enabled(name, enabled)) {
    return FsResult::error(ErrCode::kNotFound, "skill not found: " + name);
  }
  settings_->set_global(kKeySkills, agent_host_->skills().user_json());
  settings_->save_global();
  return FsResult::success();
}

FsResult FsApi::skill_remove(const std::string& name) {
  if (!agent_host_->skills().remove(name)) {
    // 内置 skill 删不掉，只能停用
    return FsResult::error(ErrCode::kInvalidArg,
                           "builtin skill cannot be removed; disable it instead");
  }
  settings_->set_global(kKeySkills, agent_host_->skills().user_json());
  settings_->save_global();
  return FsResult::success();
}

// ----- mcp.*（外部工具/技能包；注册表持久化到设置 mcp.entries）-----
namespace {
// Where an MCP SERVER is actually configured. MCP is a protocol: a server is a
// definition (command/url + args), not a directory of markdown. The old probe
// pointed at `~/.claude/skills` and `~/.claude/commands` - SKILL locations -
// so "自动探测" imported skill folders as MCP entries. That is exactly the
// report: something named in Skills also showed up under MCP.
std::vector<std::string> mcp_probe_files() {
  std::vector<std::string> files;
  const char* home = std::getenv("USERPROFILE");
  if (!home) home = std::getenv("HOME");
  std::string h = home ? home : "";
  if (!h.empty()) {
    // Claude Code / Claude Desktop style server lists.
    files.push_back(h + "/.claude.json");
    files.push_back(h + "/.claude/settings.json");
    files.push_back(h + "/.config/Claude/claude_desktop_config.json");
    files.push_back(h + "/AppData/Roaming/Claude/claude_desktop_config.json");
    // opencode / generic MCP config.
    files.push_back(h + "/.config/opencode/opencode.json");
    files.push_back(h + "/.config/flashsight/mcp.json");
    files.push_back(h + "/.flashsight/mcp.json");
  }
  return files;
}

// Directories a SKILL is imported from (SKILL.md / <name>.md per entry).
std::vector<std::string> skill_probe_dirs() {
  std::vector<std::string> dirs;
  const char* home = std::getenv("USERPROFILE");
  if (!home) home = std::getenv("HOME");
  std::string h = home ? home : "";
  if (!h.empty()) {
    dirs.push_back(h + "/.claude/skills");
    dirs.push_back(h + "/.claude/commands");
    dirs.push_back(h + "/.config/opencode/skills");
    dirs.push_back(h + "/.opencode/skills");
    dirs.push_back(h + "/.config/flashsight/skills");
    dirs.push_back(h + "/.flashsight/skills");
  }
  return dirs;
}
}  // namespace

FsResult FsApi::mcp_list() {
  return FsResult::success(Json{{"tools", mcp_registry_->list_json()}});
}

FsResult FsApi::mcp_import(const std::string& dir) {
  FsResult r = mcp_registry_->import_dir(dir);
  if (!r.ok()) return r;
  return mcp_set_enabled("", true);  // 触发持久化（保存当前整表）
}

FsResult FsApi::mcp_add(const std::string& name, const std::string& dir, bool enabled) {
  if (name.empty()) return FsResult::error(ErrCode::kInvalidArg, "empty name");
  mcp_registry_->upsert(name, dir, enabled);
  return mcp_set_enabled("", true);
}

FsResult FsApi::mcp_set_enabled(const std::string& name, bool enabled) {
  // name 为空 = 只做一次持久化（整表落盘），不改启用态
  if (!name.empty() && !mcp_registry_->set_enabled(name, enabled)) {
    return FsResult::error(ErrCode::kNotFound, "mcp tool not found: " + name);
  }
  settings_->set_global(kKeyMcpEntries, mcp_registry_->dump());
  settings_->save_global();
  return FsResult::success(Json{{"tools", mcp_registry_->list_json()}});
}

FsResult FsApi::mcp_remove(const std::string& name) {
  if (!mcp_registry_->remove(name)) {
    return FsResult::error(ErrCode::kNotFound, "mcp tool not found: " + name);
  }
  settings_->set_global(kKeyMcpEntries, mcp_registry_->dump());
  settings_->save_global();
  return FsResult::success(Json{{"tools", mcp_registry_->list_json()}});
}

FsResult FsApi::mcp_detect() {
  // MCP detection reads SERVER definitions out of the config files where those
  // clients keep them (`mcpServers` in a Claude-style config, `mcp.servers` /
  // `servers` elsewhere). It used to walk `~/.claude/skills` and
  // `~/.claude/commands` - SKILL directories - and register each subdirectory
  // as an "MCP entry", which is why a skill named in Skills also appeared under
  // MCP. Skills have their own detection (skill_detect).
  Json found = Json::array();
  std::vector<std::string> files = mcp_probe_files();
  for (size_t i = 0; i < files.size(); ++i) {
    std::error_code ec;
    if (!ghc::filesystem::is_regular_file(files[i], ec)) continue;
    int added = mcp_registry_->import_config_file(files[i]);
    if (added <= 0) continue;  // present but no server definitions in it
    Json o = Json::object();
    o["file"] = files[i];
    o["added"] = added;
    found.push_back(o);
  }
  if (!found.empty()) {
    settings_->set_global(kKeyMcpEntries, mcp_registry_->dump());
    settings_->save_global();
  }
  return FsResult::success(Json{{"scanned", found}, {"tools", mcp_registry_->list_json()}});
}

// Skill detection: the counterpart of mcp_detect, on the SKILL side. The Skill
// page's "自动探测" button used to call mcp_detect, so it could only ever find
// MCP entries - skills were reachable only through a manual directory import.
FsResult FsApi::skill_detect() {
  Json found = Json::array();
  std::vector<std::string> dirs = skill_probe_dirs();
  for (size_t i = 0; i < dirs.size(); ++i) {
    std::error_code ec;
    if (!ghc::filesystem::is_directory(dirs[i], ec)) continue;
    int added = agent_host_->skills().import_dir(dirs[i]);
    Json o = Json::object();
    o["dir"] = dirs[i];
    o["added"] = added;
    found.push_back(o);
  }
  if (!found.empty()) {
    settings_->set_global(kKeySkills, agent_host_->skills().user_json());
    settings_->save_global();
  }
  return FsResult::success(Json{{"scanned", found},
                                {"skills", agent_host_->skills().list_json(
                                    parse_think_lang(agent_host_->thinking_lang()))}});
}

FsResult FsApi::set_thinking_lang(const std::string& lang) {
  agent_host_->set_thinking_lang(lang);
  settings_->set_global(kKeyThinkingLang, lang);
  settings_->save_global();
  return FsResult::success(Json{{"thinkingLang", lang}});
}

// ----- terminal.* -----
namespace {

// The PTY stream is bytes with escape sequences in it. A saved transcript is
// read by a human (or the history grid), so the escapes are dropped: CSI / OSC /
// simple two-byte sequences, plus \r progress redraws (keep what came last).
std::string terminal_plain_text(const std::string& raw) {
  std::string out;
  out.reserve(raw.size());
  size_t i = 0;
  while (i < raw.size()) {
    unsigned char c = static_cast<unsigned char>(raw[i]);
    if (c == 0x1b && i + 1 < raw.size()) {
      char k = raw[i + 1];
      if (k == '[') {  // CSI: ESC [ params final
        size_t j = i + 2;
        while (j < raw.size()) {
          unsigned char b = static_cast<unsigned char>(raw[j]);
          if (b >= 0x40 && b <= 0x7e) { ++j; break; }
          ++j;
        }
        i = j;
        continue;
      }
      if (k == ']') {  // OSC: ESC ] ... BEL, or ESC followed by a backslash
        size_t j = i + 2;
        while (j < raw.size()) {
          if (raw[j] == 0x07) { ++j; break; }
          if (raw[j] == 0x1b && j + 1 < raw.size() && raw[j + 1] == '\\') {
            j += 2;
            break;
          }
          ++j;
        }
        i = j;
        continue;
      }
      if (k == '(' || k == ')') { i += 3; continue; }  // charset select
      i += 2;
      continue;
    }
    out.push_back(static_cast<char>(c));
    ++i;
  }
  // CRLF is a LINE ENDING (what a Windows shell prints), not a redraw: drop the
  // \r so "hello\r\n" becomes "hello" and not an empty line. A LONE \r inside a
  // line IS a redraw (progress bars), and there the last segment is what the
  // screen ended up showing.
  std::string norm;
  norm.reserve(out.size());
  for (size_t k = 0; k < out.size(); ++k) {
    if (out[k] == '\r' && k + 1 < out.size() && out[k + 1] == '\n') continue;
    norm.push_back(out[k]);
  }
  std::string text;
  text.reserve(norm.size());
  size_t start = 0;
  while (start <= norm.size()) {
    size_t nl = norm.find('\n', start);
    std::string line = norm.substr(start, nl == std::string::npos ? std::string::npos
                                                                  : nl - start);
    size_t cr = line.rfind('\r');
    if (cr != std::string::npos) line = line.substr(cr + 1);
    text += line;
    if (nl == std::string::npos) break;
    text.push_back('\n');
    start = nl + 1;
  }
  return text;
}

std::string read_file_bytes(const std::string& path) {
  std::ifstream ifs(path, std::ios::binary);
  if (!ifs) return std::string();
  std::stringstream ss;
  ss << ifs.rdbuf();
  return ss.str();
}

std::string lower_ext(const std::string& name) {
  size_t dot = name.find_last_of('.');
  if (dot == std::string::npos) return std::string();
  std::string e = name.substr(dot);
  for (size_t i = 0; i < e.size(); ++i) {
    if (e[i] >= 'A' && e[i] <= 'Z') e[i] = static_cast<char>(e[i] - 'A' + 'a');
  }
  return e;
}

}  // namespace

// Persist one terminal's metadata next to its log (best effort).
void FsApi::persist_terminal_entry(const Json& entry) {
  std::string sid = jsonutil::get<std::string>(entry, "sid", "");
  std::string tid = jsonutil::get<std::string>(entry, "tid", "");
  if (sid.empty() || tid.empty()) return;
  try {
    ghc::filesystem::path dir =
        sessions_->make_store(sid).terminal_log_path(sid, tid).parent_path();
    std::error_code ec;
    ghc::filesystem::create_directories(dir, ec);
    std::string err;
    jsonutil::write_atomic((dir / (tid + ".json")).string(), entry, &err);
  } catch (...) {
  }
}

// 写一个日志文件；超过 cap 就把它轮转成 <base>.<epoch>.log 再从空文件继续。
// NOTHING IS DISCARDED: a long session gets a chain of files instead of a
// truncated one, because the history is also what a search reads (and what the
// replay shows).
namespace {

// 按行边界切：返回 buf 中第一行结束后的位置
size_t line_start_after(const std::string& buf) {
  size_t nl = buf.find('\n');
  return nl == std::string::npos ? buf.size() : nl + 1;
}

bool append_with_rotate(const std::string& path, const std::string& text,
                        uintmax_t cap) {
  if (path.empty() || text.empty()) return false;
  std::error_code ec;
  auto sz = ghc::filesystem::file_size(path, ec);
  if (!ec && sz + text.size() > cap) {
    // <dir>/<name>.log -> <dir>/<name>.<epoch>.log
    ghc::filesystem::path p(path);
    std::string stem = p.stem().string();  // "1"
    std::string dir = p.parent_path().string();
    int64_t now = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    std::string arch = dir + "/" + stem + "." + std::to_string(now) + ".log";
    ghc::filesystem::rename(path, arch, ec);
    if (ec) {
      // Rename failed (file locked): fall back to copying the bytes.
      std::string old = read_file_bytes(path);
      std::ofstream o(arch, std::ios::binary | std::ios::trunc);
      if (o) o.write(old.data(), static_cast<std::streamsize>(old.size()));
      std::ofstream clr(path, std::ios::binary | std::ios::trunc);
    }
  }
  std::ofstream ofs(path, std::ios::binary | std::ios::app);
  if (!ofs) return false;
  ofs.write(text.data(), static_cast<std::streamsize>(text.size()));
  ofs.flush();
  return true;
}

}  // namespace

// 日志文件链：轮转档按时间从旧到新，当前日志在最后。
std::vector<std::string> FsApi::terminal_log_chain(const std::string& log_path) const {
  std::vector<std::string> out;
  if (log_path.empty()) return out;
  try {
    ghc::filesystem::path p(log_path);
    std::string dir = p.parent_path().string();
    std::string stem = p.stem().string();          // "1"
    std::string cur_name = stem + ".log";          // "1.log"
    std::vector<std::pair<int64_t, std::string> > arch;
    std::error_code ec;
    if (ghc::filesystem::exists(dir, ec)) {
      for (auto& it : ghc::filesystem::directory_iterator(dir, ec)) {
        if (!it.path().has_filename()) continue;
        std::string name = it.path().filename().string();
        // 1.<epoch>.log
        if (name.compare(0, stem.size() + 1, stem + ".") != 0) continue;
        if (lower_ext(name) != ".log") continue;
        std::string mid = name.substr(stem.size() + 1,
                                      name.size() - stem.size() - 1 - 4);
        if (mid.empty()) continue;
        bool digits = true;
        for (size_t k = 0; k < mid.size(); ++k) {
          if (mid[k] < '0' || mid[k] > '9') { digits = false; break; }
        }
        if (!digits) continue;
        arch.push_back(std::make_pair(std::strtoll(mid.c_str(), nullptr, 10),
                                      it.path().string()));
      }
    }
    std::sort(arch.begin(), arch.end());
    for (size_t i = 0; i < arch.size(); ++i) out.push_back(arch[i].second);
    if (ghc::filesystem::exists(log_path, ec)) out.push_back(log_path);
  } catch (...) {
  }
  return out;
}

// Append output to the terminal's log. Every byte the PTY produced passes here
// (TerminalManager's output sink), so the record is complete even if the app is
// closed right after the process exits.
void FsApi::append_terminal_log(const std::string& tid, const std::string& text) {
  if (tid.empty() || text.empty()) return;
  std::string path;
  {
    std::lock_guard<std::mutex> lk(term_log_mu_);
    std::map<std::string, std::string>::const_iterator it = term_log_paths_.find(tid);
    if (it != term_log_paths_.end()) path = it->second;
    if (!path.empty()) {
      // fall through to the append below with the path we found
    }
  }
  if (path.empty() && terminals_) {
    // The manager binds the path at CREATE time now (before a foreground
    // command's output is drained), so the registry is the other half of the
    // lookup. Keep the registry call OUTSIDE term_log_mu_: the manager calls the
    // sink while holding its own mutex, and nested locks are how deadlocks start.
    const TermEntry* e = terminals_->find(tid);
    if (e && !e->log_path.empty()) {
      path = e->log_path;
      std::lock_guard<std::mutex> lk(term_log_mu_);
      term_log_paths_[tid] = path;
    }
  }
  if (path.empty()) return;
  try {
    // 4 MB per file, then rotate. Never drop: the chain keeps everything.
    append_with_rotate(path, text, 4u * 1024 * 1024);
  } catch (...) {
  }
}

// 用户敲进去的东西单独记：输出里虽然有回显，但进程可能不回显（密码、全屏程序），
// 而"我敲过什么"正是搜索时最常找的东西。
void FsApi::append_terminal_input(const std::string& tid, const std::string& text) {
  if (tid.empty() || text.empty()) return;
  std::string path;
  {
    std::lock_guard<std::mutex> lk(term_log_mu_);
    std::map<std::string, std::string>::const_iterator it = term_log_paths_.find(tid);
    if (it == term_log_paths_.end()) return;
    path = it->second;
  }
  // <dir>/1.log -> <dir>/1.input.log
  ghc::filesystem::path p(path);
  std::string ipath = (p.parent_path() / (p.stem().string() + ".input.log")).string();
  try {
    append_with_rotate(ipath, text, 2u * 1024 * 1024);
  } catch (...) {
  }
}

// 在历史里搜：输出日志（含轮转档）+ 用户输入日志，返回匹配行。
FsResult FsApi::terminal_search(const std::string& tid, const std::string& query,
                                int limit) {
  if (query.empty()) {
    return FsResult::success(Json{{"matches", Json::array()}, {"scanned", 0}});
  }
  std::string path;
  {
    std::lock_guard<std::mutex> lk(term_log_mu_);
    std::map<std::string, std::string>::const_iterator it = term_log_paths_.find(tid);
    if (it != term_log_paths_.end()) path = it->second;
  }
  if (path.empty()) {
    TermEntry* e = terminals_->find(tid);
    if (e) path = e->log_path;
  }
  if (path.empty()) {
    return FsResult::success(Json{{"matches", Json::array()}, {"scanned", 0}});
  }
  std::vector<std::string> files = terminal_log_chain(path);
  ghc::filesystem::path p(path);
  std::string ipath = (p.parent_path() / (p.stem().string() + ".input.log")).string();
  if (ghc::filesystem::exists(ipath)) files.push_back(ipath);
  Json matches = Json::array();
  int scanned = 0;
  for (size_t f = 0; f < files.size() && (int)matches.size() < limit; ++f) {
    ++scanned;
    std::string raw = read_file_bytes(files[f]);
    if (raw.empty()) continue;
    std::string text = terminal_plain_text(raw);
    std::string fname = ghc::filesystem::path(files[f]).filename().string();
    size_t start = 0;
    int lineno = 0;
    while (start <= text.size() && (int)matches.size() < limit) {
      size_t nl = text.find('\n', start);
      std::string line = text.substr(start, nl == std::string::npos ? std::string::npos
                                                                    : nl - start);
      ++lineno;
      if (line.find(query) != std::string::npos) {
        matches.push_back(Json{{"file", fname}, {"line", lineno},
                               {"text", utf8::truncate(line, 400)}});
      }
      if (nl == std::string::npos) break;
      start = nl + 1;
    }
  }
  return FsResult::success(Json{{"matches", matches}, {"scanned", scanned}});
}

FsResult FsApi::terminal_list(const std::string& sid) {
  // First, hydrate the registry with records left by a previous run: the LIST is
  // the only entry point the UI has, and a terminal that is not in the registry
  // cannot be listed, shown or replayed. `add_saved` marks them exited and never
  // overwrites a live one.
  try {
    ghc::filesystem::path dir =
        sessions_->make_store(sid).terminal_log_path(sid, std::string()).parent_path();
    std::error_code ec;
    if (ghc::filesystem::exists(dir, ec)) {
      std::vector<Json> saved;
      for (auto& it : ghc::filesystem::directory_iterator(dir, ec)) {
        if (!it.path().has_filename()) continue;
        if (lower_ext(it.path().filename().string()) != ".json") continue;
        std::string body = read_file_bytes(it.path().string());
        if (body.empty()) continue;
        Json j;
        try {
          j = Json::parse(body);
        } catch (...) {
          continue;
        }
        if (jsonutil::get<std::string>(j, "sid", "") != sid) continue;
        saved.push_back(j);
      }
      std::sort(saved.begin(), saved.end(), [](const Json& a, const Json& b) {
        return jsonutil::get_int(a, "startedAt", 0) <
               jsonutil::get_int(b, "startedAt", 0);
      });
      for (size_t k = 0; k < saved.size(); ++k) terminals_->add_saved(saved[k]);
    }
  } catch (...) {
  }
  Json arr = Json::array();
  // A record is only LISTED when it can be opened. The WhaleUI project ended up
  // with 227 saved records and ONE log file: every model `terminal_exec` wrote a
  // record but no transcript, so the sidebar showed a couple of hundred "terminals"
  // that were empty shells - and clicking one looked like a crash for no reason.
  // Records with a live process or a readable log are real; the rest are noise.
  std::vector<Json> real;
  for (auto& e : terminals_->list_for_session(sid)) {
    std::error_code lec;
    // Only USER terminals (and a deliberately labelled background job) are
    // user-facing terminals. Model `terminal_exec` records - even long ones -
    // live in the transcript as tool results; listing them here made a session
    // show "十几 terminals" that were only command history, and the count did
    // not match the tabs.
    bool user_visible = e.kind == TermKind::kUser ||
                        ((e.kind == TermKind::kModelBg ||
                          e.kind == TermKind::kModelFg) &&
                         !e.label.empty());
    if (!user_visible) continue;
    bool live = e.status == TermStatus::kRunning;
    bool has_log = !e.log_path.empty() &&
                   ghc::filesystem::is_regular_file(e.log_path, lec);
    if (!live && !has_log) continue;
    Json j = e.to_json();
    j["logExists"] = has_log;
    real.push_back(j);
  }
  // Newest first, bounded: a long session accumulates one record per command.
  std::sort(real.begin(), real.end(), [](const Json& a, const Json& b) {
    return jsonutil::get_int(a, "startedAt", 0) > jsonutil::get_int(b, "startedAt", 0);
  });
  const size_t kMaxListed = 50;
  for (size_t i = 0; i < real.size() && i < kMaxListed; ++i) arr.push_back(real[i]);
  return FsResult::success(arr);
}
FsResult FsApi::terminal_show(const std::string& tid) {
  TermEntry* e = terminals_->find(tid);
  if (!e) return FsResult::error(ErrCode::kNotFound, "terminal not found: " + tid);
  // 日志缺失不报错（terminal.md §3：graceful 降级）
  Json j = e->to_json();
  j["logExists"] = !e->log_path.empty() && ghc::filesystem::exists(e->log_path);
  return FsResult::success(j);
}
// 所有终端都从这里起：UI 的交互式终端和模型调用 terminal_exec/terminal_spawn。
//
// WHY this is one function: the model's tools used to call TerminalManager::spawn
// DIRECTLY, bypassing the cwd default below - so `git log` ran in the APP's
// directory (the release folder, inside this repo) and the model read FlashSight's
// own history instead of the user's project. One spawn path is the fix.
FsResult FsApi::spawn_terminal(Json args, Json* out) {
  std::string sid = jsonutil::get<std::string>(args, "sid", "");
  // Default the working directory to the PROJECT ROOT (the session's cwd).
  // A missing cwd must never mean "wherever the app happens to run".
  std::string work = jsonutil::get<std::string>(args, "cwd", "");
  if (work.empty()) {
    std::string pid = sessions_->pid_of(sid);
    work = path_for(pid);
    if (work.empty()) {
      Json full;
      if (sessions_->load_full(sid, &full).ok() && full.contains("meta")) {
        std::string c = jsonutil::get<std::string>(full["meta"], "cwd", "");
        if (!c.empty()) work = c;
      }
    }
  }
  if (!work.empty()) args["cwd"] = work;
  // Output encoding: the setting, unless the caller named one.
  if (!args.contains("encoding")) {
    Json enc = settings_->get_value(kKeyTerminalEncoding, Json(""));
    if (enc.is_string() && !enc.get<std::string>().empty()) {
      args["encoding"] = enc.get<std::string>();
    }
  }
  Json o;
  FsResult r = terminal_mgr_->spawn(args, &o);
  if (out) *out = o;
  if (!r.ok()) return r;
  // The transcript belongs to EVERY terminal, including the ones the model
  // starts: give it a log file and write the metadata now (see persistence).
  std::string tid = jsonutil::get<std::string>(o, "tid", "");
  if (!tid.empty() && !sid.empty()) {
    TermEntry* e = terminals_->find(tid);
    if (e) {
      try {
        e->log_path = sessions_->make_store(sid).terminal_log_path(sid, tid).string();
        e->log_attached = true;
      } catch (...) {
      }
      {
        std::lock_guard<std::mutex> lk(term_log_mu_);
        term_log_paths_[tid] = e->log_path;
      }
      persist_terminal_entry(e->to_json());
    }
  }
  // 立即把首个提示符推进事件流，用户不用等下一次 tick
  terminal_tick();
  return r;
}

FsResult FsApi::terminal_spawn(const std::string& sid, const std::string& shell,
                               const std::string& label, const std::string& cwd,
                               int cols, int rows) {
  Json args = Json::object();
  args["sid"] = sid;
  args["interactive"] = true;   // 交互式 shell：没有"要执行的命令"
  args["background"] = true;
  args["label"] = label;
  args["cols"] = cols;
  args["rows"] = rows;
  if (!cwd.empty()) args["cwd"] = cwd;
  if (!shell.empty()) {
    args["shell"] = shell;
  } else {
    // 没指定就用设置里的默认 shell（用户终端 = "设置里的那个 shell"）
    Json def = settings_->get_value(kKeyTerminalShell, Json(""));
    if (def.is_string() && !def.get<std::string>().empty()) {
      args["shell"] = def.get<std::string>();
    }
  }
  Json out;
  return spawn_terminal(args, &out);
}

FsResult FsApi::terminal_write(const std::string& tid, const std::string& text) {
  // What the USER typed is its own record: the output log only has it when the
  // program echoes (a password prompt or a full-screen app does not), and
  // "what did I type" is what a search looks for.
  append_terminal_input(tid, text);
  Json out;
  return terminal_mgr_->write(Json{{"tid", tid}, {"text", text}}, &out);
}

FsResult FsApi::terminal_resize(const std::string& tid, int cols, int rows) {
  Json out;
  return terminal_mgr_->resize(Json{{"tid", tid}, {"cols", cols}, {"rows", rows}},
                               &out);
}

FsResult FsApi::terminal_grid(const std::string& tid) {
  Json out;
  FsResult r = terminal_mgr_->grid(Json{{"tid", tid}}, &out);
  if (r.ok()) return r;
  // Not a live process: a terminal from a previous run (or the manager has
  // already reaped it). Rebuild the view from the saved transcript chain so
  // "打开过的终端" can still be read - and return an EMPTY grid rather than
  // "not found" when there is no record at all: a terminal tab that was just
  // restored is allowed to be empty, and the error only looked like a bug.
  TermEntry* e = terminals_->find(tid);
  std::string log_path = e ? e->log_path : std::string();
  if (log_path.empty()) {
    std::lock_guard<std::mutex> lk(term_log_mu_);
    std::map<std::string, std::string>::const_iterator it = term_log_paths_.find(tid);
    if (it != term_log_paths_.end()) log_path = it->second;
  }
  // Every archived file plus the current one, oldest first.
  std::string raw;
  if (!log_path.empty()) {
    std::vector<std::string> chain = terminal_log_chain(log_path);
    for (size_t i = 0; i < chain.size(); ++i) {
      raw += read_file_bytes(chain[i]);
      raw += "\n";
    }
  }
  if (raw.empty()) {
    Json empty = Json::object();
    empty["cols"] = 100;
    empty["rows"] = 20;
    empty["from"] = 0;
    empty["scrollback"] = 0;
    empty["totalLines"] = 0;
    empty["lines"] = Json::array();
    empty["cursorX"] = 0;
    empty["cursorY"] = 0;
    empty["cursorVisible"] = false;
    empty["alt"] = false;
    empty["appKeys"] = false;
    empty["bracketedPaste"] = false;
    empty["title"] = "";
    empty["tid"] = tid;
    empty["status"] = "exited";
    empty["exitCode"] = e ? e->exit_code : 0;
    empty["replay"] = "empty";
    return FsResult::success(empty);
  }
  std::string text = terminal_plain_text(raw);
  // Split into lines, keep the tail (a long-lived shell's log can be huge).
  std::vector<std::string> lines;
  size_t start2 = 0;
  while (start2 <= text.size()) {
    size_t nl = text.find('\n', start2);
    std::string line = text.substr(start2, nl == std::string::npos ? std::string::npos
                                                                   : nl - start2);
    lines.push_back(line);
    if (nl == std::string::npos) break;
    start2 = nl + 1;
  }
  const size_t kMax = 2000;
  size_t from = lines.size() > kMax ? lines.size() - kMax : 0;
  Json arr = Json::array();
  size_t width = 0;
  for (size_t i = from; i < lines.size(); ++i) {
    const std::string& ln = lines[i];
    size_t w = 0;
    for (size_t k = 0; k < ln.size(); ++k) {
      if ((static_cast<unsigned char>(ln[k]) & 0xC0) != 0x80) ++w;
    }
    if (w > width) width = w;
    Json runs = Json::array();
    if (!ln.empty()) {
      Json run = Json::object();
      run["x"] = 0;
      run["t"] = ln;
      runs.push_back(run);
    }
    arr.push_back(runs);
  }
  Json g = Json::object();
  g["cols"] = static_cast<int>(width > 100 ? width : 100);
  g["rows"] = 20;
  g["from"] = 0;
  g["scrollback"] = static_cast<int>(from);
  g["totalLines"] = static_cast<int>(lines.size());
  g["lines"] = arr;
  g["cursorX"] = 0;
  g["cursorY"] = 0;
  g["cursorVisible"] = false;
  g["alt"] = false;
  g["appKeys"] = false;
  g["bracketedPaste"] = false;
  g["title"] = "";
  g["tid"] = tid;
  g["status"] = "exited";
  g["exitCode"] = e ? e->exit_code : 0;
  g["replay"] = "log";
  return FsResult::success(g);
}

FsResult FsApi::terminal_rename(const std::string& tid, const std::string& label) {
  return terminals_->set_label(tid, label);
}

FsResult FsApi::terminal_kill(const std::string& tid) {
  Json out;
  FsResult r = terminal_mgr_->kill(Json{{"tid", tid}}, &out);
  if (!r.ok()) {
    // 注册表里存在但管理器没有（如 P1 建的历史条目）仍标记退出
    TermEntry* e = terminals_->find(tid);
    if (e) return terminals_->mark_exited(tid, -1, clock_->now_ms());
  }
  return r;
}
FsResult FsApi::terminal_set_encoding(const std::string& tid, const std::string& enc) {
  return terminals_->set_encoding(tid, enc);
}

// 非阻塞轮询：把新增输出/退出推成事件。桌面端由后台线程定时调用，
// 浏览器端由 UI 定时器经 ui.terminalTick 触发（同一实现，两种传输）。
void FsApi::terminal_tick() {
  std::vector<Json> changes = terminal_mgr_->tick();
  for (size_t i = 0; i < changes.size(); ++i) {
    const Json& c = changes[i];
    std::string sid = jsonutil::get<std::string>(c, "sid", "");
    std::string tid = jsonutil::get<std::string>(c, "tid", "");
    if (c.contains("output") && c["output"].is_string()) {
      // The transcript was already written by the manager's output sink, the
      // moment the bytes were drained - not here (tick may never run again).
      std::string text = c["output"].get<std::string>();
      emit_event("terminal.output", Json{{"sid", sid}, {"tid", tid}, {"text", text}});
    }
    if (jsonutil::get<bool>(c, "justExited", false)) {
      // Freeze the entry on disk with its exit status.
      TermEntry* e = terminals_->find(tid);
      if (e) persist_terminal_entry(e->to_json());
      int exit_code = static_cast<int>(jsonutil::get_int(c, "exitCode", 0));
      emit_event("terminal.exit", Json{{"sid", sid}, {"tid", tid},
                                       {"exitCode", exit_code}});
      // wakeOnExit used to be stored and never read, and AgentHost's
      // on_terminal_exit had no caller at all - a background command could
      // finish with nobody resuming the task. This is the only exit path that
      // knows both the entry flag and the agent host.
      if (e && e->wake_on_exit && agent_host_ && !sid.empty()) {
        agent_host_->on_terminal_exit(sid, tid, exit_code);
      }
    }
  }
}

void FsApi::start_terminal_poller() {
  if (poller_) return;
  poller_stop_.store(false);
  poller_.reset(new std::thread([this]() {
    while (!poller_stop_.load()) {
      terminal_tick();
      // Scheduled wake-ups have to be polled by someone, and this thread already
      // exists and ticks often enough for a "5 minutes later" timer. Without it
      // a wake stayed in the queue forever: nothing ever called on_timer_tick.
      if (agent_host_) agent_host_->on_timer_tick();
      // The desktop has no fast grid timer, so this thread is what keeps the
      // browser's event stream live; 60ms keeps the terminal feeling like a
      // keyboard without busy-waiting.
      std::this_thread::sleep_for(std::chrono::milliseconds(60));
    }
  }));
}

void FsApi::stop_terminal_poller() {
  poller_stop_.store(true);
  if (poller_ && poller_->joinable()) poller_->join();
  poller_.reset();
}

// ----- git.* -----
// The Git panel is a VIEW of the project directory, so every method resolves
// the pid to a path here rather than trusting a path from the UI. A project
// that is not a git repository is a normal state, not an error: the panel says
// "not a repository" and offers nothing to do.
namespace {
std::vector<std::string> json_paths(const Json& paths) {
  std::vector<std::string> out;
  if (!paths.is_array()) return out;
  for (size_t i = 0; i < paths.size(); ++i) {
    if (paths[i].is_string()) out.push_back(paths[i].get<std::string>());
  }
  return out;
}
}  // namespace

FsResult FsApi::git_status(const std::string& pid) {
  std::string path = path_for(pid);
  if (path.empty()) return FsResult::error(ErrCode::kNotFound, "project not found: " + pid);
  git::Status st = git::status(path);
  Json j = Json::object();
  j["ok"] = st.ok;
  j["isRepo"] = st.is_repo;
  j["root"] = st.root;
  j["branch"] = st.branch;
  j["upstream"] = st.upstream;
  j["ahead"] = st.ahead;
  j["behind"] = st.behind;
  j["detached"] = st.detached;
  j["clean"] = st.clean;
  j["operation"] = st.operation;
  j["error"] = st.error;
  Json arr = Json::array();
  for (size_t i = 0; i < st.changes.size(); ++i) {
    const git::FileChange& c = st.changes[i];
    Json f = Json::object();
    f["path"] = c.path;
    f["origPath"] = c.orig_path;
    f["index"] = std::string(1, c.index_status);
    f["work"] = std::string(1, c.work_status);
    f["staged"] = c.staged();
    f["untracked"] = c.untracked();
    f["conflicted"] = c.conflicted();
    arr.push_back(f);
  }
  j["changes"] = arr;
  return FsResult::success(j);
}

FsResult FsApi::git_diff(const std::string& pid, const std::string& path,
                         bool staged) {
  std::string root = path_for(pid);
  if (root.empty()) return FsResult::error(ErrCode::kNotFound, "project not found: " + pid);
  return FsResult::success(git::diff(root, path, staged));
}

FsResult FsApi::git_show_head(const std::string& pid, const std::string& path) {
  std::string root = path_for(pid);
  if (root.empty()) return FsResult::error(ErrCode::kNotFound, "project not found: " + pid);
  return FsResult::success(git::show_head(root, path));
}

FsResult FsApi::git_log(const std::string& pid, int maxCount) {
  std::string root = path_for(pid);
  if (root.empty()) return FsResult::error(ErrCode::kNotFound, "project not found: " + pid);
  return FsResult::success(git::log(root, maxCount));
}

FsResult FsApi::git_branches(const std::string& pid) {
  std::string root = path_for(pid);
  if (root.empty()) return FsResult::error(ErrCode::kNotFound, "project not found: " + pid);
  return FsResult::success(git::branches(root));
}

FsResult FsApi::git_stage(const std::string& pid, const Json& paths) {
  std::string root = path_for(pid);
  if (root.empty()) return FsResult::error(ErrCode::kNotFound, "project not found: " + pid);
  return FsResult::success(git::stage(root, json_paths(paths)));
}

FsResult FsApi::git_unstage(const std::string& pid, const Json& paths) {
  std::string root = path_for(pid);
  if (root.empty()) return FsResult::error(ErrCode::kNotFound, "project not found: " + pid);
  return FsResult::success(git::unstage(root, json_paths(paths)));
}

FsResult FsApi::git_stage_all(const std::string& pid) {
  std::string root = path_for(pid);
  if (root.empty()) return FsResult::error(ErrCode::kNotFound, "project not found: " + pid);
  return FsResult::success(git::stage_all(root));
}

FsResult FsApi::git_commit(const std::string& pid, const std::string& message,
                           bool amend) {
  std::string root = path_for(pid);
  if (root.empty()) return FsResult::error(ErrCode::kNotFound, "project not found: " + pid);
  return FsResult::success(git::commit(root, message, amend));
}

FsResult FsApi::git_discard(const std::string& pid, const Json& paths) {
  std::string root = path_for(pid);
  if (root.empty()) return FsResult::error(ErrCode::kNotFound, "project not found: " + pid);
  return FsResult::success(git::discard(root, json_paths(paths)));
}

// ----- command.* -----
// The composer's "命令" menu. The LIST lives here, not in the UI: a command is
// a name + a description + an action, and the action is either "ui:<something>"
// (the UI owns it, e.g. opening the plan panel) or a backend method to call.
// Adding a command therefore does not touch the frontend at all.
//
// Skills (kind == "skill") are appended to the same list, so "/<skill>" and the
// built-in commands are discovered in one place.
FsResult FsApi::command_list(const std::string& sid) {
  (void)sid;
  Json arr = Json::array();
  auto add = [&arr](const std::string& name, const std::string& title,
                    const std::string& desc, const std::string& action,
                    const std::string& group) {
    arr.push_back(Json{{"name", name}, {"title", title}, {"description", desc},
                       {"action", action}, {"group", group}});
  };
  // Built-in commands. `action` is the contract the UI executes.
  add("new", "新建会话", "在当前项目新建一个会话", "ui:new-session", "builtin");
  add("files", "文件面板", "打开项目文件树", "ui:panel:files", "builtin");
  add("git", "Git 面板", "打开 Git 状态与变更", "ui:panel:git", "builtin");
  add("suspend", "挂起", "挂起当前任务（可继续）", "session.suspend", "builtin");
  add("resume", "继续", "继续被挂起的任务", "session.resume", "builtin");
  add("usage", "用量面板", "上下文水位与本会话成本", "ui:panel:usage", "builtin");

  // Skills: the user-callable ones only (kind == "skill"; built-in MCP entries
  // are model-invoked and have no "/name" form).
  ThinkLang lang = parse_think_lang(agent_host_->thinking_lang());
  Json skills = agent_host_->skills().list_json(lang);
  if (skills.is_array()) {
    for (size_t i = 0; i < skills.size(); ++i) {
      if (jsonutil::get<std::string>(skills[i], "kind", "skill") != "skill") continue;
      if (!jsonutil::get<bool>(skills[i], "enabled", true)) continue;
      std::string name = jsonutil::get<std::string>(skills[i], "name", "");
      if (name.empty()) continue;
      add(name, jsonutil::get<std::string>(skills[i], "title", name),
          jsonutil::get<std::string>(skills[i], "description", ""),
          "skill:" + name, "skill");
    }
  }
  return FsResult::success(Json{{"commands", arr}});
}

// Execute a command by name. Skills are loaded (their body becomes context for
// the next turn); built-in actions are dispatched by name.
FsResult FsApi::command_run(const std::string& sid, const std::string& name) {
  if (name.empty()) return FsResult::error(ErrCode::kInvalidArg, "empty command");
  if (name == "suspend") return session_suspend(sid);
  if (name == "resume") return session_resume(sid);
  // Anything else that is not a UI-owned action must be a skill.
  FsResult r = skill_load(name);
  if (r.ok()) return r;
  return FsResult::error(ErrCode::kNotFound, "unknown command: " + name);
}

// ----- usage.* -----
FsResult FsApi::usage_query(const Json& filter) {
  UsageFilter f;
  f.provider = jsonutil::get<std::string>(filter, "provider", "");
  f.model = jsonutil::get<std::string>(filter, "model", "");
  f.sid = jsonutil::get<std::string>(filter, "sid", "");
  f.from = jsonutil::get_int(filter, "from", 0);
  f.to = jsonutil::get_int(filter, "to", 0);
  UsageSummary sum = usage_->query(f);
  Json out = Json::object();
  out["calls"] = sum.calls;
  out["badLines"] = sum.bad_lines;
  out["totalIn"] = sum.total_in;
  out["totalOut"] = sum.total_out;
  out["totalCache"] = sum.total_cache;
  out["totalCost"] = sum.total_cost;
  out["costKind"] = sum.cost_kind;
  Json pm = Json::object();
  for (auto& kv : sum.by_provider_model) {
    Json e = Json::object();
    e["provider"] = kv.second.provider;
    e["model"] = kv.second.model;
    e["in"] = kv.second.in;
    e["out"] = kv.second.out;
    e["cache"] = kv.second.cache;
    e["cost"] = kv.second.cost;
    e["calls"] = kv.second.calls;
    pm[kv.first] = e;
  }
  out["byProviderModel"] = pm;
  // Model-only rollup (providers merged). The stats page uses THIS: a model
  // served by two providers is one line, and the internal provider id is not
  // shown. Requests still carry the provider (see session.send/providerId).
  Json bm = Json::object();
  for (auto& kv : sum.by_model) {
    Json e = Json::object();
    e["model"] = kv.second.model;
    e["in"] = kv.second.in;
    e["out"] = kv.second.out;
    e["cache"] = kv.second.cache;
    e["cost"] = kv.second.cost;
    e["calls"] = kv.second.calls;
    bm[kv.first] = e;
  }
  out["byModel"] = bm;
  Json days = Json::object();
  for (auto& kv : sum.by_day) {
    Json e = Json::object();
    e["in"] = kv.second.in;
    e["out"] = kv.second.out;
    e["cache"] = kv.second.cache;
    e["cost"] = kv.second.cost;
    e["calls"] = kv.second.calls;
    days[std::to_string(kv.first)] = e;
  }
  out["byDay"] = days;
  // The LAST request's prompt tokens and the model's context window: together
  // they are the context water level. Both are FACTS from the API (usage block
  // and /models), never an estimate from the model name.
  if (sum.has_last) {
    out["lastIn"] = sum.last.in;
    out["lastOut"] = sum.last.out;
    out["lastModel"] = sum.last.model;
    out["lastProvider"] = sum.last.provider;
    out["lastCache"] = sum.last.cache;
    out["lastCost"] = sum.last.cost;
    out["lastCostKind"] = sum.last.cost_kind;
    out["lastTs"] = sum.last.ts;
    int64_t window = 0;
    if (!sum.last.provider.empty() && !sum.last.model.empty()) {
      ModelEntry me = model_table_->resolve(sum.last.provider, sum.last.model);
      window = me.caps.context_len;
    }
    out["contextWindow"] = window;
  }
  // Cache efficiency. `cacheHit` is -1 when no row ever reported a cache field:
  // showing "0%" would claim the provider bills every prompt token, which is a
  // different (and wrong) statement.
  out["cacheHit"] = sum.cache_reported ? sum.cache_hit_ratio : -1.0;
  out["cacheReported"] = sum.cache_reported;
  // Cost per provider kind: one session can span a money provider and a quota
  // one, so the UI needs to know which numbers it may add up.
  Json ck = Json::object();
  for (auto& kv : sum.cost_kind_by_provider) ck[kv.first] = kv.second;
  out["costKindByProvider"] = ck;
  // The recent tail: the panel lists the last calls (this turn's cost is the
  // sum of the rows after the last user message, which the UI knows about).
  Json recent = Json::array();
  for (auto& r : sum.recent) {
    Json e = Json::object();
    e["ts"] = r.ts;
    e["provider"] = r.provider;
    e["model"] = r.model;
    e["in"] = r.in;
    e["out"] = r.out;
    e["cache"] = r.cache;
    e["cost"] = r.cost;
    e["costKind"] = r.cost_kind;
    e["latency"] = r.latency_ms;
    e["toolCalls"] = r.tool_calls;
    if (!r.error.empty()) e["error"] = r.error;
    recent.push_back(e);
  }
  out["recent"] = recent;
  return FsResult::success(out);
}
FsResult FsApi::usage_export_csv(const Json& filter, const std::string& path) {
  UsageFilter f;
  f.provider = jsonutil::get<std::string>(filter, "provider", "");
  f.model = jsonutil::get<std::string>(filter, "model", "");
  f.sid = jsonutil::get<std::string>(filter, "sid", "");
  f.from = jsonutil::get_int(filter, "from", 0);
  f.to = jsonutil::get_int(filter, "to", 0);
  // No path from the UI (the browser cannot pick one): write into the data root
  // under a timestamped name and report it back. An empty path used to mean
  // "current working directory", which for a desktop app is wherever it happened
  // to be launched from - a file nobody would find.
  ghc::filesystem::path out(path);
  if (out.empty()) {
    int64_t now = clock_ ? clock_->now_ms() : WallClock().now_ms();
    out = root_ / "exports" /
          ("usage-" + std::to_string(now) + ".csv");
  }
  FsResult r = usage_->export_csv(f, out);
  if (!r.ok()) return r;
  return FsResult::success(Json{{"path", out.string()}});
}
FsResult FsApi::usage_record(const Json& row) { return usage_->record(row); }

// ----- settings.* -----
// 项目注册表里查 pid 的路径（未注册返回空串）
std::string FsApi::path_for(const std::string& pid) const {
  nlohmann::json p = proj_->registry().get(pid);
  if (p.is_object() && p.contains("path") && p["path"].is_string()) {
    return p["path"].get<std::string>();
  }
  return "";
}

// 确保某个 pid 的项目级 SettingsStore 已构建（无 pid 用空路径占位 = 只有全局）
SettingsStore& FsApi::store_for(const std::string& pid) {
  if (pid.empty()) return *settings_;
  if (project_settings_ && project_settings_pid_ == pid) return *project_settings_;
  project_settings_pid_ = pid;
  project_settings_.reset(new SettingsStore(root_, pathutil::project_dir(path_for(pid)), true));
  return *project_settings_;
}

FsResult FsApi::settings_get(const std::string& key, const std::string& pid) {
  SettingValue sv = store_for(pid).get(key);
  Json out = Json::object();
  out["value"] = sv.value;
  out["source"] = sv.source;
  out["globalSet"] = sv.global_set;
  out["projectSet"] = sv.project_set;
  return FsResult::success(out);
}
FsResult FsApi::settings_set(const std::string& key, const Json& value, const std::string& pid) {
  FsResult r = pid.empty() ? settings_->set_global(key, value)
                           : (path_for(pid).empty()
                                  ? FsResult::error(ErrCode::kNotFound, "project not found: " + pid)
                                  : store_for(pid).set_project(key, value));
  if (!r.ok()) return r;
  // 代理设置变化立即生效到网络客户端（无需重启）
  if (key == kKeyNetworkProxyHost || key == kKeyNetworkProxyPort) {
    apply_proxy_settings();
  }
  // Web 服务设置（监听地址/端口/密码）：立即重启内嵌服务器
  if (key == kKeyServerHost || key == kKeyServerPort || key == kKeyServerPassword) {
    apply_server_settings();
  }
  // 文件黑白名单变化立即生效
  if (key == kKeyPermFileBlacklist || key == kKeyPermFileWhitelist) {
    std::vector<std::string> b, w;
    Json bl = settings_->get_value(kKeyPermFileBlacklist, Json::array());
    Json wl = settings_->get_value(kKeyPermFileWhitelist, Json::array());
    for (size_t i = 0; bl.is_array() && i < bl.size(); ++i) {
      if (bl[i].is_string()) b.push_back(bl[i].get<std::string>());
    }
    for (size_t i = 0; wl.is_array() && i < wl.size(); ++i) {
      if (wl[i].is_string()) w.push_back(wl[i].get<std::string>());
    }
    deny_list_->set_blacklist(b);
    deny_list_->set_whitelist(w);
  }
  return r;
}
FsResult FsApi::settings_unset(const std::string& key, const std::string& pid) {  if (pid.empty()) return settings_->unset_global(key);
  if (path_for(pid).empty()) return FsResult::error(ErrCode::kNotFound, "project not found: " + pid);
  return store_for(pid).unset_project(key);
}
FsResult FsApi::settings_dump(const std::string& pid) {
  Json out = Json::object();
  out["global"] = settings_->dump_global();
  if (!pid.empty()) {
    std::string path = path_for(pid);
    if (!path.empty()) {
      SettingsStore& ps = store_for(pid);
      out["project"] = ps.dump_project();
      out["ignoreGlobal"] = ps.ignore_global();
    }
  }
  return FsResult::success(out);
}
FsResult FsApi::settings_detect_shells() {
  return FsResult::success(ShellDetector::detect_all_json());
}
FsResult FsApi::environment_detect_cli_tools() {
  return FsResult::success(
      Json{{"tools", ShellDetector::detect_cli_tools_json()}});
}
FsResult FsApi::settings_set_shell(const std::string& name) {
  return settings_->set_global(kKeyTerminalShell, name);
}
FsResult FsApi::settings_set_ignore_global(const std::string& pid, bool ignore) {
  if (path_for(pid).empty()) return FsResult::error(ErrCode::kNotFound, "project not found: " + pid);
  return store_for(pid).set_ignore_global(ignore);
}

// ----- plan.* -----
FsResult FsApi::plan_get(const std::string& sid) {
  return FsResult::success(plans_->get(sid));
}
FsResult FsApi::plan_update(const std::string& sid, const Json& planTree) {
  std::string content = jsonutil::get<std::string>(planTree, "content", "");
  std::string status = jsonutil::get<std::string>(planTree, "status", "");
  std::string turn_id = jsonutil::get<std::string>(planTree, "turnId", "");
  bool new_plan = jsonutil::get<bool>(planTree, "newPlan", false);
  return plans_->update(sid, content, status, turn_id, new_plan);
}

// addon2 长任务-计划：评审（approve/reject/revise + 评价建议）
// revise：把评价作为引导注入当前会话，模型据此微调计划。
FsResult FsApi::plan_review(const std::string& sid, const std::string& verdict,
                            const std::string& comment) {
  if (verdict != "approve" && verdict != "reject" && verdict != "revise") {
    return FsResult::error(ErrCode::kInvalidArg, "verdict must be approve|reject|revise");
  }
  int64_t now = clock_->now_ms();
  Json plan = plans_->review(sid, verdict, comment, now);
  emit_event("plan.reviewed", Json{{"sid", sid}, {"verdict", verdict},
                                   {"comment", comment}});
  if (verdict == "revise" && !comment.empty()) {
    // 评价作为引导注入（走会话引导队列；模型下轮读到并微调）
    agent_host_->steering_push(sid, "immediate", comment);
  }
  return FsResult::success(Json{{"plan", plan}, {"verdict", verdict}});
}

// ----- workflow.*（addon2 长任务-工作流）-----
FsResult FsApi::workflow_define(const Json& def) { return workflows_->define(def); }

FsResult FsApi::workflow_list() {
  return FsResult::success(Json{{"defs", workflows_->list_defs()}});
}

FsResult FsApi::workflow_runs(const std::string& sid) {
  Json arr = Json::array();
  std::vector<WorkflowRun*> runs = workflows_->runs_for_sid(sid);
  for (size_t i = 0; i < runs.size(); ++i) {
    if (!runs[i]) continue;
    Json j = runs[i]->to_json();
    j["graph"] = runs[i]->graph_json();
    arr.push_back(j);
  }
  return FsResult::success(Json{{"runs", arr}});
}

FsResult FsApi::workflow_start(const std::string& sid, const std::string& name) {
  const WorkflowDef* def = workflows_->find_def(name);
  if (!def) return FsResult::error(ErrCode::kNotFound, "workflow not found: " + name);
  WorkflowRun* run = workflows_->create_run(*def, sid);
  if (!run) return FsResult::error(ErrCode::kInternal, "create_run failed");
  emit_event("workflow_started", Json{{"runId", run->id}, {"sid", sid},
                                      {"name", name}});
  start_next_pending_step(run);
  return FsResult::success(run->to_json());
}

// Approve a proposed run: this is the ONLY place a model-planned workflow starts.
FsResult FsApi::workflow_approve(const std::string& runId) {
  WorkflowRun* run = workflows_->find_run(runId);
  if (!run) return FsResult::error(ErrCode::kNotFound, "run not found: " + runId);
  if (run->state != "proposed") {
    return FsResult::error(ErrCode::kInvalidArg,
                           "run is not waiting for approval: " + run->state);
  }
  run->state = "running";
  emit_event("workflow_started", Json{{"runId", run->id}, {"sid", run->sid},
                                      {"name", run->name}});
  start_next_pending_step(run);
  Json j = run->to_json();
  j["graph"] = run->graph_json();
  return FsResult::success(j);
}

FsResult FsApi::workflow_reject(const std::string& runId) {
  WorkflowRun* run = workflows_->find_run(runId);
  if (!run) return FsResult::error(ErrCode::kNotFound, "run not found: " + runId);
  if (run->state != "proposed") {
    return FsResult::error(ErrCode::kInvalidArg,
                           "run is not waiting for approval: " + run->state);
  }
  run->state = "rejected";
  emit_event("workflow_rejected", Json{{"runId", run->id}, {"sid", run->sid}});
  return FsResult::success(run->to_json());
}

// Propose a workflow from the MODEL's plan: steps are titles + tasks, each run by
// a subagent. The run waits for the user's approval (state "proposed").
FsResult FsApi::workflow_propose(const std::string& sid, const Json& def) {
  Json req = def.is_object() ? def : Json::object();
  req["op"] = "propose";
  Json out;
  // The op handler also needs the sid.
  req["sid"] = sid;
  // Re-use the same path the tool uses, through the agent host's deps? Simpler:
  // implement here (the deps lambda cannot call a member).
  Json steps = req.contains("steps") && req["steps"].is_array() ? req["steps"]
                                                                : Json::array();
  std::string name = jsonutil::get<std::string>(req, "name", "");
  std::string goal = jsonutil::get<std::string>(req, "goal", "");
  if (name.empty()) name = goal.empty() ? std::string("模型计划") : goal;
  if (name.size() > 60) name = name.substr(0, 60);
  Json steps_out = Json::array();
  int idx = 0;
  for (size_t i = 0; i < steps.size(); ++i) {
    const Json& sj = steps[i];
    if (!sj.is_object()) continue;
    std::string title = jsonutil::get<std::string>(sj, "title", "");
    if (title.empty()) title = jsonutil::get<std::string>(sj, "name", "");
    if (title.empty()) continue;
    std::string task = jsonutil::get<std::string>(sj, "task", title);
    std::string kind = jsonutil::get<std::string>(sj, "kind", "complete-step");
    steps_out.push_back(Json{{"id", "step_" + std::to_string(++idx)},
                             {"name", title},
                             {"agents", Json::array({Json{{"kind", kind},
                                                          {"task", task}}})}});
  }
  if (steps_out.empty()) {
    return FsResult::error(ErrCode::kInvalidArg, "workflow needs at least one step");
  }
  Json wfdef = Json{{"name", name}, {"description", goal}, {"steps", steps_out}};
  FsResult dr = workflows_->define(wfdef);
  if (!dr.ok()) return dr;
  const WorkflowDef* d = workflows_->find_def(name);
  if (!d) return FsResult::error(ErrCode::kInternal, "define failed");
  WorkflowRun* run = workflows_->create_run(*d, sid);
  if (!run) return FsResult::error(ErrCode::kInternal, "create_run failed");
  run->state = "proposed";
  emit_event("workflow_proposed", Json{{"runId", run->id}, {"sid", sid},
                                       {"name", name},
                                       {"steps", static_cast<int>(steps_out.size())}});
  return FsResult::success(run->to_json());
}

FsResult FsApi::workflow_get(const std::string& runId) {
  WorkflowRun* run = workflows_->find_run(runId);
  if (!run) return FsResult::error(ErrCode::kNotFound, "run not found: " + runId);
  Json j = run->to_json();
  j["graph"] = run->graph_json();  // 侧栏图/嵌套列表用
  return FsResult::success(j);
}

FsResult FsApi::workflow_retry_step(const std::string& runId, const std::string& stepId) {
  if (!workflows_->retry_step(runId, stepId)) {
    return FsResult::error(ErrCode::kInvalidArg, "step not failed or not found");
  }
  emit_event("workflow.step", Json{{"runId", runId}, {"stepId", stepId},
                                   {"state", "pending"}});
  WorkflowRun* run = workflows_->find_run(runId);
  if (run) start_next_pending_step(run);
  return FsResult::success(run ? run->to_json() : Json::object());
}

FsResult FsApi::workflow_skip_step(const std::string& runId, const std::string& stepId) {
  if (!workflows_->skip_step(runId, stepId)) {
    return FsResult::error(ErrCode::kNotFound, "step not found: " + stepId);
  }
  emit_event("workflow.step", Json{{"runId", runId}, {"stepId", stepId},
                                   {"state", "skipped"}});
  WorkflowRun* run = workflows_->find_run(runId);
  if (run) start_next_pending_step(run);
  return FsResult::success(run ? run->to_json() : Json::object());
}

// 用户定向引导到某步的子代理（addon2：工作流也允许用户引导）
FsResult FsApi::workflow_steer(const std::string& runId, const std::string& stepId,
                               const std::string& text) {
  if (!workflows_->steer_step(runId, stepId, text)) {
    return FsResult::error(ErrCode::kNotFound, "step not found: " + stepId);
  }
  // 转发给该步正在运行的子代理（子代理只需当前引导）
  WorkflowRun* run = workflows_->find_run(runId);
  if (run) {
    for (auto& s : run->steps) {
      if (s.id != stepId) continue;
      for (auto& a : s.agents) {
        if (!a.subagentId.empty()) agent_host_->subagent_steer(a.subagentId, text);
      }
    }
  }
  return FsResult::success();
}

// 启动某步：spawn 其 agents 子代理（parallel 决定并行/顺序语义，均由 SubagentManager
// 的并行上限与 spawn 顺序体现）
FsResult FsApi::start_step(WorkflowRun* run, const std::string& stepId) {
  if (!run) return FsResult::error(ErrCode::kInternal, "null run");
  int64_t now = clock_->now_ms();
  workflows_->mark_step_running(run->id, stepId, now);
  WorkflowStep* step = nullptr;
  for (auto& s : run->steps) {
    if (s.id == stepId) { step = &s; break; }
  }
  if (!step) return FsResult::error(ErrCode::kNotFound, "step not found");
  emit_event("workflow.step", Json{{"runId", run->id}, {"stepId", stepId},
                                   {"state", "running"}});
  if (step->agents.empty()) {
    // 无子代理的步：直接完成
    workflows_->mark_step_done(run->id, stepId, now);
    return FsResult::success();
  }
  bool spawned_any = false;
  for (auto& a : step->agents) {
    std::string sub_id;
    // cwd 用会话项目根（v1：空=宿主默认）
    FsResult r = agent_host_->subagent_spawn(run->sid, a.kind, a.task, a.model,
                                             1, 0, &sub_id);
    if (r.ok()) {
      a.subagentId = sub_id;
      spawned_any = true;
    } else {
      // 单个 agent 失败不整体中断：记录错误，步标记 failed（可重试/跳过）
      workflows_->mark_step_failed(run->id, stepId, r.message, now);
      return r;
    }
  }
  (void)spawned_any;
  return FsResult::success();
}

// 子代理完成回调：推进其所属工作流步（全部 agent 完成后步 -> done）
void FsApi::workflow_on_subagent_done(const std::string& subagentId) {
  WorkflowRun* run = workflows_->find_run_by_subagent(subagentId);
  if (!run) return;
  int64_t now = clock_->now_ms();
  FsResult lr = agent_host_->subagent_list(run->sid);
  Json subs = lr.ok() ? lr.data : Json::array();
  for (auto& s : run->steps) {
    if (s.state != StepState::kRunning) continue;
    bool owns = false;
    for (auto& a : s.agents) {
      if (a.subagentId == subagentId) owns = true;
    }
    if (!owns) continue;
    // 该步全部 agent 的 status 都是终态（done/cancelled/timeout）→ 步完成
    bool all_settled = true;
    bool any_failed = false;
    for (auto& a : s.agents) {
      bool found = false;
      for (auto& rj : subs) {
        if (jsonutil::get<std::string>(rj, "id", "") != a.subagentId) continue;
        found = true;
        std::string st = jsonutil::get<std::string>(rj, "status", "");
        if (st == "running") all_settled = false;
        if (st == "cancelled" || st == "timeout") any_failed = true;
        break;
      }
      if (!found) all_settled = false;
    }
    if (!all_settled) return;
    if (any_failed) {
      workflows_->mark_step_failed(run->id, s.id, "subagent failed", now);
      emit_event("workflow.step", Json{{"runId", run->id}, {"stepId", s.id},
                                       {"state", "failed"}});
      // 失败不整体中断：等待用户重试/跳过
      return;
    }
    workflows_->mark_step_done(run->id, s.id, now);
    emit_event("workflow.step", Json{{"runId", run->id}, {"stepId", s.id},
                                     {"state", "done"}});
    start_next_pending_step(run);
    return;
  }
}

// 启动下一个 pending 步骤（顺序流水线：前一步 settled 才继续）
void FsApi::start_next_pending_step(WorkflowRun* run) {
  if (!run) return;
  // 找第一个 pending；若其前面的步都 settled（done/skipped），启动它
  for (auto& s : run->steps) {
    if (s.state == StepState::kPending) {
      start_step(run, s.id);
      return;
    }
    if (s.state == StepState::kRunning || s.state == StepState::kFailed) {
      return;  // 未 settled：等待
    }
  }
}

// ----- lock.*（供 P3 工具层权限门；P1 单测直接调）-----
FsResult FsApi::lock_acquire(const std::string& sid) {
  Json full;
  FsResult r = sessions_->load_full(sid, &full);
  if (!r.ok()) return r;
  std::string perm = jsonutil::get<std::string>(full["sessionSettings"], "filePerm", "permissive");
  std::string pid = jsonutil::get<std::string>(full["meta"], "pid", "");
  bool full_perm = perm == "full";
  std::string locked_by;
  bool ok = locks_->try_acquire(pid, sid, full_perm, clock_->now_ms(), &locked_by);
  if (!ok) {
    return FsResult::deny("lock", "another session holds the project write lock", locked_by);
  }
  return FsResult::success();
}
FsResult FsApi::lock_release(const std::string& sid) {
  locks_->release_all_for(sid);
  return FsResult::success();
}
FsResult FsApi::lock_holder(const std::string& pid) {
  return FsResult::success(Json{{"holder", locks_->holder(pid)}});
}

// ----- provider.*（P2 真实现）-----
FsResult FsApi::provider_list_providers() {
  Json arr = Json::array();
  for (auto& c : providers_->list()) {
    Json j = c.to_json();
    j["apiKey"] = mask_secret(c.api_key);  // 脱敏回传，避免密钥泄漏到 UI
    j["hasKey"] = !c.api_key.empty();
    arr.push_back(j);
  }
  return FsResult::success(arr);
}

FsResult FsApi::provider_add_provider(const Json& conn) {
  ProviderConn c = ProviderConn::from_json(conn);
  if (c.base_url.empty()) {
    return FsResult::error(ErrCode::kInvalidArg, "baseUrl required");
  }
  // 编辑已存在的连接时，UI 回传的是脱敏 key（"sk-1****abcd"）或空串：保留原 key，
  // 否则保存会把真密钥覆盖成掩码串。
  std::string incoming = jsonutil::get<std::string>(conn, "apiKey", "");
  if (!c.id.empty() && (incoming.empty() || incoming.find("****") != std::string::npos)) {
    ProviderConn old = providers_->get(c.id);
    if (!old.id.empty()) c.api_key = old.api_key;
  }
  std::string id = providers_->upsert(c);
  std::string err;
  if (!providers_->save(&err)) {
    return FsResult::error(ErrCode::kIo, err);
  }
  return FsResult::success(Json{{"id", id}});
}

FsResult FsApi::provider_get_provider(const std::string& providerId) {
  ProviderConn c = providers_->get(providerId);
  if (c.id.empty()) {
    return FsResult::error(ErrCode::kNotFound, "provider not found: " + providerId);
  }
  Json j = c.to_json();
  j["apiKey"] = mask_secret(c.api_key);  // 脱敏；编辑时留空/不改即保留原值
  j["hasKey"] = !c.api_key.empty();
  return FsResult::success(j);
}

FsResult FsApi::provider_templates() {
  // Endpoint presets. A template only pre-fills endpoint + extra headers: the
  // request SHAPE is chosen separately by 格式 (openai-chat / openai-response /
  // anthropic / custom). "套餐" is not a shape - it is just a provider that
  // happens to send x-opencode-session, so it is modelled as a header preset.
  Json arr = Json::array();
  auto add = [&arr](const char* id, const char* name, const char* base,
                    const char* desc, const Json& headers) {
    Json t = Json::object();
    t["id"] = id;
    t["name"] = name;
    t["baseUrl"] = base;
    t["description"] = desc;
    t["headers"] = headers;
    arr.push_back(t);
  };
  // 套餐/聚合服务：endpoint 不同 + 模板预填一条会话亲和头（{{SESSION}} 由后端
  // 替换成会话 id）。它和其它自定义头走同一条路径，没有单独的隐藏开关。
  add("opencode-zen", "opencode zen", "https://opencode.ai/zen/v1",
      "套餐服务：预填 x-opencode-session 会话亲和头，按额度计费。",
      Json{{"x-opencode-client", "flashsight"},
           {"x-opencode-session", "{{SESSION}}"}});
  add("opencode-go", "opencode go", "https://opencode.ai/go/v1",
      "opencode go 端点：预填 x-opencode-session。",
      Json{{"x-opencode-client", "flashsight"},
           {"x-opencode-session", "{{SESSION}}"}});
  // 官方直连
  add("openai", "OpenAI", "https://api.openai.com/v1",
      "OpenAI 官方端点（Chat 或 Response 形态）。", Json::object());
  add("anthropic", "Anthropic", "https://api.anthropic.com/v1",
      "Anthropic 官方端点：/messages + x-api-key。", Json::object());
  add("gemini", "Google Gemini", "https://generativelanguage.googleapis.com/v1beta/openai",
      "Gemini 的 OpenAI 兼容端点。", Json::object());
  add("deepseek", "DeepSeek", "https://api.deepseek.com/v1",
      "DeepSeek 官方端点（OpenAI 兼容）。", Json::object());
  add("moonshot", "Moonshot / Kimi", "https://api.moonshot.cn/v1",
      "月之暗面 OpenAI 兼容端点。", Json::object());
  add("zhipu", "智谱 GLM", "https://open.bigmodel.cn/api/paas/v4",
      "智谱 AI 的 OpenAI 兼容端点。", Json::object());
  add("dashscope", "阿里通义千问", "https://dashscope.aliyuncs.com/compatible-mode/v1",
      "DashScope 的 OpenAI 兼容端点。", Json::object());
  add("siliconflow", "SiliconFlow", "https://api.siliconflow.cn/v1",
      "硅基流动聚合端点（OpenAI 兼容）。", Json::object());
  add("openrouter", "OpenRouter", "https://openrouter.ai/api/v1",
      "多模型聚合网关（OpenAI 兼容）。", Json::object());
  // More mainstream endpoints. They are here because the BALANCE probe identifies
  // a vendor by its endpoint - a preset that types the right URL is what makes
  // "余额 / 额度" work without the user knowing the account API exists.
  add("groq", "Groq", "https://api.groq.com/openai/v1",
      "Groq 官方端点（OpenAI 兼容，低延迟推理）。", Json::object());
  add("mistral", "Mistral", "https://api.mistral.ai/v1",
      "Mistral 官方端点（OpenAI 兼容）。", Json::object());
  add("xai", "xAI Grok", "https://api.x.ai/v1",
      "xAI 官方端点（OpenAI 兼容）。", Json::object());
  add("together", "Together AI", "https://api.together.xyz/v1",
      "Together 聚合端点（OpenAI 兼容）。", Json::object());
  add("fireworks", "Fireworks AI", "https://api.fireworks.ai/inference/v1",
      "Fireworks 推理端点（OpenAI 兼容）。", Json::object());
  add("deepinfra", "DeepInfra", "https://api.deepinfra.com/v1/openai",
      "DeepInfra 推理端点（OpenAI 兼容）。", Json::object());
  add("cerebras", "Cerebras", "https://api.cerebras.ai/v1",
      "Cerebras 推理端点（OpenAI 兼容）。", Json::object());
  add("minimax", "MiniMax", "https://api.minimax.chat/v1",
      "MiniMax 官方端点（OpenAI 兼容）。", Json::object());
  add("stepfun", "阶跃星辰 StepFun", "https://api.stepfun.com/v1",
      "阶跃星辰官方端点（OpenAI 兼容）。", Json::object());
  add("volcengine", "火山方舟", "https://ark.cn-beijing.volces.com/api/v3",
      "火山引擎方舟端点（OpenAI 兼容；模型名用接入点 id）。", Json::object());
  add("ollama", "Ollama（本地）", "http://127.0.0.1:11434/v1",
      "本地推理，无需密钥。", Json::object());
  add("lmstudio", "LM Studio（本地）", "http://127.0.0.1:1234/v1",
      "本地推理，无需密钥。", Json::object());
  add("vllm", "vLLM（自建）", "http://127.0.0.1:8000/v1",
      "自建推理服务（OpenAI 兼容）。", Json::object());
  add("azure", "Azure OpenAI", "https://YOUR-RESOURCE.openai.azure.com/openai/v1",
      "需要 api-key 头（在高级里添加）。",
      Json{{"api-key", "{{YOUR_KEY}}"}});
  add("custom", "自定义端点", "https://",
      "任意兼容端点；请求形态在格式里选。", Json::object());
  return FsResult::success(arr);
}

FsResult FsApi::provider_remove_provider(const std::string& providerId) {
  if (!providers_->remove(providerId)) {
    return FsResult::error(ErrCode::kNotFound, "provider not found: " + providerId);
  }
  std::string err;
  if (!providers_->save(&err)) {
    return FsResult::error(ErrCode::kIo, err);
  }
  return FsResult::success();
}

FsResult FsApi::provider_list_models(const std::string& providerId) {
  ProviderConn conn = providers_->get(providerId);
  if (conn.id.empty()) {
    return FsResult::error(ErrCode::kNotFound, "provider not found: " + providerId);
  }
  // 一级：API 一键获取（失败回退预置/保守缺省）
  auto res = model_table_->fetch_models(providerId, conn, *net_);
  std::vector<ModelEntry> models;
  std::string level;
  std::string message;
  if (res.ok) {
    models = res.models;
    level = "api";
    // Store what the endpoint actually returned. Without this the fetched list
    // lived only in this reply: model.list (which the model panel reads) fell
    // back to the built-in preset every time, so "获取模型列表" looked like it
    // kept returning the same few fake models.
    model_table_->set_api_models(providerId, models);
    // Persist it: the context window and output cap reported here are what the
    // context water level and the token controls are computed from, and they are
    // not recoverable from anywhere else.
    settings_->set_global(kKeyProviderApiModels, model_table_->api_models_json());
  } else {
    level = "local";
    message = res.message;
    // 获取失败时返回本地已知的模型（用户自定义 + 上次成功获取的），而不是
    // 塞一整张预置表进去：那些条目既没有真的从这台供应商拿到，也调不通。
    models = model_table_->list_models(providerId);
  }
  Json arr = Json::array();
  for (auto& m : models) arr.push_back(m.to_json());
  return FsResult::success(
      Json{{"models", arr}, {"level", level}, {"message", message}});
}

FsResult FsApi::provider_test_connection(const std::string& providerId) {
  ProviderConn conn = providers_->get(providerId);
  if (conn.id.empty()) {
    return FsResult::error(ErrCode::kNotFound, "provider not found: " + providerId);
  }
  ProviderClient client(conn, net_.get());
  ChatResponse out;
  FsResult r = client.test_connection(&out);
  if (!r.ok() || !out.error.empty()) {
    return FsResult::error(r.ok() ? ErrCode::kRemote : r.code,
                           out.error.empty() ? r.message : out.error);
  }
  return FsResult::success(Json{
      {"ok", true},
      {"latencyMs", out.latency_ms},
      {"status", out.http_status},
  });
}

// ---------------------------------------------------------------------------
// 余额 / 额度
//
// The per-vendor probing lives in provider/balance_probe.cpp (unit-testable with a
// stub NetClient): every vendor that offers one uses a different private endpoint,
// the vendor is identified by the ENDPOINT, and a failure is reported with its
// reason instead of a made-up 0.
//
// This wrapper turns the enabled providers into the rows the UI renders.
// ---------------------------------------------------------------------------
FsResult FsApi::provider_balances() {
  Json rows = Json::array();
  if (!providers_ || !net_) return FsResult::success(Json{{"providers", rows}});
  std::vector<ProviderConn> all;
  {
    // Snapshot under the backend lock, then probe WITHOUT it: the UI route is
    // excluded from the global lock precisely so a slow provider cannot stall
    // session loads. Providers can be edited while probes run; the copy keeps
    // this function from reading a half-updated store.
    std::lock_guard<std::recursive_mutex> lk(mu_);
    all = providers_->list();
  }
  // IN PARALLEL. One unreachable endpoint used to make the whole request take
  // N x 8 s while the UI waited for its rows ("打开时迟迟不更新"); now the total
  // is the SLOWEST single probe. Each request builds its own client, so probing
  // from several threads is safe.
  std::vector<BalanceProbe> probes(all.size());
  std::vector<std::thread> threads;
  try {
    for (size_t i = 0; i < all.size(); ++i) {
      const ProviderConn* c = &all[i];
      BalanceProbe* out = &probes[i];
      NetClient* net = net_.get();
      threads.push_back(std::thread([c, out, net]() { *out = probe_balance(*c, net); }));
    }
  } catch (...) {
  }
  for (size_t i = 0; i < threads.size(); ++i) {
    if (threads[i].joinable()) threads[i].join();
  }
  for (size_t i = 0; i < all.size(); ++i) {
    const ProviderConn& c = all[i];
    const BalanceProbe& b = probes[i];
    if (b.status.empty() && threads.size() <= i) {
      // A probe that never ran (thread creation failed): report it as failed
      // rather than as an empty success.
      Json row = Json{{"pid", c.id},
                      {"name", c.name.empty() ? c.id : c.name},
                      {"endpoint", c.base_url},
                      {"enabled", c.enabled},
                      {"status", "failed"},
                      {"note", "获取失败：无法启动查询"}};
      rows.push_back(row);
      continue;
    }
    Json row = Json{{"pid", c.id},
                    {"name", c.name.empty() ? c.id : c.name},
                    {"endpoint", c.base_url},
                    {"enabled", c.enabled},
                    {"status", b.status.empty() ? "failed" : b.status}};
    if (!b.currency.empty()) row["currency"] = b.currency;
    if (!b.balance.empty()) row["balance"] = b.balance;
    if (!b.used.empty()) row["used"] = b.used;
    if (!b.limit.empty()) row["limit"] = b.limit;
    if (!b.note.empty()) row["note"] = b.note;
    rows.push_back(row);
  }
  return FsResult::success(Json{{"providers", rows}});
}

// ----- model.*（逐模型设置）-----
// 模型设置存在 settings 的 provider.models 键（用户改过的模型才在里面），
// 与 provider 连接分离：连接是"怎么连"，模型是"这个模型能干什么"。
namespace {

Json load_model_settings(SettingsStore* settings) {
  if (!settings) return Json::object();
  Json v = settings->get_value(kKeyProviderModels, Json::object());
  return v.is_object() ? v : Json::object();
}

// 写回时必须整体替换：模型删除是"从数组里消失"，逐键合并无法表达删除。
FsResult save_model_settings(SettingsStore* settings, const Json& all) {
  return settings->set_global(kKeyProviderModels, all);
}

}  // namespace

FsResult FsApi::model_list(const std::string& providerId) {
  ProviderConn conn = providers_->get(providerId);
  if (conn.id.empty()) {
    return FsResult::error(ErrCode::kNotFound, "provider not found: " + providerId);
  }
  std::vector<ModelEntry> models = model_table_->list_models(providerId);
  Json arr = Json::array();
  for (auto& m : models) arr.push_back(m.to_json());
  return FsResult::success(Json{{"providerId", providerId}, {"models", arr}});
}

FsResult FsApi::model_save(const std::string& providerId, const Json& model) {
  ProviderConn conn = providers_->get(providerId);
  if (conn.id.empty()) {
    return FsResult::error(ErrCode::kNotFound, "provider not found: " + providerId);
  }
  ModelEntry m = ModelEntry::from_json(model);
  if (m.id.empty()) {
    return FsResult::error(ErrCode::kInvalidArg, "model id required");
  }
  if (m.display.empty()) m.display = m.id;
  m.user_set = true;
  // Context length: optional. The UI prefers an API-reported value and falls
  // back to this default; direct API callers get the same fallback instead of a
  // stored 0 (which made the context water level uncomputable).
  // fill_model_defaults() below picks the fallback window (1M by default, small
  // only for explicit low-tier names); no fixed 128k here.
  fill_model_defaults(&m);
  // 用户显式保存的档位表以用户为准：只有用户没给档位时才由粗匹配补
  if (m.thinking.levels.empty()) {
    m.thinking.levels = thinking_levels_for(m.id);
    m.thinking.default_level = default_thinking_for(m.id);
  }
  if (!m.thinking.default_level.empty()) {
    bool ok = false;
    for (size_t i = 0; i < m.thinking.levels.size(); ++i) {
      if (m.thinking.levels[i] == m.thinking.default_level) ok = true;
    }
    if (!ok) m.thinking.default_level = m.thinking.levels[0];
  }
  model_table_->set_user_model(providerId, m);
  Json all = load_model_settings(settings_.get());
  all[providerId] = model_table_->user_models_json()[providerId];
  FsResult w = save_model_settings(settings_.get(), all);
  if (!w.ok()) return w;
  return FsResult::success(m.to_json());
}

FsResult FsApi::model_remove(const std::string& providerId, const std::string& modelId) {
  if (!model_table_->remove_user_model(providerId, modelId)) {
    // 不是用户自定义：预置/API 模型删不掉，只能停用
    return FsResult::error(ErrCode::kInvalidArg,
                           "model is not user-defined; disable it instead");
  }
  Json all = load_model_settings(settings_.get());
  Json um = model_table_->user_models_json();
  if (um.contains(providerId)) all[providerId] = um[providerId];
  else all.erase(providerId);
  FsResult w = save_model_settings(settings_.get(), all);
  if (!w.ok()) return w;
  return FsResult::success(Json{{"removed", modelId}});
}

FsResult FsApi::model_set_enabled(const std::string& providerId,
                                  const std::string& modelId, bool enabled) {
  ProviderConn conn = providers_->get(providerId);
  if (conn.id.empty()) {
    return FsResult::error(ErrCode::kNotFound, "provider not found: " + providerId);
  }
  // 预置/API 模型没有用户条目：启用状态要落成一条用户覆盖，否则存不下来。
  ModelEntry m = model_table_->resolve(providerId, modelId);
  if (m.id.empty()) {
    return FsResult::error(ErrCode::kNotFound, "model not found: " + modelId);
  }
  m.enabled = enabled;
  m.user_set = true;
  model_table_->set_user_model(providerId, m);
  Json all = load_model_settings(settings_.get());
  all[providerId] = model_table_->user_models_json()[providerId];
  FsResult w = save_model_settings(settings_.get(), all);
  if (!w.ok()) return w;
  return FsResult::success(m.to_json());
}

FsResult FsApi::model_test(const std::string& providerId, const std::string& modelId) {
  ProviderConn conn = providers_->get(providerId);
  if (conn.id.empty()) {
    return FsResult::error(ErrCode::kNotFound, "provider not found: " + providerId);
  }
  ProviderClient client(conn, net_.get());
  ChatRequest req;
  req.model = modelId;
  Json msg = Json::object();
  msg["role"] = "user";
  msg["content"] = "ping";
  req.messages.push_back(msg);
  req.max_tokens = 1;
  req.stream = false;
  ChatResponse out;
  FsResult r = client.chat(req, &out);
  if (!r.ok() || !out.error.empty()) {
    return FsResult::error(r.ok() ? ErrCode::kRemote : r.code,
                           out.error.empty() ? r.message : out.error);
  }
  return FsResult::success(Json{
      {"ok", true},
      {"model", modelId},
      {"latencyMs", out.latency_ms},
      {"status", out.http_status},
  });
}

FsResult FsApi::model_all_enabled() {
  // 会话的模型选择器要的是"所有能用的模型"，跨 provider。归属必须带上：
  // 同名模型在不同 provider 上是不同的东西（价格、能力都可能不同）。
  Json arr = Json::array();
  for (auto& c : providers_->list()) {
    if (!c.enabled) continue;
    std::vector<ModelEntry> models = model_table_->list_models(c.id);
    for (size_t i = 0; i < models.size(); ++i) {
      if (!models[i].enabled) continue;
      // Fill derived defaults (thinking ladder from the model name) on a COPY:
      // the composer builds its thinking control from this payload, so a model
      // that has a default level but was never explicitly saved must still get
      // the full ladder here - otherwise it showed only 开/关 until the user
      // opened the model settings and pressed save.
      ModelEntry m = models[i];
      fill_model_defaults(&m);
      Json j = m.to_json();
      j["providerId"] = c.id;
      j["providerName"] = c.name;
      arr.push_back(j);
    }
  }
  return FsResult::success(Json{{"models", arr}});
}

}  // namespace fs