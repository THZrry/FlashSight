// FlashSight entry point.
//
// THREE builds, chosen at configure time (the UI layer is identical in all of
// them - only the DOM backend under IDom changes):
//
//   Web     (default)      : local HTTP server + embedded wasm UI, opened in
//                            the user's browser. `xmake f` with no host flag.
//   Desktop (--webview=y)  : native window via webview (WebView2/WebKitGTK),
//                            UI + assets embedded in the document. The
//                            shipping fallback while WhaleUI matures.
//   Desktop (--whaleui=y)  : native window rendered by WhaleUI's own engine.
//
// All three embed the same static assets (web/ is packed into
// ui_assets_gen.hpp by xmake); only the Web build needs the wasm bundle.
#include <cstdio>
#include <cstdlib>
#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <ctime>
#endif

#include <ghc/filesystem.hpp>

#include "common/fs_version.hpp"
#include "core/fs_api.hpp"
#include "core/settings_store.hpp"
#include "storage/fs_store.hpp"

#if defined(FS_UI_WHALEUI) || defined(FS_UI_WEBVIEW)
namespace fs {
namespace ui {
// ui_main_whale.cpp / ui_main_webview.cpp: native window (blocks until closed)
int run_whale_ui(FsApi* api);
int run_webview_ui(FsApi* api);
}  // namespace ui
}  // namespace fs
#endif

// Every variant embeds the HTTP server (local API + remote-access groundwork),
// so ui_server.hpp is always needed.
#include "ui/ui_server.hpp"

namespace {

#if !defined(FS_UI_WHALEUI) && !defined(FS_UI_WEBVIEW)
// 用系统默认浏览器打开 URL（Windows: start / POSIX: xdg-open）
void open_browser(const std::string& url) {
#if defined(_WIN32)
  std::string cmd = "start \"\" \"" + url + "\"";
#elif defined(__APPLE__)
  std::string cmd = "open \"" + url + "\"";
#else
  std::string cmd = "xdg-open \"" + url + "\" >/dev/null 2>&1 &";
#endif
  int rc = std::system(cmd.c_str());
  (void)rc;
}
#endif

}  // namespace

int main(int argc, char** argv) {
  int port = 0;
  bool no_browser = false;
  std::string root;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--port" && i + 1 < argc) {
      port = std::atoi(argv[++i]);
    } else if (a == "--no-browser") {
      no_browser = true;
    } else if (a == "--root" && i + 1 < argc) {
      root = argv[++i];
    }
  }

  std::printf("FlashSight %s\n", fs::version_string());

  // 数据根：--root 指定；否则用 ~/.flashsight
  fs::GlobalPaths gp = fs::GlobalPaths::resolve();
  ghc::filesystem::path data_root =
      root.empty() ? gp.root : ghc::filesystem::path(root);
  fs::FsApi api(data_root);

#if defined(FS_UI_WHALEUI) || defined(FS_UI_WEBVIEW)
  // Native-window variants: the window IS the UI, so there is nothing to serve.
  //
  // The three ways to reach the UI - Web, Webview, WhaleUI - are alternatives
  // chosen at compile time, not a stack: a build that opens a window does not
  // also need a local web server, and starting one anyway cost real money in
  // confusion (two UIs, two ports, a server the desktop user never asked for).
  // The embedded server belongs to the Web build, where it is the only UI.
#if defined(FS_UI_WHALEUI)
  std::printf("UI: WhaleUI (native window)\n");
#else
  std::printf("UI: webview (native window)\n");
#endif
  std::printf("data root: %s\n", data_root.string().c_str());
#if defined(FS_UI_WHALEUI)
  return fs::ui::run_whale_ui(&api);
#else
  return fs::ui::run_webview_ui(&api);
#endif
#else
  // Web variant: the browser is the UI (decided at compile time), so the server
  // IS the app. Same settings as the desktop builds, same auth gate.
  fs::ui::UiServer server;
  server.bind_api(&api);
  api.attach_ui_server(
      [&server](int p, const std::string& host, const std::string& pw) {
        return server.restart(p, host, pw).ok();
      },
      [&server]() {
        return fs::Json{{"running", server.running()},
                    {"host", server.host()},
                    {"port", server.port()},
                    {"password", server.requires_password()},
                    {"rebinding", server.rebinding()},
                    {"error", server.last_error()},
                    {"lanIp", server.lan_ip()},
                    {"url", server.url()}};
      });
  std::string srv_host = "127.0.0.1";
  std::string srv_pw;
  {
    auto get_str = [&api](const char* key, const std::string& def) {
      fs::FsResult r = api.settings_get(key, "");
      if (!r.ok()) return def;
      const fs::Json& v = r.data.contains("value") ? r.data["value"] : fs::Json();
      return v.is_string() ? v.get<std::string>() : def;
    };
    auto get_int = [&api](const char* key, int def) {
      fs::FsResult r = api.settings_get(key, "");
      if (!r.ok()) return def;
      const fs::Json& v = r.data.contains("value") ? r.data["value"] : fs::Json();
      return v.is_number() ? static_cast<int>(v.get<int64_t>()) : def;
    };
    srv_host = get_str(fs::kKeyServerHost, "127.0.0.1");
    srv_pw = get_str(fs::kKeyServerPassword, "");
    int cfg_port = get_int(fs::kKeyServerPort, 0);
    if (port <= 0) {
      port = cfg_port;
    } else if (port != cfg_port) {
      // A --port on the command line overrides the saved setting, but the saved
      // setting is what every later rebind reads (apply_server_settings). Leaving
      // them out of sync meant: start with --port 8920, touch the listen address,
      // and the server silently jumped to the OLD saved port - the port "changed
      // by itself". Persist the override so the config tells the truth.
      // A --port on the command line overrides the saved setting, but the saved
      // setting is what every later rebind reads (apply_server_settings). Leaving
      // them out of sync meant: start with --port 8920, touch the listen address,
      // and the server silently jumped to the OLD saved port - the port "changed
      // by itself". Persist the override so the config tells the truth.
      // (apply_server_settings is a no-op while the server is not running, so this
      // does not try to rebind.)
      api.settings_set(fs::kKeyServerPort, fs::Json(port), "");
    }
  }
  fs::FsResult r = server.start(port, srv_host, srv_pw);
  if (!r.ok()) {
    std::fprintf(stderr, "failed to start UI server: %s\n", r.message.c_str());
    return 1;
  }
  std::printf("UI server: %s%s\n", server.url().c_str(),
              server.requires_password() ? " (password required remotely)" : "");
  std::printf("data root: %s\n", data_root.string().c_str());
  std::printf("(press Ctrl+C to quit)\n");

  if (!no_browser) open_browser(server.url());

  // 阻塞直到 Ctrl+C
  for (;;) {
#if defined(_WIN32)
    Sleep(1000);
#else
    struct timespec ts;
    ts.tv_sec = 1;
    ts.tv_nsec = 0;
    nanosleep(&ts, nullptr);
#endif
  }
  return 0;
#endif
}
