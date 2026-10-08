// FlashSight test runner skeleton
// Real unit tests land in P1 (after FsApi is finalized).
#include <cstdio>
#include "common/fs_version.hpp"

int main() {
  std::printf("fs_tests skeleton OK (core %s)\n", fs::version_string());
  return 0;
}