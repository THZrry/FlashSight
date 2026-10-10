// fs_log.cpp - leveled logging to stderr
#include "fs_log.hpp"

#include <atomic>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace fs {
namespace log {

namespace {

std::atomic<Level> g_level{Level::kInfo};
std::mutex g_mu;

const char* level_name(Level lv) {
  switch (lv) {
    case Level::kDebug: return "debug";
    case Level::kInfo: return "info";
    case Level::kWarn: return "warn";
    case Level::kError: return "error";
  }
  return "?";
}

void write(Level lv, const std::string& msg) {
  if (!enabled(lv)) return;
  std::lock_guard<std::mutex> lk(g_mu);
  std::time_t t = std::time(nullptr);
  std::fprintf(stderr, "[fs:%s:%lld] %s\n", level_name(lv), (long long)t, msg.c_str());
  std::fflush(stderr);
}

}  // namespace

void set_level(Level lv) { g_level.store(lv); }
bool enabled(Level lv) { return lv >= g_level.load(); }

void debug(const std::string& msg) { write(Level::kDebug, msg); }
void info(const std::string& msg) { write(Level::kInfo, msg); }
void warn(const std::string& msg) { write(Level::kWarn, msg); }
void error(const std::string& msg) { write(Level::kError, msg); }

}  // namespace log
}  // namespace fs