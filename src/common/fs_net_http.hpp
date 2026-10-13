// fs_net_http.hpp - httplib + mbedTLS 真实现（可被 FakeNet 替换）
#pragma once

#include <string>

#include "fs_net.hpp"

namespace fs {

class HttplibNetClient : public NetClient {
 public:
  explicit HttplibNetClient(const HttpRequest& sample);
  ~HttplibNetClient() override;

  FsResult request(const HttpRequest& req, HttpResponse* out) const override;

  // 默认代理（设置 permissions.network.proxyHost/Port；请求未指定时套用）
  void set_default_proxy(const std::string& host, int port) {
    proxy_host_ = host;
    proxy_port_ = port;
  }

 private:
  // 手写 URL 解析（scheme://host[:port]/path），避免依赖 URL 库
  static bool parse_url(const std::string& url, std::string* scheme,
                        std::string* host, int* port, std::string* path);
  std::string proxy_host_;
  int proxy_port_ = 0;
};

}  // namespace fs