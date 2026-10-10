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

 private:
  // 手写 URL 解析（scheme://host[:port]/path），避免依赖 URL 库
  static bool parse_url(const std::string& url, std::string* scheme,
                        std::string* host, int* port, std::string* path);
};

}  // namespace fs