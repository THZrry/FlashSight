// FlashSight desktop entry
// WhaleUI 未就位前的过渡：起本地 HTTP 服务提供内嵌的 wasm UI，打开浏览器。
// 以后接 WhaleUI 时换掉这里即可（UI 逻辑在 src/ui，与传输方式无关）。
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

#include "common/fs_version.hpp"
#include "ui/ui_server.hpp"

namespace {

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

}  // namespace

int main(int argc, char** argv) {
  int port = 0;
  bool no_browser = false;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--port" && i + 1 < argc) {
      port = std::atoi(argv[++i]);
    } else if (a == "--no-browser") {
      no_browser = true;
    }
  }

  std::printf("FlashSight %s\n", fs::version_string());

  fs::ui::UiServer server;
  fs::FsResult r = server.start(port);
  if (!r.ok()) {
    std::fprintf(stderr, "failed to start UI server: %s\n", r.message.c_str());
    return 1;
  }
  std::printf("UI server: %s\n", server.url().c_str());
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
}
