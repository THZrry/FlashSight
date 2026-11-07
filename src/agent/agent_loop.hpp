// agent_loop.hpp - Agent 主循环状态机（agent-loop.md）
//
// Design notes (why it looks like this):
//
// * The loop owns NO provider shape. It builds a provider-neutral conversation
//   and lets prompt::adapt() render it, so switching models mid-session (or
//   running against Anthropic) cannot produce a malformed request.
// * One model reply may contain several tool calls. They are executed in order
//   and each result is appended as its own `tool` message, matched by call id.
//   The old code lumped every result into one Anthropic-shaped user message,
//   which OpenAI-compatible endpoints ignored - the model then repeated the
//   same call until the iteration cap.
// * Every step is bounded: max tool rounds per turn, max consecutive identical
//   calls, max auto-continues. A loop that cannot make progress must stop and
//   say why, not spin.
#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "agent_context.hpp"
#include "fs_clock.hpp"
#include "fs_error.hpp"
#include "fs_json.hpp"
#include "history_builder.hpp"
#include "overfit.hpp"
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
  // The connection the user picked for this model. Two providers can serve the
  // same model id; empty = resolve by name (legacy).
  std::string provider;
  Json messages = Json::array();  // 中性消息（prompt::adapt 之前）
  Json tools = Json::array();     // 中性工具定义 [{name, description, parameters}]
  bool auto_mode = false;
  // 会话亲和 id（kPlan 套餐连接透传为 x-opencode-session，provider.md §5）
  std::string session_id;
  // 思考强度（模型档位；空 = 不发送）
  std::string thinking;
  // Abort predicate handed all the way down to the SSE reader (see
  // ChatRequest::cancel). The loop sets it to its own suspend flag, so 停止生成
  // cuts a streaming answer instead of waiting for it to finish.
  std::function<bool()> cancel;
};
struct ModelCallResult {
  std::string text;
  Json tool_calls = Json::array();  // 中性 [{id,name,args}]（已归一化）
  std::string error;                // 空=成功
  int64_t in_tokens = 0;
  int64_t out_tokens = 0;
  int64_t cache_tokens = 0;
  bool cache_known = false;  // false = the endpoint reported no cache figure
};

class AgentLoop {
 public:
  struct Options {
    HistoryBuilder::Options history;
    ContextInput context;           // 稳定上下文（宿主每轮刷新）
    EventFn event;                  // 事件推送
    std::string cwd;                // 项目根
    std::string sid;
    std::string pid;
    std::string model;
    std::string provider;                    // 模型所属连接（同名模型区分供应商）
    std::string permission = "permissive";  // 文件权限档
    std::string thinking;                   // 思考强度
    std::string thinking_lang = "";         // 思考语言（zh/en；空=不指定）
    // Safety net, not a feature: a model that never stops calling tools would
    // otherwise burn the user's quota forever. Set high enough that real work
    // never reaches it; when it trips the turn is PAUSED (not failed) and says so
    // in one line, and the user can continue.
    int max_tool_rounds = 200;              // 单轮最多模型往返次数（安全上限）
    int max_repeat_calls = 3;               // 连续相同调用上限
    // 12 stopped long tasks midway ("自动12轮后没有继续发起了"). The cap is only a
    // runaway guard, not a work budget; open todos/plan should be allowed to
    // finish. 50 rounds still bounds a runaway loop.
    int max_auto_continues = 50;            // 自动模式最多自动续跑轮数
    // 无人值守（子代理）：需要批准的动作直接拒绝并回填原因，而不是进入
    // waiting 等一个不会有人按的按钮。
    bool unattended = false;
    // 防过拟合（默认关，设置 session.overfitEnabled）。只发提示事件，
    // 后端不截断（agent-loop.md §5）。
    bool overfit_enabled = false;
  };

  explicit AgentLoop(Options opts);

  // 状态查询
  // atomic: the turn runs on its own thread while the UI polls state() to paint
  // the status dot. A plain enum read there is a data race.
  LoopState state() const { return state_.load(); }
  bool suspend_requested() const { return suspend_requested_.load(); }
  bool has_saved_context() const { return saved_valid_.load(); }

