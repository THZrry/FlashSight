// ui_app.hpp - User layer (rules/addon3.md "user"): the actual FlashSight UI.
//
// Model: one observable AppState; any change re-renders the whole VNode tree;
// patch() applies the minimal DOM diff. No widget objects, no manual DOM
// bookkeeping - the app is a pure function from state to tree.
//
// Navigation (per user feedback, single mode):
//   * top tab strip: 主页 / 会话N / 设置 - tabs can be added and closed
//   * home: centered title buttons 探索/项目/用量统计 (no background, only
//     color + underline); 项目 renders project cards whose body is a wide
//     expandable session list
//   * session: chat article layout + composer + free side-panel tabs
//   * settings: two-column settings page
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "ui_backend.hpp"
#include "ui_components.hpp"
#include "ui_dom.hpp"
#include "ui_vdom.hpp"

namespace fs {
namespace ui {

// One session tab.
struct SessionTab {
  std::string id;      // "s1", "s2", ...
  std::string title;   // "会话1"
  bool dirty = false;
};

// One project card (home/项目).
struct ProjectCard {
  std::string pid;
  std::string name;
  std::string path;
  std::string updated;              // "2 分钟前"
  int session_count = 0;
  // The session list is always rendered (CSS picks the layout: wide shows it
  // directly, narrow collapses it behind the header), so it is always loaded.
  bool sessions_loaded = false;
  bool expanded = false;            // narrow layout: list revealed?
  // A session row carries more than a title: the card shows WHEN it last
  // changed and WHETHER it is running, and both have to be deletable.
  struct SessionRow {
    std::string sid;
    std::string title;
    int64_t updated_ms = 0;  // full.json mtime; 0 = unknown
    bool running = false;    // a turn is in flight right now
    // Live loop state ("waiting" = blocked on the user) and the last turn's
    // error. Together with `running` these pick the status dot's colour:
    // blue = done, red = failed, yellow = waiting for you, spinner = running.
    std::string loop_state;
    std::string last_error;
    // Live terminals still running for this session. A session with a running
    // terminal can still produce output while no turn is in flight, so it gets
    // the "background" dot rather than "idle".
    int terminal_count = 0;
    // Auto-continue round in progress (0 = none). A session in an auto round is
    // doing background work even though no user message started it.
    int auto_round = 0;
    bool renaming = false;   // inline rename in progress
    std::string rename_text; // draft while renaming
  };
  std::vector<SessionRow> sessions;
};

// Side panel tab (session page). Free-form: add/close, and a kind that supports
// multiple instances keeps its OWN state here - two file panels are two
// different directories, so they cannot share one set of fields.
struct SideTab {
  std::string id;
  std::string title;
  std::string kind;   // "usage" | "files" | "ops" | "git" | "subagents" | "calls"
  // per-panel state for the kinds that carry one (files today; git will need
  // its own path/filter the same way)
  std::string path;            // files: directory being listed ("" = project root)
  struct FileEntry {
    std::string name;
    bool is_dir = false;
    int64_t size = 0;
  };
  std::vector<FileEntry> entries;
  bool loaded = false;
  std::string preview_path;
  std::string preview;
  // subagents detail panel: which subagent's transcript this panel shows
  // ("" = the list of all subagents). A transcript is long, so it gets its own
  // SIDE PANEL rather than a full-window tab: the sidebar is where subagent
  // history belongs, and a page could not be closed back to the conversation.
  std::string subagent_id;

  // ---- git panel (one per panel: two git panels can show two repos) ----
  struct GitChange {
    std::string path;
    std::string orig_path;
    char index = ' ';
    char work = ' ';
    bool staged = false;
    bool untracked = false;
    bool conflicted = false;
  };
  bool git_loaded = false;
  bool git_repo = false;
  std::string git_root;
  std::string git_branch;
  std::string git_upstream;
  std::string git_operation;   // merge / rebase / ... ("" = none)
  std::string git_error;
  int git_ahead = 0;
  int git_behind = 0;
  bool git_detached = false;
  std::vector<GitChange> git_changes;
  // The file whose diff is open ("" = none). `git_diff_staged` picks which side:
  // the index diff is what a commit would record, the work-tree diff is what is
  // still un-staged, and they are genuinely different questions.
  std::string git_diff_path;
  bool git_diff_staged = false;
  std::string git_diff_text;
  bool git_diff_truncated = false;
  std::string git_diff_error;
  // "changes" (the default) | "log" | "branches". The panel is one tab with
  // three views instead of three panels: they all answer "what is the state of
  // this repository", and stacking tabs for that would bury the file list.
  std::string git_view = "changes";
  struct GitCommit {
    std::string hash;
    std::string short_hash;
    std::string author;
    std::string subject;
    int64_t time = 0;
  };
  std::vector<GitCommit> git_commits;
  bool git_log_loaded = false;
  struct GitBranch {
    std::string name;
    bool current = false;
  };
  std::vector<GitBranch> git_branch_rows;
  bool git_branches_loaded = false;
  // The committed version of the diffed file (git.showHead), shown side by side
  // with the working copy when the user asks for "before".
  bool git_show_head = false;
  std::string git_head_text;
  bool git_head_exists = false;
  // Commit message draft. Kept per panel so switching away does not lose typing.
  std::string git_message;
  bool git_busy = false;
};

// Terminal entry for the selector.
struct TermItem {
  std::string tid;
  std::string shell;
  std::string label;   // user-given name ("" for model-started terminals)
  std::string status;  // "running" | "exited"
};

// A wake-up the backend will fire for this session (auto mode). Declared at
// namespace scope because both AppState and SessionView hold one.
struct PendingWake {
  std::string source;   // "timer" | "terminal-exit" | "subagent-done"
  std::string note;     // what the user scheduled it for
  int64_t at_ms = 0;
  int64_t in_ms = 0;
};

// The app state. Everything the UI shows derives from this.
struct AppState {
  // ---- top tabs ----
  std::vector<std::string> tab_ids;   // "home", "s1", "settings"
  std::string active_tab = "home";
  std::vector<SessionTab> sessions;
  int session_seq = 0;
  // Visit stack (most recent LAST). Closing the active tab goes BACK to where
  // you came from rather than jumping to a fixed tab: "closed a tab and landed
  // somewhere unrelated" is the whole reason this exists. Entries for tabs that
  // have since closed are skipped when popping.
  std::vector<std::string> tab_history;
  // 设置/新建这类页面**不是标签页**：它们点开就直接显示，关掉就销毁，也不进
  // 访问栈（进栈会让"关掉一个标签页回到设置"这种荒谬的回退发生）。它们盖在
  // 当前标签页之上，关掉就回到原来那个标签页。
  std::string modal_page;  // "" | "settings" | "new"

  // ---- 左侧栏模式（设置·外观·标签位置=左侧）----
  // 它不是"竖着放的标签条"：顶栏模式的标签是打开的工作集，可以关；左侧栏是
  // 项目导航，永远按项目列出全部会话，不关不消失。宽度独立于右侧面板，拖拽
  // 只改变它和中间栏之间的分配，右侧栏位置不动。
  bool nav_collapsed = false;
  int nav_width = 240;  // px

  // ---- home ----
  int home_tab = 1;  // 0 探索 / 1 项目 / 2 用量统计 / 3 搜索
  std::vector<ProjectCard> projects;
  std::string explore_query;
  // 全局搜索（主页标签栏最右的放大镜、新建页的搜索框、左栏搜索都跳到这页）
  std::string search_query;
  // 会话列表的排序方式："" = 最近活动，否则按标题/状态
  std::string session_sort;   // "" | "title" | "status"
  Json usage;              // usage.query result (empty => show placeholders)
  bool usage_loaded = false;
  std::string usage_scope;  // provider id filter ("" = 总计)
  int usage_range = 1;      // index into 1天/7天/30天/全部

