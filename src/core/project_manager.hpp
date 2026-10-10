// project_manager.hpp - ProjectManager (10-appcore)
#pragma once

#include <string>

#include <ghc/filesystem.hpp>

#include "fs_error.hpp"
#include "fs_json.hpp"
#include "settings_store.hpp"
#include "storage/project_store.hpp"
#include "storage/session_store.hpp"

namespace fs {

// 项目管理：注册表 + 目录约定 + 会话设置提供
// FsApi: project.list/open/create/remove/get
class ProjectManager {
 public:
  ProjectManager(const ghc::filesystem::path& global_dir,
                 SettingsStore* settings)
      : global_dir_(global_dir), settings_(settings),
        registry_(global_dir / "projects.json") {}

  void load();
  bool save();

  // 打开/新建项目；返回 pid（已存在则复用）。create 会建目录
  FsResult open_project(const std::string& path);
  FsResult create_project(const std::string& path, const std::string& pid = "");
  FsResult remove_project(const std::string& pid);
  FsResult get_project(const std::string& pid) const;
  FsResult list_projects() const;

  // 项目路径 ↔ pid
  std::string pid_by_path(const std::string& path) const;
  ghc::filesystem::path project_path(const std::string& pid) const;
  ProjectStore store_for(const std::string& pid) const;

  // SettingsStore 引用（会话设置默认值从这里拿）
  SettingsStore* settings() { return settings_; }
  const SettingsStore* settings() const { return settings_; }

  ProjectRegistry& registry() { return registry_; }

 private:
  ghc::filesystem::path global_dir_;
  SettingsStore* settings_;
  ProjectRegistry registry_;
};

}  // namespace fs