  // 会话级属性更新（FsApi 读 sessionSettings 后传入）
  void set_permission(const std::string& perm) { opts_.permission = perm; }
  void set_thinking_lang(const std::string& lang) { opts_.thinking_lang = lang; }
  void set_model(const std::string& m) { opts_.model = m; }
  // The PROVIDER is part of the model's identity: two providers can serve the
  // same model id, and the request must go to the one the user picked. Empty =
  // resolve by model name (backward compatible).
  void set_provider(const std::string& p) { opts_.provider = p; }
  const std::string& provider() const { return opts_.provider; }
  void set_thinking(const std::string& t) { opts_.thinking = t; }
  // The model this loop will send. Read by the host when it builds the context
  // and by subagents (which inherit the parent's model): `context().model` is
  // NOT it - a fresh loop has an empty ContextInput until set_context(), so
  // reading the model from there gave subagents an empty name.
  const std::string& model() const { return opts_.model; }
  // History thresholds are SETTINGS, and settings change while a session lives.
  // They used to be copied once when the loop was created, so moving the
  // compaction slider had no effect until the app was restarted - the option
  // was real but unreachable. The host re-reads them before every send.
  void set_history_options(const HistoryBuilder::Options& h) {
    // `tools` belongs to the context refresh (set_tools), not to the thresholds:
    // overwriting it here would empty the provider's tool list mid-session.
    Json tools = opts_.history.tools;
    opts_.history = h;
    opts_.history.tools = tools;
  }
  const HistoryBuilder::Options& history_options() const { return opts_.history; }
  void set_auto_mode(bool on) { opts_.context.auto_mode = on; }
  void set_overfit_enabled(bool on);
  void set_cwd(const std::string& cwd) { opts_.cwd = cwd; }
  // Provider tool list (neutral). Kept on the loop so a resumed turn can reuse
  // it without the host re-deriving it.
  void set_tools(const Json& tools) { opts_.history.tools = tools; }
  const Json& tools() const { return opts_.history.tools; }
  // 整份上下文刷新（refreshConfig / 每轮 send 前由宿主调用）
  void set_context(const ContextInput& ctx) { opts_.context = ctx; }
  const ContextInput& context() const { return opts_.context; }
  // 运行期状态（规划/清单/子代理/水位）：每轮开跑前由宿主提供
  std::function<RuntimeState()> runtime_state;
  // 上下文窗口（水位显示用）。atomic: read by the UI thread while the turn runs.
  std::atomic<int64_t> context_limit{0};
  // 用户点"压缩"/UI 提示时置位：下一轮构建 history 时强制压缩一次。
  void request_compress() { compress_requested_.store(true); }
  bool compress_requested() const { return compress_requested_.load(); }
  // 上一次请求的 prompt tokens（水位），由 Host 持久化到用量后回灌。
  void set_last_prompt_tokens(int64_t t) { last_in_tokens_ = t; }
  int64_t last_prompt_tokens() const { return last_in_tokens_; }

  // send：用户输入（含引导队列合并），进入 planning
  FsResult send(const std::string& text);
  // append：追加输入（引导队列 >1 合并）
  FsResult append(const std::string& text);

  // 权限门（approve 档）：批准/拒绝当前等待的 tool
  FsResult approve(bool yes);
  // ask 回答：把用户的选择作为 ask 工具的结果回填，然后继续本轮
  FsResult answer_ask(int ask_id, const std::string& selection);

  // Is this turn an auto-continue round (not a user message)? The UI uses it to
  // draw the divider and the purple "background" dot instead of a user bubble.
  bool is_auto_round() const { return current_auto_round_.load() > 0; }
  int auto_round() const { return current_auto_round_.load(); }

  // 挂起/恢复/取消唤醒
  //
  // 挂起 is GRACEFUL: the model call in flight finishes, its tools run, and the
  // loop parks before starting another round. Nothing is cut, so the transcript
  // stays consistent and 恢复 simply continues.
  void suspend();
  // 中止 is the hard stop: abort the reply being streamed and park with what
  // arrived. Separate flag on purpose - one "stop" cannot mean both "let this
  // round finish" and "cut it now".
  void interrupt();
  void resume();
  // Continue a turn that the suspend parked. `resume()` alone only clears the
  // request flag for a turn that is STILL running; once the loop actually parked
  // (it returns between tool batches), nothing is running to notice the flag, so
  // the task stayed stopped forever ("挂起之后点恢复没反应"). This re-runs the
  // saved transcript without appending a user message.
  FsResult resume_task();
  // True while a turn is parked between tool batches because of a suspend.
  bool parked() const { return parked_.load(); }

