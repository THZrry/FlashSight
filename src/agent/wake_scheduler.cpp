// wake_scheduler.cpp - WakeScheduler implementation
#include "wake_scheduler.hpp"

namespace fs {

const char* wake_source_str(WakeSource s) {
  switch (s) {
    case WakeSource::kUser: return "user";
    case WakeSource::kTerminalExit: return "terminal-exit";
    case WakeSource::kTimer: return "timer";
    case WakeSource::kSubagentDone: return "subagent-done";
  }
  return "user";
}

WakeScheduler::WakeScheduler(const Clock* clock)
    : clock_(clock), default_clock_() {
  if (!clock_) clock_ = &default_clock_;
}

PendingWake WakeScheduler::schedule(const std::string& sid, WakeSource source,
                                    int64_t at_ms, const std::string& ref) {
  cancel(sid);  // 同会话同时只有一个待唤醒
  PendingWake w;
  w.sid = sid;
  w.source = source;
  w.at_ms = at_ms;
  w.ref = ref;
  w.cancelled = false;
  wakes_.push_back(w);
  return w;
}

bool WakeScheduler::cancel(const std::string& sid) {
  bool found = false;
  for (auto& w : wakes_) {
    if (w.sid == sid && !w.cancelled) {
      w.cancelled = true;
      found = true;
    }
  }
  return found;
}

std::vector<PendingWake> WakeScheduler::due(int64_t now_ms) {
  std::vector<PendingWake> out;
  std::vector<PendingWake> keep;
  for (auto& w : wakes_) {
    if (w.cancelled) continue;
    if (w.source == WakeSource::kTimer && w.at_ms > now_ms) {
      keep.push_back(w);
      continue;
    }
    out.push_back(w);
    if (fire_fn_) fire_fn_(w);
  }
  wakes_.swap(keep);
  return out;
}

std::vector<PendingWake> WakeScheduler::pending_for_sid(const std::string& sid) const {
  std::vector<PendingWake> out;
  for (auto& w : wakes_) {
    if (w.sid == sid && !w.cancelled) out.push_back(w);
  }
  return out;
}

bool WakeScheduler::has_pending(const std::string& sid) const {
  for (auto& w : wakes_) {
    if (w.sid == sid && !w.cancelled) return true;
  }
  return false;
}

}  // namespace fs