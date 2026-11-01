// session_manager.cpp - SessionManager implementation
#include "session_manager.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "fs_path.hpp"
#include "fs_random.hpp"
#include "fs_serialize.hpp"
#include "storage/fs_patch.hpp"

namespace fs {

// 生成 sid，去重检查限定在同一项目目录（传入 pid；空 pid 时退化为不检查）
std::string SessionManager::gen_sid_for(const std::string& pid) {
  auto exists_in = [this, &pid](const std::string& sid) {
    if (pid.empty()) return false;
    nlohmann::json p = proj_->registry().get(pid);
    if (!p.is_object() || !p.contains("path") || !p["path"].is_string()) return false;
    SessionFileStore store(ProjectStore::of(ghc::filesystem::path(p["path"].get<std::string>())));
    return store.full_exists(sid);
  };
  std::string sid;
  for (int i = 0; i < 8; ++i) {  // 极小概率连续撞 8 次，直接失败不重试
    sid = "s_" + (rng_ ? rng_->hex(6) : XorShiftRandom(0).hex(6));
    if (!exists_in(sid)) return sid;
  }
  return sid;
}

bool SessionManager::valid_sid(const std::string& sid) const {
  if (sid.size() < 4 || sid.compare(0, 2, "s_") != 0) return false;
  for (char c : sid.substr(2)) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

ProjectStore SessionManager::project_store_of(const std::string& sid) const {
  auto it = sid_pid_.find(sid);
  if (it != sid_pid_.end()) {
    nlohmann::json j = proj_->registry().get(it->second);
    if (j.is_object() && j.contains("path") && j["path"].is_string()) {
      return ProjectStore::of(std::string(j["path"].get<std::string>()));
    }
  }
  // Not in the in-memory map (server restart, or a session this process never
  // opened): search the project registry for the one holding this sid. Without
  // this, session.get/send on an older session fails with "not found".
  nlohmann::json all = proj_->registry().list();
  if (all.is_array()) {
    for (size_t i = 0; i < all.size(); ++i) {
      std::string path = jsonutil::get<std::string>(all[i], "path", "");
      if (path.empty()) continue;
      ProjectStore ps = ProjectStore::of(ghc::filesystem::path(path));
      SessionFileStore store(ps);
      if (store.full_exists(sid)) {
        sid_pid_[sid] = jsonutil::get<std::string>(all[i], "pid", "");
        return ps;
      }
    }
  }
  // 兜底：未知 → 空 store（调用方已校验存在）
  return ProjectStore::of(ghc::filesystem::path());
}

SessionFileStore SessionManager::make_store(const std::string& sid) const {
  return SessionFileStore(project_store_of(sid));
}

FsResult SessionManager::load_full_checked(const std::string& sid, Json* out) const {
  if (!valid_sid(sid)) {
    return FsResult::error(ErrCode::kInvalidArg, "invalid sid: " + sid);
  }
  return make_store(sid).load_full(sid, out);
}

FsResult SessionManager::load_full(const std::string& sid, Json* out) const {
  return load_full_checked(sid, out);
}

FsResult SessionManager::new_session(const std::string& pid, const std::string& cwd) {
  nlohmann::json p = proj_->registry().get(pid);
  if (!p.is_object() || !p.contains("path") || !p["path"].is_string()) {
    return FsResult::error(ErrCode::kNotFound, "project not found: " + pid);
  }
  std::string path = p["path"].get<std::string>();
  std::string sid = gen_sid_for(pid);
  Json meta = Json::object();
  meta["sid"] = sid;
  meta["pid"] = pid;
  meta["cwd"] = cwd.empty() ? path : cwd;
  meta["createdAt"] = clock_ ? clock_->now_ms() : WallClock().now_ms();
  meta["title"] = "";
  Json defaults = Json::object();
  if (proj_->settings()) {
    defaults["model"] = proj_->settings()->get_value(kKeyNewSessionModel, "");
    defaults["thinking"] = proj_->settings()->get_value(kKeyNewSessionThinking, "");
    // 自动挡/权限支持"记忆上次"：记住上一次会话里的选择，新会话沿用。没有
    // remember 标记时用显式默认值。
    bool remember = false;
    {
      SettingValue sv = proj_->settings()->get("session.default.autoModeRemember");
      if (sv.value.is_boolean()) remember = sv.value.get<bool>();
    }
    bool remember_perm = false;
    {
      SettingValue sv = proj_->settings()->get("session.default.filePermRemember");
      if (sv.value.is_boolean()) remember_perm = sv.value.get<bool>();
    }
    defaults["filePerm"] = proj_->settings()->get_value(kKeyNewSessionFilePerm, "permissive");
    defaults["autoMode"] = proj_->settings()->get_value(kKeyNewSessionAutoMode, false);
    if (remember) {
      SettingValue last_auto = proj_->settings()->get("session.last.autoMode");
      if (last_auto.value.is_boolean()) defaults["autoMode"] = last_auto.value;
    }
    if (remember_perm) {
      SettingValue last_perm = proj_->settings()->get("session.last.filePerm");
      if (last_perm.value.is_string()) defaults["filePerm"] = last_perm.value;
    }
    defaults["enterSend"] = proj_->settings()->get_value(kKeyNewSessionEnterSend, "enter");
  }
  Json ss = default_session_settings(defaults);
  ProjectStore ps = ProjectStore::of(ghc::filesystem::path(path));
  SessionFileStore store(ps);
  FsResult r = store.create(sid, meta, ss);
  if (!r.ok()) return r;
  sid_pid_[sid] = pid;
  proj_->registry().set_last_session(path, sid);
  return FsResult::success(Json{{"sid", sid}, {"pid", pid}, {"cwd", meta["cwd"]}});
}

FsResult SessionManager::open_session(const std::string& sid) {
  Json full;
  FsResult r = load_full_checked(sid, &full);
  if (!r.ok()) return r;
  std::string pid = jsonutil::get<std::string>(full["meta"], "pid", "");
  sid_pid_[sid] = pid;
  std::string path;
  nlohmann::json p = proj_->registry().get(pid);
  if (p.is_object() && p.contains("path") && p["path"].is_string()) {
    path = p["path"].get<std::string>();
  }
  if (!path.empty()) proj_->registry().set_last_session(path, sid);
  return FsResult::success(full);
}

FsResult SessionManager::get_session(const std::string& sid) const {
  Json full;
  FsResult r = load_full_checked(sid, &full);
  if (!r.ok()) return r;
  return FsResult::success(full);
}

FsResult SessionManager::list_sessions(const std::string& pid) const {
  nlohmann::json p = proj_->registry().get(pid);
  if (!p.is_object() || !p.contains("path") || !p["path"].is_string()) {
    return FsResult::error(ErrCode::kNotFound, "project not found: " + pid);
  }
  ghc::filesystem::path sessions =
      ProjectStore::of(ghc::filesystem::path(p["path"].get<std::string>())).sessions_dir;
  std::vector<std::string> sids;
  std::error_code ec;
  if (ghc::filesystem::exists(sessions, ec)) {
    for (auto& e : ghc::filesystem::directory_iterator(sessions, ec)) {
      std::string name = e.path().filename().string();
      if (ghc::filesystem::is_directory(e.path()) &&
          ghc::filesystem::exists(e.path() / "full.json")) {
        sids.push_back(name);
      }
    }
  }
  std::sort(sids.begin(), sids.end());
  // Read through the project store directly: make_store(sid) resolves via the
  // in-memory sid->pid map, which is empty after a restart (and for sessions
  // this process never opened), so those sessions would silently disappear.
  SessionFileStore store(
      ProjectStore::of(ghc::filesystem::path(p["path"].get<std::string>())));
  Json arr = Json::array();
  for (auto& sid : sids) {
    Json j;
    Json meta;
    FsResult rr = store.load_full(sid, &j);
    if (!rr.ok()) continue;
    if (j.contains("meta")) meta = j["meta"];
    // A session's last turn may have failed; the project card shows that as a
    // red status dot, so the list has to carry it (it already reads full.json).
    if (j.contains("turns") && j["turns"].is_array() && !j["turns"].empty()) {
      const nlohmann::json& last = j["turns"][j["turns"].size() - 1];
      std::string err = jsonutil::get<std::string>(last, "error", "");
      if (!err.empty()) meta["lastError"] = err;
    }
    arr.push_back(meta);
  }
  return FsResult::success(arr);
}

FsResult SessionManager::close_session(const std::string& sid) {
  locks_.release_all_for(sid);
  sid_pid_.erase(sid);
  return FsResult::success();
}

FsResult SessionManager::delete_session(const std::string& sid) {
  if (sid.empty()) return FsResult::error(ErrCode::kInvalidArg, "empty sid");
  // Release the lock first: a leftover lock entry would make a future session
  // with the same id look occupied.
  locks_.release_all_for(sid);
  sid_pid_.erase(sid);
  // Resolve the store from the sid's own project rather than the in-memory map,
  // which is empty for a session this process never opened.
  Json full;
  FsResult lr = load_full_checked(sid, &full);
  if (!lr.ok()) return lr;
  std::string pid = jsonutil::get<std::string>(full["meta"], "pid", "");
  nlohmann::json p = proj_->registry().get(pid);
  if (!p.is_object() || !p.contains("path") || !p["path"].is_string()) {
    return FsResult::error(ErrCode::kNotFound, "project not found: " + pid);
  }
  SessionFileStore store(
      ProjectStore::of(ghc::filesystem::path(p["path"].get<std::string>())));
  return store.remove(sid);
}

FsResult SessionManager::append_turn(const std::string& sid, const Json& turn) {
  FsResult r = load_full_checked(sid, nullptr);
  if (!r.ok()) return r;
  // Stamp every turn with when it was written. The UI shows "finished at /
  // took 1m 20s" per turn, and that is only knowable if the time is stored with
  // the turn - recomputing it from file mtimes gives one number per session.
  Json t = turn;
  int64_t now = clock_ ? clock_->now_ms() : WallClock().now_ms();
  if (!t.contains("ts") || !t["ts"].is_number()) t["ts"] = now;
  // Every turn needs a STABLE id: rollback / fork / forget all address a turn by
  // it, and a turn without one could never be addressed at all (the previous
  // version never wrote this field, so all three were dead on arrival).
  if (!t.contains("turnId") || !t["turnId"].is_string() ||
      t["turnId"].get<std::string>().empty()) {
    t["turnId"] = "t_" + (rng_ ? rng_->hex(6) : XorShiftRandom(0).hex(6));
  }
  if (!t.contains("startedAt")) {
    // A turn's start is the previous turn's end; the first turn of a session
    // starts now (unknown otherwise) so the duration is never negative.
    Json full;
    if (load_full(sid, &full).ok() && full.contains("turns") &&
        full["turns"].is_array() && !full["turns"].empty()) {
      const Json& prev = full["turns"][full["turns"].size() - 1];
      int64_t prev_ts = jsonutil::get_int(prev, "ts", 0);
      t["startedAt"] = prev_ts > 0 ? prev_ts : now;
    } else {
      t["startedAt"] = now;
    }
  }
  return make_store(sid).append_turn(sid, t);
}

FsResult SessionManager::append_system_op(const std::string& sid, const std::string& op,
                                          const Json& detail) {
  Json e = Json::object();
  e["t"] = clock_ ? clock_->now_ms() : WallClock().now_ms();
  e["op"] = op;
  e["detail"] = detail;
  FsResult r = load_full_checked(sid, nullptr);
  if (!r.ok()) return r;
  return make_store(sid).append_system_op(sid, e);
}

void SessionManager::write_live_snapshot(const std::string& sid, const Json& live) {
  if (!valid_sid(sid)) return;
  // runtime.json is the "cache, safe to delete" file, which is the right home for
  // an in-flight turn. It merges rather than replaces so the runtime cache's other
  // fields survive.
  Json runtime;
  if (!make_store(sid).read_runtime(sid, &runtime).ok() || !runtime.is_object()) {
    runtime = Json::object();
  }
  runtime["live"] = live;
  make_store(sid).write_runtime(sid, runtime);
}

Json SessionManager::read_live_snapshot(const std::string& sid) const {
  if (!valid_sid(sid)) return Json::object();
  Json runtime;
  if (!make_store(sid).read_runtime(sid, &runtime).ok()) return Json::object();
  if (!runtime.is_object() || !runtime.contains("live")) return Json::object();
  return runtime["live"];
}

// ----- 子代理记录落盘 -----
// <session dir>/subagents/<id>.json. One file per record, so pruning or reading a
// single agent never rewrites a growing aggregate. Failures are swallowed: this is
// a durability nicety, and a subagent must not fail because a write failed.
void SessionManager::write_subagent_record(const std::string& sid,
                                           const std::string& id,
                                           const Json& record) const {
  if (!valid_sid(sid) || id.empty()) return;
  std::string pid = pid_of(sid);
  if (pid.empty()) return;
  Json p = proj_->registry().get(pid);
  if (!p.is_object() || !p.contains("path") || !p["path"].is_string()) return;
  ProjectStore store(ProjectStore::of(ghc::filesystem::path(p["path"].get<std::string>())));
  ghc::filesystem::path dir = store.session_dir(sid) / "subagents";
  std::error_code ec;
  ghc::filesystem::create_directories(dir, ec);
  // The id is generated internally (sub_1, sub_2...) but treat it as untrusted
  // input anyway: a path separator here would escape the session directory.
  std::string safe;
  for (size_t i = 0; i < id.size(); ++i) {
    char c = id[i];
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-';
    safe.push_back(ok ? c : '_');
  }
  std::ofstream out((dir / (safe + ".json")).string(), std::ios::binary | std::ios::trunc);
  if (!out) return;
  out << record.dump(2);
}

Json SessionManager::read_subagent_record(const std::string& sid,
                                          const std::string& id) const {
  if (!valid_sid(sid) || id.empty()) return Json::object();
  std::string pid = pid_of(sid);
  if (pid.empty()) return Json::object();
  Json p = proj_->registry().get(pid);
  if (!p.is_object() || !p.contains("path") || !p["path"].is_string()) {
    return Json::object();
  }
  ProjectStore store(ProjectStore::of(ghc::filesystem::path(p["path"].get<std::string>())));
  std::string safe;
  for (size_t i = 0; i < id.size(); ++i) {
    char c = id[i];
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-';
    safe.push_back(ok ? c : '_');
  }
  std::ifstream in((store.session_dir(sid) / "subagents" / (safe + ".json")).string(),
                   std::ios::binary);
  if (!in) return Json::object();
  std::string body((std::istreambuf_iterator<char>(in)),
                   std::istreambuf_iterator<char>());
  if (body.empty()) return Json::object();
  try {
    Json j = Json::parse(body);
    return j.is_object() ? j : Json::object();
  } catch (...) {
    return Json::object();
  }
}


FsResult SessionManager::update_session_settings(const std::string& sid,
                                                 const Json& fields) {  Json full;
  FsResult r = load_full_checked(sid, &full);
  if (!r.ok()) return r;
  if (!full.contains("sessionSettings") || !full["sessionSettings"].is_object()) {
    full["sessionSettings"] = Json::object();
  }
  for (auto it = fields.begin(); it != fields.end(); ++it) {
    full["sessionSettings"][it.key()] = it.value();
  }
  std::string err;
  // Through the store, never a raw write of full.json: the turns live in shards
  // now, so a direct write would save the head without them (and silently drop
  // every change made to `full["turns"]`).
  bool ok = make_store(sid).save_full(sid, full, &err);
  return ok ? FsResult::success() : FsResult::error(ErrCode::kIo, err);
}

// 重命名会话：title 存在 meta 里（不是 sessionSettings —— 它是身份的一部分，
// 不是模型调用的参数），列表页读的就是它。
FsResult SessionManager::rename_session(const std::string& sid, const std::string& title) {
  if (sid.empty()) return FsResult::error(ErrCode::kInvalidArg, "empty sid");
  Json full;
  FsResult r = load_full_checked(sid, &full);
  if (!r.ok()) return r;
  if (!full.contains("meta") || !full["meta"].is_object()) {
    full["meta"] = Json::object();
  }
  full["meta"]["title"] = title;
  std::string err;
  bool ok = make_store(sid).save_full(sid, full, &err);
  if (!ok) return FsResult::error(ErrCode::kIo, err);
  return FsResult::success(Json{{"sid", sid}, {"title", title}});
}

// A title the USER set is final: mark it so the automatic upkeep stops touching
// it. Renaming is an explicit action, and silently replacing it later with a
// model-generated name is exactly the kind of "the app undid my work" that
// destroys trust in an editor.
FsResult SessionManager::rename_session_by_user(const std::string& sid,
                                                const std::string& title) {
  FsResult r = rename_session(sid, title);
  if (!r.ok()) return r;
  return set_meta_flag(sid, Json{{"titleUser", true}});
}

FsResult SessionManager::set_meta_flag(const std::string& sid, const Json& fields) {
  if (sid.empty()) return FsResult::error(ErrCode::kInvalidArg, "empty sid");
  Json full;
  FsResult r = load_full_checked(sid, &full);
  if (!r.ok()) return r;
  if (!full.contains("meta") || !full["meta"].is_object()) {
    full["meta"] = Json::object();
  }
  for (auto it = fields.begin(); it != fields.end(); ++it) {
    full["meta"][it.key()] = it.value();
  }
  std::string err;
  if (!make_store(sid).save_full(sid, full, &err)) {
    return FsResult::error(ErrCode::kIo, err);
  }
  return FsResult::success();
}

FsResult SessionManager::set_model(const std::string& sid, const std::string& model) {
  return update_session_settings(sid, Json{{"model", model}});
}
FsResult SessionManager::set_thinking(const std::string& sid, const std::string& thinking) {
  return update_session_settings(sid, Json{{"thinking", thinking}});
}
FsResult SessionManager::set_permission(const std::string& sid, const std::string& perm) {
  FilePerm fp;
  if (!file_perm_from_str(perm, &fp)) {
    return FsResult::error(ErrCode::kInvalidArg, "bad filePerm: " + perm);
  }
  FsResult r = update_session_settings(sid, Json{{"filePerm", perm}});
  if (r.ok()) remember_last_session_choice(sid);
  return r;
}
FsResult SessionManager::set_auto_mode(const std::string& sid, bool auto_mode) {
  FsResult r = update_session_settings(sid, Json{{"autoMode", auto_mode}});
  if (r.ok()) remember_last_session_choice(sid);
  return r;
}

// "记忆上次" 的数据来源：用户在某次会话里真正切换过自动挡/权限，就把这一对
// 值记成"上一次的选择"。新会话在 session.default.autoModeRemember=true 且没有
// 显式默认时读它。写 settings 失败不影响会话本身（这只是给下一次用的提示）。
void SessionManager::remember_last_session_choice(const std::string& sid) {
  if (!proj_->settings()) return;
  Json full;
  if (!load_full(sid, &full).ok()) return;
  Json ss = full.value("sessionSettings", Json::object());
  if (ss.contains("autoMode")) {
    proj_->settings()->set_global("session.last.autoMode", ss["autoMode"]);
  }
  if (ss.contains("filePerm")) {
    proj_->settings()->set_global("session.last.filePerm", ss["filePerm"]);
  }
  proj_->settings()->save_global();
}
FsResult SessionManager::set_enter_send(const std::string& sid, const std::string& enter_send) {
  enter_send_from_str(enter_send, EnterSend::kEnter);
  return update_session_settings(sid, Json{{"enterSend", enter_send}});
}

std::string SessionManager::request_permission_change_token(const std::string& sid,
                                                           const std::string& new_perm) {
  XorShiftRandom tmp(0);
  std::string tok = tmp.hex(4);
  upgrade_tokens_[sid] = tok;
  return tok;
}

FsResult SessionManager::set_permission_confirm(const std::string& sid,
                                                const std::string& perm,
                                                const std::string& token) {
  auto it = upgrade_tokens_.find(sid);
  if (it == upgrade_tokens_.end() || it->second != token) {
    return FsResult::error(ErrCode::kConfirmRequired, "confirm token mismatch");
  }
  upgrade_tokens_.erase(it);
  return set_permission(sid, perm);
}

FsResult SessionManager::refresh_config(const std::string& sid) {
  Json full;
  FsResult r = load_full_checked(sid, &full);
  if (!r.ok()) return r;
  int64_t ver = jsonutil::get_int(full, "configVersion", 0) + 1;
  full["configVersion"] = ver;
  full["continuousFailures"] = 0;
  std::string err;
  bool ok = make_store(sid).save_full(sid, full, &err);
  if (!ok) return FsResult::error(ErrCode::kIo, err);
  return append_system_op(sid, "refreshConfig", Json{{"configVersion", ver}});
}
FsResult SessionManager::rollback(const std::string& sid, const std::string& turn_id,
                                  std::vector<std::string>* reverse_errors) {
  Json full;
  FsResult r = load_full_checked(sid, &full);
  if (!r.ok()) return r;
  if (!full.contains("turns") || !full["turns"].is_array()) {
    return FsResult::error(ErrCode::kNotFound, "no turns");
  }
  long idx = -1;
  for (long i = 0; i < (long)full["turns"].size(); ++i) {
    if (full["turns"][i].is_object() &&
        jsonutil::get<std::string>(full["turns"][i], "turnId", "") == turn_id) {
      idx = i;
      break;
    }
  }
  if (idx < 0) return FsResult::error(ErrCode::kNotFound, "turn not found: " + turn_id);
  if (reverse_errors) reverse_errors->clear();
  ProjectStore ps = project_store_of(sid);
  for (long i = (long)full["turns"].size() - 1; i >= idx; --i) {
    std::string tid = jsonutil::get<std::string>(full["turns"][i], "turnId", "");
    if (tid.empty()) continue;
    Json patch;
    FsResult pr = make_store(sid).read_patch(sid, tid, &patch);
    if (!pr.ok()) {
      if (reverse_errors) reverse_errors->push_back("missing patch: " + tid);
      continue;
    }
    FsResult ar = apply_patch_reverse(ps, patch);
    if (!ar.ok() && reverse_errors) reverse_errors->push_back(ar.message);
  }
  // 回滚语义（session-history.md §5）：删除该 turn 及之后 turns
  size_t keep = (size_t)idx;
  FsResult tr = make_store(sid).trim_turns(sid, keep);
  if (!tr.ok()) return tr;
  return FsResult::success(
      Json{{"rolledBack", true},
           {"errors", reverse_errors ? Json(*reverse_errors) : Json::array()}});
}

FsResult SessionManager::trim_history(const std::string& sid,
                                      const std::string& turn_id) {
  Json full;
  FsResult r = load_full_checked(sid, &full);
  if (!r.ok()) return r;
  if (!full.contains("turns") || !full["turns"].is_array()) {
    return FsResult::error(ErrCode::kNotFound, "no turns");
  }
  long idx = -1;
  for (long i = 0; i < (long)full["turns"].size(); ++i) {
    if (full["turns"][i].is_object() &&
        jsonutil::get<std::string>(full["turns"][i], "turnId", "") == turn_id) {
      idx = i;
      break;
    }
  }
  if (idx < 0) return FsResult::error(ErrCode::kNotFound, "turn not found: " + turn_id);
  return make_store(sid).trim_turns(sid, static_cast<size_t>(idx));
}

FsResult SessionManager::fork(const std::string& sid, const std::string& turn_id) {
  Json full;
  FsResult r = load_full_checked(sid, &full);
  if (!r.ok()) return r;
  if (!full.contains("turns") || !full["turns"].is_array()) {
    return FsResult::error(ErrCode::kNotFound, "no turns");
  }
  long idx = -1;
  for (long i = 0; i < (long)full["turns"].size(); ++i) {
    if (full["turns"][i].is_object() &&
        jsonutil::get<std::string>(full["turns"][i], "turnId", "") == turn_id) idx = i;
  }
  if (idx < 0) return FsResult::error(ErrCode::kNotFound, "turn not found: " + turn_id);
  std::string new_sid = gen_sid_for(pid_of(sid));
  Json new_full = full;
  new_full["meta"]["sid"] = new_sid;
  new_full["meta"]["createdAt"] = clock_ ? clock_->now_ms() : WallClock().now_ms();
  Json turns = new_full["turns"];
  turns.erase(turns.begin() + idx + 1, turns.end());
  new_full["turns"] = turns;
  Json snaps = new_full.value("snapshots", Json::array());
  if (snaps.is_array()) {
    std::set<std::string> keep;
    for (auto& t : new_full["turns"]) {
      if (t.is_object()) keep.insert(jsonutil::get<std::string>(t, "turnId", ""));
    }
    Json ns = Json::array();
    for (auto& s : snaps) {
      if (s.is_object() && s.contains("turnId") &&
          keep.count(jsonutil::get<std::string>(s, "turnId", ""))) ns.push_back(s);
    }
    new_full["snapshots"] = ns;
  }
  std::string err;
  // A fork writes a NEW session under the SAME project store as its source:
  // make_store(new_sid) cannot resolve it yet (it is not registered).
  bool ok = SessionFileStore(project_store_of(sid)).save_full(new_sid, new_full, &err);
  if (!ok) return FsResult::error(ErrCode::kIo, err);
  sid_pid_[new_sid] = pid_of(sid);
  return FsResult::success(Json{{"sid", new_sid}});
}

FsResult SessionManager::forget(const std::string& sid, const std::string& scope) {
  Json full;
  FsResult r = load_full_checked(sid, &full);
  if (!r.ok()) return r;
  if (scope == "all") {
    for (auto& t : full["turns"]) {
      if (t.is_object()) t["forgotten"] = true;
    }
  } else {
    bool found = false;
    for (auto& t : full["turns"]) {
      if (t.is_object() && jsonutil::get<std::string>(t, "turnId", "") == scope) {
        t["forgotten"] = true;
        found = true;
      }
    }
    if (!found) return FsResult::error(ErrCode::kNotFound, "turn not found: " + scope);
  }
  std::string err;
  // The marked turns live in the shards, so this must go through the store.
  bool ok = make_store(sid).save_full(sid, full, &err);
  return ok ? FsResult::success() : FsResult::error(ErrCode::kIo, err);
}

std::string SessionManager::pid_of(const std::string& sid) const {
  auto it = sid_pid_.find(sid);
  if (it != sid_pid_.end()) return it->second;
  Json full;
  if (load_full_checked(sid, &full).ok() && full.contains("meta")) {
    return jsonutil::get<std::string>(full["meta"], "pid", "");
  }
  return "";
}

int64_t SessionManager::full_mtime_ms(const std::string& sid) const {
  std::string pid = pid_of(sid);
  if (pid.empty()) return 0;
  nlohmann::json p = proj_->registry().get(pid);
  if (!p.is_object() || !p.contains("path") || !p["path"].is_string()) return 0;
  ghc::filesystem::path full =
      ProjectStore::of(ghc::filesystem::path(p["path"].get<std::string>())).full_json(sid);
  std::error_code ec;
  if (!ghc::filesystem::exists(full, ec)) return 0;
  auto t = ghc::filesystem::last_write_time(full, ec);
  if (ec) return 0;
  // duration_cast, not .count(): file_time_type's period is platform-defined
  // (100ns ticks on Windows), so a raw count would be off by four orders of
  // magnitude and the UI would show nonsense dates.
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             t.time_since_epoch()).count();
}

}  // namespace fs
