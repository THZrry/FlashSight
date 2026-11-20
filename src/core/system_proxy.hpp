// system_proxy.hpp - resolve the OS/browser proxy for "跟随系统" mode.
#pragma once

#include <string>

namespace fs {

struct SystemProxy {
  std::string host;
  int port = 0;
  bool valid() const { return !host.empty() && port > 0; }
};

// Best-effort: Windows system proxy (WinHTTP/IE settings) or the standard
// HTTPS_PROXY / HTTP_PROXY / ALL_PROXY environment variables elsewhere.
SystemProxy system_http_proxy();

}  // namespace fs
