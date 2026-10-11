// net_tools.hpp - network.fetch/search（回调式；search 先接可配 HTTP API）
#pragma once

#include <string>

#include "tool_registry.hpp"

namespace fs {

void register_net_tools(ToolRegistry* registry);

}  // namespace fs