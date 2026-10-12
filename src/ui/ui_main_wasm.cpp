// ui_main_wasm.cpp - wasm 入口：BrowserDom + UiApp + NullBackend 装配
// 仅 emscripten 下编译（xmake -p wasm 的 fs_ui_wasm target）。
#include "ui_app.hpp"
#include "ui_backend.hpp"
#include "ui_dom_browser.hpp"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

namespace fs {
namespace ui {
namespace {
BrowserDom* g_dom = nullptr;
UiApp* g_app = nullptr;
NullBackend* g_backend = nullptr;
AppState g_state;
}  // namespace

// 由 JS 调用（web/index.html 的 onRuntimeInitialized）启动 UI
extern "C" EMSCRIPTEN_KEEPALIVE void fs_ui_start() {
  if (g_app) return;
  g_dom = new BrowserDom();
  g_state = AppState::make();
  g_backend = new NullBackend();
  g_app = new UiApp(*g_dom, g_state, g_backend);
  g_app->mount();
}

// 由 JS 调用：切换顶层页（0=主页 1=会话 2=设置）
extern "C" EMSCRIPTEN_KEEPALIVE void fs_ui_show(int page) {
  if (!g_app) return;
  switch (page) {
    case 0: g_app->show(Page::kHome); break;
    case 1: g_app->show(Page::kSession); break;
    case 2: g_app->show(Page::kSettings); break;
    default: break;
  }
}

// 由 JS 调用：推后端事件（演示/联调用）
extern "C" EMSCRIPTEN_KEEPALIVE void fs_ui_fire_event(const char* event,
                                                      const char* payload) {
  if (!g_backend) return;
  Json p = Json::object();
  if (payload && *payload) {
    try {
      p = Json::parse(payload);
    } catch (...) {
    }
  }
  g_backend->fire(event ? event : "", p);
}

}  // namespace ui
}  // namespace fs
#endif  // __EMSCRIPTEN__
