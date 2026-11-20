// system_proxy.cpp - resolve the OS/browser proxy for "跟随系统" mode.
#include "system_proxy.hpp"

#include <cctype>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#endif

namespace fs {

namespace {

std::string lower_ascii(std::string s) {
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] >= 'A' && s[i] <= 'Z') s[i] = static_cast<char>(s[i] - 'A' + 'a');
  }
  return s;
}

// "http://host:8080/" / "host:8080" / "http=host:8080;https=..." -> host + port.
bool parse_proxy_spec(std::string spec, std::string* host, int* port) {
  host->clear();
  *port = 0;
  if (spec.empty()) return false;
  // A WinHTTP proxy list names a different proxy per scheme.
  size_t eq = spec.find('=');
  if (eq != std::string::npos && spec.find(';') != std::string::npos) {
    std::string pick;
    size_t pos = 0;
    while (pos <= spec.size()) {
      size_t semi = spec.find(';', pos);
      std::string part = spec.substr(pos, semi == std::string::npos
                                              ? std::string::npos
                                              : semi - pos);
      size_t e = part.find('=');
      if (e != std::string::npos) {
        std::string scheme = lower_ascii(part.substr(0, e));
        if (scheme == "https" || (pick.empty() && scheme == "http")) {
          pick = part.substr(e + 1);
          if (scheme == "https") break;
        }
      } else if (pick.empty()) {
        pick = part;
      }
      if (semi == std::string::npos) break;
      pos = semi + 1;
    }
    spec = pick;
  }
  std::string s = spec;
  size_t scheme = s.find("://");
  if (scheme != std::string::npos) s = s.substr(scheme + 3);
  size_t slash = s.find('/');
  if (slash != std::string::npos) s = s.substr(0, slash);
  if (s.empty()) return false;
  // host:port (an IPv6 literal without brackets is not supported; WinHTTP also
  // hands it bracketed).
  size_t colon = s.rfind(':');
  if (colon != std::string::npos && colon + 1 < s.size()) {
    bool digits = true;
    for (size_t i = colon + 1; i < s.size(); ++i) {
      if (!std::isdigit(static_cast<unsigned char>(s[i]))) { digits = false; break; }
    }
    if (digits) {
      *host = s.substr(0, colon);
      *port = std::atoi(s.c_str() + colon + 1);
    }
  }
  if (host->empty()) {
    *host = s;
    *port = 80;
  }
  return !host->empty() && *port > 0;
}

std::string env_proxy() {
  const char* names[] = {"HTTPS_PROXY", "https_proxy", "HTTP_PROXY", "http_proxy",
                         "ALL_PROXY", "all_proxy"};
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
    const char* v = std::getenv(names[i]);
    if (v && *v) return v;
  }
  return std::string();
}

}  // namespace

SystemProxy system_http_proxy() {
  SystemProxy out;
  std::string spec;
#if defined(_WIN32)
  WINHTTP_CURRENT_USER_IE_PROXY_CONFIG cfg;
  std::memset(&cfg, 0, sizeof(cfg));
  if (WinHttpGetIEProxyConfigForCurrentUser(&cfg)) {
    if (cfg.lpszProxy) {
      std::wstring w(cfg.lpszProxy);
      int need = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0,
                                       nullptr, nullptr);
      if (need > 1) {
        spec.resize(static_cast<size_t>(need - 1));
        ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &spec[0], need, nullptr,
                              nullptr);
      }
    }
    // These are allocated by WinHTTP even when unused.
    if (cfg.lpszProxy) ::GlobalFree(cfg.lpszProxy);
    if (cfg.lpszProxyBypass) ::GlobalFree(cfg.lpszProxyBypass);
    if (cfg.lpszAutoConfigUrl) ::GlobalFree(cfg.lpszAutoConfigUrl);
  }
#endif
  if (!parse_proxy_spec(spec, &out.host, &out.port)) {
    // Environment fallback (the only path on POSIX). A PAC-only Windows setup
    // has no fixed endpoint we can apply, so it also lands here.
    parse_proxy_spec(env_proxy(), &out.host, &out.port);
  }
  return out;
}

}  // namespace fs
