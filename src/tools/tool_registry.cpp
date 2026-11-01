// tool_registry.cpp - tool registry implementation
#include "tool_registry.hpp"

#include <cctype>

namespace fs {

bool is_wire_safe_tool_name(const std::string& name) {
  if (name.empty() || name.size() > 64) return false;
  for (char c : name) {
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-';
    if (!ok) return false;
  }
  return true;
}

std::string wire_safe_tool_name(const std::string& name) {
  if (name.empty()) return "tool";
  std::string out;
  out.reserve(name.size());
  for (char c : name) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '_' || c == '-') {
      out.push_back(c);
    } else {
      // '.' and '/' are the common separators in tool naming; map them to '_'
      // so `read_file` becomes `file_read` rather than `fileread`.
      out.push_back('_');
    }
  }
  // A name starting with a digit or '-' is legal by the regex but confusing in
  // a function-call position; prefix it so it stays readable.
  if (!out.empty() && (std::isdigit(static_cast<unsigned char>(out[0])) || out[0] == '-')) {
    out = "t_" + out;
  }
  if (out.size() > 64) out.resize(64);
  return out;
}

void ToolRegistry::register_tool(const std::string& name, const ToolDef& def,
                                 ToolFn fn) {
  // The provider rejects illegal names with a 400 for the WHOLE request, so a
  // single bad tool would break every turn. Normalising here (and keeping the
  // definition's name in sync) makes that impossible.
  std::string key = wire_safe_tool_name(name);
  ToolDef d = def;
  d.name = key;
  tools_[key] = Entry{d, std::move(fn)};
}

bool ToolRegistry::has(const std::string& name) const {
  return tools_.count(name) > 0;
}

const ToolDef* ToolRegistry::find(const std::string& name) const {
  auto it = tools_.find(name);
  return it == tools_.end() ? nullptr : &it->second.def;
}

FsResult ToolRegistry::invoke(const ToolContext& ctx, const std::string& name,
                              const Json& args) const {
  auto it = tools_.find(name);
  if (it == tools_.end()) {
    return FsResult::error(ErrCode::kNotFound, "tool not registered: " + name);
  }
  return it->second.fn(ctx, args);
}

Json ToolRegistry::defs_json() const {
  Json arr = Json::array();
  for (auto& kv : tools_) {
    Json j = Json::object();
    j["type"] = "function";
    Json fn = Json::object();
    fn["name"] = kv.second.def.name;
    fn["description"] = kv.second.def.description;
    fn["parameters"] = kv.second.def.parameters;
    j["function"] = fn;
    arr.push_back(j);
  }
  return arr;
}

Json ToolRegistry::neutral_defs() const {
  Json arr = Json::array();
  for (auto& kv : tools_) {
    Json j = Json::object();
    j["name"] = kv.second.def.name;
    j["description"] = kv.second.def.description;
    j["parameters"] = kv.second.def.parameters;
    arr.push_back(j);
  }
  return arr;
}

std::vector<std::string> ToolRegistry::names() const {
  std::vector<std::string> out;
  for (auto& kv : tools_) out.push_back(kv.first);
  return out;
}

void ToolRegistry::remove(const std::string& name) {
  tools_.erase(name);
}

}  // namespace fs