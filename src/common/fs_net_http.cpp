// fs_net_http.cpp - httplib + mbedTLS based NetClient
#include "fs_net_http.hpp"

#include <string>

#include "httplib.h"

namespace fs {

namespace {

FsResult map_httplib_error(httplib::Error e) {
  switch (e) {
    case httplib::Error::Connection:
    case httplib::Error::Read:
    case httplib::Error::Write:
    case httplib::Error::ConnectionClosed:
      return FsResult::error(ErrCode::kNetwork, "network io error");
    case httplib::Error::ConnectionTimeout:
    case httplib::Error::Timeout:
      return FsResult::error(ErrCode::kTimeout, "network timeout");
    case httplib::Error::SSLConnection:
    case httplib::Error::SSLLoadingCerts:
    case httplib::Error::SSLServerVerification:
    case httplib::Error::SSLServerHostnameVerification:
      return FsResult::error(ErrCode::kNetwork, "tls/ssl error");
    default:
      return FsResult::error(ErrCode::kNetwork, "http client error");
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
  bool tls = (scheme == "https");
  if (tls) {
    httplib::SSLClient cli(host, port);
    cli.enable_server_certificate_verification(req.verify_cert);
    cli.set_connection_timeout(req.timeout_ms / 1000, req.timeout_ms % 1000);
    cli.set_read_timeout(req.timeout_ms / 1000, req.timeout_ms % 1000);
    cli.set_write_timeout(req.timeout_ms / 1000, req.timeout_ms % 1000);
    if (!req.proxy_host.empty() && req.proxy_port > 0) {
      cli.set_proxy(req.proxy_host, req.proxy_port);
    }
    return execute(cli, req, path, out);
  } else {
    httplib::Client cli(host, port);
    cli.set_connection_timeout(req.timeout_ms / 1000, req.timeout_ms % 1000);
    cli.set_read_timeout(req.timeout_ms / 1000, req.timeout_ms % 1000);
    cli.set_write_timeout(req.timeout_ms / 1000, req.timeout_ms % 1000);
    if (!req.proxy_host.empty() && req.proxy_port > 0) {
      cli.set_proxy(req.proxy_host, req.proxy_port);
    }
    return execute(cli, req, path, out);
  }
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
  return !host->empty();
}

}  // namespace fs