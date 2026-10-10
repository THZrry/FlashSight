// settings_store.hpp - two-level settings (global + project), source-tagged
// 10-appcore「SettingsStore」；settings.* FsApi 来源标注
#pragma once

#include <map>
#include <string>
#include <vector>

#include <ghc/filesystem.hpp>

#include "fs_error.hpp"
#include "fs_json.hpp"
#include "fs_types.hpp"

namespace fs {

// 设置键（全局/项目两级；冲突项目优先，可 ignoreGlobal）
// 键组织：使用点分路径，如 "permissions.file.blacklist"、"terminal.shell"
constexpr char kKeyNewSessionModel[] = "session.default.model";
constexpr char kKeyNewSessionThinking[] = "session.default.thinking";
constexpr char kKeyNewSessionFilePerm[] = "session.default.filePerm";
constexpr char kKeyNewSessionAutoMode[] = "session.default.autoMode";
constexpr char kKeyNewSessionEnterSend[] = "session.default.enterSend";
constexpr char kKeyTerminalShell[] = "terminal.shell";           // 显式指定（空=自动探测）
constexpr char kKeyTerminalEncoding[] = "terminal.encoding";     // 空=系统默认
constexpr char kKeyPermNetworkEnabled[] = "permissions.network.enabled";  // 网络总开关
constexpr char kKeyPermNetworkVerifyCert[] = "permissions.network.verifyCert";
constexpr char kKeyPermTerminalEnabled[] = "permissions.terminal.enabled";
constexpr char kKeyPermAutoGitInit[] = "permissions.files.autoGitInit";
constexpr char kKeyPermFileBlacklist[] = "permissions.files.blacklist";   // string[]
constexpr char kKeyPermFileWhitelist[] = "permissions.files.whitelist";   // string[]
constexpr char kKeyEnvVars[] = "environment.vars";               // {name: {value, source}}
constexpr char kKeyCliTools[] = "environment.cliTools";          // {key: {path, source}}
constexpr char kKeyUiLang[] = "ui.language";                     // "auto|zh|en"

// 返回时带来源标注
struct SettingValue {
  nlohmann::json value;
  std::string source;  // "global" | "project" | "merged-default"
  bool global_set = false;
  bool project_set = false;
};

class SettingsStore {
 public:
  SettingsStore(const ghc::filesystem::path& global_dir,
                const ghc::filesystem::path& project_dir,
                bool allow_global_read = true);

  // 读取：项目覆盖全局（除非 ignoreGlobal），未设返回 merged-default
  SettingValue get(const std::string& key) const;
  nlohmann::json get_value(const std::string& key,
                           const nlohmann::json& def) const;

  // 写入全局或项目级
  FsResult set_global(const std::string& key, const nlohmann::json& value);
  FsResult set_project(const std::string& key, const nlohmann::json& value);
  // 删除（恢复默认）
  FsResult unset_global(const std::string& key);
  FsResult unset_project(const std::string& key);

  // 项目级 ignoreGlobal 开关
  FsResult set_ignore_global(bool ignore);
  bool ignore_global() const { return ignore_global_; }

  // 持久化（加载与保存；保存 = 原子写）
  void load();
  FsResult save_global();
  FsResult save_project();  // 项目级为空也可写

  // 全量导出（设置页用，带来源）
  nlohmann::json dump_global() const { return global_->doc; }
  nlohmann::json dump_project() const { return project_->doc; }

 private:
  struct Doc {
    ghc::filesystem::path path;
    nlohmann::json doc = nlohmann::json::object();
  };

  ghc::filesystem::path global_dir_;
  ghc::filesystem::path project_dir_;
  bool allow_global_read_ = true;
  bool ignore_global_ = false;
  Doc* global_ = nullptr;
  Doc* project_ = nullptr;

  Doc global_doc_;
  Doc project_doc_;

  ghc::filesystem::path global_file() const;
  ghc::filesystem::path project_file() const;

  FsResult persist(Doc& d);
  void load_doc(Doc& d);
};

// 取得差值键列表（调试用）
std::vector<std::string> diff_keys(const nlohmann::json& a, const nlohmann::json& b);

}  // namespace fs