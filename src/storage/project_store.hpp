// project_store.hpp - projects.json registry read/write
#pragma once

#include <string>
#include <vector>

#include <ghc/filesystem.hpp>

#include "fs_error.hpp"
#include "fs_json.hpp"

namespace fs {

// 项目注册表（session-history.md §1 projects.json）：{pid: {path, lastSession, lastActive}}
class ProjectRegistry {
 public:
  explicit ProjectRegistry(const ghc::filesystem::path& projects_file)
      : file_(projects_file) {}

  void load();

  // 按路径找项目（已规范化的路径比较）→ pid（空字符串=不存在）
  std::string find_by_path(const std::string& path) const;
  // 注册（新建项目）；已存在则更新 lastActive，返回 pid
  std::string register_project(const std::string& path, const std::string& pid);
  // pid → 项目对象（不存在返回 null json）
  nlohmann::json get(const std::string& pid) const;
  // 全量列表（按 lastActive 降序）
  nlohmann::json list() const;
  // 删除
  bool remove(const std::string& pid);
  void set_last_session(const std::string& path, const std::string& sid);
  int64_t count() const;

  // 持久化
  bool save(std::string* err);

 private:
  ghc::filesystem::path file_;
  nlohmann::json doc_ = nlohmann::json::object();
  std::string gen_pid();
};

}  // namespace fs