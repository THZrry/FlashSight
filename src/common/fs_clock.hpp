// fs_clock.hpp - injectable clock (arch.md §6: 单测不碰真时钟)
#pragma once

#include <cstdint>

namespace fs {

using ClockMs = int64_t;

// 可注入时钟接口；单测用假时钟，生产用 WallClock
class Clock {
 public:
  virtual ~Clock() = default;
  virtual ClockMs now_ms() const = 0;
};

// 生产时钟：基于 std::chrono 系统墙钟
class WallClock final : public Clock {
 public:
  ClockMs now_ms() const override;
};

}  // namespace fs