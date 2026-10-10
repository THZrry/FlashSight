// session_manager.cpp - SessionManager implementation
#include "session_manager.hpp"

#include <algorithm>
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
    defaults["filePerm"] = proj_->settings()->get_value(kKeyNewSessionFilePerm, "permissive");
    defaults["autoMode"] = proj_->settings()->get_value(kKeyNewSessionAutoMode, false);
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
  Json arr = Json::array();
  for (auto& sid : sids) {
    Json j;
    Json meta;
    FsResult rr = make_store(sid).load_full(sid, &j);
    if (!rr.ok()) continue;
    if (j.contains("meta")) meta = j["meta"];
    arr.push_back(meta);
  }
  return FsResult::success(arr);
}

FsResult SessionManager::close_session(const std::string& sid) {
  locks_.release_all_for(sid);
  sid_pid_.erase(sid);
  return FsResult::success();
}

FsResult SessionManager::append_turn(const std::string& sid, const Json& turn) {
  FsResult r = load_full_checked(sid, nullptr);
  if (!r.ok()) return r;
  return make_store(sid).append_turn(sid, turn);
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

FsResult SessionManager::update_session_settings(const std::string& sid,
                                                 const Json& fields) {
  Json full;
  FsResult r = load_full_checked(sid, &full);
  if (!r.ok()) return r;
  if (!full.contains("sessionSettings") || !full["sessionSettings"].is_object()) {
    full["sessionSettings"] = Json::object();
  }
  for (auto it = fields.begin(); it != fields.end(); ++it) {
    full["sessionSettings"][it.key()] = it.value();
  }
  std::string err;
  bool ok = jsonutil::write_atomic(project_store_of(sid).full_json(sid).string(), full, &err);
  return ok ? FsResult::success() : FsResult::error(ErrCode::kIo, err);
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
  return update_session_settings(sid, Json{{"filePerm", perm}});
}
FsResult SessionManager::set_auto_mode(const std::string& sid, bool auto_mode) {
  return update_session_settings(sid, Json{{"autoMode", auto_mode}});
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
  bool ok = jsonutil::write_atomic(project_store_of(sid).full_json(sid).string(), full, &err);
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
  bool ok = jsonutil::write_atomic(project_store_of(sid).full_json(new_sid).string(),
                                   new_full, &err);
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
  bool ok = jsonutil::write_atomic(project_store_of(sid).full_json(sid).string(), full, &err);
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

}  // namespace fs