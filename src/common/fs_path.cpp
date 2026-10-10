// fs_path.cpp - path utilities (Win %USERPROFILE%, Linux $HOME)
#include "fs_path.hpp"

#include <cstdlib>

namespace fs {
namespace pathutil {

namespace {
#if defined(_WIN32)
const char* kHomeVar = "USERPROFILE";
const char* kHomeDriveVar = "HOMEDRIVE";
const char* kHomePathVar = "HOMEPATH";
#else
const char* kHomeVar = "HOME";
#endif
}  // namespace

FsPath home_dir() {
  const char* h = std::getenv(kHomeVar);
  if (h && *h) return FsPath(h);
#if defined(_WIN32)
  const char* d = std::getenv(kHomeDriveVar);
  const char* p = std::getenv(kHomePathVar);
  if (d && p && *d && *p) return FsPath(std::string(d) + p);
#endif
  return FsPath(".");
}

FsPath expand_user(const FsPath& p) {
  if (p.empty()) return p;
  std::string s = p.string();
  if (s == "~") return home_dir();
  if (s.size() > 1 && s[0] == '~' && (s[1] == '/' || s[1] == '\\')) {
    return home_dir() / FsPath(s.substr(2));
  }
  return p;
}

FsPath normalize(const FsPath& p) {
  FsPath a = p.is_absolute() ? p : home_dir() / FsPath(".") / p;
  // 统一先绝对化
  return expand_user(a).lexically_normal();
}

bool is_within(const FsPath& child, const FsPath& parent) {
  FsPath c = normalize(child);
  FsPath p = normalize(parent);
  auto it = c.begin();
  for (auto it2 = p.begin(); it2 != p.end(); ++it2, ++it) {
    if (it == c.end() || *it != *it2) return false;
  }
  return true;
}

std::string display(const FsPath& p) {
  FsPath n = normalize(p);
  FsPath h = home_dir();
  if (is_within(n, h)) {
    std::string s = n.string();
    std::string hs = h.string();
    if (s.compare(0, hs.size(), hs) == 0) {
      if (s.size() == hs.size()) return "~";
      return "~" + s.substr(hs.size());
    }
  }
  return n.string();
}

FsPath global_dir() { return home_dir() / ".flashsight"; }

FsPath project_dir(const FsPath& project_path) {
  return normalize(project_path) / ".flashsight";
}

FsPath session_dir(const FsPath& project_path, const std::string& sid) {
  return project_dir(project_path) / "sessions" / sid;
}

}  // namespace pathutil
}  // namespace fs