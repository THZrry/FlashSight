// mcp_tools.hpp - MCP 与 Skill 管理（本地工具，从外部文件夹导入）
#pragma once

#include <string>
#include <vector>

#include "tool_registry.hpp"

namespace fs {

// MCP/Skill 注册表：外部工具导入 + 启用开关（自定义工具启用变化进 HistoryBuilder 前缀）
class McpRegistry {
 public:
  // 从外部文件夹导入（claude code、open code 等路径预置由宿主提供）
  FsResult import_dir(const std::string& dir);
  bool enable(const std::string& name);
  bool disable(const std::string& name);
  std::vector<std::string> list() const;  // 名称列表
  Json list_json() const;                  // 详情（含启用态）

  // 启用态变化（HistoryBuilder 前缀用）
  std::vector<std::string> changed_since(const Json& snapshot) const;

 private:
  struct Entry {
    std::string name;
    std::string dir;
    bool enabled = true;
  };
  std::vector<Entry> entries_;
};

void register_mcp_tools(ToolRegistry* registry, McpRegistry* mcp);

}  // namespace fs