  // ---- session (active one) ----
  std::string session_input;
  // 编辑并重发 armed on a turn: its text is back in the composer and the next
  // send REPLACES that turn (and everything after it) instead of appending. The
  // trim happens at send time, not on the click - pressing 编辑 and then changing
  // your mind must not destroy the answer.
  std::string edit_turn_id;
  // Attached files/images shown as chips ABOVE the input (a path is not text:
  // mixing it into the message made it impossible to tell what was attached).
  std::vector<std::string> attachments;
  std::vector<comp::ChatMsg> messages;
  // Paging: a session can hold hundreds of turns, so only the newest
  // `kHistoryPage` are loaded initially. Scrolling to the top asks for the
  // previous page; those turns are then FROZEN (one rev each, never re-parsed).
  // `history_total` is how many turns exist on disk, so the UI knows when it has
  // reached the beginning.
  int history_total = 0;
  // Absolute index (into the stored turn array) of the OLDEST loaded turn. This
  // is NOT messages.size(): a page can skip turns that are not user/assistant,
  // and starting the next page from the count would re-fetch the same range.
  int history_start = 0;
  bool history_has_more = false;
  bool history_loading_older = false;
  // The scroll anchor recorded before prepending older turns: the distance from
  // the bottom. Restoring it keeps the reader looking at the same line.
  int history_anchor_from_bottom = -1;
  // The page animation's replay token (see session_loaded).
  bool session_loaded = false;
  // Chat scroll offset for this session. Saved when leaving the tab and put back
  // on the way in; a first visit starts at the BOTTOM (the latest turn is what
  // you came for, not the beginning of the conversation).
  int hist_scroll = -1;  // -1 = never measured: start at the bottom
  // Is the view pinned to the end? Turned on when the user lands at the bottom,
  // off as soon as they scroll up. New output only follows the end while this is
  // true - yanking the view down while someone reads history is worse than not
  // following.
  bool hist_follow = true;
  std::string model;
  int model_index = 0;
  // Whether `model` still exists among the ENABLED models. A session remembers
  // the model it last used, but that model can disappear - the user disabled it,
  // or the whole provider was deleted. Sending would then fail with a provider
  // error unrelated to "this model is gone", so the composer clears the model box
  // and blocks the send button instead.
  bool model_available = true;
  // Checked against model.allEnabled; empty = not asked yet.
  std::vector<std::string> available_model_ids;
  // Thinking levels the CURRENT model was configured with (model table), plus its
  // provider's display name. Both come from model.allEnabled: guessing levels from
  // the model NAME (thinking_levels_for) missed every custom/new model, so the
  // 思考强度 control silently never rendered.
  std::vector<std::string> model_thinking_levels;
  // The model's OWN configured default level (model table `thinking.default`).
  // The composer pre-selects this instead of inventing a "默认" row: the row the
  // user configured in the model settings is the one that should be shown, and a
  // literal "默认" option next to concrete levels is meaningless (what does
  // "default" mean when the model already named a default?). Empty = inherit.
  std::string model_thinking_default;
  std::string model_provider_label;
  std::string model_check_sid;
  std::string permission = "permissive";
  int perm_index = 1;
  bool auto_mode = true;
  bool suspended = false;
  // A suspend/resume request is in flight. The button is disabled meanwhile: the
  // backend decides when the loop actually parks (it finishes the current tool
  // first), so a second click before the reply sent the OPPOSITE request.
  bool suspend_pending = false;
  // A turn is running for this session. The composer's primary button is 发送
  // while false and 中止 while true.
  bool turn_running = false;
  // The loop was interrupted (model failed / user stopped it). Shows a bar with
  // a "继续" action: the backend can resume from the saved point, and a toast
  // alone left the user with nothing to press.
  bool interrupted = false;
  // Why the loop stopped (provider error / HTTP status). Shown in the interrupt
  // bar: "interrupted" without a reason reads like a UI bug.
  std::string interrupt_reason;
  // Scheduled wake-ups for this session (auto mode). Shown in the same bar slot
  // as the interrupt notice: both mean "the task is parked and something will
  // happen without you", so they belong in the same place above the input.
  std::vector<PendingWake> pending_wakes;
  // Inline "schedule a wake-up" input in the composer (auto mode only).
  bool schedule_open = false;
  std::string schedule_text;
  // "enter" (default) or "ctrlEnter": whether plain Enter sends. It is a SESSION
  // setting, and the DOM adapter reads it from the textarea's data-enter-send
  // attribute, so it must live in the session state.
  std::string enter_send = "enter";
  // Context water level + per-session cost/tokens, all from usage.session.
  // -1 / "" mean "not loaded yet": the panel shows a placeholder rather than a
  // made-up number (it used to display a hard-coded 42% and ¥0.21).
  int context_pct = -1;
  // Absolute water level: the numbers behind the percentage. A percentage alone
  // cannot answer "is there room for one more file read" - 42% of a 1M window
  // and 42% of a 32k window are different situations.
  int64_t context_tokens = -1;   // last request's prompt tokens
  int64_t context_window = 0;    // model context window (0 = unknown)
  // Cache: `cache_hit_pct` is -1 when the provider never reported cache fields
  // (that is not the same as a 0% hit rate).
  int cache_hit_pct = -1;
  int64_t session_cache_tokens = 0;
  bool cache_reported = false;
  // Cost kind for the session: money | quota | unknown. A quota provider
  // reports cost 0 inside the plan, so "¥0.00" would be a lie.
  std::string session_cost_kind = "unknown";
  int64_t usage_calls = 0;       // recorded model calls in this session
  // 思考档位（会话级）：空 = 用模型默认。可选值来自当前模型自己的档位表
  // （粗匹配 + 用户在模型设置里的覆盖），不是全局固定枚举。
  std::string thinking;
  std::string session_cost;
  int64_t session_tokens_in = -1;
  int64_t session_tokens_out = 0;
  bool session_usage_loaded = false;
  // This turn's usage (rows since the last user message). -1 = unknown.
  int64_t turn_cost_micros = -1;  // money in micro-units (avoids float compare)
  int64_t turn_in = -1;
  int64_t turn_out = -1;
  std::string turn_cost_kind;

  // ---- side panel ----
  std::vector<SideTab> side_tabs;
  std::string active_side = "usage";
  // Visit stack for the side panel, same purpose as tab_history: closing the
  // active panel returns to the previous one instead of jumping to the first.
  std::vector<std::string> side_history;
  int side_seq = 0;
  // "+" shows a temporary PAGE listing every panel kind (with a one-line
  // explanation), not a small popup: it is where new panels are discovered, and
  // a menu could not carry the descriptions. Picking one opens that panel.
  bool side_menu_open = false;
  int side_width = 420;         // side panel width (px); persisted in state
  bool side_collapsed = false;  // side panel hidden (toggle in the divider)
  // ---- side panel / 文件树 ----
  // The file tree state lives per PANEL (SideTab::path/entries/preview), because
  // two file panels can sit in two different directories. Only the project of
  // the active session is shared.
  std::string active_pid;                 // project of the active session
  // ---- side panel / 操作记录 + 工具调用记录 ----
  struct OpRow {
    std::string op;
    std::string detail;
  };
  std::vector<OpRow> session_ops;         // systemOps from session.get
  // (No tool-call list here: the 工具调用 panel derives its rows from the
  // transcript's own tool blocks, so it cannot drift from what is on screen.)
  // ---- composer / skill picker ----
  bool skill_menu_open = false;
  // The 思考档位 list opened by the composer's thinking button. It is the ONLY
  // thinking control (the old select + dead icon button pair read as two).
  bool thinking_menu_open = false;
  // The command menu's contents come from command.list (backend), so the UI has
  // no hard-coded list to keep in sync.
  struct CommandRow {
    std::string name;
    std::string title;
    std::string description;
    std::string action;  // "ui:..." or a backend method name
    std::string group;   // "builtin" | "skill"
  };
  std::vector<CommandRow> command_menu;
  // ---- composer inline completion ----
  // Typing "/" at the very START of the input opens the command (skill) list;
  // typing "/" or "./" LATER opens file/directory candidates for the fragment
  // under the caret. Both are "hints": nothing is inserted until the user picks.
  enum class CompleteKind { kNone, kCommand, kPath };
  CompleteKind complete_kind = CompleteKind::kNone;
  std::string complete_query;   // what was typed after the trigger
  int complete_index = 0;       // keyboard selection (↑/↓)
  // Path completion context: the directory being listed ("" = project root) and
  // the partial name being filtered.
  std::string complete_dir;
  std::string complete_prefix;
  std::vector<std::pair<std::string, bool> > complete_items;  // (name, isDir)

