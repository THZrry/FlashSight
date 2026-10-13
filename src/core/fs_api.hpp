// fs_api.hpp - FsApi 统一入口（arch.md §5 方法表）
// 同进程函数调用；以后整体换 HTTP（UI 与后端不耦合）。
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>

#include "fs_clock.hpp"
#include "fs_error.hpp"
#include "fs_json.hpp"
#include "fs_net_http.hpp"
#include "fs_random.hpp"
#include "core/deny_list.hpp"
#include "core/lock_arbiter.hpp"
#include "core/plan_store.hpp"
#include "core/project_manager.hpp"
#include "core/session_manager.hpp"
#include "core/settings_store.hpp"
#include "core/shell_detect.hpp"
#include "core/terminal_manager.hpp"
#include "core/terminal_registry.hpp"
#include "core/usage_store.hpp"
#include "core/workflow.hpp"
#include "agent/agent_host.hpp"
#include "provider/model_table.hpp"
#include "provider/provider_client.hpp"
#include "provider/provider_runtime.hpp"
#include "provider/provider_store.hpp"
#include "storage/fs_store.hpp"
#include "tools/mcp_tools.hpp"

namespace fs {

// P1 骨架：project/session/terminal-list/usage/settings/plan 全实现；
// tool.*/provider.*/mcp.* 返回 kNotSupported 桩（P2/P3 接真实现）。
class FsApi {
 public:
  // root_dir 可注入（单测用临时目录）；clock/rng 可注入（单测假时钟/固定种子）
  explicit FsApi(const ghc::filesystem::path& root_dir,
                 const Clock* clock = nullptr, Random* rng = nullptr);
  ~FsApi();

  // ----- project.* -----
  FsResult project_list();
  FsResult project_open(const std::string& path);
  FsResult project_create(const std::string& path);
  FsResult project_remove(const std::string& pid);
  FsResult project_get(const std::string& pid);
  // 打开系统"选择文件夹"对话框（浏览器无法提供本机路径，由本进程弹窗）
  // 返回 {path} 或 cancelled=true
  FsResult project_pick_folder();
  // 解析浏览器拖放拿到的文件夹名（浏览器不给绝对路径）：在常见根目录中搜索
  // {name} -> {path} 或 {notFound:true}
  FsResult project_resolve_drop(const std::string& name);
  // 在资源管理器中打开项目文件夹：{pid} -> {}
  FsResult project_reveal(const std::string& pid);

  // ----- session.* -----
  FsResult session_new(const std::string& pid, const std::string& cwd);
  FsResult session_open(const std::string& sid);
  FsResult session_close(const std::string& sid);
  FsResult session_fork(const std::string& sid, const std::string& turnId);
  FsResult session_rollback(const std::string& sid, const std::string& turnId);
  FsResult session_forget(const std::string& sid, const std::string& scope);
  FsResult session_list(const std::string& pid);
  FsResult session_set_model(const std::string& sid, const std::string& model);
  FsResult session_set_thinking(const std::string& sid, const std::string& thinking);
  FsResult session_set_permission(const std::string& sid, const std::string& perm);
  FsResult session_set_permission_confirm(const std::string& sid, const std::string& perm,
                                          const std::string& token);
  FsResult session_request_permission_token(const std::string& sid,
                                            const std::string& newPerm);
  FsResult session_set_auto_mode(const std::string& sid, bool autoMode);
  FsResult session_set_enter_send(const std::string& sid, const std::string& enterSend);
  FsResult session_refresh_config(const std::string& sid);
  FsResult session_append_turn(const std::string& sid, const Json& turn);
  FsResult session_get(const std::string& sid);
  // P3: send/append/suspend/resume/cancelWake（AgentHost）
  FsResult session_send(const std::string& sid, const std::string& text,
                        const Json& attachments);
  FsResult session_append(const std::string& sid, const std::string& text);
  FsResult session_suspend(const std::string& sid);
  FsResult session_resume(const std::string& sid);
  FsResult session_cancel_wake(const std::string& sid);
  // addon2 任务健壮性：打断后续接（给原任务发引导并续跑）
  FsResult session_continue(const std::string& sid, const std::string& guidance);
  // approve 档权限门：批准/拒绝当前等待的 tool
  FsResult session_approve(const std::string& sid, bool yes);

