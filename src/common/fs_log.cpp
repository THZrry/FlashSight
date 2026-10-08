// FlashSight skeleton common unit: logging stub (fs_log)
#include <cstdio>
#include <ctime>
#include <mutex>

namespace fs {
namespace log {

namespace {
std::mutex g_mu;
}

void debug(const char* msg) {
  std::lock_guard<std::mutex> lk(g_mu);
  std::time_t t = std::time(nullptr);
  std::fprintf(stderr, "[fs %lld] %s\n", (long long)t, msg);
}

}  // namespace log
}  // namespace fs