// ui_server.hpp - 本地 HTTP 服务器：提供内嵌 web 资产 + 后端 API。
//
// WhaleUI 未就位前的过渡方案：exe 起本地服务，浏览器打开即用（wasm UI）。
// 服务两类请求：
//   GET  /<asset>              内嵌静态资源（index.html/style.css/fs_ui.js/.wasm）
//   POST /api/call             调后端方法（UiApi -> FsApi）
//   GET  /api/events?since=N   拉取后端事件（自 N 之后）
// 事件来自 FsApi 的 event sink，存进环形缓冲；wasm 侧定时轮询。
#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "fs_error.hpp"
#include "fs_json.hpp"

namespace fs {

class FsApi;

namespace ui {

class UiServer {
 public:
  UiServer();
  ~UiServer();

  // 绑定 FsApi：注册 /api/call 分派与事件采集。须在 start 前调用。
  void bind_api(FsApi* api);

  // 监听 127.0.0.1 随机/指定端口；返回实际 URL（如 http://127.0.0.1:8123/）
  FsResult start(int port = 0);
  void stop();
  bool running() const;
  std::string url() const;

  // 事件缓冲（测试可直接取；start 后由 FsApi sink 填充）
  Json drain_events(int64_t since, int64_t* next);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace ui
}  // namespace fs
