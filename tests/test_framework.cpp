// test_framework.cpp - minimal test framework implementation + main
#include "test_framework.hpp"

#include <cstdio>
#include <cstdlib>

#include <ghc/filesystem.hpp>

#if defined(_WIN32)
#include <process.h>
#define fs_getpid _getpid
#else
#include <unistd.h>
#define fs_getpid getpid
#endif

namespace fstest {

int g_failures = 0;
int g_checks = 0;

std::vector<Case>& registry() {
  static std::vector<Case> cases;
  return cases;
}

void register_case(const std::string& name, std::function<void()> fn) {
  registry().push_back(Case{name, std::move(fn)});
}

bool check(bool cond, const char* expr, const char* file, int line) {
  ++g_checks;
  if (!cond) {
    ++g_failures;
    std::printf("  FAIL %s:%d  %s\n", file, line, expr);
  }
  return cond;
}

TempDir::TempDir(const std::string& tag) {
  ghc::filesystem::path base = ghc::filesystem::temp_directory_path() /
                               ("fs_tests_" + tag + "_" + std::to_string(fs_getpid()));
  std::error_code ec;
  ghc::filesystem::remove_all(base, ec);
  ghc::filesystem::create_directories(base, ec);
  path_ = base.string();
}

TempDir::~TempDir() {
  std::error_code ec;
  ghc::filesystem::remove_all(ghc::filesystem::path(path_), ec);
}

int run_all() {
  // unbuffered: a crashing case must still leave its name in the log
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  int case_fail = 0;
  for (auto& c : registry()) {
    int before = g_failures;
    std::printf("[ RUN  ] %s\n", c.name.c_str());
    c.fn();
    if (g_failures > before) {
      ++case_fail;
      std::printf("[ FAIL ] %s\n", c.name.c_str());
    } else {
      std::printf("[  OK  ] %s\n", c.name.c_str());
    }
  }
  std::printf("----\ncases: %d, failed cases: %d, checks: %d, failed checks: %d\n",
              (int)registry().size(), case_fail, g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}

}  // namespace fstest

int main() { return fstest::run_all(); }