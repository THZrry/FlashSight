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

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core/fs_api.hpp"
#include "ui_native_backend.hpp"

namespace fs {
namespace ui {

IDom* make_whale_dom(whaleui_app_t* app, whaleui_window_t* win);  // ui_dom_whale.cpp

namespace {

// Directory the running executable lives in ("" when undeterminable). The
// assets are copied next to the exe by xmake's after_build, so this is where
// the stylesheet is looked up first - otherwise the UI would only style itself
// when launched from exactly one working directory.
std::string exe_dir() {
#if defined(_WIN32)
  char buf[MAX_PATH];
  DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return std::string();
  std::string p(buf, n);
  size_t slash = p.find_last_of("\\/");
  return slash == std::string::npos ? std::string() : p.substr(0, slash);
#else
  return std::string();
#endif
}

// Load a web asset through the VFS, trying the plausible locations in order:
// next to the exe under web/ (the layout xmake's after_build copies), next to
// the exe directly, then the repository layout (running from the source tree).
std::string read_asset(const char* rel) {
  std::vector<std::string> cands;
  std::string dir = exe_dir();
  if (!dir.empty()) {
    cands.push_back(dir + "/web/" + rel);
    cands.push_back(dir + "/" + rel);
  }
  cands.push_back(std::string("web/") + rel);
  for (size_t i = 0; i < cands.size(); ++i) {
    char* data = nullptr;
    size_t len = 0;
    if (whaleui_fs_load(cands[i].c_str(), &data, &len) == 0) {
      std::string out(data, len);
      std::free(data);
      return out;
    }
  }
  return std::string();
}

// The shell document: the same stylesheets the browser loads, inlined (the
// engine reads <link> through the VFS too, but inlining keeps the document
// self-contained), plus the empty root the app renders into.
//
// The trailing rule is the icon-font bridge: the subset stylesheet declares its
// family through @font-face, which WhaleUI does not implement, and a font
// registered through whaleui_font_register() is named after its FILE
// ("fa-solid-900"). Pointing .fa at that name is what makes the glyphs resolve.
// Drop it once WhaleUI supports @font-face (see doc-internal/ui-whaleui.md §5D).
//
// icons.css carries the brand/file icons as inline SVG data URIs (background-
// image), so it needs no font - it is inlined verbatim.
std::string shell_html(const std::string& css, const std::string& fa_css,
                       const std::string& icon_css) {
  std::string html;
  html += "<!DOCTYPE html><html><head><meta charset=\"utf-8\">";
  html += "<title>FlashSight</title><style>";
  html += css;
  html += fa_css;
  html += icon_css;
  html += ".fa{font-family:\"fa-solid-900\",var(--font)}\n";
  html += "</style></head><body><div id=\"fs-root\"></div></body></html>";
  return html;
}

// FS_UI_DEBUG=1 dumps the rendered DOM once after mount; =2 keeps dumping it on
// a timer, which is how you see what a click actually did (the native backend
// has no devtools). See doc-internal/ui-whaleui.md.
int debug_mode() {
  const char* v = std::getenv("FS_UI_DEBUG");
  if (!v || !*v) return 0;
  if (v[0] == '2') return 2;
  return v[0] == '1' ? 1 : 0;
}

bool debug_opacity() {
  const char* v = std::getenv("FS_UI_DEBUG_OPACITY");
  return v && *v && v[0] != '0';
}

void dump_dom(whaleui_window_t* win) {
  whaleui_dom_document_t* doc = whaleui_window_get_document(win);
  whaleui_dom_element_t* root = whaleui_dom_get_element_by_id(doc, "fs-root");
  const char* html = root ? whaleui_dom_get_outer_html(root) : nullptr;
  std::fprintf(stderr, "--- fs-root outerHTML ---\n%s\n--- end ---\n",
               html ? html : "(no root)");
  // FS_UI_DEBUG=2 also walks the tree and prints each element's laid-out box,
  // which separates "not laid out" (query fails) from "laid out but painted
  // wrong" - the two look identical in a screenshot.
  if (debug_mode() != 2 || !root) return;
  std::fprintf(stderr, "--- layout boxes (tag.class rect) ---\n");
  std::vector<std::pair<whaleui_dom_element_t*, int> > stack;
  stack.push_back(std::make_pair(root, 0));
  int shown = 0;
  // FS_UI_DEBUG_LIMIT caps how many elements the dump prints. The default 80
  // only reaches the sidebar, so a later dump (taken after the async project
  // load) never showed the main area - the one thing the dump exists to check.
  int limit = 80;
  if (const char* lim = std::getenv("FS_UI_DEBUG_LIMIT")) {
    int v = std::atoi(lim);
    if (v > 0) limit = v;
  }
  while (!stack.empty() && shown < limit) {
    whaleui_dom_element_t* el = stack.back().first;
    int depth = stack.back().second;
    stack.pop_back();
    whaleui_dom_rect_t r;
    bool have = whaleui_dom_get_bounding_client_rect(el, &r) == 0;
    // FS_UI_DEBUG_OPACITY=1 adds the computed opacity and animation of every
    // element. "Invisible but laid out" is by far the hardest failure to see
    // from a box dump: the geometry is perfect and the pixel is not there,
    // which is exactly what a stuck entrance animation (opacity 0) looks like.
    char opa[32] = {0};
    char anim[64] = {0};
    if (debug_opacity()) {
      whaleui_style_declaration_t* d =
          whaleui_dom_get_computed_style(el, nullptr);
      if (d) {
        whaleui_style_declaration_get(d, "opacity", opa, sizeof(opa));
        whaleui_style_declaration_get(d, "animation", anim, sizeof(anim));
        whaleui_style_declaration_destroy(d);
      }
    }
    char line[768];
    std::snprintf(line, sizeof(line),
                  "%*s<%s class=\"%s\"> %s %.0f,%.0f %.0fx%.0f%s%s\n",
                  depth * 2, "", whaleui_dom_tag_name(el),
                  whaleui_dom_get_attribute(el, "class") ? whaleui_dom_get_attribute(el, "class") : "",
                  have ? "" : "NO-BOX",
                  have ? r.x : 0.0f, have ? r.y : 0.0f,
                  have ? r.width : 0.0f, have ? r.height : 0.0f,
                  opa[0] ? " opacity=" : "", opa[0] ? opa : "");
    if (anim[0]) {
      std::fprintf(stderr, "%*s   animation=\"%s\"\n", depth * 2, "", anim);
    }
    std::fprintf(stderr, "%s", line);
    ++shown;
    std::vector<whaleui_dom_element_t*> kids;
    for (whaleui_dom_element_t* c = whaleui_dom_last_child(el); c;
         c = whaleui_dom_previous_sibling(c)) {
      kids.push_back(c);
    }
    for (size_t i = 0; i < kids.size(); ++i) {
      stack.push_back(std::make_pair(kids[i], depth + 1));
    }
  }
  std::fprintf(stderr, "--- end layout ---\n");
}

}  // namespace

// Entry point called by main.cpp (the desktop path).
int run_whale_ui(FsApi* api) {
  whaleui_app_t* app = whaleui_app_create();
  if (!app) {
    std::fprintf(stderr, "whaleui_app_create failed\n");
    return 1;
  }
  // Desktop-native look, and the app follows the OS light/dark setting (the
  // app's own ui.mode setting then writes the resolved value onto :root).
  whaleui_app_set_theme(app, WHALEUI_THEME_SYSTEM);
  whaleui_app_set_theme_style(app, "fluent");

  whaleui_window_t* win = whaleui_window_create(app, "FlashSight", 1280, 820);
  if (!win) {
    std::fprintf(stderr, "whaleui_window_create failed\n");
    whaleui_app_destroy(app);
    return 1;
  }
  // The custom title strip is not wired yet (borderless needs the hit-test and
  // drag path), so keep the system frame for now - see doc-internal/ui-whaleui.md.
  whaleui_window_set_bordered(win, 1);
  whaleui_window_set_min_size(win, 900, 600);

  // Style sources: the UI stylesheet (web/style.css) + the Font Awesome subset
  // + the brand/file icon sheet, plus the icon font registered through the VFS
  // so the .fa-* glyphs resolve.
  std::string css = read_asset("style.css");
  std::string fa_css = read_asset("icons/fa-subset.css");
  std::string icon_css = read_asset("icons/icons.css");
  if (css.empty()) {
    std::fprintf(stderr,
                 "warning: style.css not found next to the exe or under web/ "
                 "(running unstyled)\n");
  }
  if (icon_css.empty()) {
    std::fprintf(stderr, "warning: icons/icons.css not found (brand/file icons "
                         "will be blank)\n");
  }
  whaleui_font_register_system_defaults();
  {
    std::string dir = exe_dir();
    std::string fa;
    if (!dir.empty()) fa = dir + "/web/icons/fa-solid-900.woff2";
    if (fa.empty() || whaleui_font_register(fa.c_str()) == nullptr) {
      whaleui_font_register("web/icons/fa-solid-900.woff2");
    }
  }

  if (whaleui_window_load_html(win, shell_html(css, fa_css, icon_css).c_str()) != 0) {
    std::fprintf(stderr, "whaleui_window_load_html failed\n");
    whaleui_window_destroy(win);
    whaleui_app_destroy(app);
    return 1;
  }

  // The document exists, so the adapter can resolve its root and bind.
  IDom* dom = make_whale_dom(app, win);
  NativeBackend backend(api);  // in-process FsApi calls, no HTTP
  int rc = 0;
  {
    AppState st = AppState::make();
    UiApp ui(*dom, st, &backend);
    ui.mount();

    // FS_UI_DEBUG=1 dumps the rendered DOM once after mount (see above).
    if (debug_mode() == 1) dump_dom(win);
    if (debug_mode() == 2) {
      whaleui_set_interval([](double, void* ud) { dump_dom(static_cast<whaleui_window_t*>(ud)); },
                           win, 3000);
    }

    // Backend events + terminal refresh run on WhaleUI timers (the browser
    // build uses emscripten_set_interval for the same two loops). Ids are kept
    // so the loop can be left without callbacks pointing at a destroyed UiApp.
    long long tick = whaleui_set_interval(
        [](double, void* ud) { static_cast<UiApp*>(ud)->pump(); }, &ui, 250);
    long long term = whaleui_set_interval(
        [](double, void* ud) { static_cast<UiApp*>(ud)->tick_terminal(); }, &ui, 50);
    // Resource log for the WhaleUI host too: the earlier freeze diagnostics only
    // covered the WebView build, so a WhaleUI incident had no memory/object data.
    // One line every 30 s with process+system memory, DOM slot count and the
    // app's arrays; FS_RES_LOG=0 opts out.
    long long res = whaleui_set_interval(
        [](double, void* ud) {
          const char* env = std::getenv("FS_RES_LOG");
          if (env && std::strcmp(env, "0") == 0) return;
          UiApp* app = static_cast<UiApp*>(ud);
          if (!app) return;
          const char* tmp = std::getenv("TEMP");
          if (!tmp || !*tmp) tmp = std::getenv("TMPDIR");
          std::string dir = tmp && *tmp ? tmp : "/tmp";
          std::string path = dir + "/flashsight_res.log";
          int64_t sec = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
          unsigned long handles = 0;
#if defined(_WIN32)
          DWORD wh = 0;
          ::GetProcessHandleCount(::GetCurrentProcess(), &wh);
          handles = static_cast<unsigned long>(wh);
#endif
          if (FILE* f = std::fopen(path.c_str(), "a")) {
            std::fprintf(f, "[res] %lld ui=whale handles=%lu %s\n",
                         static_cast<long long>(sec),
                         static_cast<unsigned long>(handles),
                         app->debug_stats().c_str());
            std::fclose(f);
          }
        },
        &ui, 30000);

    whaleui_window_show(win);
    rc = whaleui_app_run(app);  // blocks until the window closes

    whaleui_clear_interval(tick);
    whaleui_clear_interval(term);
    whaleui_clear_interval(res);
  }

  // Order matters: the DOM adapter's destructor removes its listeners, which
  // needs the document (and therefore the window) still alive.
  delete dom;
  whaleui_window_destroy(win);
  whaleui_app_destroy(app);
  return rc;
}

}  // namespace ui
}  // namespace fs

#endif  // FS_UI_WHALEUI
