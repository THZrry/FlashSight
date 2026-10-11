// mcp_tools.cpp - McpRegistry + mcp.* 工具
#include "mcp_tools.hpp"

#include <ghc/filesystem.hpp>

namespace fs {

FsResult McpRegistry::import_dir(const std::string& dir) {
  std::error_code ec;
  if (!ghc::filesystem::exists(dir, ec) || !ghc::filesystem::is_directory(dir, ec)) {
    return FsResult::error(ErrCode::kNotFound, "mcp dir not found: " + dir);
  }
  // 每个子目录视为一个工具包（v1 简化：只记名 + 路径）
  for (auto& e : ghc::filesystem::directory_iterator(dir, ec)) {
    if (!ghc::filesystem::is_directory(e.path(), ec)) continue;
    std::string name = e.path().filename().string();
    bool found = false;
    for (auto& en : entries_) {
      if (en.name == name) {
        en.dir = e.path().string();
        found = true;
        break;
      }
    }
    if (!found) entries_.push_back(Entry{name, e.path().string(), true});
  }
  return FsResult::success();
}

bool McpRegistry::enable(const std::string& name) {
  for (auto& en : entries_) {
    if (en.name == name) {
      en.enabled = true;
      return true;
    }
  }
  return false;
}

bool McpRegistry::disable(const std::string& name) {
  for (auto& en : entries_) {
    if (en.name == name) {
      en.enabled = false;
      return true;
    }
  }
  return false;
}

std::vector<std::string> McpRegistry::list() const {
  std::vector<std::string> out;
  for (auto& en : entries_) out.push_back(en.name);
  return out;
}

Json McpRegistry::list_json() const {
  Json arr = Json::array();
  for (auto& en : entries_) {
    Json j = Json::object();
    j["name"] = en.name;
    j["dir"] = en.dir;
    j["enabled"] = en.enabled;
    arr.push_back(j);
  }
  return arr;
}

std::vector<std::string> McpRegistry::changed_since(const Json& snapshot) const {
  // snapshot: {name: enabled}；返回启用态变化的工具名
  std::vector<std::string> out;
  if (!snapshot.is_object()) return out;
  for (auto& en : entries_) {
    auto it = snapshot.find(en.name);
    if (it == snapshot.end() || !it->is_boolean() || it->get<bool>() != en.enabled) {
      out.push_back(en.name);
    }
  }
  return out;
}

void register_mcp_tools(ToolRegistry* registry, McpRegistry* mcp) {
  // mcp.import
  {
    ToolDef def;
    def.name = "mcp.import";
    def.description = "从外部文件夹导入 MCP/Skill 工具";
    Json params = Json::object();
    params["type"] = "object";
    Json props = Json::object();
    props["path"] = Json{{"type", "string"}, {"description", "外部文件夹路径"}};
    params["properties"] = props;
    params["required"] = Json::array({"path"});
    def.parameters = params;
    registry->register_tool("mcp.import", def,
      [mcp](const ToolContext& ctx, const Json& args) -> FsResult {
        (void)ctx;
        if (!mcp) return FsResult::error(ErrCode::kInternal, "mcp registry missing");
        return mcp->import_dir(jsonutil::get<std::string>(args, "path", ""));
      });
  }
  // mcp.enable / mcp.disable / mcp.list
  {
    ToolDef def;
    def.name = "mcp.enable";
    def.description = "启用 MCP/Skill 工具";
    Json params = Json::object();
    params["type"] = "object";
    Json props = Json::object();
    props["name"] = Json{{"type", "string"}, {"description", "工具名"}};
    params["properties"] = props;
    params["required"] = Json::array({"name"});
    def.parameters = params;
    registry->register_tool("mcp.enable", def,
      [mcp](const ToolContext& ctx, const Json& args) -> FsResult {
        (void)ctx;
        if (!mcp) return FsResult::error(ErrCode::kInternal, "mcp registry missing");
        bool ok = mcp->enable(jsonutil::get<std::string>(args, "name", ""));
        return ok ? FsResult::success()
                  : FsResult::error(ErrCode::kNotFound, "mcp tool not found");
      });
  }
  {
    ToolDef def;
    def.name = "mcp.disable";
    def.description = "禁用 MCP/Skill 工具";
    Json params = Json::object();
    params["type"] = "object";
    Json props = Json::object();
    props["name"] = Json{{"type", "string"}, {"description", "工具名"}};
    params["properties"] = props;
    params["required"] = Json::array({"name"});
    def.parameters = params;
    registry->register_tool("mcp.disable", def,
      [mcp](const ToolContext& ctx, const Json& args) -> FsResult {
        (void)ctx;
        if (!mcp) return FsResult::error(ErrCode::kInternal, "mcp registry missing");
        bool ok = mcp->disable(jsonutil::get<std::string>(args, "name", ""));
        return ok ? FsResult::success()
                  : FsResult::error(ErrCode::kNotFound, "mcp tool not found");
      });
  }
  {
    ToolDef def;
    def.name = "mcp.list";
    def.description = "列出 MCP/Skill 工具";
    Json params = Json::object();
    params["type"] = "object";
    def.parameters = params;
    registry->register_tool("mcp.list", def,
      [mcp](const ToolContext& ctx, const Json& args) -> FsResult {
        (void)ctx;
        (void)args;
        if (!mcp) return FsResult::error(ErrCode::kInternal, "mcp registry missing");
        return FsResult::success(Json{{"tools", mcp->list_json()}});
      });
  }
}

}  // namespace fs