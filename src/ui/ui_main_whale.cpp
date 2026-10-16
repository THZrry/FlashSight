// ui_main_whale.cpp - desktop entry point: WhaleUI window + WhaleDom + UiApp.
//
// The WhaleUI counterpart of ui_main_wasm.cpp: instead of an HTTP server and a
// browser, the UI renders into a native window. Everything between the adapter
// and the app is identical - that is the point of the four-layer split.
//
// Compiled only when WhaleUI is enabled (`xmake f --whaleui=y`); see
// doc-internal/ui-whaleui.md for the switch-over checklist.
#include "ui_app.hpp"
#include "ui_backend.hpp"
#include "ui_dom.hpp"

#ifdef FS_UI_WHALEUI

#include <whaleui.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "core/fs_api.hpp"
#include "ui_native_backend.hpp"

namespace fs {
namespace ui {

IDom* make_whale_dom(whaleui_app_t* app, whaleui_window_t* win);  // ui_dom_whale.cpp

namespace {

// The app's own CSS, embedded at build time (the wasm bundle packs web/ into
// ui_assets_gen.hpp; the desktop build reads the same files from disk because
// the theme/stylesheet lives in web/style.css either way).
std::string read_asset(const char* rel) {
  char* data = nullptr;
  size_t len = 0;
  if (whaleui_fs_load(rel, &data, &len) != 0) return std::string();
  std::string out(data, len);
  std::free(data);
  return out;
}

// The shell document: the same two stylesheets the browser loads, inlined (the
// engine reads <link> through the VFS too, but inlining keeps the document
// self-contained and independent of the working directory), plus the empty root
// the app renders into.
std::string shell_html(const std::string& css, const std::string& icons) {
  std::string html;
  html += "<!DOCTYPE html><html><head><meta charset=\"utf-8\">";
  html += "<title>FlashSight</title><style>";
  html += css;
  html += icons;
  html += "</style></head><body><div id=\"fs-root\"></div></body></html>";
  return html;
}

}  // namespace

// Entry point called by main.cpp (the desktop path).
int run_whale_ui(FsApi* api) {
  whaleui_app_t* app = whaleui_app_create();
  if (!app) {
    std::fprintf(stderr, "whaleui_app_create failed\n");
    return 1;
  }
  // Desktop-native look, and the app follows the OS light/dark setting.
  whaleui_app_set_theme(app, WHALEUI_THEME_SYSTEM);
  whaleui_app_set_theme_style(app, "fluent");

  whaleui_window_t* win = whaleui_window_create(app, "FlashSight", 1280, 820);
  if (!win) {
    std::fprintf(stderr, "whaleui_window_create failed\n");
    whaleui_app_destroy(app);
    return 1;
  }
  // The UI draws its own title strip? Not yet - keep the system frame for now
  // (borderless needs the hit-test/drag path; see doc-internal/ui-whaleui.md).
  whaleui_window_set_bordered(win, 1);
  whaleui_window_set_min_size(win, 900, 600);

  // Style sources: the UI stylesheet (web/style.css) + the icon subset. The
  // icon font is registered through the VFS so the .fa-* glyphs resolve.
  std::string css = read_asset("web/style.css");
  std::string icons = read_asset("web/icons/fa-subset.css");
  if (css.empty()) {
    std::fprintf(stderr, "warning: web/style.css not found (running unstyled)\n");
  }
  whaleui_font_register_system_defaults();
  whaleui_font_register("web/icons/fa-solid-900.woff2");

  if (whaleui_window_load_html(win, shell_html(css, icons).c_str()) != 0) {
    std::fprintf(stderr, "whaleui_window_load_html failed\n");
    whaleui_window_destroy(win);
    whaleui_app_destroy(app);
    return 1;
  }

  IDom* dom = make_whale_dom(app, win);
  NativeBackend backend(api);
  AppState st = AppState::make();
  UiApp ui(*dom, st, &backend);
  ui.mount();

  // Backend events + terminal refresh are driven by a WhaleUI interval timer
  // (the browser build uses emscripten_set_interval for the same two loops).
  whaleui_set_interval(
      [](double, void* ud) { static_cast<UiApp*>(ud)->pump(); }, &ui, 250);
  whaleui_set_interval(
      [](double, void* ud) { static_cast<UiApp*>(ud)->tick_terminal(); }, &ui, 50);

  whaleui_window_show(win);
  int rc = whaleui_app_run(app);  // blocks until the window closes

  whaleui_window_destroy(win);
  whaleui_app_destroy(app);
  delete dom;
  return rc;
}

}  // namespace ui
}  // namespace fs

#endif  // FS_UI_WHALEUI
