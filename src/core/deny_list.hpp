// deny_list.hpp - 黑白名单（permissions.md §3）
#pragma once

#include <string>
#include <vector>

#include <ghc/filesystem.hpp>

#include "fs_json.hpp"

namespace fs {

// 名单匹配结果：黑名单 > 白名单 > 三档默认
struct PathMatch {
  bool denied = false;         // 命中黑名单 → 拒绝（deny-list）
  bool whitelist_allow = false; // 命中白名单 → 放行（无视默认档）
  std::string matched;         // 命中的名单项（可解释）
};

// 黑白名单判定器：黑/白各一组路径前缀（glob 简单前缀匹配：** 结尾通配）
class DenyList {
 public:
  void set_blacklist(const std::vector<std::string>& paths);
  void set_whitelist(const std::vector<std::string>& paths);
  std::vector<std::string> blacklist() const;
  std::vector<std::string> whitelist() const;

  // 判定给定规范化绝对路径
  PathMatch match(const ghc::filesystem::path& p) const;

  // 从设置 JSON（数组）加载
  void from_json(const Json& json);

 private:
  std::vector<ghc::filesystem::path> black_;
  std::vector<ghc::filesystem::path> white_;
  bool path_matches(const ghc::filesystem::path& p,
                    const std::vector<ghc::filesystem::path>& list,
                    std::string* matched) const;
};

}  // namespace fs