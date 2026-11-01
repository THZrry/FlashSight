// net_tools.cpp - fetch_url / search_web（回调式；search 走可配 HTTP 搜索 API）
#include "net_tools.hpp"

namespace fs {

void register_net_tools(ToolRegistry* registry) {
  // fetch_url（高风险；证书开关；sizeCap；脱敏 trace 由宿主负责）
  {
    ToolDef def;
    def.name = "fetch_url";
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
    registry->register_tool("fetch_url", def,
      [](const ToolContext& ctx, const Json& args) -> FsResult {
        if (!ctx.net_fetch) {
          return FsResult::error(ErrCode::kNotSupported, "network fetch not wired");
        }
        Json out;
        FsResult r = ctx.net_fetch(args, &out);
        return r.ok() ? FsResult::success(out) : r;
      });
  }

  // search_web（走可配搜索 API；配置与解析见 search_backend.hpp）
  {
    ToolDef def;
    def.name = "search_web";
    def.description =
        "Web 搜索（需在 设置-模型-搜索 配置后端：tavily/brave/searxng/custom）. "
        "Web search. Returns {hits:[{title,url,snippet}]}. Requires a configured "
        "search backend; without one the error says where to configure it.";
    Json params = Json::object();
    params["type"] = "object";
    Json props = Json::object();
    props["query"] = Json{{"type", "string"}, {"description", "搜索词"}};
    props["count"] = Json{{"type", "integer"}, {"description", "条数（默认5）"}};
    params["properties"] = props;
    params["required"] = Json::array({"query"});
    def.parameters = params;
    registry->register_tool("search_web", def,
      [](const ToolContext& ctx, const Json& args) -> FsResult {
        std::string query = jsonutil::get<std::string>(args, "query", "");
        if (query.empty()) {
          return FsResult::error(ErrCode::kInvalidArg, "empty query");
        }
        // A dedicated callback, NOT net_fetch: search has its own request shape
        // and response parsing. Routing it through net_fetch (the old
        // `{search:true}` marker) made every call fail with "empty url" because
        // that callback only reads `url`.
        if (!ctx.web_search) {
          return FsResult::error(
              ErrCode::kNotSupported,
              "web search is not configured: pick a backend in Settings > "
              "Model > Search (tavily / brave / searxng / custom)");
        }
        Json req = Json::object();
        req["query"] = query;
        req["count"] = jsonutil::get_int(args, "count", 0);
        Json out;
        FsResult r = ctx.web_search(req, &out);
        return r.ok() ? FsResult::success(out) : r;
      });
  }
}

}  // namespace fs