  // 中断后续接（addon2）：给原任务发送特定引导，从保存点继续
  FsResult continue_task(const std::string& guidance);

  // 最后一轮的文本输出 / 消息数组（子代理摘要用）
  const std::string& last_text() const { return last_text_; }
  const Json& last_messages() const { return last_messages_; }

  // Live turn snapshot, for a UI that (re)opens a session while it is running.
  //
  // The transcript on disk only has the finished turns, so a tab closed during
  // a turn and reopened later showed nothing of the answer that was streaming -
  // the work was still happening, invisibly. This is the in-memory half of
  // "实时保存": the user turn is on disk immediately, and the in-flight answer
  // is served from here until the turn ends and is written.
  // -> {running, text, startedAt, autoRound, tools}
  Json live_state() const;
  // Throttled write of live_state() to disk (see the .cpp for why).
  void maybe_persist_live();

  // 模型调用抽象（测试可注入）
  std::function<FsResult(const ModelCallRequest&, ModelCallResult*,
                         const std::function<void(const std::string&)>&)>
      call_model;

  // 会话历史（Host 注入：返回 full.json）
  std::function<FsResult(Json*)> load_history;
  // 落库一轮
  std::function<void(const Json&)> persist_turn;
  // Write the IN-FLIGHT turn to disk (a crash-safe snapshot), called every couple
  // of seconds while a turn runs. The turn is only appended to the session when it
  // finishes, so a crash mid-turn used to lose everything the model had produced -
  // the user saw exactly that after a webview crash. `sid` + the payload let the
  // host stash it next to the session (runtime.json) and replay it on reopen.
  // Empty = not wired (tests).
  std::function<void(const std::string& sid, const Json& live)> persist_live;
  // Ask the model for a short session title. Called by the host when the turn
  // count or the tool-call count makes the placeholder title worth replacing.
  // Empty = not wired (tests).
  std::function<void(const std::string& sid, const std::string& text)> summarize_title;
  // 工具执行抽象（Host 注入）
  std::function<FsResult(const ToolContext&, const std::string&, const Json&)> invoke_tool;

 private:
  Options opts_;
  // atomic: the turn runs on its own thread while the UI polls these to paint
  // the status dot / the suspend button. A plain enum/bool read there is a
  // data race, and it is the one place a torn value would actually show.
  std::atomic<LoopState> state_{LoopState::kIdle};
  std::atomic<bool> suspend_requested_{false};
  // Hard stop (中止): aborts the streaming reply itself. Kept apart from
  // suspend_requested_ so 挂起 can mean "finish this round, then wait".
  std::atomic<bool> abort_requested_{false};
  // Set when the loop parks because of a suspend (it returned at a tool-batch
  // boundary with the transcript saved). Distinguishes "parked by the user from
  // a still-running turn" - only the former needs resume_task to continue.
  std::atomic<bool> parked_{false};

  std::vector<std::string> steering_queue_;

  // 等待中的工具（approve 门）
  std::string pending_tool_;
  Json pending_args_ = Json::object();
  std::string pending_call_id_;
  std::string current_call_id_;
  Json pending_tool_results_ = Json::array();

  // Approval gate state. A model reply can carry several tool calls; when one
  // of them needs the user's yes, the rest must not be lost. `gate_messages_`
  // is the conversation up to and including the assistant message that asked,
  // `gate_calls_` the calls still to run, `gate_index_` the one being asked
  // about. Approving executes it, appends the tool message and resumes.
  Json gate_messages_ = Json::array();
  Json gate_calls_ = Json::array();
  size_t gate_index_ = 0;
  Json gate_tool_msgs_ = Json::array();  // results already produced in this group

  // 本轮执行过的调用（落库用）
  Json executed_calls_ = Json::array();
  // Between-tools narration of this turn, in order. It is the model's own running
  // commentary ("let me check X first"), not the answer - stored with the turn so
  // the work log can show it after a reload instead of losing it.
  Json interim_texts_ = Json::array();
  // For each entry of interim_texts_: how many tool calls had already run when
  // it arrived. That is where the note belongs in the work stream (a round can
  // issue several tools, so the note index and the tool index are not the same
  // thing). Persisted as `interimAfter`.
  std::vector<int> interim_after_;

