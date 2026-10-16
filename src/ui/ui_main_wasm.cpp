// ui_main_wasm.cpp - wasm entry point: platform DOM + UiApp + backend.
// Compiled only for the wasm target (ui_dom_wasm.cpp provides make_platform_dom,
// ui_http_backend.cpp provides make_platform_backend).
#include "ui_app.hpp"
#include "ui_backend.hpp"
#include "ui_dom.hpp"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/eventloop.h>

namespace fs {
namespace ui {

IDom* make_platform_dom();          // ui_dom_wasm.cpp
IUiBackend* make_platform_backend();  // ui_http_backend.cpp

namespace {
UiApp* g_app = nullptr;
IUiBackend* g_backend = nullptr;
AppState g_state;

// Timer: poll backend events (stream.delta / turn.done / permission.prompt ...).
void fs_ui_tick(void* /*user_data*/) {
  if (g_app) g_app->pump();
}

// Timer: terminal refresh. Separate and much faster than the event pump - a
// terminal has to keep up with typing, and the 250ms event cadence made
// keystrokes appear in batches. Only does work while a terminal is visible.
void fs_ui_term_tick(void* /*user_data*/) {
  if (g_app) g_app->tick_terminal();
}
}  // namespace

// Called from index.html once the wasm runtime is ready.
extern "C" EMSCRIPTEN_KEEPALIVE void fs_ui_start() {
  if (g_app) return;
  IDom* dom = make_platform_dom();
  g_state = AppState::make();
  g_backend = make_platform_backend();
  g_app = new UiApp(*dom, g_state, g_backend);
  g_app->mount();
  // ~4 events/sec is plenty for a local server; keeps the tab idle-cheap.
  emscripten_set_interval(fs_ui_tick, 250, nullptr);
  // The terminal gets its own, much tighter cadence (~20/sec). It is a no-op
  // unless the drawer is open on a running terminal, so the idle cost is zero.
  emscripten_set_interval(fs_ui_term_tick, 50, nullptr);
}

// Switch top-level tab by id ("home", "settings", "s1"...).
extern "C" EMSCRIPTEN_KEEPALIVE void fs_ui_show(const char* tab_id) {
  if (!g_app) return;
  g_app->show(tab_id ? tab_id : "home");
}

// Push a backend event (demo/dev hook).
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
  // NullBackend exposes fire(); the HTTP backend has no local fire path, so
  // route through the DOM event bus for manual testing instead.
  (void)event;
  (void)p;
}

// Event callback from the DOM adapter (see ui_dom_wasm.cpp fs_dom_bind).
extern "C" void fs_ui_fire(int handler_id, const char* value) {
  bus().fire(handler_id, value ? value : "");
}

// diagnostics: {eventCount, renderCount, messageCount, activeTabLen}
extern "C" EMSCRIPTEN_KEEPALIVE void fs_ui_debug(int* out) {
  if (!out) return;
  if (!g_app) {
    out[0] = out[1] = out[2] = -1;
    return;
  }
  out[0] = g_app->event_count();
  out[1] = g_app->render_count();
  out[2] = static_cast<int>(g_app->message_count());
}

}  // namespace ui
}  // namespace fs
#endif  // __EMSCRIPTEN__
