// mcp_tools.cpp - McpRegistry + mcp.* 工具
#include "mcp_tools.hpp"

#include <fstream>
#include <sstream>

#include <ghc/filesystem.hpp>

namespace fs {

namespace {

std::string read_file_text(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return "";
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// The server map lives under one of several keys depending on the client:
// Claude uses `mcpServers`, opencode uses `mcp.servers`, some tools use
// `servers` at the top level. Pick whichever is present.
const Json* find_server_map(const Json& root) {
  if (!root.is_object()) return nullptr;
  if (root.contains("mcpServers") && root["mcpServers"].is_object()) {
    return &root["mcpServers"];
  }
  if (root.contains("mcp") && root["mcp"].is_object() &&
      root["mcp"].contains("servers") && root["mcp"]["servers"].is_object()) {
    return &root["mcp"]["servers"];
  }
  if (root.contains("servers") && root["servers"].is_object()) {
    return &root["servers"];
  }
  return nullptr;
}

}  // namespace

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
    if (!found) {
      Entry en;
      en.name = name;
      en.dir = e.path().string();
      en.kind = "dir";
      entries_.push_back(en);
    }
  }
  return FsResult::success();
}

int McpRegistry::import_config_file(const std::string& path) {
  std::error_code ec;
  if (!ghc::filesystem::is_regular_file(path, ec)) return 0;
  std::string text = read_file_text(path);
  if (text.empty()) return 0;
  Json root;
  try {
    root = Json::parse(text);
  } catch (...) {
    return 0;  // not JSON (or a JSONC file we cannot parse): not a server list
  }
  const Json* servers = find_server_map(root);
  if (!servers) return 0;
  int added = 0;
  for (auto it = servers->begin(); it != servers->end(); ++it) {
    if (!it.value().is_object()) continue;
    std::string name = it.key();
    if (name.empty()) continue;
    bool exists = false;
    for (auto& en : entries_) {
      if (en.name == name) {
        exists = true;
        break;
      }
    }
    upsert_server(name, it.value());
    if (!exists) ++added;
  }
  return added;
}

void McpRegistry::upsert(const std::string& name, const std::string& dir, bool enabled) {
  if (name.empty()) return;
  for (auto& en : entries_) {
    if (en.name == name) {
      en.dir = dir;
      en.kind = "dir";
      en.enabled = enabled;
      return;
    }
  }
  Entry en;
  en.name = name;
  en.dir = dir;
  en.kind = "dir";
  en.enabled = enabled;
  entries_.push_back(en);
}

void McpRegistry::upsert_server(const std::string& name, const Json& server) {
  if (name.empty()) return;
  for (auto& en : entries_) {
    if (en.name == name) {
      en.kind = "server";
      en.server = server;
      return;
    }
  }
  Entry en;
  en.name = name;
  en.kind = "server";
  en.server = server;
  entries_.push_back(en);
}

bool McpRegistry::enable(const std::string& name) { return set_enabled(name, true); }
bool McpRegistry::disable(const std::string& name) { return set_enabled(name, false); }

bool McpRegistry::set_enabled(const std::string& name, bool enabled) {
  for (auto& en : entries_) {
    if (en.name == name) {
      en.enabled = enabled;
      return true;
    }
  }
  return false;
}

bool McpRegistry::remove(const std::string& name) {
  for (std::vector<Entry>::iterator it = entries_.begin(); it != entries_.end(); ++it) {
    if (it->name == name) {
      entries_.erase(it);
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
    j["kind"] = en.kind;
    if (!en.server.empty()) j["server"] = en.server;
    j["enabled"] = en.enabled;
    arr.push_back(j);
  }
  return arr;
}

Json McpRegistry::dump() const { return list_json(); }

// An entry whose `dir` points inside a skills/commands folder is a skill that was
// imported here by the old "探测" button, not an MCP server.
bool McpRegistry::is_skill_dir_entry(const Json& entry) {
  if (!entry.is_object()) return false;
  std::string kind = jsonutil::get<std::string>(entry, "kind", "");
  if (kind == "server") return false;
  std::string dir = jsonutil::get<std::string>(entry, "dir", "");
  if (dir.empty()) return false;
  // Normalise backslashes so the match works on Windows paths too.
  std::string d = dir;
  for (size_t i = 0; i < d.size(); ++i) {
    if (d[i] == '\\') d[i] = '/';
  }
  static const char* kSkillSegs[] = {"/.claude/skills", "/.claude/commands",
                                     "/opencode/skills", "/flashsight/skills",
                                     "/.opencode/skills"};
  for (const char* seg : kSkillSegs) {
    if (d.find(seg) != std::string::npos) return true;
  }
  return false;
}

void McpRegistry::load(const Json& arr) {
  if (!arr.is_array()) return;
  entries_.clear();
  for (size_t i = 0; i < arr.size(); ++i) {
    const Json& e = arr[i];
    if (is_skill_dir_entry(e)) continue;  // belongs to the skill registry
    Entry en;
    en.name = jsonutil::get<std::string>(e, "name", "");
    en.dir = jsonutil::get<std::string>(e, "dir", "");
    en.kind = jsonutil::get<std::string>(e, "kind", en.dir.empty() ? "server" : "dir");
    if (e.contains("server") && e["server"].is_object()) en.server = e["server"];
    en.enabled = jsonutil::get<bool>(e, "enabled", true);
    if (!en.name.empty()) entries_.push_back(en);
  }
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
  // mcp_import
  {
    ToolDef def;
    def.name = "mcp_import";
    def.description = "从外部文件夹导入 MCP/Skill 工具";
    Json params = Json::object();
    params["type"] = "object";
    Json props = Json::object();
    props["path"] = Json{{"type", "string"}, {"description", "外部文件夹路径"}};
    params["properties"] = props;
    params["required"] = Json::array({"path"});
    def.parameters = params;
    registry->register_tool("mcp_import", def,
      [mcp](const ToolContext& ctx, const Json& args) -> FsResult {
        (void)ctx;
        if (!mcp) return FsResult::error(ErrCode::kInternal, "mcp registry missing");
        return mcp->import_dir(jsonutil::get<std::string>(args, "path", ""));
      });
  }
  // mcp_enable / mcp_disable / mcp_list
  {
    ToolDef def;
    def.name = "mcp_enable";
    def.description = "启用 MCP/Skill 工具";
    Json params = Json::object();
    params["type"] = "object";
    Json props = Json::object();
    props["name"] = Json{{"type", "string"}, {"description", "工具名"}};
    params["properties"] = props;
    params["required"] = Json::array({"name"});
    def.parameters = params;
    registry->register_tool("mcp_enable", def,
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
    def.name = "mcp_disable";
    def.description = "禁用 MCP/Skill 工具";
    Json params = Json::object();
    params["type"] = "object";
    Json props = Json::object();
    props["name"] = Json{{"type", "string"}, {"description", "工具名"}};
    params["properties"] = props;
    params["required"] = Json::array({"name"});
    def.parameters = params;
    registry->register_tool("mcp_disable", def,
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
    def.name = "mcp_list";
    def.description = "列出 MCP/Skill 工具";
    Json params = Json::object();
    params["type"] = "object";
    def.parameters = params;
    registry->register_tool("mcp_list", def,
      [mcp](const ToolContext& ctx, const Json& args) -> FsResult {
        (void)ctx;
        (void)args;
        if (!mcp) return FsResult::error(ErrCode::kInternal, "mcp registry missing");
        return FsResult::success(Json{{"tools", mcp->list_json()}});
      });
  }
}

}  // namespace fs