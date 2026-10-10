// test_framework.hpp - minimal unit test framework (no third-party dep)
#pragma once

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace fstest {

struct Case {
  std::string name;
  std::function<void()> fn;
};

std::vector<Case>& registry();
void register_case(const std::string& name, std::function<void()> fn);

// 失败计数（进程内全局）
extern int g_failures;
extern int g_checks;

bool check(bool cond, const char* expr, const char* file, int line);

int run_all();

// RAII temp dir：每个测试用例建独立临时目录，析构删除
class TempDir {
 public:
  TempDir(const std::string& tag);
  ~TempDir();
  const std::string& path() const { return path_; }

 private:
  std::string path_;
};

}  // namespace fstest

#define FS_TEST(name)                                                     \
  static void fs_test_##name();                                           \
  static const bool fs_test_reg_##name =                                  \
      (fstest::register_case(#name, fs_test_##name), true);               \
  static void fs_test_##name()

#define EXPECT_TRUE(x) fstest::check(static_cast<bool>(x), #x, __FILE__, __LINE__)
#define EXPECT_FALSE(x) fstest::check(!static_cast<bool>(x), "!(" #x ")", __FILE__, __LINE__)
#define EXPECT_EQ(a, b) fstest::check((a) == (b), #a " == " #b, __FILE__, __LINE__)
#define EXPECT_NE(a, b) fstest::check((a) != (b), #a " != " #b, __FILE__, __LINE__)
#define ASSERT_TRUE(x)                                                    \
  do {                                                                    \
    if (!fstest::check(static_cast<bool>(x), #x, __FILE__, __LINE__))     \
      return;                                                             \
  } while (0)