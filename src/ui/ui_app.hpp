// ui_app.hpp - 页面装配（四页骨架，全部经 IDom；逻辑与后端只经 FsApi/事件交互）
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
  kUsage,
  kSettings,
};

// UI 应用状态（响应式源）
struct AppState {
  std::shared_ptr<Signal<Page>> page;
  std::shared_ptr<Signal<std::string>> title;
  std::shared_ptr<Signal<std::string>> session_input;
  std::shared_ptr<Signal<bool>> terminal_visible;
  // 会话页控件状态
  std::shared_ptr<Signal<int>> model_index;      // 模型下拉
  std::shared_ptr<Signal<int>> perm_index;       // 权限下拉
  std::shared_ptr<Signal<bool>> auto_mode;       // 自动挡
  std::shared_ptr<Signal<bool>> ask_modal;       // ask 弹窗
  std::shared_ptr<Signal<int>> side_tab;         // 右侧栏标签
  std::shared_ptr<Signal<int>> context_pct;      // 上下文水位
  std::shared_ptr<Signal<bool>> suspended;       // 挂起态
  // 设置页
  std::shared_ptr<Signal<int>> settings_nav;     // 左侧导航
  std::shared_ptr<Signal<std::string>> search_text;

  static AppState make() {
    AppState s;
    s.page = Signal<Page>::make(Page::kHome);
    s.title = Signal<std::string>::make("FlashSight");
    s.session_input = Signal<std::string>::make("");
    s.terminal_visible = Signal<bool>::make(false);
    s.model_index = Signal<int>::make(0);
    s.perm_index = Signal<int>::make(0);
    s.auto_mode = Signal<bool>::make(true);
    s.ask_modal = Signal<bool>::make(false);
    s.side_tab = Signal<int>::make(0);
    s.context_pct = Signal<int>::make(42);
    s.suspended = Signal<bool>::make(false);
    s.settings_nav = Signal<int>::make(0);
    s.search_text = Signal<std::string>::make("");
    return s;
  }
};

// UiApp：用 Widget 库搭建页面；切页只重建内容区。
// 后端经 IUiBackend 注入（桌面 NativeBackend 进程内直调；浏览器 wasm 同逻辑）。
class UiApp {
 public:
  UiApp(IDom& dom, AppState state, IUiBackend* backend = nullptr);

  // 全量搭建一次；之后靠 Signal 绑定自动更新
  void mount();

  // 手动切页（tab 点击）
  void show(Page p);

 private:
  IDom& dom_;
  AppState state_;
  IUiBackend* backend_ = nullptr;
  Node content_{0};
  std::vector<Subscription> bindings_;
  // Widget 存活期管理：rebuild 时清空重建（Widget 析构不断开 DOM，只断订阅）
  std::vector<std::shared_ptr<Widget>> widgets_;
  template <typename W, typename... Args>
  W* own(Args&&... args) {
    auto w = std::make_shared<W>(std::forward<Args>(args)...);
    W* p = w.get();
    widgets_.push_back(std::move(w));
    return p;
  }
  void drop_widgets() { widgets_.clear(); }

  void build_titlebar(Node parent);
  void build_tabs(Node parent);
  void build_home(Node parent);
  void build_session(Node parent);
  void build_usage(Node parent);
  void build_settings(Node parent);
  void rebuild_content();
  static const char* page_name(Page p);
};

}  // namespace ui
}  // namespace fs