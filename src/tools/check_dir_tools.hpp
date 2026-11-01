// check_dir_tools.hpp - check_dir 子代理筛目录工具（addon2.md）
// 子代理推荐用：给需求，工具列出目录（基础+详细），模型按需筛查返回。
#pragma once

#include <string>

#include "tool_registry.hpp"

namespace fs {

// 注册 check_dir 到 registry
void register_check_dir_tools(ToolRegistry* registry);

}  // namespace fs