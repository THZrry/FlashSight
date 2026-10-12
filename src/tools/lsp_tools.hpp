// lsp_tools.hpp - LSP 工具（addon2.md 依赖特定环境：内置 MCP，符号重构/整项目操作）
// lsp(lang, cmd)：确保对应 LSP 服务器启动并执行命令。
// 进程管理由宿主回调（复用 terminal 的 spawn 层）；设置-环境配置各语言 LSP 服务器。
#pragma once

#include "tool_registry.hpp"

namespace fs {

// 注册 lsp 工具到 registry
void register_lsp_tools(ToolRegistry* registry);

}  // namespace fs