  // ----- terminal.*（注册表部分 P1；spawn/exec 实际执行 P3 reproc）-----
  FsResult terminal_list(const std::string& sid);
  FsResult terminal_show(const std::string& tid);
  FsResult terminal_kill(const std::string& tid);
  FsResult terminal_set_encoding(const std::string& tid, const std::string& enc);

  // ----- usage.* -----
  FsResult usage_query(const Json& filter);
  FsResult usage_export_csv(const Json& filter, const std::string& path);
  FsResult usage_record(const Json& row);

  // ----- settings.* -----
  FsResult settings_get(const std::string& key, const std::string& pid);
  FsResult settings_set(const std::string& key, const Json& value, const std::string& pid);
  FsResult settings_unset(const std::string& key, const std::string& pid);
  FsResult settings_dump(const std::string& pid);  // 全量（带来源标注）
  FsResult settings_detect_shells();
  FsResult settings_set_shell(const std::string& name);
  FsResult settings_set_ignore_global(const std::string& pid, bool ignore);

  // ----- plan.* -----
  FsResult plan_get(const std::string& sid);
  FsResult plan_update(const std::string& sid, const Json& planTree);
  // addon2：计划评审（approve/reject/revise + 评价建议；revise 注入引导微调）
  FsResult plan_review(const std::string& sid, const std::string& verdict,
                       const std::string& comment);

  // ----- workflow.*（addon2 长任务-工作流）-----
  FsResult workflow_define(const Json& def);
  FsResult workflow_list();
  FsResult workflow_start(const std::string& sid, const std::string& name);
  FsResult workflow_get(const std::string& runId);
  FsResult workflow_retry_step(const std::string& runId, const std::string& stepId);
  FsResult workflow_skip_step(const std::string& runId, const std::string& stepId);
  FsResult workflow_steer(const std::string& runId, const std::string& stepId,
                          const std::string& text);

  // ----- 锁（供 P3 工具层过权限门用；P1 单测直接调）-----
  FsResult lock_acquire(const std::string& sid);  // 按会话权限档申请项目写锁
  FsResult lock_release(const std::string& sid);
  FsResult lock_holder(const std::string& pid);

  // ----- tool.*（P3 真实现，经 AgentHost）-----
  FsResult tool_invoke(const std::string& sid, const std::string& name,
                       const Json& args);
  FsResult tool_stub(const std::string& name) {
    return FsResult::error(ErrCode::kNotSupported,
                           "tool not implemented until P3: " + name);
  }
  // ----- ask 回答 -----
  FsResult ask_answer(const std::string& sid, int ask_id, const std::string& selection);
  // ----- subagent.* -----
  FsResult subagent_spawn(const std::string& sid, const std::string& kind,
                          const std::string& task, const std::string& model,
                          int depth, int64_t timeout_ms);
  FsResult subagent_cancel(const std::string& id);
  FsResult subagent_list(const std::string& sid);
  // addon2：子代理引导（子代理无轮概念，只需当前引导）
  FsResult subagent_steer(const std::string& id, const std::string& text);
  // ----- steering.*（addon1.md 引导队列）-----
  FsResult steering_push(const std::string& sid, const std::string& kind,
                         const std::string& text);
  FsResult steering_list(const std::string& sid);
  FsResult provider_list_providers();
  FsResult provider_add_provider(const Json& conn);
  FsResult provider_remove_provider(const std::string& providerId);
  FsResult provider_list_models(const std::string& providerId);
  FsResult provider_test_connection(const std::string& providerId);
  // ----- skill.*（addon2：中英双语 skill，按需加载）-----
  FsResult skill_list();
  FsResult skill_load(const std::string& name);
  // 浏览器控制 skill 的加载口（addon2：不直接提供，用 /browser-load 按需加载）
  FsResult browser_load();
  // 思考语言（"auto|zh|en"）
  FsResult set_thinking_lang(const std::string& lang);
  FsResult mcp_stub() {
    return FsResult::error(ErrCode::kNotSupported, "mcp: P3");
  }

