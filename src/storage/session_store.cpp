// session_store.cpp - session files read/write helpers
#include "session_store.hpp"

#include <fstream>
#include <set>
#include <sstream>

#include "fs_json.hpp"
#include "fs_path.hpp"
#include "fs_serialize.hpp"

namespace fs {

namespace {

// How many turns live in one shard file.
//
// Why shard at all: every append re-wrote the WHOLE full.json (read-modify-write
// of one file), so a session with hundreds of turns paid O(n) on every single
// turn and the file grew to hundreds of KB. The user's own session hit 90 KB
// after six turns because tool results are stored in full.
//
// 16 turns per shard keeps each file in the tens-of-KB range for typical use
// (the report suggested ~16 KB per file). `turns/000.json` holds turns 0-15,
// `turns/001.json` the next 16, and so on.
constexpr int kTurnsPerShard = 16;

// Load every shard for a session into one array, oldest first.
Json read_turns_sharded(const ProjectStore& ps, const std::string& sid) {
  Json all = Json::array();
  std::error_code ec;
  if (!ghc::filesystem::is_directory(ps.turns_dir(sid), ec)) return all;
  for (int i = 0;; ++i) {
    ghc::filesystem::path p = ps.turn_shard(sid, i);
    if (!ghc::filesystem::is_regular_file(p, ec)) break;
    std::ifstream ifs(p.string(), std::ios::binary);
    if (!ifs) break;
    std::stringstream ss;
    ss << ifs.rdbuf();
    try {
      Json part = Json::parse(ss.str());
      if (part.is_array()) {
        for (size_t k = 0; k < part.size(); ++k) all.push_back(part[k]);
      }
    } catch (...) {
      // A corrupt shard must not lose the shards after it: stop reading this
      // one but keep what was already parsed.
      break;
    }
  }
  return all;
}

// Write shards [from_index, ...) from `turns`, and delete shards past the end so
// a rollback (which shortens the history) cannot leave stale turns on disk.
bool write_turns_sharded(const ProjectStore& ps, const std::string& sid,
                         const Json& turns, int from_index, bool write_from_scratch,
                         std::string* err) {
  std::error_code ec;
  ghc::filesystem::create_directories(ps.turns_dir(sid), ec);
  if (!turns.is_array()) return true;
  int total = static_cast<int>(turns.size());
  int first = write_from_scratch ? 0 : from_index;
  if (first < 0) first = 0;
  int first_shard = first / kTurnsPerShard;
  int last_shard = total > 0 ? (total - 1) / kTurnsPerShard : -1;
  for (int s = first_shard; s <= last_shard; ++s) {
    Json part = Json::array();
    int lo = s * kTurnsPerShard;
    int hi = lo + kTurnsPerShard;
    if (hi > total) hi = total;
    for (int i = lo; i < hi; ++i) part.push_back(turns[static_cast<size_t>(i)]);
    if (!jsonutil::write_atomic(ps.turn_shard(sid, s).string(), part, err)) {
      return false;
    }
  }
  // Drop shards that no longer have turns (rollback / forget).
  for (int s = last_shard + 1;; ++s) {
    ghc::filesystem::path p = ps.turn_shard(sid, s);
    if (!ghc::filesystem::is_regular_file(p, ec)) break;
    ghc::filesystem::remove(p, ec);
  }
  return true;
}

}  // namespace

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
  ghc::filesystem::create_directories(ps_.turns_dir(sid), ec);
  Json full = Json::object();
  full["meta"] = meta;
  full["sessionSettings"] = session_settings;
  full["turns"] = Json::array();
  full["systemOps"] = Json::array();
  full["snapshots"] = Json::array();
  std::string err;
  if (!save_full(sid, full, &err)) {
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
  Json j;
  try {
    j = Json::parse(ss.str());
  } catch (const std::exception& e) {
    return FsResult::error(ErrCode::kParse, std::string("full.json parse: ") + e.what());
  }
  // Sharded layout: turns live in turns/*.json and full.json keeps only
  // meta/settings/ops. A session written by an older build has `turns` inline -
  // that form is still read as-is (and migrated on the next append), so nothing
  // is lost when moving between versions.
  //
  // The rule is "do SHARD FILES exist", not "does the turns/ directory exist":
  // every session directory carries an empty turns/ from creation, so testing
  // the directory would make a legacy file look sharded and silently drop its
  // inline history.
  Json inline_turns = j.contains("turns") && j["turns"].is_array() ? j["turns"]
                                                                  : Json::array();
  Json sharded = read_turns_sharded(ps_, sid);
  if (!sharded.empty()) {
    j["turns"] = sharded;
  } else {
    j["turns"] = inline_turns;
  }
  if (!j["turns"].is_array()) j["turns"] = Json::array();
  if (out) *out = std::move(j);
  return FsResult::success();
}

// Write a full.json-shaped object back: turns go to shards, everything else to
// full.json (which is small and cheap to rewrite).
bool SessionFileStore::save_full(const std::string& sid, const Json& full,
                                 std::string* err) const {
  Json head = Json::object();
  for (auto it = full.begin(); it != full.end(); ++it) {
    if (it.key() == "turns") continue;  // sharded
    head[it.key()] = it.value();
  }
  if (!jsonutil::write_atomic(full_path(sid).string(), head, err)) return false;
  return write_turns_sharded(ps_, sid,
                             full.contains("turns") ? full["turns"] : Json::array(), 0,
                             true, err);
}

FsResult SessionFileStore::append_turn(const std::string& sid, const Json& turn) {
  // Append without rewriting the whole history.
  //
  // The old body read full.json, pushed onto `turns` and re-wrote the entire
  // file - O(n) JSON work per turn, with the file growing without bound (the
  // user's session reached 90 KB in six turns). The count lives in full.json
  // (tiny), so an append only touches the LAST shard plus that head file.
  Json head;
  FsResult r = load_full(sid, &head);
  if (!r.ok()) return r;
  int count = static_cast<int>(head.contains("turns") && head["turns"].is_array()
                                   ? head["turns"].size() : 0);
  int shard_idx = count / kTurnsPerShard;

  // MIGRATION: a legacy session keeps its turns inline in full.json and has no
  // shard files. Append the new turn at the end of that inline array and let
  // save_full fan the whole thing out to shards; from then on every append
  // touches only the last shard.
  {
    std::error_code ec;
    bool have_shard = ghc::filesystem::is_regular_file(ps_.turn_shard(sid, 0), ec);
    if (!have_shard && count > 0) {
      head["turns"].push_back(turn);
      std::string merr;
      if (!save_full(sid, head, &merr)) {
        return FsResult::error(ErrCode::kIo, merr);
      }
      return FsResult::success();
    }
  }

  // Read only that shard, append, write it back.
  Json part = Json::array();
  {
    ghc::filesystem::path p = ps_.turn_shard(sid, shard_idx);
    std::error_code ec;
    if (ghc::filesystem::is_regular_file(p, ec)) {
      std::ifstream ifs(p.string(), std::ios::binary);
      if (ifs) {
        std::stringstream ss;
        ss << ifs.rdbuf();
        try {
          Json parsed = Json::parse(ss.str());
          if (parsed.is_array()) part = parsed;
        } catch (...) {
        }
      }
    }
  }
  part.push_back(turn);
  {
    std::error_code ec;
    ghc::filesystem::create_directories(ps_.turns_dir(sid), ec);
  }
  std::string err;
  if (!jsonutil::write_atomic(ps_.turn_shard(sid, shard_idx).string(), part, &err)) {
    return FsResult::error(ErrCode::kIo, err);
  }
  // The head file keeps no inline turns any more (they are in the shard now).
  if (head.contains("turns")) head.erase("turns");
  if (!jsonutil::write_atomic(full_path(sid).string(), head, &err)) {
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
  if (!save_full(sid, full, &err)) return FsResult::error(ErrCode::kIo, err);
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
  if (!save_full(sid, full, &err)) {
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

bool SessionFileStore::mark_seen(const std::string& sid, int64_t ms) const {
  if (sid.empty() || ms <= 0) return false;
  ghc::filesystem::path p = ps_.session_dir(sid) / "seen.at";
  std::error_code ec;
  ghc::filesystem::create_directories(p.parent_path(), ec);
  ghc::filesystem::path tmp = p;
  tmp += ".tmp";
  {
    std::ofstream out(tmp.string(), std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << ms << "\n";
    out.flush();
    if (!out) return false;
  }
  ghc::filesystem::rename(tmp, p, ec);
  if (ec) {
    ghc::filesystem::remove(p, ec);
    ec.clear();
    ghc::filesystem::rename(tmp, p, ec);
  }
  return !ec;
}

int64_t SessionFileStore::read_seen_ms(const std::string& sid) const {
  if (sid.empty()) return 0;
  std::ifstream in((ps_.session_dir(sid) / "seen.at").string(), std::ios::binary);
  if (!in) return 0;
  int64_t ms = 0;
  in >> ms;
  if (!in || ms <= 0) return 0;
  return ms;
}

ghc::filesystem::path SessionFileStore::todos_path(const std::string& sid) const {
  return ps_.session_dir(sid) / "todos.json";
}

ghc::filesystem::path SessionFileStore::compaction_path(const std::string& sid) const {
  return ps_.session_dir(sid) / "compaction.json";
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