// tool_registry.hpp - tool 接口与注册表（arch.md 工具层）
#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "fs_error.hpp"
#include "fs_json.hpp"

namespace fs {

// 工具调用的上下文（P3 Loop 提供：会话/项目/权限门/网络）
struct ToolContext {
  std::string sid;
  std::string pid;
  std::string cwd;          // 项目根
  bool auto_mode = false;   // 自动模式（ask 工具被移除 + 高风险降级）
  std::string permission = "permissive";  // 文件权限档：approve/permissive/full
  std::string thinking_lang = "";         // 思考语言（中英双语 skill；空=默认）
  std::function<FsResult(const std::string&, const Json&)> invoke_tool;  // 递归调子代理等
  // 回调：向主会话发事件（terminal.output / truncate.hint 等）
  std::function<void(const std::string&, const Json&)> emit;
  // 终端 spawn 回调（reproc 在 FsApi/Python 层；工具层只调回调）
  // args: {shell, cwd, encoding, background, wakeOnExit} -> {tid} / error
  std::function<FsResult(const Json&, Json*)> terminal_spawn;
  // 终端 poll 回调：{tid} -> {status, output, exitCode}
  std::function<FsResult(const Json&, Json*)> terminal_poll;
  // 终端 kill 回调：{tid} -> {}
  std::function<FsResult(const Json&, Json*)> terminal_kill;
  // 网络回调（fetch/search）：复用 NetClient 语义
  // args: {url, method, headers, timeoutMs, sizeCap, verifyCert, proxy} -> {status, body}
  std::function<FsResult(const Json&, Json*)> net_fetch;
  // 进程（todo）回调（addon2 长任务-进程）：{op:"add"|"list"|"check"|"uncheck"|"erase"|"clear", ...}
  // owner 由宿主按当前 sid/子代理 id 决定，工具层不感知
  std::function<FsResult(const Json&, Json*)> todo_op;
  // LSP 回调（addon2 依赖特定环境）：{lang, cmd, args} -> 结果
  std::function<FsResult(const Json&, Json*)> lsp_call;
};

// 工具定义（给模型的 JSON schema）
struct ToolDef {
  std::string name;
  std::string description;
  Json parameters = Json::object();  // JSON Schema
};

// 工具执行函数：args -> FsResult（data 为结果）
using ToolFn = std::function<FsResult(const ToolContext&, const Json& args)>;

class ToolRegistry {
 public:
  void register_tool(const std::string& name, const ToolDef& def, ToolFn fn);
  bool has(const std::string& name) const;
  // 执行工具；未注册返回 kNotFound
  FsResult invoke(const ToolContext& ctx, const std::string& name,
                  const Json& args) const;

  // 工具定义列表（给 HistoryBuilder 的前缀）
  Json defs_json() const;

  // 移除（自动模式移除 ask 用）
  void remove(const std::string& name);

  const ToolDef* find(const std::string& name) const;

 private:
  struct Entry {
    ToolDef def;
    ToolFn fn;
  };
  std::map<std::string, Entry> tools_;
};

}  // namespace fs