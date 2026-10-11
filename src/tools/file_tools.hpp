// file_tools.hpp - 内置文件工具（roadmap 30-agent 内置工具）
// file.read（offset/limit+二进制守卫）/ ls / find-grep / write / diffWrite / rm
#pragma once

#include <string>

#include "tool_registry.hpp"

namespace fs {

// 注册全部文件工具到 registry（含 JSON schema）
void register_file_tools(ToolRegistry* registry);

}  // namespace fs