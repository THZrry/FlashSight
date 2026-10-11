// wake_scheduler.hpp - Suspender / WakeScheduler (agent-loop.md 4, addon1)
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "fs_clock.hpp"
#include "fs_error.hpp"
#include "fs_json.hpp"

namespace fs {

// 唤醒源（agent-loop.md 4）
enum class WakeSource {
  kUser,            // 用户输入
  kTerminalExit,    // 后台终端退出
  kTimer,           // 模型定时唤醒
  kSubagentDone,    // 子代理完成
};

const char* wake_source_str(WakeSource s);

// 待唤醒记录
struct PendingWake {
  std::string sid;
  WakeSource source = WakeSource::kUser;
  int64_t at_ms = 0;          // kTimer：到点时间；其余为 0（立即）
  std::string ref;            // tid / subagent id
  bool cancelled = false;
};

// WakeScheduler：模型自设唤醒（单次不设上限，用户可取消）；
// 项目级可见 + 输入框区显示（P4 UI；本阶段只给数据 + wake.fired 事件）
class WakeScheduler {
 public:
  using FireFn = std::function<void(const PendingWake&)>;

  explicit WakeScheduler(const Clock* clock = nullptr);

  // schedule：模型自设一次唤醒；at_ms<=0 为"下次立即"
  PendingWake schedule(const std::string& sid, WakeSource source, int64_t at_ms,
                       const std::string& ref = "");
  // cancel：取消待唤醒（用户手动）
  bool cancel(const std::string& sid);
  // due：到点的唤醒（on_timer 时调；返回触发的列表并清除）
  std::vector<PendingWake> due(int64_t now_ms);

  // 项目级可见
  std::vector<PendingWake> pending_for_sid(const std::string& sid) const;
  bool has_pending(const std::string& sid) const;

  void set_fire_fn(FireFn fn) { fire_fn_ = std::move(fn); }

 private:
  const Clock* clock_;
  WallClock default_clock_;
  std::vector<PendingWake> wakes_;
  FireFn fire_fn_;
};

}  // namespace fs