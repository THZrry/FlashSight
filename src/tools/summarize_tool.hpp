// summarize_tool.hpp - session.summarize：本轮要点记录（单会话持久记忆）
// 模型每轮调用，记要点到 turn.keyPoints；压缩时附加到上下文。
#pragma once

#include <string>

#include "tool_registry.hpp"

namespace fs {

void register_summarize_tool(ToolRegistry* registry);

}  // namespace fs