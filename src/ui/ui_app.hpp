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
};

// Terminal entry for the selector.
struct TermItem {
  std::string tid;
  std::string shell;
  std::string label;   // user-given name ("" for model-started terminals)
  std::string status;  // "running" | "exited"
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

  // ---- home ----
  int home_tab = 1;  // 0 探索 / 1 项目 / 2 用量统计
  std::vector<ProjectCard> projects;
  std::string explore_query;
  Json usage;              // usage.query result (empty => show placeholders)
  bool usage_loaded = false;

  // ---- session (active one) ----
  std::string session_input;
  std::vector<comp::ChatMsg> messages;
  // the active session's history has been fetched. Used as the page animation's
  // replay token: the page animates when it is created, and again when the
  // history actually arrives - otherwise a slow load plays the animation over
  // an empty page and it is gone before there is anything to see.
  bool session_loaded = false;
  std::string model = "gpt-4o-mini";
  int model_index = 0;
  std::string permission = "permissive";
  int perm_index = 1;
  bool auto_mode = true;
  bool suspended = false;
  int context_pct = 42;
  std::string session_cost = "¥0.21";

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
  int side_width = 320;         // side panel width (px); persisted in state
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
  struct CallRow {
    std::string name;
    std::string args;
    bool ok = true;
  };
  std::vector<CallRow> session_calls;     // toolCalls flattened from turns
  // ---- composer / skill picker ----
  bool skill_menu_open = false;
  std::vector<std::string> skill_menu;    // skill names for the "/" picker

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
  };
  std::vector<SubagentRow> subagents;

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
  bool pv_send_session = false;  // send x-opencode-session (opencode-style endpoints)
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
  std::string history_recent = "20";
  std::string history_budget = "8000";
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
    std::string dir;          // 所属目录/来源（卡片上显示）
    bool loadable = true;
  };
  std::vector<SkillRow> skills;            // skill.list result
  bool skills_loaded = false;
  std::string skill_filter;                // 本地搜索（列表可能很长）
  std::string skill_new_dir;
  // ---- settings / MCP 与外部工具 ----
  struct McpRow {
    std::string name;
    std::string dir;
    std::string description;  // 卡片上的简介
    bool enabled = true;
  };
  std::vector<McpRow> mcp_tools;
  bool mcp_loaded = false;
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

  // ---- overlays ----
  bool ask_modal = false;
  std::string ask_text;
  int ask_id = 0;           // ask tool id (0 = permission prompt, not ask tool)
  // Generic confirm modal. Deleting a session is destructive, so it must not
  // fire straight from a click; `kind` says what to do on OK.
  std::string confirm_kind;   // "session" | ""
  std::string confirm_id;
  std::string confirm_text;
  bool plan_open = false;
  std::vector<std::string> plan_steps;
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
  std::string session_input;
  std::string model = "gpt-4o-mini";
  int model_index = 0;
  std::string permission = "permissive";
  int perm_index = 1;
  bool auto_mode = true;
  bool suspended = false;
  int context_pct = 42;
  std::string session_cost = "¥0.21";

  std::vector<SideTab> side_tabs;
  std::string active_side = "usage";
  int side_seq = 0;
  bool side_menu_open = false;
  int side_width = 320;      // side panel width (px); persisted across switches
  bool side_collapsed = false;  // side panel hidden

  std::string active_pid;

  std::vector<AppState::OpRow> session_ops;
  std::vector<AppState::CallRow> session_calls;

  bool skill_menu_open = false;
  std::vector<std::string> skill_menu;

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
  std::vector<std::string> plan_steps;
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
  // Load provider list + detected shells into settings.
  void load_providers();
  void load_shells();
  // Load subagents for a session.
  void load_subagents(const std::string& sid);
  // Load a file panel's directory (the panel keeps its own path/entries).
  void load_files(const std::string& tab_id, const std::string& pid,
                  const std::string& rel);
  // Read one file for a preview panel.
  void preview_file(const std::string& tab_id, const std::string& rel);
  // Open a file's contents in its OWN side tab (not inline under the tree).
  void open_file_preview(const std::string& rel);
  // Loaders for the settings sections.
  void load_preset_agents();
  void load_skills();
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
  void send_message();
  void open_new_session();
  // native folder dialog -> register as project (opened project list refresh)
  void pick_and_open_folder();
  // create a session under a specific project (from its card)
  void new_session_in_project(const std::string& pid);
  // 删除会话（先确认；不可恢复）
  void confirm_delete_session(const std::string& sid, const std::string& title);
  void delete_session(const std::string& sid);
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
  // 顶层 "+"：打开"新建"页面（不是标签页）
  void open_new_tab();
  // 设置/新建这类"盖在当前标签页之上、不入栈"的页面
  void open_modal_page(const std::string& page);
  void close_modal_page();
  // open a panel by kind (adds a new one; singleton kinds are opened through
  // open_or_focus_side_tab)
  void open_side_tab(const std::string& kind);
  void close_side_tab(const std::string& id);
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
  // diagnostics
  int event_count() const { return event_count_; }
  int render_count() const { return render_count_; }
  size_t message_count() const { return st_.messages.size(); }
  // test hook: the relative-time formatter (a free function in the .cpp)
  static std::string rel_time_for_test(int64_t ms);

 private:
  IDom& dom_;
  AppState st_;
  IUiBackend* backend_ = nullptr;
  VNodePtr tree_;
  Node root_{0};
  std::string last_rendered_tab_;
  std::function<void()> unsubscribe_events_;
  int64_t last_stream_turn_ = -1;
  int event_count_ = 0;
  int render_count_ = 0;
  bool render_in_progress_ = false;  // nested render deferral (sync backends)
  bool render_pending_ = false;
  // ask the next render to focus the terminal surface (only on spawn / tab
  // switch / click - focusing every render would fight the composer, since
  // pump() re-renders several times a second)
  bool term_focus_pending_ = false;
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
  void switch_tab(const std::string& id);
  // pull history + system ops + tool calls for a session into its view
  void load_session(const std::string& sid);

  // ---- page builders (return the page body for the active tab) ----
  VNodePtr build_shell();
  VNodePtr build_home();
  VNodePtr build_home_explore();
  VNodePtr build_home_projects();
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
