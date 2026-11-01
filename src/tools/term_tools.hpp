// term_tools.hpp - terminal_exec/spawn/kill/poll（回调式，reproc 在宿主层）
#pragma once

#include <string>

#include "tool_registry.hpp"

namespace fs {

// 注册终端工具（spawn/exec 走 ctx.terminal_spawn 回调；kill/poll 同理）
void register_term_tools(ToolRegistry* registry);

}  // namespace fs