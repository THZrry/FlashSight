// vision_tools.hpp - screenshot/readImage（回调式；截图由宿主/浏览器驱动提供）
#pragma once

#include <string>

#include "tool_registry.hpp"

namespace fs {

void register_vision_tools(ToolRegistry* registry);

}  // namespace fs