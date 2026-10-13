// agent_loop.hpp - Agent 主循环状态机（agent-loop.md 1-7, addon1.md）
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "fs_clock.hpp"
#include "fs_error.hpp"
#include "fs_json.hpp"
#include "history_builder.hpp"
#include "tool_registry.hpp"

namespace fs {

// Loop 状态（agent-loop.md 1）
enum class LoopState {
  kIdle,
  kPlanning,
  kToolCalling,
  kWaiting,      // 等 permission 批准 / ask 回答
  kTurnDone,
};

const char* loop_state_str(LoopState s);

// 事件回调（后端->UI，ui-events.md 2）
using EventFn = std::function<void(const std::string& event, const Json& payload)>;

// 模型调用抽象（ProviderClient 包装，便于测试桩）
struct ModelCallRequest {
  std::string model;
  Json messages = Json::array();
  Json tools = Json::array();
  bool auto_mode = false;
  // 会话亲和 id（kPlan 套餐连接透传为 x-opencode-session，provider.md §5）
  std::string session_id;
};
struct ModelCallResult {
  std::string text;
  Json tool_calls = Json::array();  // [{name,args,id}]
  std::string error;                // 空=成功
};

class AgentLoop {
 public:
  struct Options {
    HistoryBuilder::Options history;
    EventFn event;                  // 事件推送
    std::string cwd;               // 项目根
    std::string sid;
    std::string pid;
    std::string model;
    std::string permission = "permissive";  // 文件权限档（工具上下文注入用）
    std::string thinking_lang = "";         // 思考语言（中英双语 skill）
  };

  explicit AgentLoop(Options opts);

  // 状态查询
  LoopState state() const { return state_; }

  // 会话级属性更新（FsApi 读 sessionSettings 后传入）
  void set_permission(const std::string& perm) { opts_.permission = perm; }
  void set_thinking_lang(const std::string& lang) { opts_.thinking_lang = lang; }

  // send：用户输入（含引导队列合并），进入 planning
  FsResult send(const std::string& text);

  // append：追加输入（引导队列 >1 合并）
  FsResult append(const std::string& text);

  // 权限门（approve 档）：批准/拒绝当前等待的 tool
  FsResult approve(bool yes);

  // ask 回答
  FsResult answer_ask(int ask_id, const std::string& selection);

  // 挂起/恢复/取消唤醒
  void suspend();
  void resume();
  void cancel_wake();

  // addon2 任务健壮性：挂起-继续（打断后续接）。
  // 中断（模型挂起/失败）后，给原任务发送特定引导即从保存点续跑，
  // 无需用户重开会话。子代理同理（SubagentManager::steer）。
  FsResult continue_task(const std::string& guidance);
  bool has_saved_context() const { return saved_valid_; }

  // 子代理结果回调（自动模式唤醒父）
  FsResult on_subagent_done(const std::string& subagent_id, const std::string& summary);

  // 后台终端退出回调
  FsResult on_terminal_exit(const std::string& tid, int exit_code);

  // 定时器到点回调
  FsResult on_timer();

  // 模型调用抽象（测试可注入）
  std::function<FsResult(const ModelCallRequest&, ModelCallResult*)> call_model;

  // 工具执行抽象（Host 注入；批准后真调工具）
  std::function<FsResult(const ToolContext&, const std::string&, const Json&)> invoke_tool;

 private:
  Options opts_;
  LoopState state_ = LoopState::kIdle;
  bool suspend_requested_ = false;
  bool wake_scheduled_ = false;
  bool overfit_enabled_ = false;
  int64_t wake_at_ms_ = 0;

  // 引导队列（addon1.md：当前队列，可编辑/排序/删除，>1 合并）
  std::vector<std::string> steering_queue_;

  // 等待中的工具（approve 门）
  std::string pending_tool_;
  Json pending_args_;

  // 当前 turn 的 tool 结果（供回填）
  Json pending_tool_results_ = Json::array();

  // 连续输出（防过拟合检测用）
  std::vector<std::string> recent_outputs_;

  // 中断-继续保存点（addon2）：中断/失败时的 messages 上下文
  Json saved_messages_ = Json::array();
  bool saved_valid_ = false;

  FsResult run_turn(const std::string& user_text);
  // 从给定 messages 继续跑（工具循环 + 回填），供 continue_task 复用
  FsResult run_loop(Json messages, const std::string& model, const Json& tools);
  FsResult handle_tool_call(const Json& call);
  FsResult execute_tool(const std::string& name, const Json& args);  // addon2：真调工具
  FsResult finish_turn(const Json& turn);
  void emit(const std::string& event, const Json& payload);
  bool is_high_risk(const std::string& tool) const;
  bool should_deferred(const std::string& tool) const;
};

}  // namespace fs