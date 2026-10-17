// agent_host.hpp - AgentHost：Loop/工具/子代理/唤醒/引导队列的宿主（FsApi 持有）
// 职责：按 sid 管理 AgentLoop；工具回调接线（terminal/net/vision）；事件扇出；落库。
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "agent_loop.hpp"
#include "fs_clock.hpp"
#include "fs_error.hpp"
#include "fs_json.hpp"
#include "overfit.hpp"
#include "skill_registry.hpp"
#include "steering_queue.hpp"
#include "subagent.hpp"
#include "mcp_tools.hpp"
#include "todo_store.hpp"
#include "tool_registry.hpp"
#include "wake_scheduler.hpp"

namespace fs {

// 宿主依赖（FsApi 注入：存储/网络/provider）
struct AgentHostDeps {
  // 会话 full.json 读写（SessionManager 接口子集）
  std::function<FsResult(const std::string&, Json*)> load_full;
  std::function<FsResult(const std::string&, const Json&)> append_turn;
  std::function<FsResult(const std::string&, const Json&)> write_patch;
  // 模型调用（ProviderClient 包装）
  std::function<FsResult(const ModelCallRequest&, ModelCallResult*)> call_model;
  // 事件扇出（UI 订阅）
  EventFn event;
  // 终端回调（FsApi 的 TerminalManager；工具层只调回调）
  std::function<FsResult(const Json&, Json*)> terminal_spawn;
  std::function<FsResult(const Json&, Json*)> terminal_poll;
  std::function<FsResult(const Json&, Json*)> terminal_kill;
  // 网络回调（fetch/search；FsApi 的 NetClient）
  std::function<FsResult(const Json&, Json*)> net_fetch;
  // 黑白名单判定（FsApi 的 DenyList；空 = 全部放行）
  std::function<bool(const std::string& abs_path, std::string* reason)> deny_check;
  const Clock* clock = nullptr;
};

class AgentHost {
 public:
  explicit AgentHost(AgentHostDeps deps);

  // 注册全部内置工具（file/term/net/vision/mcp/ask；ask 在自动模式下移除）
  void register_builtin_tools(McpRegistry* mcp);

  // ----- send/append（FsApi session.send/append）-----
  FsResult send(const std::string& sid, const std::string& text,
                const Json& attachments, bool auto_mode,
                const std::string& permission = "permissive");
  FsResult append(const std::string& sid, const std::string& text);

  // 打断后续接（addon2 任务健壮性）：给原任务发送特定引导并续跑
  FsResult continue_task(const std::string& sid, const std::string& guidance);
  // 子代理引导（addon2：子代理无轮概念，只需当前引导）
  FsResult subagent_steer(const std::string& id, const std::string& text);
  // 批准/拒绝（approve 档权限门）
  FsResult approve(const std::string& sid, bool yes);

  // ----- 挂起/恢复/取消唤醒 -----
  FsResult suspend(const std::string& sid);
  FsResult resume(const std::string& sid);
  FsResult cancel_wake(const std::string& sid);

  // ----- 工具直接调用（FsApi tool.*）-----
  FsResult tool_invoke(const std::string& sid, const std::string& name,
                       const Json& args, bool auto_mode,
                       const std::string& permission = "permissive");

  // ----- ask 回答（FsApi tool.ask.answer）-----
  FsResult ask_answer(const std::string& sid, int ask_id, const std::string& selection);

  // ----- 子代理（FsApi subagent.*）-----
  FsResult subagent_spawn(const std::string& sid, const std::string& kind,
                          const std::string& task, const std::string& model,
                          int depth, int64_t timeout_ms, std::string* out_id);
  FsResult subagent_cancel(const std::string& id);
  FsResult subagent_list(const std::string& sid);

  // ----- 唤醒事件（终端退出/定时器/子代理完成）-----
  FsResult on_terminal_exit(const std::string& sid, const std::string& tid, int exit_code);
  FsResult on_timer_tick();
  FsResult on_subagent_done(const std::string& id, const std::string& summary);

  // ----- 引导队列（addon1.md）-----
  FsResult steering_push(const std::string& sid, const std::string& kind,
                         const std::string& text);
  FsResult steering_edit(const std::string& sid, int id, const std::string& text);
  FsResult steering_erase(const std::string& sid, int id);
  FsResult steering_reorder(const std::string& sid, const std::vector<int>& order);
  FsResult steering_list(const std::string& sid);

  // ----- 进程（todo，addon2 长任务-进程）-----
  // owner 缺省为 sid；子代理传自己的 id 得到独立进程树
  FsResult todo_op(const std::string& owner, const Json& args);
  int todo_pending(const std::string& owner) const;
  TodoStore& todos() { return todos_; }

  // ----- skill（addon2：中英双语，按需加载）-----
  SkillRegistry& skills() { return skills_; }

  // 思考语言设置（addon2；"auto|zh|en"）
  void set_thinking_lang(const std::string& lang) { thinking_lang_ = lang; }
  const std::string& thinking_lang() const { return thinking_lang_; }

  // 工具注册表（测试/上层用）
  ToolRegistry& tools() { return tools_; }
  SubagentManager& subagents() { return subagents_; }
  WakeScheduler& wakes() { return wakes_; }

  // 该会话是否正在跑（有 loop 且处于运行态）。UI 用它显示会话状态：
  // "正在运行"是当前事实，不是落盘的历史，所以从内存问而不是读文件。
  bool is_running(const std::string& sid) const;
  // Live loop state for a session ("idle"|"planning"|"toolCalling"|"waiting"|
  // "turnDone"). The UI turns this into a status dot: waiting means the model is
  // blocked on the user (a permission prompt), which is different from running.
  std::string loop_state(const std::string& sid) const;

 private:
  AgentHostDeps deps_;
  ToolRegistry tools_;
  SubagentManager subagents_;
  WakeScheduler wakes_;
  TodoStore todos_;
  SkillRegistry skills_;
  std::string thinking_lang_ = "auto";
  std::map<std::string, std::unique_ptr<AgentLoop>> loops_;
  std::map<std::string, SteeringQueue> steering_;
  std::map<std::string, OverfitDetector> overfit_;
  // 待落库的要点（session.keypoint 事件收集；persist_turn 时写入 turn.keyPoints）
  std::map<std::string, std::vector<Json>> pending_keypoints_;

  // 或建 Loop
  AgentLoop& loop_for(const std::string& sid, bool auto_mode,
                      const std::string& permission = "permissive");
  // 工具调用上下文
  ToolContext make_ctx(const std::string& sid, bool auto_mode);
  // 事件转发
  void emit(const std::string& event, const Json& payload);
  // 落库 turn（含 toolCalls/usage/ask/sysop）
  FsResult persist_turn(const std::string& sid, const Json& turn);
};

}  // namespace fs