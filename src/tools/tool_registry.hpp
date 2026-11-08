// tool_registry.hpp - tool 接口与注册表（arch.md 工具层）
#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "fs_error.hpp"
#include "fs_json.hpp"

namespace fs {

// Tool names go straight into the provider request, and BOTH OpenAI and
// Anthropic require them to match `^[a-zA-Z0-9_-]{1,64}$`. A dot is illegal:
// the endpoint answers 400 before the model ever sees the request. Every
// built-in tool is therefore named `snake_case` (read_file, edit_file, ...),
// and this function is the single place that normalises any other source
// (imported MCP tools, user-defined skills) into a legal wire name.
//
// Normalisation is deterministic, so the same input always yields the same
// name - the registry key, the schema sent to the model and the name in the
// model's tool call all agree.
std::string wire_safe_tool_name(const std::string& name);

// True when `name` is already a legal provider tool name.
bool is_wire_safe_tool_name(const std::string& name);

// 工具调用的上下文（P3 Loop 提供：会话/项目/权限门/网络）
struct ToolContext {
  std::string sid;
  std::string pid;
  std::string cwd;          // 项目根
  bool auto_mode = false;   // 自动模式（ask 工具被移除 + 高风险降级）
  std::string permission = "permissive";  // 文件权限档：approve/permissive/full
  // 用户刚刚批准了这一次调用（权限门 approve 之后）。只对本次调用生效：
  // permissive 下访问项目外的路径需要批准，批准后这一次要真的放行，否则
  // "问完还是拒绝"——比不问更糟。
  bool approved = false;
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
  // Web 搜索：{query, count} -> {hits:[{title,url,snippet}], provider}
  // 与 net_fetch 分开：搜索有它自己的请求形态与响应解析（见 search_backend.hpp），
  // 早期把它塞进 net_fetch 的 `{search:true}` 标记里，导致每次都报 "empty url"。
  std::function<FsResult(const Json&, Json*)> web_search;
  // 电脑控制总开关（设置-电脑控制）。空 = 视为开启（单测/无设置时）。
  // 返回 false 时截图工具拒绝执行：这是用户显式关掉的能力，不是权限门。
  std::function<bool()> computer_enabled;
  // 进程（todo）回调（addon2 长任务-进程）：{op:"add"|"list"|"check"|"uncheck"|"erase"|"clear", ...}
  // owner 由宿主按当前 sid/子代理 id 决定，工具层不感知
  std::function<FsResult(const Json&, Json*)> todo_op;
  // 规划树（PlanStore）：{op:"update"|"get", plan?} -> {plan}
  std::function<FsResult(const Json&, Json*)> plan_op;
  // 定时/事件唤醒（WakeScheduler）：{op:"schedule"|"cancel"|"list", delayMs, note}
  std::function<FsResult(const Json&, Json*)> wake_op;
  // Emergency auto-mode brake: the model calls auto_stop only when no safe next
  // step exists. Empty = not wired (tests/subagents without a loop).
  std::function<FsResult()> stop_auto;
  // 子代理：{op:"spawn"|"list"|"cancel"|"steer", ...}
  std::function<FsResult(const Json&, Json*)> subagent_op;
  // 工作流：{op:"start"|"list"|"get", ...}
  std::function<FsResult(const Json&, Json*)> workflow_op;
  // LSP 回调（addon2 依赖特定环境）：{lang, cmd, args} -> 结果
  std::function<FsResult(const Json&, Json*)> lsp_call;
  // 黑白名单判定（permissions.md §3）：给定规范化绝对路径，返回是否放行；
  // 拒绝时 reason 带原因。未接线（空）时视为全部放行。
  std::function<bool(const std::string& abs_path, std::string* reason)> deny_check;
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
  // Registers under `name` as given. Callers must pass a wire-safe name (the
  // built-ins all do); register_tool also normalises as a safety net so a
  // tool added later cannot break every request with a 400.
  void register_tool(const std::string& name, const ToolDef& def, ToolFn fn);
  bool has(const std::string& name) const;
  // 执行工具；未注册返回 kNotFound
  FsResult invoke(const ToolContext& ctx, const std::string& name,
                  const Json& args) const;

  // 工具定义列表（provider 原生形态，旧接口；新代码用 neutral_defs）
  Json defs_json() const;

  // 中性工具定义 [{name, description, parameters}]。
  // 这是唯一给模型看的形状：由 provider::prompt::adapt 按当前端点渲染成
  // OpenAI function / Anthropic input_schema / Responses 形态。
  Json neutral_defs() const;
  // 名字列表（稳定顺序：map 已排序）
  std::vector<std::string> names() const;

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