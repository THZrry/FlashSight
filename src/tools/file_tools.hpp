// file_tools.hpp - 内置文件工具（roadmap 30-agent 内置工具）
//
// read_file（分页 + hash + 读取守卫）/ ls / find-grep / write（整文件，新建用）/
// edit_file（行号替换 + hash + expect 双重校验，主要编辑路径）/ rm。
// 编辑协议与"为什么同时要 hash 和 expect"见 file_tools.cpp 顶部注释。
#pragma once

#include <string>

#include "tool_registry.hpp"

namespace fs {

// 注册全部文件工具到 registry（含 JSON schema）
void register_file_tools(ToolRegistry* registry);

}  // namespace fs