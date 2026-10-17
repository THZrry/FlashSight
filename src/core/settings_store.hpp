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
// --- P3 补齐（goal.md 设置面；UI 在 P4 落地，键先行）---
// 模型偏好（子代理/摘要模型的通用偏好）
constexpr char kKeyModelPrefSubagent[] = "model.pref.subagent";   // {model, thinking}
constexpr char kKeyModelPrefSummary[] = "model.pref.summary";     // {model} 空=继承会话模型
// 逐模型设置（provider.md §4：用户自定义 > API > 预置 > 保守缺省）。
// 形状：{providerId: [ModelEntry...]}；存的是**用户改过**的模型，
// 所以"是否启用/能力标签/思考档位/重试"这些覆盖项都在这里，与 provider 连接分开。
constexpr char kKeyProviderModels[] = "provider.models";
// 子代理开关/模型（settings-子代理；预置名见 builtin_preset_agents）
constexpr char kKeySubagents[] = "subagents";                     // {name: {enabled, model}}
// 长期记忆（provider.md 5：用户偏好管理）
constexpr char kKeyLongMemory[] = "model.longMemory";             // {key: value}
// 外观（设置-通用-外观）
// 调色板与深浅是两个维度：ui.theme 选"哪套配色"，ui.mode 选"这套配色的浅色/深色"
// （auto = 跟随系统）。拆开之后每套配色都自动有浅深两面。
constexpr char kKeyUiTheme[] = "ui.theme";                        // 调色板名
constexpr char kKeyUiMode[] = "ui.mode";                          // "auto|light|dark"
constexpr char kKeyUiFont[] = "ui.font";                          // {family, size, monoFamily}
constexpr char kKeyUiCodeHighlight[] = "ui.codeHighlight";        // bool
constexpr char kKeyUiTabs[] = "ui.tabs";                          // "top|left" 标签页位置
constexpr char kKeyUiTabTooltip[] = "ui.tabTooltip";              // bool 悬停显示完整会话名
constexpr char kKeyUiMaxTabs[] = "ui.maxTabs";                    // 会话标签上限（0=不限）
// 浏览器控制（设置-通用-浏览器控制）
constexpr char kKeyBrowserEnabled[] = "browser.enabled";
constexpr char kKeyBrowserDriver[] = "browser.driver";            // driver 路径/名
constexpr char kKeyBrowserPerms[] = "browser.permissions";       // {net, certs}
// 电脑控制（设置-通用-电脑控制）
constexpr char kKeyComputerEnabled[] = "computer.enabled";
constexpr char kKeyComputerScreenshot[] = "computer.screenshot";  // 截图开关
// 快捷键（设置-其他-快捷键；v1 只做可配表）
constexpr char kKeyShortcuts[] = "shortcuts";                     // {action: keys}
// 用量保留天数（usage.md 3；默认 90）
constexpr char kKeyUsageRetainDays[] = "usage.retainDays";
// HistoryBuilder 阈值（session-history.md 3；N 与 token 预算进设置）
constexpr char kKeyHistoryRecentTurns[] = "history.recentTurns";  // 默认 20
constexpr char kKeyHistoryTokenBudget[] = "history.tokenBudget";  // 默认 8000
// 防过拟合开关（agent-loop.md 5；默认关）
constexpr char kKeyOverfitEnabled[] = "session.overfitEnabled";
// 子代理上限（agent-loop.md 6；默认并行 4/深度 3）
constexpr char kKeySubagentMaxParallel[] = "subagents.maxParallel";
constexpr char kKeySubagentMaxDepth[] = "subagents.maxDepth";
// 思考语言（addon2：工具 skill 中英双语，切换输出语言；"auto|zh|en"）
constexpr char kKeyThinkingLang[] = "model.thinkingLang";
// LSP 服务器表（addon2 依赖特定环境）：{lang: {command, args[]}}；可自动在 PATH 搜索
constexpr char kKeyLspServers[] = "lsp.servers";
// MCP / 外部工具注册表（[{name, dir, enabled}]；重启后恢复）
constexpr char kKeyMcpEntries[] = "mcp.entries";
// 用户 skill 表（[{name,title,description,body,dir,enabled}]；内置 skill 是代码，
// 不进设置，只有用户导入/新建的才需要持久化）
constexpr char kKeySkills[] = "skills.entries";
// 网络代理（httplib 不读系统代理；在此显式设置）
constexpr char kKeyNetworkProxyHost[] = "permissions.network.proxyHost";
constexpr char kKeyNetworkProxyPort[] = "permissions.network.proxyPort";

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

  // 环境变量合并（terminal.md 5：全局 + 项目覆盖，ignoreGlobal 时只取项目）
  // 返回 {name: value}；附带来源标注版本见 merged_env_sourced
  std::map<std::string, std::string> merged_env() const;
  // CLI 工具路径合并（同规则；返回 {key: path}）
  std::map<std::string, std::string> merged_cli_tools() const;

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

  // 把 ui.language 推给 i18n 层（load 之后与 set_global 时各调一次）
  void sync_language();

  FsResult persist(Doc& d);
  void load_doc(Doc& d);
};

// 取得差值键列表（调试用）
std::vector<std::string> diff_keys(const nlohmann::json& a, const nlohmann::json& b);

}  // namespace fs