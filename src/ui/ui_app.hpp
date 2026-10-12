// ui_app.hpp - 页面装配（顶部标签页模式；对齐 web/index.html）
// 单模式：顶部 tab（主页/会话/+），页面内可用 subtabs。不做侧边栏模式。
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "ui_backend.hpp"
#include "ui_dom.hpp"
#include "ui_reactive.hpp"
#include "ui_widgets.hpp"

namespace fs {
namespace ui {

// 顶层页面
enum class Page {
  kHome,
  kSession,
  kSettings,
};

// 主页内的子页（探索/项目/用量统计）
enum class HomeTab {
  kExplore,
  kProject,
  kUsage,
};

// UI 应用状态（响应式源）
struct AppState {
  std::shared_ptr<Signal<Page>> page;
  std::shared_ptr<Signal<HomeTab>> home_tab;
  std::shared_ptr<Signal<std::string>> title;
  // 会话
  std::shared_ptr<Signal<std::string>> session_input;
  std::shared_ptr<Signal<int>> model_index;
  std::shared_ptr<Signal<int>> perm_index;
  std::shared_ptr<Signal<bool>> auto_mode;
  std::shared_ptr<Signal<bool>> suspended;
  std::shared_ptr<Signal<bool>> plan_open;
  std::shared_ptr<Signal<int>> side_tab;
  std::shared_ptr<Signal<int>> context_pct;
  std::shared_ptr<Signal<bool>> terminal_visible;
  std::shared_ptr<Signal<bool>> ask_modal;
  // 设置
  std::shared_ptr<Signal<int>> settings_nav;
  std::shared_ptr<Signal<int>> provider_index;

  static AppState make() {
    AppState s;
    s.page = Signal<Page>::make(Page::kHome);
    s.home_tab = Signal<HomeTab>::make(HomeTab::kProject);
    s.title = Signal<std::string>::make("FlashSight");
    s.session_input = Signal<std::string>::make("");
    s.model_index = Signal<int>::make(0);
    s.perm_index = Signal<int>::make(0);
    s.auto_mode = Signal<bool>::make(true);
    s.suspended = Signal<bool>::make(false);
    s.plan_open = Signal<bool>::make(false);
    s.side_tab = Signal<int>::make(0);
    s.context_pct = Signal<int>::make(42);
    s.terminal_visible = Signal<bool>::make(false);
    s.ask_modal = Signal<bool>::make(false);
    s.settings_nav = Signal<int>::make(0);
    s.provider_index = Signal<int>::make(0);
    return s;
  }
};

// UiApp：Widget 库装配；后端经 IUiBackend 注入
// （桌面 NativeBackend 进程内直调 FsApi；浏览器 wasm 同一套逻辑）。
class UiApp {
 public:
  UiApp(IDom& dom, AppState state, IUiBackend* backend = nullptr);

  void mount();
  void show(Page p);

 private:
  IDom& dom_;
  AppState state_;
  IUiBackend* backend_ = nullptr;
  Node content_{0};
  std::vector<Subscription> bindings_;
  std::vector<std::shared_ptr<Widget>> widgets_;

  template <typename W, typename... Args>
  W* own(Args&&... args) {
    auto w = std::make_shared<W>(std::forward<Args>(args)...);
    W* p = w.get();
    widgets_.push_back(std::move(w));
    return p;
  }
  void drop_widgets() { widgets_.clear(); }

  void rebuild_content();
  void build_tabstrip(Node parent);
  void build_home(Node parent);
  void build_home_project(Node parent);
  void build_home_usage(Node parent);
  void build_session(Node parent);
  void build_settings(Node parent);
  static const char* page_name(Page p);
  static const char* home_tab_name(HomeTab t);
};

}  // namespace ui
}  // namespace fs