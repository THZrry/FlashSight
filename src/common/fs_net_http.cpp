// fs_net_http.cpp - httplib + mbedTLS based NetClient
#include "fs_net_http.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "httplib.h"

namespace fs {

namespace {

// Closes the socket when the caller cancels. `on_chunk` is not invoked while a
// read is blocked, so without this an idle/hung SSE stream could only be
// stopped by the per-read timeout (minutes); 中止 looked dead for that long.
class CancelWatch {
 public:
  CancelWatch(const std::function<bool()>& cancel, std::function<void()> stop) {
    if (!cancel) return;
    thread_ = std::thread([this, cancel, stop]() {
      while (!done_.load()) {
        if (cancel()) {
          stop();
          return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
    });
  }
  ~CancelWatch() {
    done_.store(true);
    if (thread_.joinable()) thread_.join();
  }

 private:
  std::atomic<bool> done_{false};
  std::thread thread_;
};

FsResult map_httplib_error(httplib::Error e) {
  // The mapped message says what the APP should do; the detail says what
  // actually happened. Without the detail every transport failure surfaced as
  // the same "network io error", which made a TLS handshake failure
  // indistinguishable from a DNS failure or a dropped connection.
  const std::string detail = httplib::to_string(e);
  switch (e) {
    case httplib::Error::Connection:
    case httplib::Error::Read:
    case httplib::Error::Write:
    case httplib::Error::ConnectionClosed:
      return FsResult::error(ErrCode::kNetwork, "network io error (" + detail + ")");
    case httplib::Error::ConnectionTimeout:
    case httplib::Error::Timeout:
      return FsResult::error(ErrCode::kTimeout, "network timeout (" + detail + ")");
    case httplib::Error::SSLConnection:
    case httplib::Error::SSLLoadingCerts:
    case httplib::Error::SSLServerVerification:
    case httplib::Error::SSLServerHostnameVerification:
      return FsResult::error(ErrCode::kNetwork, "tls/ssl error (" + detail + ")");
    default:
      return FsResult::error(ErrCode::kNetwork, "http client error (" + detail + ")");
  }
}

// httplib SSLClient/Client 无公共基类：模板按方法名执行请求
template <typename Cli>
FsResult execute(Cli& cli, const HttpRequest& req, const std::string& path,
                 HttpResponse* out) {
  httplib::Headers h;
  for (auto& kv : req.headers) h.emplace(kv.first, kv.second);
  httplib::Result res;
  if (req.method == "GET") {
    res = cli.Get(path.c_str(), h);
  } else if (req.method == "POST") {
    res = cli.Post(path.c_str(), h, req.body, "application/json");
  } else if (req.method == "DELETE") {
    res = cli.Delete(path.c_str(), h);
  } else if (req.method == "PUT") {
    res = cli.Put(path.c_str(), h, req.body, "application/json");
  } else {
    return FsResult::error(ErrCode::kInvalidArg, "unsupported method: " + req.method);
  }
  httplib::Error err = res.error();
  if (err != httplib::Error::Success) return map_httplib_error(err);
  out->status = res->status;
  out->body = res->body;
  return FsResult::success();
}

// Same request, but the body is handed over as it arrives. httplib's
// ContentReceiver runs per read() chunk, so an SSE stream reaches on_chunk while
// the model is still generating.
template <typename Cli>
FsResult execute_stream(Cli& cli, const HttpRequest& req, const std::string& path,
                        const std::function<bool(const char*, size_t)>& on_chunk,
                        HttpResponse* out) {
  httplib::Headers h;
  for (auto& kv : req.headers) h.emplace(kv.first, kv.second);
  httplib::ContentReceiver recv = [&on_chunk, out](const char* data, size_t len) {
    // Keep a copy so a non-streaming consumer still sees the whole body, and
    // forward it so a streaming one can render incrementally.
    out->body.append(data, len);
    return on_chunk ? on_chunk(data, len) : true;
  };
  httplib::Result res;
  if (req.method == "POST") {
    res = cli.Post(path.c_str(), h, req.body, "application/json", recv);
  } else if (req.method == "GET") {
    res = cli.Get(path.c_str(), h, recv);
  } else {
    // The streaming overloads only exist for GET/POST; the rest are small
    // (DELETE/PUT) and do not stream.
    return execute(cli, req, path, out);
  }
  httplib::Error err = res.error();
  if (err != httplib::Error::Success) return map_httplib_error(err);
  out->status = res->status;
  return FsResult::success();
}

}  // namespace

HttplibNetClient::HttplibNetClient(const HttpRequest&) {}
HttplibNetClient::~HttplibNetClient() {}

FsResult HttplibNetClient::request(const HttpRequest& req, HttpResponse* out) const {
  if (!out) return FsResult::error(ErrCode::kInvalidArg, "null response ptr");
  std::string host, scheme, path;
  int port = 0;
  if (!parse_url(req.url, &scheme, &host, &port, &path)) {
    return FsResult::error(ErrCode::kInvalidArg, "bad url: " + req.url);
  }
  // 请求级代理优先；否则用设置的默认代理（permissions.network.proxy*）
  std::string ph = req.proxy_host.empty() ? proxy_host_ : req.proxy_host;
  int pp = req.proxy_host.empty() ? proxy_port_ : req.proxy_port;
  bool tls = (scheme == "https");
  FsResult r;
  if (tls) {
    httplib::SSLClient cli(host, port);
    cli.enable_server_certificate_verification(req.verify_cert);
    cli.set_connection_timeout(req.timeout_ms / 1000, req.timeout_ms % 1000);
    cli.set_read_timeout(req.timeout_ms / 1000, req.timeout_ms % 1000);
    cli.set_write_timeout(req.timeout_ms / 1000, req.timeout_ms % 1000);
    if (!ph.empty() && pp > 0) cli.set_proxy(ph, pp);
    CancelWatch watch(req.cancel, [&cli]() { cli.stop(); });
    r = execute(cli, req, path, out);
  } else {
    httplib::Client cli(host, port);
    cli.set_connection_timeout(req.timeout_ms / 1000, req.timeout_ms % 1000);
    cli.set_read_timeout(req.timeout_ms / 1000, req.timeout_ms % 1000);
    cli.set_write_timeout(req.timeout_ms / 1000, req.timeout_ms % 1000);
    if (!ph.empty() && pp > 0) cli.set_proxy(ph, pp);
    CancelWatch watch(req.cancel, [&cli]() { cli.stop(); });
    r = execute(cli, req, path, out);
  }
  // FS_NET_DEBUG=1 traces every request. A network failure reported by the UI
  // is otherwise impossible to attribute: the error code alone does not say
  // which host, proxy or scheme was attempted.
  if (std::getenv("FS_NET_DEBUG")) {
    std::fprintf(stderr, "[net] %s %s%s proxy=%s:%d tls=%d -> ok=%d code=%d status=%d %s\n",
                 req.method.c_str(), host.c_str(), path.c_str(),
                 ph.empty() ? "-" : ph.c_str(), pp, (int)tls, (int)r.ok(),
                 (int)r.code, out->status, r.message.c_str());
  }
  return r;
}

FsResult HttplibNetClient::request_stream(
    const HttpRequest& req,
    const std::function<bool(const char*, size_t)>& on_chunk,
    HttpResponse* out) const {
  if (!out) return FsResult::error(ErrCode::kInvalidArg, "null response ptr");
  std::string host, scheme, path;
  int port = 0;
  if (!parse_url(req.url, &scheme, &host, &port, &path)) {
    return FsResult::error(ErrCode::kInvalidArg, "bad url: " + req.url);
  }
  std::string ph = req.proxy_host.empty() ? proxy_host_ : req.proxy_host;
  int pp = req.proxy_host.empty() ? proxy_port_ : req.proxy_port;
  bool tls = (scheme == "https");
  FsResult r;
  if (tls) {
    httplib::SSLClient cli(host, port);
    cli.enable_server_certificate_verification(req.verify_cert);
    cli.set_connection_timeout(req.timeout_ms / 1000, req.timeout_ms % 1000);
    cli.set_read_timeout(req.timeout_ms / 1000, req.timeout_ms % 1000);
    cli.set_write_timeout(req.timeout_ms / 1000, req.timeout_ms % 1000);
    if (!ph.empty() && pp > 0) cli.set_proxy(ph, pp);
    CancelWatch watch(req.cancel, [&cli]() { cli.stop(); });
    r = execute_stream(cli, req, path, on_chunk, out);
  } else {
    httplib::Client cli(host, port);
    cli.set_connection_timeout(req.timeout_ms / 1000, req.timeout_ms % 1000);
    cli.set_read_timeout(req.timeout_ms / 1000, req.timeout_ms % 1000);
    cli.set_write_timeout(req.timeout_ms / 1000, req.timeout_ms % 1000);
    if (!ph.empty() && pp > 0) cli.set_proxy(ph, pp);
    CancelWatch watch(req.cancel, [&cli]() { cli.stop(); });
    r = execute_stream(cli, req, path, on_chunk, out);
  }
  if (std::getenv("FS_NET_DEBUG")) {
    std::fprintf(stderr, "[net] (stream) %s %s%s -> ok=%d code=%d status=%d %s\n",
                 req.method.c_str(), host.c_str(), path.c_str(), (int)r.ok(),
                 (int)r.code, out->status, r.message.c_str());
  }
  return r;
}

bool HttplibNetClient::parse_url(const std::string& url, std::string* scheme,
                                 std::string* host, int* port, std::string* path) {
  size_t pos = url.find("://");
  if (pos == std::string::npos) return false;
  *scheme = url.substr(0, pos);
  size_t host_start = pos + 3;
  size_t path_start = url.find('/', host_start);
  std::string hostport = (path_start == std::string::npos)
                             ? url.substr(host_start)
                             : url.substr(host_start, path_start - host_start);
  *path = (path_start == std::string::npos) ? "/" : url.substr(path_start);
  size_t colon = hostport.find(':');
  if (colon == std::string::npos) {
    *host = hostport;
    *port = (*scheme == "https") ? 443 : 80;
  } else {
    *host = hostport.substr(0, colon);
    try {
      *port = std::stoi(hostport.substr(colon + 1));
    } catch (...) {
      return false;
    }
  }
  return !host->empty() && *port > 0;
}

}  // namespace fs