  // ---- terminal ----
  std::vector<TermItem> terminals;
  std::string active_term;   // terminal used for 停止 etc.
  bool terminal_open = false;
  int term_height = 340;     // drawer height (px); dragged on the top edge
  // per-terminal accumulated output (tid -> text). "所有终端" concatenates
  // these; a specific tab shows just its own. Kept per terminal so switching
  // tabs never loses what another one printed.
  std::map<std::string, std::string> term_outputs;
  std::string term_tab = "all";  // selected tab: "all" | tid
  // The terminal engine's screen grids (tid -> terminal.grid reply). A terminal
  // is a grid of styled cells, not a text stream, so this - not term_outputs -
  // is what the panel draws. term_outputs stays for the aggregate overview and
  // for copy.
  std::map<std::string, Json> term_grids;
  // creating a terminal is one click: it uses the shell from settings, and the
  // name can be edited on the tab afterwards
  std::string term_renaming;      // tid whose tab is being renamed ("" = none)
  std::string term_rename_text;
  // last size reported to the backend (so we only resize on a real change)
  int term_cols = 0;
  int term_rows = 0;
  // DECCKM for the selected terminal (arrows are SS3 vs CSI). Read from its
  // grid; needed when encoding a key press.
  bool term_app_keys = false;
  // tab display order (the user's arrangement, not the backend's tid sort)
  std::vector<std::string> term_order;
  // terminals the user closed the tab for. The PROCESS keeps running (the model
  // may still be using it, and only 停止 kills it); it is just not shown. Kept
  // per session so a closed tab does not come back on the next refresh, while a
  // terminal the model starts later still appears normally.
  std::vector<std::string> term_hidden;
  // ---- terminal search (shown once the strip gets crowded) ----
  std::string term_filter;         // live pattern; "" = no filtering
  bool term_filter_input = false;  // also match the commands typed into it
  bool term_filter_text = false;   // also match its whole screen (output)
  // what has been typed into each terminal (tid -> command text). Kept so
  // "输入匹配" can search commands, which are not part of the terminal's NAME.
  std::map<std::string, std::string> term_input_log;

  // ---- subagents ----
  struct SubagentRow {
    std::string id;
    std::string kind;
    std::string status;
    std::string task;
    std::string model;
    std::string summary;
    int64_t started_at = 0;
    int64_t ended_at = 0;
    // Full transcript (the record's `full`): a subagent's conversation does not
    // belong in the main chat, but it must be READABLE - otherwise "what did it
    // actually do" has no answer.
    Json transcript = Json::array();
    bool open = false;      // transcript expanded in the sidebar
    // What the subagent is doing right now (its streamed text so far). Filled
    // from the tagged stream.delta events, so the panel can show live work
    // instead of just "running".
    std::string live_text;
    // The steer box's draft. Steering is a typed instruction, not a fixed
    // "继续" - the button that sent a constant read as doing nothing.
    std::string steer_draft;
  };
  std::vector<SubagentRow> subagents;
  // Loaded transcripts for subagent TABS, keyed by subagent id. Kept separate
  // from the list rows: the list omits transcripts (they are huge), and a tab
  // fetches its own on open.
  struct SubagentDetail {
    bool loaded = false;
    SubagentRow row;
  };
  std::map<std::string, SubagentDetail> subagent_details;

  // ---- workflows (long-task pipelines) ----
  struct WorkflowDefRow {
    std::string name;
    std::string description;
    int steps = 0;
  };
  struct WorkflowStepRow {
    std::string id;
    std::string name;
    std::string state;   // pending|running|done|failed|skipped
    int attempts = 0;
    std::string error;
  };
  struct WorkflowRunRow {
    std::string id;
    std::string name;
    std::string state;   // pending|running|done|failed
    std::vector<WorkflowStepRow> steps;
    bool open = true;
  };
  std::vector<WorkflowDefRow> workflow_defs;
  bool workflow_defs_loaded = false;
  std::vector<WorkflowRunRow> workflow_runs;
  bool workflow_runs_loaded = false;
  bool workflows_inflight = false;

  // ---- settings / 外观 ----
  // Read from the settings store (ui.theme / ui.font / ui.codeHighlight) and
  // pushed to the DOM adapter, which owns the CSS variables.
  std::string ui_theme = "default";   // palette name
  std::string ui_mode = "auto";       // "auto" | "light" | "dark"
  std::string ui_font;                // ui.font.family ("" = built-in stack)
  std::string ui_mono;                // ui.font.monoFamily
  int ui_font_size = 0;               // ui.font.size (0 = built-in size)

  // ---- settings / 通用 ----
  std::string settings_nav = "general";  // section key; nav and body always agree
  std::string thinking_lang = "auto";
  std::string ui_lang = "auto";          // "auto|zh|en"; drives i18n in-process
  // ---- settings / 供应商（多提供商管理）----
  struct ProviderRow {
    std::string id;
    std::string name;
    std::string kind;
    std::string endpoint;
    bool enabled = true;
    bool has_key = false;
  };
  std::vector<ProviderRow> providers;
  bool providers_loaded = false;
  // 余额 / 额度（provider.balances）：一家一行。status = ok | unsupported |
  // failed；失败的原因在 note 里，UI 原样显示（"获取失败：HTTP 401"），绝不补 0。
  struct BalanceRow {
    std::string pid;
    std::string name;
    std::string endpoint;
    std::string status;
    std::string currency;
    std::string balance;
    std::string used;
    std::string limit;
    std::string note;
    bool enabled = true;
  };
  std::vector<BalanceRow> balances;
  bool balances_loading = false;
  int64_t balances_at = 0;   // last successful query (ms since epoch)
  struct ProviderTemplate {
    std::string id;
    std::string name;
    std::string kind;
    std::string base_url;
    std::string description;
    std::vector<std::pair<std::string, std::string> > headers;
  };
  std::vector<ProviderTemplate> provider_templates;
  bool provider_templates_loaded = false;
  std::string provider_selected;   // id being edited ("" = none selected)
  bool provider_creating = false;  // new-provider wizard open
  bool pv_template_picked = false; // template chosen -> show the form
  // draft fields for the create/edit form
  std::string pv_name;
  std::string pv_kind = "openai-chat";
  std::string pv_base_url;
  std::string pv_key;
  std::vector<std::pair<std::string, std::string> > pv_headers;  // custom headers
  std::string pv_new_header_k;
  std::string pv_new_header_v;
  bool pv_send_session = false;  // legacy: 兼容旧配置（新配置走 {{SESSION}} 头）
  std::string pv_body_text;      // 请求体覆盖（JSON 文本草稿）
  bool pv_advanced = false;                   // "高级" section expanded
  std::vector<std::string> provider_models;   // listModels result
  std::string provider_models_level;          // "api" | "preset"
  std::string provider_status;                // test/connect result line

