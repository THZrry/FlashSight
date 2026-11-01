// mcp_tools.hpp - MCP 与 Skill 管理（本地工具，从外部文件夹导入）
#pragma once

#include <string>
#include <vector>

#include "tool_registry.hpp"

namespace fs {

// MCP/Skill 注册表：外部工具导入 + 启用开关（自定义工具启用变化进 HistoryBuilder 前缀）
//
// MCP and Skill are different things and must not be conflated:
//   * an MCP SERVER is a protocol endpoint (a command to spawn or a URL),
//     declared in a config file (`mcpServers` / `mcp.servers`); the MODEL calls
//     the tools it exposes.
//   * a SKILL is a markdown instruction set the USER invokes with `/name`.
// The old registry named both "mcp" and probed SKILL directories, so a skill
// folder showed up under MCP. Entries now carry a `kind` and the server config.
class McpRegistry {
 public:
  // 从外部文件夹导入（claude code、open code 等路径预置由宿主提供）
  FsResult import_dir(const std::string& dir);
  // 从一个 MCP 配置文件导入：识别 `mcpServers`（Claude）与 `mcp.servers` /
  // `servers`（opencode 等）。返回新增条目数（0 = 文件里没有服务器定义）。
  int import_config_file(const std::string& path);
  // 新增一条（名称 + 目录；同名更新目录）
  void upsert(const std::string& name, const std::string& dir, bool enabled = true);
  // 新增/更新一条服务器定义（command 或 url + 参数）
  void upsert_server(const std::string& name, const Json& server);
  bool enable(const std::string& name);
  bool disable(const std::string& name);
  bool set_enabled(const std::string& name, bool enabled);
  bool remove(const std::string& name);
  std::vector<std::string> list() const;  // 名称列表
  Json list_json() const;                  // 详情（含启用态）
  // 整表读写（宿主持持久化到设置；重启后恢复）
  Json dump() const;
  void load(const Json& arr);
  // True when a stored entry is really a SKILL (its dir lives under a skills /
  // commands folder). Legacy tables still hold these from when skill directories
  // were imported through the MCP page - which is why "gsd" appeared under MCP.
  // The host uses this at load time to move them to the skill registry.
  static bool is_skill_dir_entry(const Json& entry);

  // 启用态变化（HistoryBuilder 前缀用）
  std::vector<std::string> changed_since(const Json& snapshot) const;

 private:
  struct Entry {
    std::string name;
    std::string dir;             // 目录导入的来源（仅 skill 式条目）
    std::string kind = "server"; // "server"（MCP 定义）| "dir"（目录导入）
    Json server = Json::object();// {command|url, args, env, ...}
    bool enabled = true;
  };
  std::vector<Entry> entries_;
};

void register_mcp_tools(ToolRegistry* registry, McpRegistry* mcp);

}  // namespace fs