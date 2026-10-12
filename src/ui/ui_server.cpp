// ui_server.cpp - 本地 HTTP 服务（httplib），从内存提供内嵌资产
#include "ui_server.hpp"

#include <atomic>
#include <thread>

#include "httplib.h"
#include "ui_assets_gen.hpp"

namespace fs {
namespace ui {

struct UiServer::Impl {
  std::unique_ptr<httplib::Server> svr;
  std::thread th;
  std::atomic<bool> running{false};
  int port = 0;
};

UiServer::UiServer() : impl_(new Impl()) {}

UiServer::~UiServer() { stop(); }

FsResult UiServer::start(int port) {
  if (impl_->running.load()) return FsResult::success();
  impl_->svr.reset(new httplib::Server());

  // 静态资产：/ -> index.html；其余按名字
  impl_->svr->Get("/(.*)", [](const httplib::Request& req, httplib::Response& res) {
    std::string path = req.matches.size() > 1 ? req.matches[1].str() : "";
    if (path.empty() || path == "/") path = "index.html";
    for (auto& a : assets::kFiles()) {
      if (path == a.name) {
        res.set_content(reinterpret_cast<const char*>(a.data), a.size, a.mime);
        return;
      }
    }
    res.status = 404;
    res.set_content("not found", "text/plain");
  });

  // 端口：0 = 让系统分配
  int p = impl_->svr->bind_to_any_port("127.0.0.1", 0);
  if (port > 0) {
    p = port;
    if (!impl_->svr->bind_to_port("127.0.0.1", port)) {
      return FsResult::error(ErrCode::kIo, "cannot bind port " + std::to_string(port));
    }
  }
  impl_->port = p;
  impl_->running.store(true);
  impl_->th = std::thread([this]() { impl_->svr->listen_after_bind(); });
  // 等 listen 就绪（简单轮询）
  for (int i = 0; i < 100 && !impl_->svr->is_running(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return FsResult::success();
}

void UiServer::stop() {
  if (impl_ && impl_->running.load()) {
    impl_->svr->stop();
    if (impl_->th.joinable()) impl_->th.join();
    impl_->running.store(false);
  }
}

bool UiServer::running() const { return impl_ && impl_->running.load(); }

std::string UiServer::url() const {
  if (!impl_) return "";
  return "http://127.0.0.1:" + std::to_string(impl_->port) + "/";
}

}  // namespace ui
}  // namespace fs
