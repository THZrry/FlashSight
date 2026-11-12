// test_path_range.cpp - greedy path-interval parsing for composer completion.
//
// The old implementation took the whitespace token around the caret. These
// cases are exactly where that failed: a path glued to Chinese prose, a path
// followed by Chinese punctuation, a Windows drive path, a URL that must NOT be
// treated as a path, and a caret in the middle of an interval.
#include "test_framework.hpp"

#include "ui/ui_path_range.hpp"

#include <string>

using namespace fs::ui;

namespace {

std::string slice(const std::string& s, const PathRange& r) {
  return s.substr(r.start, r.end - r.start);
}

}  // namespace

FS_TEST(path_ranges_glued_to_cjk_prose) {
  std::string s = "看下./src/ui/ma";
  std::vector<PathRange> rs = parse_path_ranges(s);
  ASSERT_TRUE(rs.size() == 1);
  EXPECT_EQ(slice(s, rs[0]), std::string("./src/ui/ma"));
}

FS_TEST(path_ranges_find_ascii_path_inside_cjk_prefix) {
  std::string s = "这是src/ui，然后";
  std::vector<PathRange> rs = parse_path_ranges(s);
  ASSERT_TRUE(rs.size() == 1);
  EXPECT_EQ(slice(s, rs[0]), std::string("src/ui"));
}

FS_TEST(path_ranges_windows_drive_and_spaces) {
  std::string s = "打开 C:\\aaa\\FlashSight\\src 这个";
  std::vector<PathRange> rs = parse_path_ranges(s);
  ASSERT_TRUE(rs.size() == 1);
  EXPECT_EQ(slice(s, rs[0]), std::string("C:\\aaa\\FlashSight\\src"));
}

FS_TEST(path_ranges_drive_glued_to_cjk) {
  std::string s = "看C:/tmp/x";
  std::vector<PathRange> rs = parse_path_ranges(s);
  ASSERT_TRUE(rs.size() == 1);
  EXPECT_EQ(slice(s, rs[0]), std::string("C:/tmp/x"));
}

FS_TEST(path_ranges_skip_urls) {
  std::string s = "看 https://example.com/a 和 src/ui";
  std::vector<PathRange> rs = parse_path_ranges(s);
  ASSERT_TRUE(rs.size() == 1);
  EXPECT_EQ(slice(s, rs[0]), std::string("src/ui"));
}

FS_TEST(path_ranges_stop_at_quotes_and_punctuation) {
  std::string s = "看 \"src/ui\"、文档/项目。";
  std::vector<PathRange> rs = parse_path_ranges(s);
  ASSERT_TRUE(rs.size() == 2);
  EXPECT_EQ(slice(s, rs[0]), std::string("src/ui"));
  EXPECT_EQ(slice(s, rs[1]), std::string("文档/项目"));
}

FS_TEST(path_ranges_multiple_and_caret_hit) {
  std::string s = "a/b 和 c/d";
  std::vector<PathRange> rs = parse_path_ranges(s);
  ASSERT_TRUE(rs.size() == 2);
  size_t caret_in_second = s.find("c/d") + 1;
  EXPECT_EQ(path_range_at(rs, caret_in_second), 1);
  // At the end of the first path the marker belongs to THAT path.
  EXPECT_EQ(path_range_at(rs, s.find("a/b") + 3), 0);
  // Between the paths: no completion position.
  EXPECT_EQ(path_range_at(rs, s.find(" 和 ") + 1), -1);
}

FS_TEST(path_ranges_prose_without_separator_is_not_a_path) {
  std::vector<PathRange> rs = parse_path_ranges("这是一句普通的话");
  EXPECT_TRUE(rs.empty());
  rs = parse_path_ranges("and or 并且");
  EXPECT_TRUE(rs.empty());
}
