// agent_host.hpp - AgentHost：Loop/工具/子代理/唤醒/引导队列的宿主（FsApi 持有）
// 职责：按 sid 管理 AgentLoop；工具回调接线（terminal/net/vision）；事件扇出；落库。
#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
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
  // 模型调用（ProviderClient 包装）。第三个参数是流式回调：每段文本到达时调一次。
  std::function<FsResult(const ModelCallRequest&, ModelCallResult*,
                         const std::function<void(const std::string&)>&)>
      call_model;
  // 事件扇出（UI 订阅）
  EventFn event;
  // 终端回调（FsApi 的 TerminalManager；工具层只调回调）
  std::function<FsResult(const Json&, Json*)> terminal_spawn;
  std::function<FsResult(const Json&, Json*)> terminal_poll;
  std::function<FsResult(const Json&, Json*)> terminal_kill;
  // 网络回调（fetch/search；FsApi 的 NetClient）
  std::function<FsResult(const Json&, Json*)> net_fetch;
  // Web 搜索：{query, count} -> {hits, provider}（未配置时返回可读错误）
  std::function<FsResult(const Json&, Json*)> web_search;
  // 黑白名单判定（FsApi 的 DenyList；空 = 全部放行）
  std::function<bool(const std::string& abs_path, std::string* reason)> deny_check;
  // 设置读取（pid 空 = 只用全局；非空 = 项目覆盖全局）。上下文层用它填环境/
  // 记忆/上限。agent 层不碰 SettingsStore，所以由宿主注入。
  std::function<Json(const std::string& pid, const std::string& key, const Json& def)>
      setting;
  // 项目指令文件（AGENTS.md / .flashsight/instructions.md）。宿主读盘，
  // 因为 agent 层不做文件 IO。
  std::function<std::string(const std::string& cwd)> project_instructions;
  std::function<std::string(const std::string& cwd)> instructions_source;
  // git 摘要（branch/dirty/...）；空 = 不显示
  std::function<Json(const std::string& cwd)> git_summary;
  // 上下文窗口（token）：模型表查询；0 = 未知
  std::function<int64_t(const std::string& model)> context_limit;
  // 上一次请求的 prompt tokens（从用量日志读；重启后水位仍可用）
  std::function<int64_t(const std::string& sid)> last_prompt_tokens;
  // 规划树（PlanStore）；空 = 无规划
  std::function<Json(const std::string& sid)> plan_get;
  // Session title upkeep (placeholder, then a model summary). Called after each
  // persisted turn; empty = no automatic titles (tests).
  std::function<void(const std::string& sid)> title_upkeep;
  // Write the in-flight turn snapshot next to the session (crash safety).
  // Empty = not wired (tests).
  std::function<void(const std::string& sid, const Json& live)> write_live;
  // Persist a finished subagent's record next to the session. The record used to
  // live only in memory, so a restart made every "打开记录" button in an older
  // transcript dead. Empty = not wired (tests).
  std::function<void(const std::string& sid, const std::string& id, const Json& rec)>
      write_subagent;
  std::function<FsResult(const std::string& sid, const Json& plan)> plan_update;
  // 工作流（WorkflowStore）。工具层只转发 {op, sid, ...}
  std::function<FsResult(const Json&, Json*)> workflow_op;
  const Clock* clock = nullptr;
};

class AgentHost {
 public:
  explicit AgentHost(AgentHostDeps deps);
  ~AgentHost();
  AgentHost(const AgentHost&) = delete;
  AgentHost& operator=(const AgentHost&) = delete;

  // The ONE lock shared with FsApi. Every public method that touches loops_ /
  // session_ctx_ / subagents_ / todos_ takes it; the turn's own callbacks take
  // it too (recursive, so nesting is fine). It is never held across a model
  // call - that is what keeps suspend/approve responsive while a turn runs.
  void set_shared_mutex(std::recursive_mutex* mu) { mu_ = mu; }

  // 注册全部内置工具（file/term/net/vision/mcp/ask；ask 在自动模式下移除）
  void register_builtin_tools(McpRegistry* mcp);

  // ----- send/append（FsApi session.send/append）-----
  // model 是会话当前选中的模型：会话页换模型后立刻发送时必须带上，否则 loop
  // 会一直用创建时的模型（空串则回退到第一个 provider 的默认模型）。
  FsResult send(const std::string& sid, const std::string& text,
                const Json& attachments, bool auto_mode,
                const std::string& permission = "permissive",
                const std::string& model = "",
                const std::string& thinking = "");
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
  // One subagent's full record (transcript included). A list deliberately omits
  // the transcript, so the detail view asks for it per agent.
  FsResult subagent_detail(const std::string& id);
  // Continue a FINISHED subagent with more work, in the same context. A subagent
  // that reported back is not necessarily done with its area: the parent may
  // want "now also do X there". The run is re-opened under the same id and seeded
  // with its previous transcript, so the panel row continues rather than
  // spawning a second unrelated agent. Returns the run's summary.
  FsResult subagent_follow_up(const std::string& id, const std::string& text);

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

  // ----- 上下文压缩 -----
  // 手动压缩：下一轮构建 history 时强制压缩一次（用户点"压缩"或水位提示）。
  FsResult request_compress(const std::string& sid);
  // 当前水位（给 UI：不依赖 UI 自己算）：{promptTokens, contextLimit, pct,
  // compressAtPct, lastPromptTokens}
  FsResult context_status(const std::string& sid);
  // The in-flight turn of a session (text streamed so far, tools run so far).
  // A tab reopened mid-turn renders from this until the turn lands on disk.
  // -> {running, text, startedAt, autoRound, tools}
  FsResult live_turn(const std::string& sid);

