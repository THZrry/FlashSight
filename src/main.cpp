// FlashSight desktop entry
#include <cstdio>
#include "common/fs_version.hpp"

int main() {
  std::printf("FlashSight %s (skeleton)\n", fs::version_string());
  return 0;
}