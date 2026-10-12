// regex_tools.hpp - file.regex 工具（addon2.md：regex 通配替换）
// 正则引擎移植自 QuickJS（WhaleJS src/libregexp.c），MIT 许可，Fabrice Bellard。
// 支持 JS 风格 regex：分组 $1..$9、$& 全匹配；多文件替换；替换前留 patch 可回滚。
#pragma once

#include <string>

#include "tool_registry.hpp"

namespace fs {

// 注册 file.regex 到 registry
void register_regex_tools(ToolRegistry* registry);

}  // namespace fs