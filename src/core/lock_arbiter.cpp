// lock_arbiter.cpp - LockArbiter (permissions.md §2)
#include "lock_arbiter.hpp"

namespace fs {

// 局部默认时钟兜底（构造时没有注入就用它）
namespace {
WallClock g_default_clock;
}  // namespace

LockArbiter::LockArbiter(const Clock* clock, int64_t timeout_ms)
    : clock_(clock), timeout_ms_(timeout_ms) {
  if (!clock_) clock_ = &g_default_clock;
}bool LockArbiter::try_acquire(const std::string& project_key, const std::string& sid,
                              bool is_full_perm, int64_t now_ms, std::string* locked_by) {
  // full 权限不设会话写锁
  if (is_full_perm) return true;
  sweep_expired(now_ms);
  auto it = locks_.find(project_key);
  if (it != locks_.end() && it->second.sid != sid) {
    if (locked_by) *locked_by = it->second.sid;
    return false;
  }
  locks_[project_key] = Lock{sid, now_ms, false};
  return true;
}

void LockArbiter::release(const std::string& project_key, const std::string& sid) {
  auto it = locks_.find(project_key);
  if (it != locks_.end() && it->second.sid == sid) {
    locks_.erase(it);
  }
}

void LockArbiter::release_all_for(const std::string& sid) {
  for (auto it = locks_.begin(); it != locks_.end();) {
    if (it->second.sid == sid) it = locks_.erase(it);
    else ++it;
  }
}

std::string LockArbiter::holder(const std::string& project_key) const {
  auto it = locks_.find(project_key);
  if (it == locks_.end()) return "";
  return it->second.sid;
}

bool LockArbiter::is_held_by(const std::string& project_key, const std::string& sid) const {
  return holder(project_key) == sid;
}

void LockArbiter::sweep_expired(int64_t now_ms) {
  if (timeout_ms_ <= 0 || now_ms <= 0) return;
  for (auto it = locks_.begin(); it != locks_.end();) {
    if (it->second.full) {  // full 不设锁，不应出现在这里
      it = locks_.erase(it);
      continue;
    }
    if (now_ms - it->second.acquired_at > timeout_ms_) {
      it = locks_.erase(it);
    } else {
      ++it;
    }
  }
}

}  // namespace fs