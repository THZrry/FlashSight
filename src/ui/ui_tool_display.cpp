// ui_tool_display.cpp - human-facing tool row labels (see the header).
#include "ui_tool_display.hpp"

#include <string>

#include "fs_utf8.hpp"

namespace fs {
namespace ui {
namespace {

std::string cut(const std::string& s, size_t n) {
  return utf8::truncate(s, n, "…");
}

int64_t i64(const Json& j, const char* k, int64_t def = 0) {
  return jsonutil::get_int(j, k, def);
}

std::string str(const Json& j, const char* k, const std::string& def = "") {
  return jsonutil::get<std::string>(j, k, def);
}

bool has(const Json& j, const char* k) { return j.is_object() && j.contains(k); }

}  // namespace

std::string tool_display_summary(const std::string& name, const Json& args,
                                 const std::string& detail, bool ok) {
  // A failed call is a failed call for every tool except the two where the
  // subject still matters (which command/agent failed). The row's red state
  // carries the rest.
  if (!ok && name.rfind("subagent_", 0) != 0 && name != "complete_step" &&
      name != "terminal_exec" && name != "terminal_spawn") {
    return "failed";
  }
  Json d = Json::object();
  if (!detail.empty()) {
    try {
      Json parsed = Json::parse(detail);
      if (parsed.is_object()) d = parsed;
    } catch (...) {
    }
  }
  if (name == "read_file") {
    std::string path = str(args, "path", str(d, "path"));
    if (path.empty()) path = "read";
    if (has(d, "startLine") && has(d, "endLine")) {
      return path + " " + std::to_string(i64(d, "startLine")) + "-" +
             std::to_string(i64(d, "endLine")) + "/" +
             std::to_string(i64(d, "totalLines")) + " lines";
    }
    return path;
  }
  if (name == "edit_file") {
    int64_t edits = 0;
    if (args.is_object() && args.contains("edits") && args["edits"].is_array()) {
      edits = static_cast<int64_t>(args["edits"].size());
    } else if (has(d, "changes") && d["changes"].is_array()) {
      edits = static_cast<int64_t>(d["changes"].size());
    } else {
      edits = 1;
    }
    std::string out = std::to_string(edits) + " edit(s)";
    if (has(d, "warnings") && d["warnings"].is_array()) {
      out += "; " + std::to_string(d["warnings"].size()) + " warning(s)";
    }
    return out;
  }
  if (name == "write_file") {
    std::string path = str(args, "path", str(d, "path"));
    return path.empty() ? "write" : path;
  }
  if (name == "list_dir") {
    std::string path = str(args, "path", ".");
    return path.empty() ? "." : cut(path, 120);
  }
  if (name == "terminal_exec" || name == "terminal_spawn") {
    std::string cmd = str(args, "command", str(args, "shell"));
    if (cmd.empty()) cmd = str(args, "cmd");
    return cmd.empty() ? (ok ? "ok" : "failed") : cut(cmd, 100);
  }
  if (name.compare(0, 9, "subagent_") == 0 || name == "complete_step") {
    std::string id = str(d, "id", str(args, "id"));
    std::string status = str(d, "status");
    if (status.empty()) status = ok ? "ok" : "failed";
    std::string step = str(d, "step", str(args, "step"));
    std::string head = !step.empty() ? step : id;
    if (!head.empty()) return head + " · " + status;
    return status;
  }
  if (name.compare(0, 5, "todo_") == 0) {
    if (name == "todo_add") {
      int64_t n = i64(d, "added", 1);
      return "+" + std::to_string(n) + " · " + std::to_string(i64(d, "pending")) +
             " pending";
    }
    if (name == "todo_check") {
      std::string id = std::to_string(i64(d, "id", i64(args, "id")));
      return id + (jsonutil::get<bool>(d, "done", true) ? " done" : " open");
    }
    if (name == "todo_erase") {
      return "#" + std::to_string(i64(d, "removed", i64(args, "id"))) + " removed";
    }
    if (name == "todo_clear") return "cleared";
    if (name == "todo_list") {
      int64_t n = has(d, "items") && d["items"].is_array()
                      ? static_cast<int64_t>(d["items"].size())
                      : i64(d, "total");
      return std::to_string(n) + " item(s)";
    }
    return ok ? "ok" : "failed";
  }
  if (name == "plan_update" || name == "plan_get") {
    int64_t seq = i64(d, "seq");
    std::string id = seq > 0 ? ("#" + std::to_string(seq)) : std::string("plan");
    std::string status = str(d, "status");
    if (!jsonutil::get<bool>(d, "exists", false) && name == "plan_get") {
      return id + " · none";
    }
    return status.empty() ? id : id + " · " + status;
  }
  if (name == "find_files" || name == "file_regex" || name == "grep") {
    std::string q = str(args, "query", str(args, "pattern"));
    return q.empty() ? (ok ? "ok" : "failed") : cut(q, 100);
  }
  return ok ? "ok" : "failed";
}

}  // namespace ui
}  // namespace fs
