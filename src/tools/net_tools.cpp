// net_tools.cpp - network.fetch/search（回调式；search 走可配 HTTP 搜索 API）
#include "net_tools.hpp"

namespace fs {

void register_net_tools(ToolRegistry* registry) {
  // network.fetch（高风险；证书开关；sizeCap；脱敏 trace 由宿主负责）
  {
    ToolDef def;
    def.name = "network.fetch";
    def.description = "HTTP(S) 抓取（高风险；method/headers/timeoutMs/sizeCap/verifyCert）";
    Json params = Json::object();
    params["type"] = "object";
    Json props = Json::object();
    props["url"] = Json{{"type", "string"}, {"description", "目标 URL"}};
    props["method"] = Json{{"type", "string"}, {"description", "GET/POST（默认GET）"}};
    props["headers"] = Json{{"type", "object"}, {"description", "请求头"}};
    props["timeoutMs"] = Json{{"type", "integer"}, {"description", "超时毫秒（默认30000）"}};
    props["sizeCap"] = Json{{"type", "integer"}, {"description", "最大字节（0=不限）"}};
    props["verifyCert"] = Json{{"type", "boolean"}, {"description", "证书校验（默认true）"}};
    params["properties"] = props;
    params["required"] = Json::array({"url"});
    def.parameters = params;
    registry->register_tool("network.fetch", def,
      [](const ToolContext& ctx, const Json& args) -> FsResult {
        if (!ctx.net_fetch) {
          return FsResult::error(ErrCode::kNotSupported, "network fetch not wired");
        }
        Json out;
        FsResult r = ctx.net_fetch(args, &out);
        return r.ok() ? FsResult::success(out) : r;
      });
  }

  // network.search（可配 HTTP 搜索 API；宿主通过 net_fetch 转发到配置的搜索 endpoint）
  {
    ToolDef def;
    def.name = "network.search";
    def.description = "Web 搜索（走可配搜索 API；宿主配置 endpoint）";
    Json params = Json::object();
    params["type"] = "object";
    Json props = Json::object();
    props["query"] = Json{{"type", "string"}, {"description", "搜索词"}};
    props["count"] = Json{{"type", "integer"}, {"description", "条数（默认10）"}};
    params["properties"] = props;
    params["required"] = Json::array({"query"});
    def.parameters = params;
    registry->register_tool("network.search", def,
      [](const ToolContext& ctx, const Json& args) -> FsResult {
        if (!ctx.net_fetch) {
          return FsResult::error(ErrCode::kNotSupported, "network search not wired");
        }
        // 宿主把 search 转成配置的搜索 API 调用（通过 net_fetch 的特殊标记）
        Json fwd = Json::object();
        fwd["search"] = true;
        fwd["query"] = jsonutil::get<std::string>(args, "query", "");
        fwd["count"] = jsonutil::get_int(args, "count", 10);
        Json out;
        FsResult r = ctx.net_fetch(fwd, &out);
        return r.ok() ? FsResult::success(out) : r;
      });
  }
}

}  // namespace fs