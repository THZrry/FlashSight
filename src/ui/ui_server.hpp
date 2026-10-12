// ui_server.hpp - 本地 HTTP 服务器：从内存提供内嵌的 web 资产
// WhaleUI 未就位前的过渡方案：exe 起本地服务，浏览器打开即用（wasm UI）。
#pragma once

#include <memory>
#include <string>

#include "fs_error.hpp"

namespace fs {
namespace ui {

class UiServer {
 public:
  UiServer();
  ~UiServer();

  // 监听 127.0.0.1 随机/指定端口；返回实际 URL（如 http://127.0.0.1:8123/）
  FsResult start(int port = 0);
  void stop();
  bool running() const;
  std::string url() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace ui
}  // namespace fs