  // ----- 测试钩子 -----
  // 子代理构建工具集后回调一次（只读代理裁剪是否正确，靠它验证）。
  // 生产代码不设，开销为零。
  void set_subagent_tool_probe(std::function<void(const Json&)> fn) {
    subagent_tool_probe_ = std::move(fn);
  }
  // 等所有子代理线程结束（测试收尾；析构也会 join）。
  void wait_subagents();
  // Join and drop finished subagent threads. A completed-but-unjoined
  // std::thread keeps its stack reserved for the life of the process, so
  // without this every subagent ever spawned leaked its stack. Called before
  // each spawn and from the periodic tick.
  void reap_subagent_threads();
  // Test hook: prune finished records for one session down to `keep`.
  int subagent_prune_for_test(const std::string& sid, size_t keep) {
    std::lock_guard<std::recursive_mutex> host_lock(lock());
    return subagents_.prune_finished(sid, keep);
  }

  // 该会话是否正在跑（有 loop 且处于运行态）。UI 用它显示会话状态：
  // "正在运行"是当前事实，不是落盘的历史，所以从内存问而不是读文件。
  bool is_running(const std::string& sid) const;
  // Live loop state for a session ("idle"|"planning"|"toolCalling"|"waiting"|
  // "turnDone"). The UI turns this into a status dot: waiting means the model is
  // blocked on the user (a permission prompt), which is different from running.
  std::string loop_state(const std::string& sid) const;
  // The auto-continue round this session is currently on (0 = not in one).
  // The UI uses it for the purple "background" dot and the round divider.
  int auto_round(const std::string& sid) const;
  // Any session with a turn in flight? Used by the UI's status refresh and by
  // FsApi's turn-thread bookkeeping.
  bool any_running() const;

 private:
  AgentHostDeps deps_;
  // Shared with FsApi (see set_shared_mutex). nullptr = single-threaded use
  // (tests), in which case the lock is simply not taken.
  std::recursive_mutex* mu_ = nullptr;
  // Convenience: one lock object for the whole host when no external mutex was
  // injected. Declared before the state it guards.
  std::recursive_mutex own_mu_;
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
  // 待落库的要点（session.keypoint 事件收集；persist_turn 时写入 turn.keyPoints）
  std::map<std::string, std::vector<Json>> pending_keypoints_;
  // emit() 会被子代理工作线程调用，而 pending_keypoints_ 也由主线程读写，
  // 所以单独一把锁（不要复用 subagent_mu_，避免锁序纠缠）。
  std::mutex keypoint_mu_;
  // 子代理执行线程（spawn 非阻塞：模型拿到 id 后可以继续做别的）。
  // 每项带子代理 id：线程跑完但没 join 时仍占着栈，reap_subagent_threads()
  // 依据记录状态把它们 join 掉并移出（否则每条子代理都泄漏一份线程栈）。
  struct SubagentThread {
    std::string id;
    std::thread thread;
  };
  std::vector<SubagentThread> subagent_threads_;
  std::mutex subagent_mu_;

  // 子代理执行器（宿主内部实现；seed 是父会话的环境快照）
  FsResult run_subagent(const Json& req, const ContextInput& seed, Json* out);
  // 每会话的上下文快照（send 时记录；子代理继承它，不继承主会话的历史）
  std::map<std::string, ContextInput> session_ctx_;
  // 子代理 → 它实现的清单项（step 标题 / todo id）。用于侧栏进度显示与
  // 完成时自动勾选；不进提示词，也不进会话历史。
  std::map<std::string, Json> subagent_steps_;
  // 子代理工具集探针（测试用；生产为空）
  std::function<void(const Json&)> subagent_tool_probe_;

  // 或建 Loop
  AgentLoop& loop_for(const std::string& sid, bool auto_mode,
                      const std::string& permission = "permissive");
  // The lock every public entry point takes. Returns own_mu_ when no external
  // mutex was injected (tests / embedded use). const so the read-only queries
  // (is_running, loop_state, todo_pending) can take it too.
  std::recursive_mutex& lock() const {
    return mu_ ? *mu_ : const_cast<std::recursive_mutex&>(own_mu_);
  }
  // 工具调用上下文
  ToolContext make_ctx(const std::string& sid, bool auto_mode);
  // 事件转发
  void emit(const std::string& event, const Json& payload);
  // 落库 turn（含 toolCalls/usage/ask/sysop）
  FsResult persist_turn(const std::string& sid, const Json& turn);  // 用当前设置/会话/项目状态重建某会话的 ContextInput。
  // 每轮 send 前调用：模型、权限、工具集、项目指令都可能已变。
  ContextInput build_context(const std::string& sid, bool auto_mode,
                             const std::string& permission,
                             const std::string& model,
                             const std::string& thinking);
  // 会话当前模型/思考档（loop 创建时读 sessionSettings，之后随 send 更新）
  void apply_session_state(AgentLoop* loop, const Json& full,
                           const std::string& sid, bool auto_mode);
  // 每轮的状态块（规划/清单/子代理/唤醒）
  RuntimeState collect_runtime(const std::string& sid);
  // 项目上下文缓存（cwd -> instructions），避免每轮读盘
  struct ProjectCtx {
    std::string instructions;
    std::string source;
  };
  std::map<std::string, ProjectCtx> project_ctx_;
};

}  // namespace fs