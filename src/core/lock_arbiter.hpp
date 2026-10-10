// lock_arbiter.hpp - LockArbiter (permissions.md §2)
#pragma once

#include <map>
#include <string>

#include "fs_clock.hpp"

namespace fs {

// 会话写锁：非完整权限（approve/permissive）同项目同一时间只允许一个会话可写。
// 完整权限（full）不设锁（走 OS 语义）。锁随会话关闭/切换/超时兜底释放。
class LockArbiter {
 public:
  LockArbiter(const Clock* clock, int64_t timeout_ms = 30 * 60 * 1000);

  // 试图以某会话、某项目、某权限档获取写锁。
  // 返回 true=获得；false=被其他会话持有（输出持有者 sid）。
  bool try_acquire(const std::string& project_key, const std::string& sid,
                   bool is_full_perm, int64_t now_ms, std::string* locked_by);

  // 释放：会话离开项目时调用；幂等。
  void release(const std::string& project_key, const std::string& sid);
  void release_all_for(const std::string& sid);

  // 查询持有者（空串=无）
  std::string holder(const std::string& project_key) const;
  bool is_held_by(const std::string& project_key, const std::string& sid) const;

  // 兜底：超时释放（遍历过期的锁）
  void sweep_expired(int64_t now_ms);

 private:
  struct Lock {
    std::string sid;
    int64_t acquired_at = 0;
    bool full = false;
  };
  const Clock* clock_;
  int64_t timeout_ms_;
  std::map<std::string, Lock> locks_;
};

}  // namespace fs