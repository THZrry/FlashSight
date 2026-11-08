// fs_net.hpp - stubbable network layer (arch.md §6: 单测不碰真网)
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>

#include "fs_error.hpp"

namespace fs {

// HTTP 响应（脱敏后）
struct HttpResponse {
  int status = 0;               // 0 = 网络层失败
  std::string body;
  std::map<std::string, std::string> headers;  // key 小写
  std::string error;            // 网络层错误描述（status==0 时）
};

struct HttpRequest {
  std::string method = "GET";
  std::string url;
  std::map<std::string, std::string> headers;
  std::string body;
  int timeout_ms = 30000;
  int64_t size_cap = 0;         // 0 = 不限；超限报错
  bool verify_cert = true;      // 证书开关（permissions.md §5）
  // 预留代理（httplib 不读系统代理 env，见 notes <4>）
  std::string proxy_host;
  int proxy_port = 0;
  // Polled while the request is blocked in a socket read. A watcher in the
  // httplib implementation calls `Client::stop()` so an idle SSE stream can be
  // aborted immediately (the on_chunk callback is not called when no bytes
  // arrive, so checking cancellation only there waited for the read timeout).
  std::function<bool()> cancel;
};

// 网络抽象接口：生产用 httplib+mbedTLS 实现（P2 接 provider 时落地），单测注入 FakeNet
// request 声明为 const：发请求不改变客户端状态（FakeNet/真实现都满足）
class NetClient {
 public:
  virtual ~NetClient() = default;
  virtual FsResult request(const HttpRequest& req, HttpResponse* out) const = 0;

  // Streaming variant: `on_chunk` is called as bytes arrive, so an SSE response
  // can be rendered while the model is still talking. Returning false from the
  // callback aborts the transfer (used when the UI cancels).
  //
  // The default implementation issues a normal request and delivers the body in
  // one chunk, so a stub NetClient keeps working without implementing this -
  // only "is the text incremental?" changes, never the result.
  virtual FsResult request_stream(
      const HttpRequest& req,
      const std::function<bool(const char*, size_t)>& on_chunk,
      HttpResponse* out) const {
    FsResult r = request(req, out);
    if (r.ok() && on_chunk && !out->body.empty()) {
      on_chunk(out->body.data(), out->body.size());
    }
    return r;
  }
};

}  // namespace fs