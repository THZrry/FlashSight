// fs_api.cpp - FsApi unified entry implementation (P1 scope)
#include "fs_api.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "core/folder_picker.hpp"
#include "fs_path.hpp"
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
  deny_list_.reset(new DenyList());
  plans_.reset(new PlanStore());
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
  deps.call_model = [ps, mt, net, us, clk](const ModelCallRequest& req,
                                           ModelCallResult* out) -> FsResult {
    if (!out) return FsResult::error(ErrCode::kInvalidArg, "null result");
    // 按模型名找 provider：v1 用第一个启用的连接
    std::vector<ProviderConn> conns = ps->list();
    ProviderConn conn;
    bool found = false;
    for (auto& c : conns) {
      if (c.enabled) {
        conn = c;
        found = true;
        break;
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
    creq.messages = req.messages;
    creq.tools = req.tools;
    creq.stream = false;
    creq.session_id = req.session_id;  // kPlan: x-opencode-session 亲和 id
    ProviderRuntime runtime(conn, net, us, clk);
    ChatResponse cresp;
    FsResult r = runtime.call(creq, &cresp, nullptr);
    if (!r.ok()) {
      out->error = cresp.error.empty() ? r.message : cresp.error;
      return r;
    }
    out->text = cresp.text;
    out->tool_calls = cresp.tool_calls;
    out->error = "";
    return FsResult::success();
  };
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
  deps.terminal_spawn = [tm](const Json& a, Json* out) -> FsResult {
    return tm->spawn(a, out);
  };
  deps.terminal_poll = [tm](const Json& a, Json* out) -> FsResult {
    return tm->poll(a, out);
  };
  deps.terminal_kill = [tm](const Json& a, Json* out) -> FsResult {
    return tm->kill(a, out);
  };
  // 网络回调（network.fetch / network.search 经 NetClient；宿主负责证书/超时/大小）
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
  agent_host_.reset(new AgentHost(std::move(deps)));
  agent_host_->register_builtin_tools(mcp_registry_.get());
  // addon2：思考语言设置（工具 skill 中英双语）注入宿主
  Json tl = settings_->get_value(kKeyThinkingLang, Json("auto"));
  agent_host_->set_thinking_lang(tl.is_string() ? tl.get<std::string>() : "auto");
  // MCP/外部工具注册表：从设置恢复（重启后保留）
  Json mcp = settings_->get_value(kKeyMcpEntries, Json::array());
  if (mcp.is_array()) mcp_registry_->load(mcp);
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
}

void FsApi::apply_proxy_settings() {
  std::string host = settings_->get_value(kKeyNetworkProxyHost, Json("")).is_string()
                         ? settings_->get_value(kKeyNetworkProxyHost, Json("")).get<std::string>()
                         : "";
  Json pv = settings_->get_value(kKeyNetworkProxyPort, Json(0));
  int port = pv.is_number() ? static_cast<int>(pv.get<int64_t>()) : 0;
  if (net_) net_->set_default_proxy(host, port);
}

// 事件扇出：给所有监听者（本地 UI + HTTP API 服务）；无监听者则丢弃（后端不依赖 UI）
void FsApi::emit_event(const std::string& event, const Json& payload) {
  for (std::map<int, std::function<void(const std::string&, const Json&)> >::iterator it =
           event_listeners_.begin();
       it != event_listeners_.end(); ++it) {
    if (it->second) it->second(event, payload);
  }
}

FsApi::~FsApi() {}

// ----- project.* -----
FsResult FsApi::project_list() { return proj_->list_projects(); }
FsResult FsApi::project_open(const std::string& path) { return proj_->open_project(path); }
FsResult FsApi::project_create(const std::string& path) {
  return proj_->create_project(path);
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
  for (auto& e : ghc::filesystem::directory_iterator(dir, ec)) {
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
  return FsResult::success(Json{{"entries", arr}, {"path", rel}});
}

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
  if (!ec && sz > 512 * 1024) {
    return FsResult::error(ErrCode::kInvalidArg, "file too large to preview (>512KB)");
  }
  std::ifstream ifs(file.string(), std::ios::binary);
  if (!ifs) return FsResult::error(ErrCode::kIo, "cannot open file: " + rel);
  std::stringstream ss;
  ss << ifs.rdbuf();
  return FsResult::success(Json{{"content", ss.str()}});
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
FsResult FsApi::session_forget(const std::string& sid, const std::string& scope) {
  return sessions_->forget(sid, scope);
}
FsResult FsApi::session_list(const std::string& pid) { return sessions_->list_sessions(pid); }
FsResult FsApi::session_set_model(const std::string& sid, const std::string& model) {
  FsResult r = sessions_->set_model(sid, model);
  if (r.ok()) sessions_->append_system_op(sid, "setModel", Json{{"model", model}});
  return r;
}
FsResult FsApi::session_set_thinking(const std::string& sid, const std::string& thinking) {
  return sessions_->set_thinking(sid, thinking);
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
  return sessions_->set_auto_mode(sid, autoMode);
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
FsResult FsApi::session_get(const std::string& sid) { return sessions_->get_session(sid); }

// ----- P3: send/append/suspend/resume/cancelWake -----
FsResult FsApi::session_send(const std::string& sid, const std::string& text,
                             const Json& attachments) {
  Json full;
  FsResult r = sessions_->load_full(sid, &full);
  if (!r.ok()) return r;
  Json ss = full.value("sessionSettings", Json::object());
  bool auto_mode = jsonutil::get<bool>(ss, "autoMode", false);
  std::string perm = jsonutil::get<std::string>(ss, "filePerm", "permissive");
  return agent_host_->send(sid, text, attachments, auto_mode, perm);
}

FsResult FsApi::session_append(const std::string& sid, const std::string& text) {
  return agent_host_->append(sid, text);
}

FsResult FsApi::session_suspend(const std::string& sid) {
  return agent_host_->suspend(sid);
}

FsResult FsApi::session_resume(const std::string& sid) {
  return agent_host_->resume(sid);
}

FsResult FsApi::session_cancel_wake(const std::string& sid) {
  return agent_host_->cancel_wake(sid);
}

// addon2 任务健壮性：打断后续接
FsResult FsApi::session_continue(const std::string& sid, const std::string& guidance) {
  return agent_host_->continue_task(sid, guidance);
}

// approve 档权限门
FsResult FsApi::session_approve(const std::string& sid, bool yes) {
  return agent_host_->approve(sid, yes);
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

// ----- skill.*（addon2：中英双语 skill，按需加载）-----
FsResult FsApi::skill_list() {
  return FsResult::success(
      agent_host_->skills().list_json(parse_think_lang(agent_host_->thinking_lang())));
}

FsResult FsApi::skill_load(const std::string& name) {
  return agent_host_->skills().load(name,
                                    parse_think_lang(agent_host_->thinking_lang()));
}

// 浏览器控制 skill 的加载口（addon2：/browser-load 按需加载，不直接提供）
FsResult FsApi::browser_load() { return skill_load("browser-control"); }

// ----- mcp.*（外部工具/技能包；注册表持久化到设置 mcp.entries）-----
namespace {
// 常见外部工具目录（claude code / opencode / 用户配置）
std::vector<std::string> mcp_probe_dirs() {
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
  // 探测常见目录：存在的导入，并把结果返回（不改已有条目）
  Json found = Json::array();
  std::vector<std::string> dirs = mcp_probe_dirs();
  for (size_t i = 0; i < dirs.size(); ++i) {
    std::error_code ec;
    if (!ghc::filesystem::exists(dirs[i], ec) ||
        !ghc::filesystem::is_directory(dirs[i], ec)) {
      continue;
    }
    mcp_registry_->import_dir(dirs[i]);
    Json o = Json::object();
    o["dir"] = dirs[i];
    found.push_back(o);
  }
  if (!found.empty()) {
    settings_->set_global(kKeyMcpEntries, mcp_registry_->dump());
    settings_->save_global();
  }
  return FsResult::success(Json{{"scanned", found}, {"tools", mcp_registry_->list_json()}});
}

FsResult FsApi::set_thinking_lang(const std::string& lang) {
  agent_host_->set_thinking_lang(lang);
  settings_->set_global(kKeyThinkingLang, lang);
  settings_->save_global();
  return FsResult::success(Json{{"thinkingLang", lang}});
}

// ----- terminal.* -----
FsResult FsApi::terminal_list(const std::string& sid) {
  Json arr = Json::array();
  for (auto& e : terminals_->list_for_session(sid)) arr.push_back(e.to_json());
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

// ----- usage.* -----
FsResult FsApi::usage_query(const Json& filter) {
  UsageFilter f;
  f.provider = jsonutil::get<std::string>(filter, "provider", "");
  f.model = jsonutil::get<std::string>(filter, "model", "");
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
    pm[kv.first] = e;
  }
  out["byProviderModel"] = pm;
  Json days = Json::object();
  for (auto& kv : sum.by_day) {
    Json e = Json::object();
    e["in"] = kv.second.in;
    e["out"] = kv.second.out;
    e["cache"] = kv.second.cache;
    e["cost"] = kv.second.cost;
    days[std::to_string(kv.first)] = e;
  }
  out["byDay"] = days;
  return FsResult::success(out);
}
FsResult FsApi::usage_export_csv(const Json& filter, const std::string& path) {
  UsageFilter f;
  f.provider = jsonutil::get<std::string>(filter, "provider", "");
  f.model = jsonutil::get<std::string>(filter, "model", "");
  f.from = jsonutil::get_int(filter, "from", 0);
  f.to = jsonutil::get_int(filter, "to", 0);
  return usage_->export_csv(f, ghc::filesystem::path(path));
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
FsResult FsApi::settings_unset(const std::string& key, const std::string& pid) {
  if (pid.empty()) return settings_->unset_global(key);
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
  // v1：整体替换；delta 合并留 P3（agent-loop.md §7 planDelta）
  plans_->update(sid, planTree);
  return FsResult::success(planTree);
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

FsResult FsApi::workflow_start(const std::string& sid, const std::string& name) {
  const WorkflowDef* def = workflows_->find_def(name);
  if (!def) return FsResult::error(ErrCode::kNotFound, "workflow not found: " + name);
  WorkflowRun* run = workflows_->create_run(*def, sid);
  if (!run) return FsResult::error(ErrCode::kInternal, "create_run failed");
  emit_event("workflow.started", Json{{"runId", run->id}, {"sid", sid},
                                      {"name", name}});
  start_next_pending_step(run);
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
  // 新建向导模板：kind + baseUrl + 默认 header + 说明。用户选择后填写并保存。
  Json arr = Json::array();
  auto add = [&arr](const char* id, const char* name, const char* kind,
                    const char* base, const char* desc, const Json& headers) {
    Json t = Json::object();
    t["id"] = id;
    t["name"] = name;
    t["kind"] = kind;
    t["baseUrl"] = base;
    t["description"] = desc;
    t["headers"] = headers;
    arr.push_back(t);
  };
  add("opencode-zen", "opencode zen（套餐）", "plan",
      "https://opencode.ai/zen/v1",
      "套餐连接：自动携带 x-opencode-session 会话亲和头，按额度计费。",
      Json::object());
  add("openai-chat", "OpenAI 兼容（Chat Completions）", "openai-chat",
      "https://api.openai.com/v1",
      "标准 /chat/completions，Bearer 鉴权。多数第三方兼容端点选这个。",
      Json::object());
  add("openai-response", "OpenAI Response API", "openai-response",
      "https://api.openai.com/v1",
      "使用 /responses 与 input 数组；输出在 output[].content[].text。",
      Json::object());
  add("anthropic", "Anthropic Messages", "anthropic",
      "https://api.anthropic.com/v1",
      "使用 /messages、x-api-key 与 anthropic-version 头。",
      Json::object());
  add("custom", "自定义（endpoint + header）", "custom",
      "https://",
      "复用 Chat 形态，仅 endpoint 与自定义 header 不同。",
      Json::object());
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
  } else {
    level = "preset";
    message = res.message;
    // 回退预置：只返回该 provider 未知的常见模型（用户自定义 > API > 预置 > 保守缺省）
    for (auto& m : builtin_models_preset()) models.push_back(m);
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

}  // namespace fs