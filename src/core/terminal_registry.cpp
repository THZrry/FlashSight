// terminal_registry.cpp - TerminalRegistry implementation
#include "terminal_registry.hpp"

#include <algorithm>

#include "fs_serialize.hpp"

namespace fs {

Json TermEntry::to_json() const {
  Json j = Json::object();
  j["tid"] = tid;
  j["sid"] = sid;
  j["kind"] = term_kind_str(kind);
  j["shell"] = shell;
  if (!label.empty()) j["label"] = label;
  j["cwd"] = cwd;
  j["encoding"] = encoding;
  j["status"] = term_status_str(status);
  j["exitCode"] = exit_code;
  j["startedAt"] = started_at;
  j["endedAt"] = ended_at;
  j["wakeOnExit"] = wake_on_exit;
  j["logAttached"] = log_attached;
  if (!log_path.empty()) j["logPath"] = log_path;
  return j;
}

TermEntry TermEntry::from_json(const Json& j) {
  TermEntry e;
  e.tid = jsonutil::get<std::string>(j, "tid", "");
  e.sid = jsonutil::get<std::string>(j, "sid", "");
  std::string k = jsonutil::get<std::string>(j, "kind", "user");
  e.kind = k == "model-fg" ? TermKind::kModelFg
           : k == "model-bg" ? TermKind::kModelBg
                             : TermKind::kUser;
  e.shell = jsonutil::get<std::string>(j, "shell", "");
  e.label = jsonutil::get<std::string>(j, "label", "");
  e.cwd = jsonutil::get<std::string>(j, "cwd", "");
  e.encoding = jsonutil::get<std::string>(j, "encoding", "");
  std::string s = jsonutil::get<std::string>(j, "status", "running");
  e.status = s == "exited" ? TermStatus::kExited
             : s == "killed" ? TermStatus::kKilled
                             : TermStatus::kRunning;
  e.exit_code = static_cast<int>(jsonutil::get_int(j, "exitCode", 0));
  e.started_at = jsonutil::get_int(j, "startedAt", 0);
  e.ended_at = jsonutil::get_int(j, "endedAt", 0);
  e.wake_on_exit = jsonutil::get<bool>(j, "wakeOnExit", false);
  e.log_attached = jsonutil::get<bool>(j, "logAttached", false);
  e.log_path = jsonutil::get<std::string>(j, "logPath", "");
  return e;
}

TerminalRegistry::TerminalRegistry(const Clock* clock)
    : clock_(clock), default_clock_() {
  if (!clock_) clock_ = &default_clock_;
}

TermEntry TerminalRegistry::create(const std::string& sid, TermKind kind,
                                   const std::string& shell, const std::string& cwd,
                                   const std::string& encoding,
                                   bool wake_on_exit, int64_t now_ms,
                                   const std::string& label) {
  std::string tid;
  do {
    tid = std::to_string(next_tid_++);
  } while (entries_.find(tid) != entries_.end());
  TermEntry e;
  e.tid = tid;
  e.sid = sid;
  e.kind = kind;
  e.shell = shell;
  e.label = label;
  e.cwd = cwd;
  e.encoding = encoding;
  e.status = TermStatus::kRunning;
  e.started_at = now_ms ? now_ms : clock_->now_ms();
  e.wake_on_exit = wake_on_exit;
  entries_[tid] = e;
  return e;
}

TermEntry* TerminalRegistry::find(const std::string& tid) {
  auto it = entries_.find(tid);
  return it == entries_.end() ? nullptr : &it->second;
}
const TermEntry* TerminalRegistry::find(const std::string& tid) const {
  auto it = entries_.find(tid);
  return it == entries_.end() ? nullptr : &it->second;
}

FsResult TerminalRegistry::mark_exited(const std::string& tid, int exit_code,
                                       int64_t now_ms) {
  TermEntry* e = find(tid);
  if (!e) return FsResult::error(ErrCode::kNotFound, "terminal not found: " + tid);
  e->status = TermStatus::kExited;
  e->exit_code = exit_code;
  e->ended_at = now_ms ? now_ms : clock_->now_ms();
  return FsResult::success();
}

FsResult TerminalRegistry::promote_to_background(const std::string& tid, int64_t now_ms) {
  TermEntry* e = find(tid);
  if (!e) return FsResult::error(ErrCode::kNotFound, "terminal not found: " + tid);
  if (e->kind == TermKind::kModelFg) {
    e->kind = TermKind::kModelBg;
    e->wake_on_exit = true;
  }
  (void)now_ms;
  return FsResult::success();
}

FsResult TerminalRegistry::set_encoding(const std::string& tid, const std::string& enc) {
  TermEntry* e = find(tid);
  if (!e) return FsResult::error(ErrCode::kNotFound, "terminal not found: " + tid);
  e->encoding = enc;
  return FsResult::success();
}

std::vector<TermEntry> TerminalRegistry::list_for_session(const std::string& sid) const {
  std::vector<TermEntry> out;
  for (auto& kv : entries_) {
    if (kv.second.sid == sid) out.push_back(kv.second);
  }
  std::sort(out.begin(), out.end(),
            [](const TermEntry& a, const TermEntry& b) { return a.tid < b.tid; });
  return out;
}

size_t TerminalRegistry::count_running(const std::string& sid) const {
  size_t n = 0;
  for (auto& kv : entries_) {
    if (kv.second.sid == sid && kv.second.status == TermStatus::kRunning) ++n;
  }
  return n;
}

size_t TerminalRegistry::total_for_session(const std::string& sid) const {
  size_t n = 0;
  for (auto& kv : entries_) {
    if (kv.second.sid == sid) ++n;
  }
  return n;
}

}  // namespace fs