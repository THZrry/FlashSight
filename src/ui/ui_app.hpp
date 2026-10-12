// ui_app.hpp - User layer (rules/addon3.md "user"): the actual FlashSight UI.
//
// Model: one observable AppState; any change re-renders the whole VNode tree;
// patch() applies the minimal DOM diff. No widget objects, no manual DOM
// bookkeeping - the app is a pure function from state to tree.
//
// Navigation (per user feedback, single mode):
//   * top tab strip: 主页 / 会话N / 设置 - tabs can be added and closed
//   * home: centered title buttons 探索/项目/用量统计 (no background, only
//     color + underline); 项目 renders project cards whose right half is an
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
  bool expanded = false;            // right half: session list open?
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

  // ---- terminal ----
  std::vector<TermItem> terminals;
  std::string active_term;
  bool terminal_open = false;

  // ---- subagents ----
  struct SubagentRow {
    std::string id;
    std::string kind;
    std::string status;
    std::string task;
  };
  std::vector<SubagentRow> subagents;

  // ---- settings ----
  int settings_nav = 0;
  int provider_index = 0;
  std::string provider_endpoint = "https://opencode.ai/zen/v1";
  std::string provider_key;
  std::string provider_header = "x-opencode-session: auto";
  std::string thinking_lang = "auto";

  // ---- overlays ----
  bool ask_modal = false;
  std::string ask_text;
  bool plan_open = false;
  std::vector<std::string> plan_steps;
  std::string toast;

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

  // actions (public: also callable from tests and future menu bindings)
  void toast(const std::string& text);
  void send_message();
  void open_new_session();
  void close_tab(const std::string& id);
  void open_side_tab(const std::string& kind);
  void close_side_tab(const std::string& id);

 private:
  IDom& dom_;
  AppState st_;
  IUiBackend* backend_ = nullptr;
  VNodePtr tree_;
  Node root_{0};
  std::string last_rendered_tab_;

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
