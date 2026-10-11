// ask_tool.hpp - ask 工具（agent-loop.md 3）
#pragma once

#include <string>

#include "tool_registry.hpp"

namespace fs {

// 注册 ask 工具（question/options/recommended/allowCustom）。
// 手动模式：发 ask.prompt 事件等用户选（Loop.answer_ask 完成链路）。
// 自动模式：调用方在 defs 里拿掉 ask（见 register 时 skip_ask）。
void register_ask_tool(ToolRegistry* registry);

}  // namespace fs