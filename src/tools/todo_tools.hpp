// todo_tools.hpp - 进程（todo）工具（addon2.md 长任务-进程）
// "最轻量的那个，管单轮任务，列出本轮任务，完成一项就打勾"
// 子代理也提供进程工具（addon2 明确要求）。
#pragma once

#include "tool_registry.hpp"

namespace fs {

// 注册 todo.* 工具（todo_add/list/check/uncheck/clear）
void register_todo_tools(ToolRegistry* registry);

}  // namespace fs