  // ---- 逐模型设置（provider.models）----
  // 模型不是供应商的一个属性：能力、思考档位、重试都因模型而异，所以模型
  // 自己是一等对象。这里缓存当前选中 provider 的模型列表 + 正在编辑的草稿。
  struct ModelRow {
    std::string id;
    std::string display;
    // Which provider this model belongs to. model.allEnabled returns models
    // across providers, so a row has to carry its owner.
    std::string provider;
    std::string icon;          // lobe-icons 键（空 = 用通用图标）
    bool enabled = true;
    bool user_set = false;     // 用户保存过 -> 可删除
    bool from_api = false;
    bool from_preset = false;
    // 能力
    bool vision = false;
    bool audio = false;
    bool video = false;
    bool pdf = false;
    bool structured = false;
    bool tool_use = false;
    bool thinking_cap = false;
    long long context_len = 0;
    long long max_output = 0;
    // 档位 + 重试
    std::vector<std::string> thinking_levels;
    std::string thinking_default;
    int retry_max = 0;
    int retry_backoff_ms = 0;
  };
  std::vector<ModelRow> models;        // 当前 provider 的模型列表
  bool models_loaded = false;
  std::string models_provider;         // models 属于哪个 provider
  std::string model_selected;          // 选中要编辑的模型 id
  bool model_creating = false;         // 新增模型草稿
  std::string model_filter;            // 模型列表搜索
  std::string model_status;            // 测试结果行
  // 模型草稿（编辑中，未保存）
  ModelRow model_draft;
  std::string model_new_id;            // 新增时输入的模型名
  std::string model_new_level;         // 新增思考档位的输入草稿
  // 会话里的模型选择器（弹出层）：左供应商 / 右模型
  bool model_picker_open = false;
  // Which widget opened it. The picker is a real component reused by the
  // composer and by settings fields, so exactly one place may render it.
  std::string model_picker_owner;
  // What a pick does: "" = the session's model, otherwise a settings key. This
  // is what lets the same component serve both without a second code path.
  std::string model_picker_target;
  std::string model_picker_provider;
  std::string model_picker_query;
  // The model the picker should OPEN ON. The left column is a provider list, so
  // opening on whatever provider was last browsed in settings showed an empty
  // right pane for a session whose model lives elsewhere ("不会自动放到选择的
  // 那一页"). Resolved once per open, when the model list arrives.
  std::string model_picker_focus;
  std::vector<ModelRow> model_picker_models;   // 选中 provider 的启用模型
  bool model_picker_loaded = false;
  std::string model_picker_status;
  // Detected shells (name + resolved path). The path is what makes the list
  // useful: two entries named "pwsh" from different installs are otherwise
  // indistinguishable, and the user may want to see which one will run.
  struct ShellRow {
    std::string name;
    std::string path;
  };
  std::vector<ShellRow> shell_rows;  // settings.detectShells result
  int shell_index = 0;
  // Water level at which the agent compacts (% of the model's window).
  // (There is no "recent turns window" or "token budget" field: neither is a
  // setting any more - they folded history on every turn regardless of the water
  // level, which is exactly the "每轮都压缩" bug.)
  std::string history_compress_at = "80";
  // The keep-turns count has NO UI field on purpose: it is an internal constant
  // (the settings page used to expose it, and a knob nobody can observe reads as
  // a fake control). The backend still honours the key for old configs.
  // section payloads (settings.get replies, cached per section)
  Json settings_values = Json::object();   // key -> {value, source}
  bool settings_dump_loaded = false;
  bool settings_dump_inflight = false;
  std::string settings_loaded_for;  // nav section whose loads were triggered
  struct PresetRow {
    std::string name;
    std::string description;
    bool enabled = true;
    bool read_only = false;
  };
  std::vector<PresetRow> preset_agents;
  bool presets_loaded = false;
  struct SkillRow {
    std::string name;
    std::string title;
    std::string description;
    std::string dir;
    std::string kind;      // "skill" (callable as /name) | "mcp" (built-in MCP)
    bool loadable = true;
    bool enabled = true;
    bool user = false;     // imported/created by the user: may be deleted
  };
  std::vector<SkillRow> skills;            // skill.list result
  bool skills_loaded = false;
  bool skills_inflight = false;            // request in flight (build-driven load)
  std::string skill_filter;                // 本地搜索（列表可能很长）
  std::string skill_new_dir;
  // Inline "new skill" form: a user skill is name + title + body, and the body
  // is what the model receives when it loads it.
  bool skill_creating = false;
  std::string skill_new_name;
  std::string skill_new_title;
  std::string skill_new_body;
  // ---- settings / MCP 与外部工具 ----
  struct McpRow {
    std::string name;
    std::string dir;          // 旧的"目录导入"来源（kind=dir）
    std::string description;  // 卡片上的简介
    // A real MCP server definition (kind=server): what to run / where to connect.
    std::string kind;         // "server" | "dir"
    std::string server_command;
    std::string server_url;
    std::vector<std::string> server_args;
    bool enabled = true;
  };
  std::vector<McpRow> mcp_tools;
  bool mcp_loaded = false;
  bool mcp_inflight = false;  // request in flight (build-driven load)
  std::string mcp_new_name;
  std::string mcp_new_dir;
  std::string mcp_filter;   // 本地搜索
  std::vector<std::pair<std::string, std::string> > long_memory;  // 长期记忆
  std::vector<std::pair<std::string, std::string> > env_vars;     // 环境变量
  std::vector<std::pair<std::string, std::string> > cli_tools;    // CLI 工具
  // add-row drafts for the editable key/value tables
  std::string mem_new_key;
  std::string mem_new_value;
  std::string env_new_key;
  std::string env_new_value;
  std::string cli_new_key;
  std::string cli_new_value;
  std::string file_blacklist_text;  // 文件黑名单（多行编辑草稿）
  std::string file_whitelist_text;  // 文件白名单（多行编辑草稿）
  std::string about_version = "0.1.0";
  // Web 服务当前状态（设置-Web 服务页回显；server.status）
  bool server_running = false;
  std::string server_host;
  int server_port = 0;
  bool server_requires_password = false;
  std::string server_lan_url;
  // 上一次重新监听失败的原因（空 = 正常）
  std::string server_error;
  // Typing drafts for the Web-service fields. They hold what is in the box while
  // the user types; only Enter commits (a per-keystroke commit would rebind the
  // listener once per digit).
  std::string server_port_draft;
  std::string server_password_draft;

  // ---- overlays ----
  // One overlay serves two different questions, and they must not be confused:
  //   ask_id == 0  -> a PERMISSION prompt ("allow this tool?"), answered with
  //                   session.approve(yes/no).
  //   ask_id  > 0  -> an ASK question (options + optional custom input),
  //                   answered with tool.askAnswer(selection).
  bool ask_modal = false;
  std::string ask_text;                 // the question / the prompt line
  int ask_id = 0;                       // ask tool id (0 = permission prompt)
  // One ASK prompt can contain several questions (the model asks 2-3 things at
  // once). They are answered in order with a "1/3" counter, and the composer
  // area shows them instead of the input box - a floating dialog covered the
  // thing being discussed and its options read as plain text.
  struct AskItem {
    int ask_id = 0;
    std::string question;
    std::vector<std::string> options;
    int recommended = 0;
    bool allow_custom = true;
    std::string custom_draft;
    std::string answer;   // empty = not answered yet
  };
  std::vector<AskItem> asks;
  int ask_index = 0;                    // which question is showing (1/x in the UI)
  std::vector<std::string> ask_options; // legacy single-question view
  int ask_recommended = 0;
  bool ask_allow_custom = true;
  std::string ask_custom_draft;         // legacy single-question view
  // Auto-continue round counter for the active session (0 = not in an auto round).
  // Drives the "auto round" divider in the transcript and the purple dot.
  int auto_round = 0;
  // Generic confirm modal. Deleting a session is destructive, so it must not
  // fire straight from a click; `kind` says what to do on OK.
  std::string confirm_kind;   // "session" | "rollback" | "forget" | ""
  std::string confirm_id;
  std::string confirm_arg;    // second value (turnId / permission level)
  std::string confirm_text;
  // ---- right-click menu ----
  // The browser's native menu is suppressed (see the DOM adapters); this is what
  // replaces it. Only the actions that make sense for what was right-clicked are
  // offered, so it is never a wall of disabled browser items.
  bool ctx_open = false;
  int ctx_x = 0;              // viewport coords of the click
  int ctx_y = 0;
  std::string ctx_kind;       // "message" | "tab" | "session" | "composer" | ""
  std::string ctx_id;         // turnId / tab id / session id
  std::string ctx_label;      // what the menu header shows
  bool plan_open = false;
  // The plan is model-authored state with a STATUS per step, mirroring the
  // backend's plan tree ({title,status,note?}). A flat vector<string> could not
  // show progress, and progress is the whole point of the panel - it is also
  // what the loop reads to decide whether auto mode continues.
  struct PlanStep {
    std::string title;
    std::string status = "pending";  // pending|running|done|skipped|blocked
    std::string note;
  };
  std::vector<PlanStep> plan_steps;
  std::string plan_goal;
  // Inline plan editing: -1 = no step being edited. The plan is user-editable,
  // not just model output, so the panel needs an edit path.
  int plan_editing = -1;
  std::string plan_edit_text;
  std::string plan_review_text;
  std::string toast;
  int64_t toast_deadline_ms = 0;  // pump() clears the toast after this

