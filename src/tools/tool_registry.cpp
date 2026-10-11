// tool_registry.cpp - tool registry implementation
#include "tool_registry.hpp"

namespace fs {

void ToolRegistry::register_tool(const std::string& name, const ToolDef& def,
                                 ToolFn fn) {
  tools_[name] = Entry{def, std::move(fn)};
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

void ToolRegistry::remove(const std::string& name) {
  tools_.erase(name);
}

}  // namespace fs