// ui_app.hpp - 页面装配（四页骨架，全部经 IDom；逻辑与后端只经 FsApi/事件交互）
#pragma once

#include <memory>
#include <string>

#include "ui_dom.hpp"
#include "ui_reactive.hpp"

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

  static AppState make() {
    AppState s;
    s.page = Signal<Page>::make(Page::kHome);
    s.title = Signal<std::string>::make("FlashSight");
    s.session_input = Signal<std::string>::make("");
    s.terminal_visible = Signal<bool>::make(false);
    return s;
  }
};

// UiApp：用 IDom 搭建四页骨架；切页只重建内容区（diff 更新在 WhaleUI/联调阶段做）。
class UiApp {
 public:
  UiApp(IDom& dom, AppState state);

  // 全量搭建一次；之后靠 Signal 绑定自动更新
  void mount();

  // 手动切页（tab 点击）
  void show(Page p);

 private:
  IDom& dom_;
  AppState state_;
  Node content_{0};
  std::vector<Subscription> bindings_;

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