  static AppState make();
};

// Everything that belongs to ONE session tab. The app keeps the active view
// spread flat over AppState (so builders read plain fields) and stashes it
// here when the user switches tabs, restoring it on the way back. Without this
// The app's per-scope UI state. The framework keeps ONE live copy (the flat
// fields on AppState) and stashes the rest in a KeyedStore<SessionView>; the
// scoping mechanism itself is generic (ui_vdom.hpp KeyedStore), this struct is
// just the payload for "a session tab" scopes.
struct SessionView {
  bool loaded = false;  // history/panels fetched from the backend yet?
  std::vector<comp::ChatMsg> messages;
  bool session_loaded = false;
  int history_total = 0;
  // Absolute index (into the stored turn array) of the OLDEST loaded turn. This
  // is NOT messages.size(): a page can skip turns that are not user/assistant,
  // and starting the next page from the count would re-fetch the same range.
  int history_start = 0;
  bool history_has_more = false;
  bool history_loading_older = false;
  int history_anchor_from_bottom = -1;
  int hist_scroll = -1;  // chat scroll offset (see AppState::hist_scroll)
  bool hist_follow = true;  // pinned to the end (see AppState::hist_follow)
  std::string session_input;
  std::string edit_turn_id;  // see AppState::edit_turn_id
  std::vector<std::string> attachments;
  // 空 = 还没选模型。凭空给一个 gpt-4o-mini 会让人以为它已经配好且能用，而
  // 实际供应商可能一个模型都没连；按钮显示"选择模型"，或在设置里配了会话
  // 默认模型时用那一个（见 build_session / send_message）。
  std::string model;
  int model_index = 0;
  // See AppState::model_available: false = the remembered model is gone, so the
  // composer shows an empty model box and the send button is disabled.
  bool model_available = true;
  std::vector<std::string> available_model_ids;
  // See AppState: levels configured for the current model, and its provider label.
  std::vector<std::string> model_thinking_levels;
  std::string model_thinking_default;
  std::string model_provider_label;
  std::string model_check_sid;
  std::string permission = "permissive";
  int perm_index = 1;
  bool auto_mode = true;
  bool suspended = false;
  // See AppState: a pending suspend/resume request, and whether a turn is running.
  bool suspend_pending = false;
  bool turn_running = false;
  bool interrupted = false;
  // Why the loop stopped (provider error / HTTP status). Shown in the interrupt
  // bar: "interrupted" without a reason reads like a UI bug.
  std::string interrupt_reason;
  std::vector<PendingWake> pending_wakes;
  bool schedule_open = false;
  std::string schedule_text;
  std::string enter_send = "enter";
  int context_pct = -1;
  int64_t context_tokens = -1;
  int64_t context_window = 0;
  int cache_hit_pct = -1;
  int64_t session_cache_tokens = 0;
  bool cache_reported = false;
  std::string session_cost_kind = "unknown";
  int64_t usage_calls = 0;
  // 思考档位（会话级）：空 = 用模型默认。可选值来自当前模型自己的档位表
  // （粗匹配 + 用户在模型设置里的覆盖），不是全局固定枚举。
  std::string thinking;
  std::string session_cost;
  int64_t session_tokens_in = -1;
  int64_t session_tokens_out = 0;
  bool session_usage_loaded = false;
  int64_t turn_cost_micros = -1;
  int64_t turn_in = -1;
  int64_t turn_out = -1;
  std::string turn_cost_kind;

  std::vector<SideTab> side_tabs;
  std::string active_side = "usage";
  int side_seq = 0;
  bool side_menu_open = false;
  int side_width = 420;      // side panel width (px); persisted across switches
  bool side_collapsed = false;  // side panel hidden

  std::string active_pid;

  std::vector<AppState::OpRow> session_ops;

  bool skill_menu_open = false;
  bool thinking_menu_open = false;
  std::vector<AppState::CommandRow> command_menu;
  // Mirrors AppState's inline completion (see there).
  AppState::CompleteKind complete_kind = AppState::CompleteKind::kNone;
  std::string complete_query;
  int complete_index = 0;
  std::string complete_dir;
  std::string complete_prefix;
  std::vector<std::pair<std::string, bool> > complete_items;

  std::vector<AppState::WorkflowDefRow> workflow_defs;
  bool workflow_defs_loaded = false;
  std::vector<AppState::WorkflowRunRow> workflow_runs;
  bool workflow_runs_loaded = false;

  std::vector<TermItem> terminals;
  std::string active_term;
  bool terminal_open = false;
  int term_height = 340;
  std::map<std::string, std::string> term_outputs;
  std::string term_tab = "all";
  std::map<std::string, Json> term_grids;
  std::string term_renaming;
  std::string term_rename_text;
  int term_cols = 0;
  int term_rows = 0;
  bool term_app_keys = false;
  // Tab display order (tids). The backend returns terminals sorted by tid, but a
  // tab strip is a WORKING SET: a terminal you just created or reopened belongs
  // at the end, where you left it, not back in numeric position. New tids are
  // appended here as they appear; reopening moves one to the end.
  std::vector<std::string> term_order;
  std::vector<std::string> term_hidden;
  std::string term_filter;
  bool term_filter_input = false;
  bool term_filter_text = false;
  std::map<std::string, std::string> term_input_log;

  std::vector<AppState::SubagentRow> subagents;

  bool plan_open = false;
  // Same shape as AppState::plan_steps (see the note there): status is what the
  // panel displays and what the loop reads.
  std::vector<AppState::PlanStep> plan_steps;
  std::string plan_goal;
  // Inline plan editing: -1 = no step being edited. The plan is user-editable,
  // not just model output, so the panel needs an edit path.
  int plan_editing = -1;
  std::string plan_edit_text;
  std::string plan_review_text;
};

// UiApp: owns the render loop. backend_ is the transport to the core
// (NullBackend today; NativeBackend/WhaleBackend later).
class UiApp {
 public:
  UiApp(IDom& dom, AppState state, IUiBackend* backend = nullptr);
  ~UiApp();

  void mount();
  // re-render whole tree and patch
  void render();
  void show(const std::string& tab_id);

  AppState& state() { return st_; }
  // mutate + render in one call (the common path)
  void update(const std::function<void(AppState&)>& f);

