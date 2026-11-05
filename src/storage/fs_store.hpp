// fs_store.hpp - global & project file paths/schema (session-history.md §1)
#pragma once

#include <string>

#include <ghc/filesystem.hpp>

#include "fs_error.hpp"
#include "fs_json.hpp"

namespace fs {

// 全局文件布局（session-history.md §1）
struct GlobalPaths {
  ghc::filesystem::path root;             // ~/.flashsight
  ghc::filesystem::path config;           // config.json
  ghc::filesystem::path providers;        // providers.json
  ghc::filesystem::path projects;         // projects.json
  ghc::filesystem::path usage;            // usage/calls.jsonl 及其目录

  static GlobalPaths resolve();
};

// 项目级布局
struct ProjectStore {
  ghc::filesystem::path project_root;     // <project>
  ghc::filesystem::path data_dir;         // <project>/.flashsight
  ghc::filesystem::path project_json;     // project.json
  ghc::filesystem::path sessions_dir;     // sessions/

  // 会话路径
  ghc::filesystem::path session_dir(const std::string& sid) const;
  ghc::filesystem::path full_json(const std::string& sid) const;
  // 历史分片：老的 turns 落在 turns/000.json、001.json…，full.json 只留
  // meta/settings/最近一段。见 session-history.md §2.1。
  ghc::filesystem::path turns_dir(const std::string& sid) const;
  ghc::filesystem::path turn_shard(const std::string& sid, int index) const;
  ghc::filesystem::path runtime_json(const std::string& sid) const;
  ghc::filesystem::path patches_dir(const std::string& sid) const;
  ghc::filesystem::path patch(const std::string& sid, const std::string& turn_id) const;
  ghc::filesystem::path terminals_dir(const std::string& sid) const;
  ghc::filesystem::path terminal_log(const std::string& sid, const std::string& tid) const;
  // 规划文档：<project>/.flashsight/plans/<sid>.md
  // The plan is a DOCUMENT (addon2 长任务-计划), not a JSON step tree: the model
  // writes it, re-reads it each round, derives the task list from it, and marks
  // it complete when the work is done.
  ghc::filesystem::path plans_dir() const;
  ghc::filesystem::path plan_file(const std::string& sid) const;

  static ProjectStore of(const ghc::filesystem::path& project_root);
};

}  // namespace fs