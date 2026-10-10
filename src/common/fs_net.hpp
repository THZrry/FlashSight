// fs_net.hpp - stubbable network layer (arch.md §6: 单测不碰真网)
#pragma once

#include <cstdint>
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
};

// 网络抽象接口：生产用 httplib+mbedTLS 实现（P2 接 provider 时落地），单测注入 FakeNet
class NetClient {
 public:
  virtual ~NetClient() = default;
  virtual FsResult request(const HttpRequest& req, HttpResponse* out) = 0;
};

}  // namespace fs