  // Drive backend events (browser polls; desktop is a no-op). Call from a
  // timer/loop; applies stream.delta/turn.done/permission.prompt/etc.
  void pump();
  // Terminal-only refresh, on a much faster timer than pump(): a terminal has to
  // feel like a keyboard, and waiting for the 250ms event pump made keystrokes
  // appear in batches instead of one by one.
  void tick_terminal();
  // Async backend call with a completion handler (browser-safe). The sync
  // call() is unavailable in the wasm build, so all UI actions go through here.
  void call_async(const std::string& method, const Json& args,
                  std::function<void(const BackendResult&)> done = nullptr);
  // Pull project.list / session.list from the backend into AppState.
  void load_projects();
  // Pull terminal.list for the active session.
  void load_terminals(const std::string& sid);
  // Open (or focus) a session tab and load its history from the backend.
  void open_session(const std::string& sid, const std::string& title = "");
  // Load a project's session list into its card.
  void load_sessions(size_t project_index);
  // Load usage summary into the usage page.
  void load_usage();
  // Balance / credit for every provider (provider.balances). `force` ignores the
  // staleness window; the in-flight guard is always honoured.
  void load_balances(bool force = false);
  // Load per-session usage for the side panel (context level, cost, tokens).
  void load_session_usage(const std::string& sid);
  // Pull the in-flight turn from the backend and append it as a streaming
  // message. The disk transcript only has finished turns, so without this a
  // reopened tab showed nothing while an answer was still being written.
  void attach_live_turn(const std::string& sid);
  // Load provider list + detected shells into settings.
  void load_providers();
  void load_shells();
  // Load subagents for a session.
  void load_subagents(const std::string& sid);
  // Send the typed guidance to one subagent (the sidebar's steer box).
  void send_subagent_steer(size_t index);
  // Workflows: definitions (what can be started) and runs (what is happening).
  void load_workflows(const std::string& sid);
  // Load a file panel's directory (the panel keeps its own path/entries).
  void load_files(const std::string& tab_id, const std::string& pid,
                  const std::string& rel);
  // Read one file for a preview panel.
  void preview_file(const std::string& tab_id, const std::string& rel);
  // Open (or focus) a subagent's transcript in the SIDEBAR (a "subagent" panel).
  void open_subagent_tab(const std::string& subagent_id, const std::string& title);
  // Load one subagent's full record into its tab (transcript included).
  void load_subagent_detail(const std::string& subagent_id);

  // ---- composer inline completion ("/" commands, "/" and "./" paths) ----
  // Recompute the hint from the CURRENT input text (called on every edit).
  void update_complete_from_input(const std::string& text);
  // Insert the highlighted candidate (Tab / click / Enter).
  void accept_complete();
  // Rows the menu currently shows (commands, or path candidates).
  int complete_row_count() const;
  // The floating menu above the textarea.
  VNodePtr build_complete_menu();
  // ---- git panel ----
  // Refresh status for one git panel (branch / ahead-behind / changed files).
  void load_git(const std::string& tab_id);
  // Fetch the diff for one path into that panel (staged selects the index side).
  void load_git_diff(const std::string& tab_id, const std::string& path, bool staged);
  // Stage / unstage / discard / commit. Each refreshes the panel on success.
  void git_stage_paths(const std::string& tab_id, const std::vector<std::string>& paths,
                       bool stage);
  void git_stage_all(const std::string& tab_id);
  void git_discard_paths(const std::string& tab_id,
                         const std::vector<std::string>& paths);
  void git_commit(const std::string& tab_id, const std::string& message, bool amend);
  // Log / branches / HEAD views of the same panel (the backend is one repo).
  void load_git_log(const std::string& tab_id);
  void load_git_branches(const std::string& tab_id);
  void load_git_head(const std::string& tab_id, const std::string& path);
  // Open a file's contents in its OWN side tab (not inline under the tree).
  void open_file_preview(const std::string& rel);
  // Execute one entry of the composer's command menu. "ui:*" actions are handled
  // here; anything else is a backend action (a skill load, suspend, ...).
  void run_command(const AppState::CommandRow& cmd);
  // Index of the message whose stable uid matches (-1 when it is gone). Message
  // handlers MUST resolve their target through this: a cached node keeps the
  // closure it was built with, and prepending an older page shifts every index.
  int find_message_by_uid(int64_t uid) const;
  // A per-turn action from the message menu: "fork" | "rollback" | "forget".
  void turn_action(size_t message_index, const std::string& action);
  // Session operations addressed by a turn (the turn id lives on the message).
  void rollback_to_turn(const std::string& sid, const std::string& turn_id);
  void forget_turn(const std::string& sid, const std::string& turn_id);
  void fork_from_turn(const std::string& sid, const std::string& turn_id);
  // Ask the backend for this session's loop state (idle / suspended / ...) and
  // mirror it into `suspended`. The button label follows the BACKEND, not the
  // click: suspension is a request that the loop may take a moment to honour.
  void refresh_loop_state(const std::string& sid);
  // Replies to session.loopState arrive asynchronously and can be OLDER than an
  // event that already changed the state (a `loop.interrupted` that cleared
  // 中止). Only the reply to the LATEST request is applied.
  int loop_state_seq_ = 0;
  // Pull the session's pending wake-ups (scheduled tasks / event wakes) into
  // state so the composer can show a countdown and offer to cancel them.
  void refresh_pending_wakes();
  // Project/nav session rows carry running / terminal / auto-round status, and
  // nothing refreshed them when those changed. `mark_sessions_dirty()` is called
  // from the events that do change them; pump() coalesces the refresh.
  void mark_sessions_dirty() { sessions_dirty_ = true; }
  void refresh_session_rows();
  // Re-read the open session's systemOps (the 操作记录 panel is a live log: it
  // used to be fetched once, when the session opened).
  void refresh_session_ops();
  // Override a session row with the LIVE state of the active session, so the dot
  // reacts to a turn/terminal instantly instead of waiting for the list refresh.
  ProjectCard::SessionRow live_session_row(ProjectCard::SessionRow row) const;
  // The status dot for a session row: live state for the active session, and a
  // "finished/failed" dot only while it is still news (cleared once you open it).
  H dot_for(const ProjectCard::SessionRow& row) const;
  // When each session was last shown. The backend's loopState stays "turnDone"
  // after a turn, so the blue dot used to be permanent.
  std::map<std::string, int64_t> session_seen_ms_;
  // Set once, when the 供应商 page finds no providers and opens the create
  // wizard by itself. One shot: re-opening it after the user closed it would
  // fight them.
  bool provider_auto_opened_ = false;
  bool sessions_dirty_ = false;
  int64_t sessions_refresh_ms_ = 0;
  // A workflow event (proposed / step / rejected) changed what the panel shows.
  bool workflows_dirty_ = false;
  int64_t workflows_refresh_ms_ = 0;
  int64_t last_ops_refresh_ms_ = 0;
  // 读取 Web 服务当前监听状态（设置-Web 服务页）
  void refresh_server_status();
  // Ask which models are usable and clear the composer's model box if the one
  // this session remembered has been disabled or its provider deleted.
  void refresh_model_availability();
  // Loaders for the settings sections.
  void load_preset_agents();  void load_skills();
  void load_provider_templates();
  void load_mcp_tools();
  void load_settings_section(const std::string& section);
  // Save a settings key (global) and reflect it locally.
  void set_setting(const std::string& key, const Json& value);
  // Parse a settings value map ({key:{value,...}}) into local caches.
  void apply_settings(const Json& values);
  // Push the appearance settings to the DOM adapter (palette/mode/fonts). Called
  // after apply_settings and whenever one of them changes.
  void apply_appearance();

