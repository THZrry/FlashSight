// ui_main_wasm.cpp - wasm entry point: platform DOM + UiApp + backend.
// Compiled only for the wasm target (ui_dom_wasm.cpp provides make_platform_dom).
#include "ui_app.hpp"
#include "ui_backend.hpp"
#include "ui_dom.hpp"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

namespace fs {
namespace ui {

// Provided by ui_dom_wasm.cpp
IDom* make_platform_dom();

namespace {
UiApp* g_app = nullptr;
NullBackend* g_backend = nullptr;
AppState g_state;
}  // namespace

// Called from index.html once the wasm runtime is ready.
extern "C" EMSCRIPTEN_KEEPALIVE void fs_ui_start() {
  if (g_app) return;
  IDom* dom = make_platform_dom();
  g_state = AppState::make();
  g_backend = new NullBackend();
  g_app = new UiApp(*dom, g_state, g_backend);
  g_app->mount();
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
  g_backend->fire(event ? event : "", p);
}

// Event callback from the DOM adapter (see ui_dom_wasm.cpp fs_dom_bind).
extern "C" void fs_ui_fire(int handler_id, const char* value) {
  bus().fire(handler_id, value ? value : "");
}

}  // namespace ui
}  // namespace fs
#endif  // __EMSCRIPTEN__
