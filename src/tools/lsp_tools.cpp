// lsp_tools.cpp - LSP 工具（addon2.md：内置 MCP，确保服务器启动并执行命令）
// 实际 LSP 进程由宿主回调 lsp_call 提供（复用 terminal spawn 层 + JSON-RPC）。
#include "lsp_tools.hpp"

namespace fs {

void register_lsp_tools(ToolRegistry* registry) {
  ToolDef def;
  def.name = "lsp";
  def.description =
      "LSP 语言服务调用. Invoke a language server (symbol refactor / project-wide ops). "
      "lsp(lang, cmd): 确保该语言 LSP 服务器启动并执行命令. "
      "适用 rename/find-references 等；先在设置-环境配置各语言 LSP。";
  Json params = Json::object();
  params["type"] = "object";
  Json props = Json::object();
  props["lang"] = Json{{"type", "string"}, {"description", "语言标识，如 cpp/rust/python/js"}};
  props["cmd"] = Json{{"type", "string"},
                      {"description", "LSP 命令，如 rename/findReferences/hover"
                                       "（或原始 JSON-RPC method）"}};
  props["args"] = Json{{"type", "object"}, {"description", "命令参数（如 {path,line,column,newName}）"}};
  params["properties"] = props;
  params["required"] = Json::array({"lang", "cmd"});
  def.parameters = params;
  registry->register_tool("lsp", def,
    [](const ToolContext& ctx, const Json& args) -> FsResult {
      if (!ctx.lsp_call) {
        return FsResult::error(ErrCode::kNotSupported,
                               "lsp not wired (configure language servers in Settings > Environment)");
      }
      Json out;
      FsResult r = ctx.lsp_call(args, &out);
      return r.ok() ? FsResult::success(out) : r;
    });
}

}  // namespace fs