  // actions (public: also callable from tests and future menu bindings)
  void toast(const std::string& text);
  // The core of a send once the text is decided: optimistic echo, then
  // session.send. Split out because 编辑并重发 has to trim the old turn FIRST and
  // only then send, so its callback needs to run exactly this.
  void dispatch_message(const std::string& sid, const std::string& text,
                        const std::string& shown, const Json& attachments);
  void send_message();
  void open_new_session();
  // native folder dialog -> register as project (opened project list refresh)
  void pick_and_open_folder();
  // create a session under a specific project (from its card)
  void new_session_in_project(const std::string& pid);
  // 删除会话（先确认；不可恢复）
  void confirm_delete_session(const std::string& sid, const std::string& title);
  void delete_session(const std::string& sid);
  // 重命名会话：行内编辑，失焦/Enter 提交
  void begin_rename_session(const std::string& sid);
  void commit_rename_session(const std::string& sid);
  // handle a drop anywhere on the window. kind: "dir" | "file" | "url" | "text"
  // (a folder is opened as a project; a file name / url / text goes to the
  // composer as an attachment reference)
  void handle_drop(const std::string& kind, const std::string& value);
  // handle a dropped folder name: resolve to a path, then open as project
  void handle_dropped_folder(const std::string& name);
  // an upload finished: insert the saved files' paths into the composer so the
  // model can read them (the bytes never reach the UI)
  void handle_upload_reply(const std::string& reply_json);
  // create a terminal under the active session, running the shell from settings
  void start_terminal(const std::string& shell, const std::string& label);
  // is this terminal's tab closed? (it keeps running; the overview lists it)
  bool term_tab_closed(const std::string& tid) const {
    for (size_t i = 0; i < st_.term_hidden.size(); ++i) {
      if (st_.term_hidden[i] == tid) return true;
    }
    return false;
  }
  // terminals in display order (the user's arrangement, not the backend's tid
  // sort) - the strip and the overview both read this
  std::vector<const TermItem*> ordered_terminals() const;
  // terminals matching the current filter, in display order (empty filter = all)
  std::vector<const TermItem*> filtered_terminals() const;
  // the filter row itself (only built once the strip is crowded)
  H build_terminal_search();
  // send one key press to the selected terminal (VT-encoded by the term engine)
  void send_terminal_key(const KeyPress& k);
  // send the queued keystrokes in order (one write in flight at a time)
  void flush_terminal_keys();
  // pull the screen grid for one terminal (what the panel draws)
  void load_terminal_grid(const std::string& tid);
  // tell the backend how many cells fit (ConPTY forwards it to the child)
  void resize_terminal(int cols, int rows);
  // rename a terminal's tab (label is stored in the registry)
  void rename_terminal(const std::string& tid, const std::string& label);
  // Remove a terminal from the tab strip WITHOUT killing it: the process keeps
  // running (the model may still be using it) and it stays listed in the
  // overview, where clicking it brings the tab back. Only 停止 ends it.
  void close_terminal_tab(const std::string& tid);
  // bring a closed terminal's tab back (the overview card does this on click)
  void reopen_terminal_tab(const std::string& tid);
  void close_tab(const std::string& id);
  // reorder a tab by dropping it onto another (keys, not indices: the strip is
  // keyed by tab id)
  void move_tab(const std::string& from, const std::string& to);
  // re-apply the persisted tab order (ui.tabOrder) to the open tabs
  void apply_saved_tab_order();
  // 顶层 "+"：打开"新建"页面（不是标签页）
  void open_new_tab();
  // 设置/新建这类"盖在当前标签页之上、不入栈"的页面
  void open_modal_page(const std::string& page);
  void close_modal_page();
  // open a panel by kind (adds a new one; singleton kinds are opened through
  // open_or_focus_side_tab)
  void open_side_tab(const std::string& kind);
  void close_side_tab(const std::string& id);
  // reorder a side panel by dropping it onto another
  void move_side_tab(const std::string& from, const std::string& to);
  // activate the existing tab of `kind`, or create one. Singleton kinds focus;
  // multi kinds (files, git) always add.
  void open_or_focus_side_tab(const std::string& kind);
  // load one panel's own data (each panel has its own state)
  void load_side_tab(const std::string& id);
  // ---- settings / 供应商 actions ----
  void select_provider(const std::string& id);   // load into the edit form
  void begin_new_provider(const std::string& template_id);
  void save_provider();                          // upsert the draft
  void delete_provider(const std::string& id);
  void test_provider(const std::string& id);
  void fetch_provider_models(const std::string& id);
  // ---- settings / 逐模型 actions ----
  void load_models(const std::string& provider_id);   // fill st_.models
  void select_model(const std::string& model_id);     // load into the draft
  void begin_new_model();
  void save_model();
  void delete_model(const std::string& model_id);
  void toggle_model_enabled(const std::string& model_id, bool enabled);
  void test_model(const std::string& model_id);
  // ---- composer / 模型选择器 ----
  // owner: which widget opened it (only that one renders the popup).
  // target: "" = the session's model, otherwise a settings key.
  void open_model_picker(const std::string& owner, const std::string& target);
  void pick_model(const std::string& provider_id, const std::string& model_id);
  void load_picker_models(const std::string& provider_id);
  // The popup itself. Shared by the composer and by settings fields so there is
  // one model picker in the app, not two that drift apart.
  H build_model_picker(const std::string& owner, const std::string& current);
  // A settings field's model button (opens the picker for `setting_key`).
  H build_model_field(const std::string& owner, const std::string& setting_key,
                      const std::string& current, const std::string& placeholder);
  // ---- settings / MCP actions ----
  void detect_mcp();
  void add_mcp();
  void remove_mcp(const std::string& name);
  void toggle_mcp(const std::string& name, bool enabled);
  // ---- settings / Skill actions ----
  void add_skill();
  // Skill directories (SKILL.md / <name>.md), separate from MCP server config.
  void detect_skills();
  // Does `key` ("Ctrl+W") match the binding configured for `action`? Reads the
  // Settings > 快捷键 value, falling back to `def`. The shortcut table's values
  // were never consulted before, which made the whole page decorative.
  bool shortcut_matches(const std::string& key, const std::string& action,
                        const std::string& def);
  // ---- settings / 环境（CLI 工具探测）----
  void detect_cli_tools();
  // 选文件并把结果写进某个设置键（驱动路径 / CLI 工具路径）
  void pick_file_into(const std::string& setting_key, const std::string& filter_desc,
                      const std::string& filter_spec);
  // CLI 工具路径的两种选文件场景
  void pick_cli_tool_path(const std::string& key);
  void pick_cli_tool_draft();
  // ---- settings / key-value tables (长期记忆 / 环境变量 / CLI 工具) ----
  void add_kv(const std::string& key_setting, const std::string& k, const std::string& v);
  void remove_kv(const std::string& key_setting, const std::string& k);
  // test hook: apply a backend event as the sink would
  void on_backend_event_for_test(const std::string& event, const Json& payload) {
    on_backend_event(event, payload);
  }
  // test hook: the current VNode tree. Keys are framework-level identities and
  // deliberately do not reach the DOM, so list-keying can only be asserted here.
  const VNodePtr& tree_for_test() const { return tree_; }
  // test hook: rebuild one stored turn exactly as the loader does.
  comp::ChatMsg turn_to_message_for_test(const Json& t, int64_t abs_index) {
    return turn_to_message(t, abs_index);
  }
  // test hook: what the composer's input handler does on every edit.
  void on_input_for_test(const std::string& text) {
    st_.session_input = text;
    update_complete_from_input(text);
  }
  // diagnostics
  int event_count() const { return event_count_; }
  int render_count() const { return render_count_; }
  size_t message_count() const { return st_.messages.size(); }
  // test hook: the relative-time formatter (a free function in the .cpp)
  static std::string rel_time_for_test(int64_t ms);
  // test hook: resolve a message by its stable uid (see find_message_by_uid)
  int find_message_by_uid_for_test(int64_t uid) const { return find_message_by_uid(uid); }