  // 内部组件访问（测试/上层组合用）
  ProjectManager& projects() { return *proj_; }
  SessionManager& sessions() { return *sessions_; }
  SettingsStore& settings() { return *settings_; }
  UsageStore& usage() { return *usage_; }
  LockArbiter& locks() { return *locks_; }
  TerminalRegistry& terminals() { return *terminals_; }
  TerminalManager& terminal_mgr() { return *terminal_mgr_; }
  DenyList& deny_list() { return *deny_list_; }
  PlanStore& plans() { return *plans_; }
  WorkflowStore& workflows() { return *workflows_; }

  // 事件订阅（P4 UI 接入；addon2 工作流推进也经此驱动）
  // 支持多个监听者（本地 UI + HTTP API 服务各一个）；返回 id 供退订
  int add_event_listener(std::function<void(const std::string&, const Json&)> fn) {
    int id = next_listener_id_++;
    event_listeners_[id] = std::move(fn);
    return id;
  }
  void remove_event_listener(int id) { event_listeners_.erase(id); }
  // 兼容旧用法：追加一个监听者
  void set_event_sink(std::function<void(const std::string&, const Json&)> sink) {
    if (sink) add_event_listener(std::move(sink));
  }

 private:
  // 确保某个 pid 的项目级 SettingsStore 已构建（无 pid 用空路径占位 = 只有全局）
  SettingsStore& store_for(const std::string& pid);
  // 项目注册表里查 pid 的路径（未注册返回空串）
  std::string path_for(const std::string& pid) const;
  // 工作流：启动某步（spawn 其 agents 的子代理）
  FsResult start_step(WorkflowRun* run, const std::string& stepId);
  // 工作流：子代理完成时推进（全 settled -> done/failed，再启下一步）
  void workflow_on_subagent_done(const std::string& subagentId);
  // 工作流：启动下一个 pending 步骤（顺序流水线）
  void start_next_pending_step(WorkflowRun* run);
  // 事件扇出（UI 订阅；未订阅则丢弃）
  void emit_event(const std::string& event, const Json& payload);

  ghc::filesystem::path root_;
  WallClock default_clock_;
  const Clock* clock_;
  XorShiftRandom default_rng_;
  Random* rng_;

  std::unique_ptr<SettingsStore> settings_;          // 全局（空项目路径）
  std::unique_ptr<SettingsStore> project_settings_;  // 按 pid 懒构建
  std::string project_settings_pid_;
  std::unique_ptr<ProjectManager> proj_;
  std::unique_ptr<SessionManager> sessions_;
  std::unique_ptr<UsageStore> usage_;
  std::unique_ptr<LockArbiter> locks_;
  std::unique_ptr<TerminalRegistry> terminals_;
  std::unique_ptr<TerminalManager> terminal_mgr_;
  std::unique_ptr<DenyList> deny_list_;
  std::unique_ptr<PlanStore> plans_;
  std::unique_ptr<WorkflowStore> workflows_;
  std::map<int, std::function<void(const std::string&, const Json&)> > event_listeners_;
  int next_listener_id_ = 1;
  // P2: provider 层
  std::unique_ptr<ProviderStore> providers_;
  std::unique_ptr<ModelTable> model_table_;
  std::unique_ptr<HttplibNetClient> net_;
  // P3: agent 宿主（Loop/工具/子代理/唤醒/引导队列）
  std::unique_ptr<AgentHost> agent_host_;
  std::unique_ptr<McpRegistry> mcp_registry_;
};

}  // namespace fs