// fs_api.cpp - FsApi unified entry implementation (P1 scope)
#include "fs_api.hpp"

#include "fs_path.hpp"

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
  deny_list_.reset(new DenyList());
  plans_.reset(new PlanStore());
  // P2: provider 层
  providers_.reset(new ProviderStore(root_ / "providers.json"));
  model_table_.reset(new ModelTable());
  model_table_->set_preset(builtin_models_preset());
  HttpRequest sample;
  net_.reset(new HttplibNetClient(sample));
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
  // P1 注册表标记；实际 kill 在 P3（reproc）
  return terminals_->mark_exited(tid, -1, clock_->now_ms());
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
  if (pid.empty()) return settings_->set_global(key, value);
  if (path_for(pid).empty()) return FsResult::error(ErrCode::kNotFound, "project not found: " + pid);
  return store_for(pid).set_project(key, value);
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
    arr.push_back(j);
  }
  return FsResult::success(arr);
}

FsResult FsApi::provider_add_provider(const Json& conn) {
  ProviderConn c = ProviderConn::from_json(conn);
  if (c.base_url.empty()) {
    return FsResult::error(ErrCode::kInvalidArg, "baseUrl required");
  }
  std::string id = providers_->upsert(c);
  std::string err;
  if (!providers_->save(&err)) {
    return FsResult::error(ErrCode::kIo, err);
  }
  return FsResult::success(Json{{"id", id}});
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