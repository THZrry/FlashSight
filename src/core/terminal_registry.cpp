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
  if (timed_out) j["timedOut"] = true;
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
  e.timed_out = jsonutil::get<bool>(j, "timedOut", false);
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
// Load a terminal RECORD from disk (a previous run's terminal).
//
// The process is gone by definition, so the entry is marked exited; it stays in
// the list so "打开过的终端" is still there after a restart, and its log can be
// replayed. Returns false when the id is already live (never overwrite reality
// with a stale file).
bool TerminalRegistry::add_saved(const Json& j) {
  TermEntry e = TermEntry::from_json(j);
  if (e.tid.empty()) return false;
  if (entries_.find(e.tid) != entries_.end()) return false;
  e.status = TermStatus::kExited;
  e.exit_code = jsonutil::get_int(j, "exitCode", e.exit_code);
  e.ended_at = jsonutil::get_int(j, "endedAt", e.ended_at);
  entries_[e.tid] = e;
  // Keep the id sequence above the loaded ones: a new terminal must not reuse a
  // historical id (its log file would be appended to).
  int64_t n = std::strtoll(e.tid.c_str(), nullptr, 10);
  if (n > 0 && n < 1000000000) {
    while (next_tid_ <= n) ++next_tid_;
  }
  return true;
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

void TerminalRegistry::mark_timed_out(const std::string& tid) {
  TermEntry* e = find(tid);
  if (e) e->timed_out = true;
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

FsResult TerminalRegistry::set_label(const std::string& tid, const std::string& label) {
  TermEntry* e = find(tid);
  if (!e) return FsResult::error(ErrCode::kNotFound, "terminal not found: " + tid);
  e->label = label;
  return FsResult::success(Json{{"tid", tid}, {"label", label}});
}

void TerminalRegistry::detach_log(const std::string& tid) {
  TermEntry* e = find(tid);
  if (!e) return;
  e->log_path.clear();
  e->log_attached = false;
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