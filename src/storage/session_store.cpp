// session_store.cpp - session files read/write helpers
#include "session_store.hpp"

#include <fstream>
#include <set>
#include <sstream>

#include "fs_json.hpp"
#include "fs_path.hpp"
#include "fs_serialize.hpp"

namespace fs {

Json default_session_settings(const Json& defaults) {
  Json s = Json::object();
  s["model"] = jsonutil::get<std::string>(defaults, "model", "");
  s["thinking"] = jsonutil::get<std::string>(defaults, "thinking", "");
  s["filePerm"] = jsonutil::get<std::string>(defaults, "filePerm", "permissive");
  s["autoMode"] = jsonutil::get<bool>(defaults, "autoMode", false);
  s["enterSend"] = jsonutil::get<std::string>(defaults, "enterSend", "enter");
  return s;
}

bool SessionFileStore::exists(const std::string& sid) const {
  return ghc::filesystem::exists(ps_.session_dir(sid));
}
bool SessionFileStore::full_exists(const std::string& sid) const {
  return ghc::filesystem::exists(full_path(sid));
}

FsResult SessionFileStore::create(const std::string& sid, const Json& meta,
                                  const Json& session_settings) {
  ghc::filesystem::path dir = ps_.session_dir(sid);
  std::error_code ec;
  ghc::filesystem::create_directories(ps_.patches_dir(sid), ec);
  ghc::filesystem::create_directories(ps_.terminals_dir(sid), ec);
  Json full = Json::object();
  full["meta"] = meta;
  full["sessionSettings"] = session_settings;
  full["turns"] = Json::array();
  full["systemOps"] = Json::array();
  full["snapshots"] = Json::array();
  std::string err;
  if (!jsonutil::write_atomic(full_path(sid).string(), full, &err)) {
    return FsResult::error(ErrCode::kIo, err);
  }
  return FsResult::success(full);
}

FsResult SessionFileStore::load_full(const std::string& sid, Json* out) const {
  if (!full_exists(sid)) return FsResult::error(ErrCode::kNotFound, "session not found: " + sid);
  std::ifstream ifs(full_path(sid).string(), std::ios::binary);
  if (!ifs) return FsResult::error(ErrCode::kIo, "cannot open full.json: " + full_path(sid).string());
  std::stringstream ss;
  ss << ifs.rdbuf();
  try {
    Json j = Json::parse(ss.str());
    if (out) *out = std::move(j);
    return FsResult::success();
  } catch (const std::exception& e) {
    return FsResult::error(ErrCode::kParse, std::string("full.json parse: ") + e.what());
  }
}

FsResult SessionFileStore::append_turn(const std::string& sid, const Json& turn) {
  Json full;
  FsResult r = load_full(sid, &full);
  if (!r.ok()) return r;
  full["turns"].push_back(turn);
  std::string err;
  if (!jsonutil::write_atomic(full_path(sid).string(), full, &err)) {
    return FsResult::error(ErrCode::kIo, err);
  }
  return FsResult::success();
}

FsResult SessionFileStore::append_system_op(const std::string& sid, const Json& op) {
  Json full;
  FsResult r = load_full(sid, &full);
  if (!r.ok()) return r;
  full["systemOps"].push_back(op);
  std::string err;
  if (!jsonutil::write_atomic(full_path(sid).string(), full, &err)) {
    return FsResult::error(ErrCode::kIo, err);
  }
  return FsResult::success();
}

FsResult SessionFileStore::trim_turns(const std::string& sid, size_t keep) {
  Json full;
  FsResult r = load_full(sid, &full);
  if (!r.ok()) return r;
  if (full["turns"].is_array() && full["turns"].size() > keep) {
    Json t = full["turns"];
    t.erase(t.begin() + static_cast<long>(keep), t.end());
    full["turns"] = t;
  }
  if (full["snapshots"].is_array()) {
    Json s = full["snapshots"];
    for (auto it = s.begin(); it != s.end();) {
      if (it->is_object() && it->contains("turnId") && (*it)["turnId"].is_string()) {
        // 若 snapshot 的 turnId 不在剩余 turns 里则删
        bool found = false;
        for (auto t : full["turns"]) {
          if (t.is_object() && t.contains("turnId") && t["turnId"] == (*it)["turnId"]) found = true;
        }
        if (!found) it = s.erase(it);
        else ++it;
      } else {
        ++it;
      }
    }
    full["snapshots"] = s;
  }
  std::string err;
  if (!jsonutil::write_atomic(full_path(sid).string(), full, &err)) {
    return FsResult::error(ErrCode::kIo, err);
  }
  return FsResult::success();
}

FsResult SessionFileStore::remove(const std::string& sid) {
  if (sid.empty()) return FsResult::error(ErrCode::kInvalidArg, "empty sid");
  ghc::filesystem::path dir = ps_.session_dir(sid);
  std::error_code ec;
  if (!ghc::filesystem::exists(dir, ec)) {
    return FsResult::error(ErrCode::kNotFound, "session not found: " + sid);
  }
  ghc::filesystem::remove_all(dir, ec);
  if (ec) {
    return FsResult::error(ErrCode::kIo, "cannot remove session dir: " + ec.message());
  }
  return FsResult::success();
}

FsResult SessionFileStore::write_patch(const std::string& sid, const std::string& turn_id,
                                      const Json& patch) {
  ghc::filesystem::path p = ps_.patch(sid, turn_id);
  std::string err;
  if (!jsonutil::write_atomic(p.string(), patch, &err)) {
    return FsResult::error(ErrCode::kIo, err);
  }
  return FsResult::success();
}

FsResult SessionFileStore::read_patch(const std::string& sid, const std::string& turn_id,
                                     Json* out) const {
  ghc::filesystem::path p = ps_.patch(sid, turn_id);
  if (!ghc::filesystem::exists(p)) {
    return FsResult::error(ErrCode::kNotFound, "patch not found: " + turn_id);
  }
  std::ifstream ifs(p.string(), std::ios::binary);
  if (!ifs) return FsResult::error(ErrCode::kIo, "cannot open patch: " + p.string());
  try {
    Json j = Json::parse(ifs);
    if (out) *out = std::move(j);
    return FsResult::success();
  } catch (const std::exception& e) {
    return FsResult::error(ErrCode::kParse, std::string("patch parse: ") + e.what());
  }
}

std::vector<std::string> SessionFileStore::list_patches(const std::string& sid) const {
  std::vector<std::string> out;
  ghc::filesystem::path d = ps_.patches_dir(sid);
  std::error_code ec;
  if (!ghc::filesystem::exists(d, ec)) return out;
  for (auto& e : ghc::filesystem::directory_iterator(d, ec)) {
    if (e.path().extension() == ".json") out.push_back(e.path().stem().string());
  }
  std::sort(out.begin(), out.end());
  return out;
}

FsResult SessionFileStore::write_runtime(const std::string& sid, const Json& runtime) {
  std::string err;
  if (!jsonutil::write_atomic(ps_.runtime_json(sid).string(), runtime, &err)) {
    return FsResult::error(ErrCode::kIo, err);
  }
  return FsResult::success();
}

FsResult SessionFileStore::read_runtime(const std::string& sid, Json* out) const {
  ghc::filesystem::path p = ps_.runtime_json(sid);
  if (!ghc::filesystem::exists(p)) {
    return FsResult::error(ErrCode::kNotFound, "runtime not found: " + sid);
  }
  std::ifstream ifs(p.string(), std::ios::binary);
  if (!ifs) return FsResult::error(ErrCode::kIo, "cannot open runtime: " + p.string());
  try {
    Json j = Json::parse(ifs);
    if (out) *out = j;
    return FsResult::success();
  } catch (const std::exception& e) {
    return FsResult::error(ErrCode::kParse, std::string("runtime parse: ") + e.what());
  }
}

ghc::filesystem::path SessionFileStore::terminal_log_path(const std::string& sid,
                                                          const std::string& tid) const {
  return ps_.terminal_log(sid, tid);
}

std::vector<std::string> SessionFileStore::list_terminal_logs(const std::string& sid) const {
  std::vector<std::string> out;
  ghc::filesystem::path d = ps_.terminals_dir(sid);
  std::error_code ec;
  if (!ghc::filesystem::exists(d, ec)) return out;
  for (auto& e : ghc::filesystem::directory_iterator(d, ec)) {
    if (e.path().extension() == ".log") out.push_back(e.path().stem().string());
  }
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace fs