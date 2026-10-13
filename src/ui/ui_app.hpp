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
  bool expanded = false;            // session list loaded?
  std::vector<std::pair<std::string, std::string> > sessions;  // sid, title
};

// Side panel tab (session page). Free-form: add/close.
struct SideTab {
  std::string id;
  std::string title;
  std::string kind;   // "usage" | "files" | "ops" | "git" | "subagents" | "calls" | "terminal"
};

// Terminal entry for the selector.
struct TermItem {
  std::string tid;
  std::string shell;
  std::string status;  // "running" | "exited"
};

// The app state. Everything the UI shows derives from this.
struct AppState {
  // ---- top tabs ----
  std::vector<std::string> tab_ids;   // "home", "s1", "settings"
  std::string active_tab = "home";
  std::vector<SessionTab> sessions;
  int session_seq = 0;

  // ---- home ----
  int home_tab = 1;  // 0 探索 / 1 项目 / 2 用量统计
  std::vector<ProjectCard> projects;
  std::string explore_query;
  Json usage;              // usage.query result (empty => show placeholders)
  bool usage_loaded = false;

  // ---- session (active one) ----
  std::string session_input;
  std::vector<comp::ChatMsg> messages;
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
  int side_seq = 0;
  bool side_menu_open = false;  // "+" popup with side tab kinds
  // ---- side panel / 文件树 ----
  std::string active_pid;                 // project of the active session
  std::string files_path;                 // dir being listed ("" = project root)
  struct FileEntry {
    std::string name;
    bool is_dir = false;
    int64_t size = 0;
  };
  std::vector<FileEntry> file_entries;
  bool files_loaded = false;
  std::string file_preview_path;          // file whose content is shown
  std::string file_preview;               // preview content

  // ---- terminal ----
  std::vector<TermItem> terminals;
  std::string active_term;
  bool terminal_open = false;
  std::string terminal_output;  // last poll output for the active terminal

  // ---- subagents ----
  struct SubagentRow {
    std::string id;
    std::string kind;
    std::string status;
    std::string task;
  };
  std::vector<SubagentRow> subagents;

  // ---- settings ----
  std::string settings_nav = "general";  // section key; nav and body always agree
  std::string thinking_lang = "auto";
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
  };
  std::vector<ProviderTemplate> provider_templates;
  bool provider_templates_loaded = false;
  std::string provider_selected;   // id being edited ("" = none selected)
  bool provider_creating = false;  // new-provider wizard open
  // draft fields for the create/edit form
  std::string pv_name;
  std::string pv_kind = "openai-chat";
  std::string pv_base_url;
  std::string pv_key;
  std::vector<std::pair<std::string, std::string> > pv_headers;  // custom headers
  std::string pv_new_header_k;
  std::string pv_new_header_v;
  std::vector<std::string> provider_models;  // listModels result
  std::string provider_models_level;          // "api" | "preset"
  std::string provider_status;                // test/connect result line
  std::vector<std::string> shells;  // settings.detectShells result
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
    bool loadable = true;
  };
  std::vector<SkillRow> skills;            // skill.list result
  bool skills_loaded = false;
  // ---- settings / MCP 与外部工具 ----
  struct McpRow {
    std::string name;
    std::string dir;
    bool enabled = true;
  };
  std::vector<McpRow> mcp_tools;
  bool mcp_loaded = false;
  std::string mcp_new_name;
  std::string mcp_new_dir;
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
  bool plan_open = false;
  std::vector<std::string> plan_steps;
  std::string toast;
  int64_t toast_deadline_ms = 0;  // pump() clears the toast after this

  static AppState make();
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
  // Load the side-panel file tree (project-relative dir) into AppState.
  void load_files(const std::string& pid, const std::string& rel);
  // Read one file for the side-panel preview.
  void preview_file(const std::string& rel);
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

  // actions (public: also callable from tests and future menu bindings)
  void toast(const std::string& text);
  void send_message();
  void open_new_session();
  // native folder dialog -> register as project (opened project list refresh)
  void pick_and_open_folder();
  // create a session under a specific project (from its card)
  void new_session_in_project(const std::string& pid);
  // handle a dropped folder name: resolve to a path, then open as project
  void handle_dropped_folder(const std::string& name);
  void close_tab(const std::string& id);
  void open_side_tab(const std::string& kind);
  void close_side_tab(const std::string& id);
  // activate the existing tab of `kind`, or create one (never defaults)
  void open_or_focus_side_tab(const std::string& kind);
  // ---- settings / 供应商 actions ----
  void select_provider(const std::string& id);   // load into the edit form
  void begin_new_provider(const std::string& template_id);
  void save_provider();                          // upsert the draft
  void delete_provider(const std::string& id);
  void test_provider(const std::string& id);
  void fetch_provider_models(const std::string& id);
  // ---- settings / MCP actions ----
  void detect_mcp();
  void add_mcp();
  void remove_mcp(const std::string& name);
  void toggle_mcp(const std::string& name, bool enabled);
  // ---- settings / key-value tables (长期记忆 / 环境变量 / CLI 工具) ----
  void add_kv(const std::string& key_setting, const std::string& k, const std::string& v);
  void remove_kv(const std::string& key_setting, const std::string& k);
  // test hook: apply a backend event as the sink would
  void on_backend_event_for_test(const std::string& event, const Json& payload) {
    on_backend_event(event, payload);
  }
  // diagnostics
  int event_count() const { return event_count_; }
  int render_count() const { return render_count_; }
  size_t message_count() const { return st_.messages.size(); }

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

  // backend event handler (bound in mount)
  void on_backend_event(const std::string& event, const Json& payload);
  // append streamed text to the last assistant message (creating it if needed)
  void append_stream_delta(const std::string& text);
  // find the active session id ("" if the active tab is not a session)
  std::string active_sid() const;

  // ---- page builders (return the page body for the active tab) ----
  VNodePtr build_shell();
  VNodePtr build_home();
  VNodePtr build_home_explore();
  VNodePtr build_home_projects();
  VNodePtr build_home_usage();
  VNodePtr build_session();
  VNodePtr build_settings();
  VNodePtr build_side_panel();
  VNodePtr build_terminal_drawer();
  VNodePtr build_overlays();
};

}  // namespace ui
}  // namespace fs