  // 防过拟合
  OverfitDetector overfit_;
  std::vector<std::string> recent_outputs_;

  // 中断-继续保存点
  Json saved_messages_ = Json::array();
  std::atomic<bool> saved_valid_{false};
  std::string saved_user_text_;

  std::string last_user_text_;
  // Streaming text of the turn in flight, appended as deltas arrive. Read by
  // live_state() so a reopened tab can show an answer that is still being
  // written. Guarded by live_mu_ (the UI thread reads it).
  mutable std::mutex live_mu_;
  std::string live_text_;
  // Narration from COMPLETED rounds, kept apart from the current round's text.
  // Without this, reopening a running tab concatenated every round's narration
  // into the answer ("切页面思考跑到任务输出").
  Json live_interim_ = Json::array();
  std::vector<int64_t> live_interim_after_;
  Json live_tools_ = Json::array();
  int64_t live_started_ms_ = 0;
  bool live_running_ = false;
  // Throttle for maybe_persist_live (ms of the last write).
  int64_t last_live_persist_ms_ = 0;
  int64_t turn_started_ms_ = 0;
  // The user turn was written to disk when the turn STARTED, so the end-of-turn
  // persist must not write it a second time.
  bool user_turn_persisted_ = false;
  int64_t last_in_tokens_ = 0;
  int64_t last_out_tokens_ = 0;
  int auto_continues_ = 0;
  std::string last_text_;
  Json last_messages_ = Json::array();
  std::atomic<bool> compress_requested_{false};
  // 当前轮是不是自动续跑轮（>0 = 第 N 轮自动续跑）。UI 据此画分隔线与后台小圆点，
  // 也据此决定是否把这一轮渲染成"用户消息"。
  std::atomic<int> current_auto_round_{0};
  // ask 工具挂起时的现场。一个回复里可能有多条 ask（"1/2" 那种），所以是一组。
  Json pending_asks_ = Json::array();  // [{askId, callId, question, options, ...}]
  Json ask_answers_ = Json::object();  // askId -> selection（回答齐了才续跑）
  int ask_seq_ = 0;

  FsResult run_turn(const std::string& user_text);
  // Write ONLY the user turn. Called when the turn fails before any answer
  // exists: the user's message must still be in the session file, otherwise
  // reopening the session shows the answer without the question.
  void persist_user_turn_only(int64_t now_ms);
  // Persist a turn that died mid-flight (provider error, tool exception, a
  // thrown nlohmann dump). Everything the turn produced is written - partial
  // answer, executed tool calls, narration - plus an `error` field, so reopening
  // the session shows what happened instead of nothing at all.
  bool persist_failed_turn(const std::string& error, const std::string& partial_text);
  FsResult run_loop(Json messages, const std::string& model, const Json& tools);
  // The body of run_loop. The public entry wraps it, so ANY exception still
  // persists the turn and reports `loop.interrupted` instead of unwinding to the
  // caller with the turn lost.
  FsResult run_loop_impl(Json messages, const std::string& model, const Json& tools);
  // Close a turn that died: persist what it produced, report it, return an error.
  FsResult fail_turn(const std::string& error);
  // Execute one assistant reply's tool calls; returns when the group is done,
  // parked on an approval, or interrupted.
  FsResult run_tool_group(const Json& calls, Json* messages);
  FsResult execute_tool(const std::string& name, const Json& args);
  FsResult call_model_once(const Json& messages, const Json& tools,
                           ModelCallResult* out);
  void emit(const std::string& event, const Json& payload);
  bool is_high_risk(const std::string& tool) const;
  bool needs_approval(const std::string& tool, const Json& args) const;
  // 自动模式：规划/清单未闭合则再跑一轮（有上限）
  bool should_auto_continue(std::string* why);
  // Tool list actually in force: context.tools wins, history.tools is the
  // legacy fallback. Both the prompt index and the provider list use this.
  const Json& active_tools() const;
  // 上下文水位 + 本轮状态块
  std::string runtime_block();
};

}  // namespace fs
