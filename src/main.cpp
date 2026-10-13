// FlashSight desktop entry
// WhaleUI 未就位前的过渡：起本地 HTTP 服务，提供内嵌的 wasm UI + 后端 API。
// 以后接 WhaleUI 时换成 NativeBackend（进程内直调 FsApi），UI 逻辑不变。
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
#include "storage/fs_store.hpp"
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

  fs::ui::UiServer server;
  server.bind_api(&api);
  fs::FsResult r = server.start(port);
  if (!r.ok()) {
    std::fprintf(stderr, "failed to start UI server: %s\n", r.message.c_str());
    return 1;
  }
  std::printf("UI server: %s\n", server.url().c_str());
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
}
