// fs_clock.cpp - wall clock impl
#include "fs_clock.hpp"

#include <chrono>

namespace fs {

ClockMs WallClock::now_ms() const {
  using namespace std::chrono;
  auto now = system_clock::now().time_since_epoch();
  return duration_cast<milliseconds>(now).count();
}

}  // namespace fs