 private:
  IDom& dom_;
  AppState st_;
  IUiBackend* backend_ = nullptr;
  VNodePtr tree_;
  Node root_{0};
  std::string last_rendered_tab_;
  std::function<void()> unsubscribe_events_;
  int64_t last_stream_turn_ = -1;
  // Last time the wake countdown was refreshed (the label only needs to be
  // right to the minute, so this throttles the request to once per 5 s).
  int64_t last_wake_refresh_ms_ = 0;
  // Last repaint of the live "generating for 12s" counter (once a second).
  int64_t last_stream_tick_ms_ = 0;
  // Stream text arrived since the last paint, and when that paint happened. See
  // pump(): deltas are coalesced instead of rendering once per token.
  bool stream_dirty_ = false;
  // A programmatic scroll restore is in flight. The scroll handler must not write
  // the reported offset back while this is set: a restore against a container that
  // is still short gets clamped, and saving that clamped value is what made a
  // return visit land at the top of the transcript.
  bool restoring_scroll_ = false;
  // A restore is owed to the next render: the remembered offset, and whether the
  // view was pinned to the end. Applied right after the patch, before paint.
  bool pending_scroll_apply_ = false;
  int pending_scroll_target_ = -1;
  bool pending_scroll_follow_ = false;
  // A subagent's live preview changed. Painted from pump(), never per delta:
  // a subagent emits hundreds of deltas per second and repainting each one
  // froze the app seconds after it started.
  bool subagent_stream_dirty_ = false;
  int64_t last_stream_paint_ms_ = 0;
  int event_count_ = 0;
  int render_count_ = 0;
  // How many times refresh_server_status has re-read while a rebind was in
  // flight; capped so a stuck rebind cannot poll forever.
  int server_status_polls_ = 0;
  bool render_in_progress_ = false;  // nested render deferral (sync backends)
  bool render_pending_ = false;
  // ask the next render to focus the terminal surface (only on spawn / tab
  // switch / click - focusing every render would fight the composer, since
  // pump() re-renders several times a second)
  bool term_focus_pending_ = false;
  // Build-driven loads (the explore page needs the skill/MCP lists) must not
  // re-request on every render while the first reply is still in flight - that
  // request storm starved the event loop and made clicks look dead.
  bool skills_inflight_ = false;
  bool mcp_inflight_ = false;
  // same idea for the inline session-rename input: focus it on the render that
  // starts the rename, not on every render (pump re-renders ~4x/sec and would
  // steal focus back from whatever the user clicked).
  bool session_rename_focus_ = false;
  // a grid fetch is in flight (pump re-reads the visible terminal's screen; the
  // guard keeps a slow reply from stacking up requests)
  bool term_grid_inflight_ = false;
  // round-robin cursor for the overview's grid refresh (one terminal per tick,
  // so the request rate stays bounded however many terminals are open)
  size_t term_grid_rr_ = 0;
  // is the terminal view scrolled to the bottom? New output only follows the
  // view when it is (yanking the view down while someone reads history is worse
  // than not following).
  bool term_at_bottom_ = true;
  // Keys are bytes and their ORDER is the meaning. Writes go over an async
  // transport, so two keystrokes could otherwise land out of order (a fast
  // typist's "d" arriving after the Enter). Keystrokes therefore queue here and
  // exactly one write is in flight at a time.
  std::string term_key_queue_;
  bool term_key_inflight_ = false;
  // per-scope UI state: framework KeyedStore keyed by tab id. The flat fields
  // on AppState are the live copy of whichever scope is active.
  KeyedStore<SessionView> views_;
  std::string last_session_key_;  // sid whose view the flat fields currently hold
  // (The context menu's node is looked up by key in render(): a cached Node went
  // stale the moment the menu was rebuilt for another target.)
  // Rendered message nodes, keyed by "msg:<sid>:<index>". A session can hold
  // hundreds of turns; without this every render re-parsed every markdown block
  // and rebuilt every node, and the diff then compared all of them against
  // themselves. A message that did not change hands back the SAME VNode, so the
  // framework's pointer-equality short circuit skips the whole subtree.
  //
  // Validity is checked by revision AND a cheap structural signature. The
  // revision is the fast path (a streamed delta bumps it); the signature exists
  // because state is also mutated directly by handlers and tests, and a cache
  // that silently served a stale node whenever someone forgot to bump a number
  // would be a very unpleasant bug.
  struct MsgNode {
    VNodePtr node;
    uint64_t rev = 0;
    uint64_t sig = 0;
  };
  std::map<std::string, MsgNode> msg_nodes_;
  // Monotonic revision source: every mutation (a streamed delta, a fold toggle,
  // a freshly loaded history) takes a NEW number.
  uint64_t next_msg_rev_ = 1;
  // Cheap fingerprint of the fields that affect a message's rendering: sizes and
  // flags only, no string comparisons.
  static uint64_t message_sig(const comp::ChatMsg& m);
  // 方向感知：这次切页是"前进"还是"后退"。前进（去了一个不在访问栈里的页）
  // 从右侧滑入，后退（回到栈顶那一页，比如关掉标签回到来处）从左侧滑入，方向
  // 与实际导航方向一致。在 switch_tab 里按 tab_history 判定，build_shell 读它
  // 选动画意图。"1"=前进，"-1"=后退，空=默认。
  std::string page_dir_;
  // The body currently playing its entrance. A switch triggers several renders
  // (the click, then the panel's own data load); re-deriving "am I entering?"
  // from the previous key on every render made the second render drop the delay
  // mid-animation, which is why the panel snapped in instead of sliding.
  std::string side_entering_key_;
  // backend event handler (bound in mount)
  void on_backend_event(const std::string& event, const Json& payload);
  // route a session-scoped event into that session's view (active or stashed)
  void apply_session_event(const std::string& sid, const std::string& event,
                           const Json& payload);
  // append streamed text to the last assistant message (creating it if needed)
  void append_stream_delta(const std::string& text);
  // find the active session id ("" if the active tab is not a session)
  std::string active_sid() const;

  // ---- per-tab state isolation ----
  // stash the flat session fields into views_[sid]
  void save_session_view(const std::string& sid);
  // load views_[sid] into the flat session fields (fresh defaults when absent)
  void load_session_view(const std::string& sid);
  // switch to `id`, saving the current tab's view and restoring the target's
  void switch_tab(const std::string& id, int dir = 0);
  // pull a session's info + its NEWEST page of history into its view
  void load_session(const std::string& sid);
  // Ask the NEXT render to position the transcript at the remembered offset in
  // the same frame it patches the content (see the definition for why a deferred
  // restore is wrong).
  void request_history_scroll_restore();
  // Apply a pending restore; called at the end of render(), after the patch.
  void apply_pending_history_scroll();
  // prepend the previous page of history (called when the user scrolls to the top)
  void load_older_history();
  // keep a pinned view at the end after content was appended (call AFTER render)
  void follow_history_if_pinned();
  // Turn JSON -> ChatMsg (frozen, one revision). abs_index is the turn's
  // position in the stored session and becomes the message's stable identity.
  comp::ChatMsg turn_to_message(const Json& t, int64_t abs_index);
  // how many turns one page holds
  static const int kHistoryPage = 30;

  // ---- page builders (return the page body for the active tab) ----
  VNodePtr build_shell();
  // the normal window content (tab strip / nav rail + page slot), split out so
  // a modal page can be layered ON TOP of it without replacing it
  void build_shell_into(H& win);
  // the content area both navigation modes share (also the frame the leaving
  // page is pinned to during a two-view transition)
  VNodePtr build_page_slot();
  // 左侧栏（标签位置=左侧）：按项目列出全部会话的导航栏，不是竖着的标签条。
  VNodePtr build_nav_rail();
  VNodePtr build_home();
  VNodePtr build_home_explore();
  VNodePtr build_home_projects();
  // 全局搜索页：跨项目搜会话（名称/路径），点结果直接打开
  VNodePtr build_home_search();
  VNodePtr build_home_usage();
  // 顶层 "+" 的页面：项目列表 + 每个项目的会话（点会话即开、点新建即建）
  VNodePtr build_new_tab();
  VNodePtr build_session();
  VNodePtr build_settings();
  VNodePtr build_side_panel();
  VNodePtr build_terminal_drawer();
  VNodePtr build_overlays();
};

}  // namespace ui
}  // namespace fs
