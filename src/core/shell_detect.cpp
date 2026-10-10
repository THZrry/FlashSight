// shell_detect.cpp - real detection via `where` (win) / `which` (posix), cached
#include "shell_detect.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include <ghc/filesystem.hpp>

namespace fs {

namespace {

// returns trimmed first line of cmd output, or "" on failure
std::string run_capture(const std::string& cmd) {
  std::string out;
#if defined(_WIN32)
  FILE* f = _popen(cmd.c_str(), "r");
#else
  FILE* f = popen(cmd.c_str(), "r");
#endif
  if (!f) return out;
  char buf[256];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf) - 1, f)) > 0) {
    buf[n] = 0;
    out.append(buf, n);
  }
#if defined(_WIN32)
  _pclose(f);
#else
  pclose(f);
#endif
  // trim trailing newline/CR
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
  return out;
}

// probe a named executable: returns path (first found) or ""
std::string which(const std::string& name) {
  if (name.empty()) return "";
  // 已在 PATH 里的直接找（相对名）
  std::string res;
#if defined(_WIN32)
  res = run_capture("where " + name + " 2>nul");
#else
  res = run_capture("command -v " + name + " 2>/dev/null");
#endif
  if (!res.empty()) return res;
  // 绝对路径直接判存在
  ghc::filesystem::path p(name);
  if (p.is_absolute() && ghc::filesystem::exists(p)) return name;
  return "";
}

}  // namespace

ShellInfo probe_named(const std::string& name) {
  ShellInfo si;
  si.name = name;
  si.path = which(name);
  si.priority = si.path.empty() ? -1 : 1;
  return si;
}

std::vector<ShellInfo> ShellDetector::detect_all() {
  std::vector<ShellInfo> out;
  // 探测顺序：bash > zsh > pwsh7+ > powershell > cmd（cmd 兜底必可用）
  out.push_back(probe_named("bash"));
  out.push_back(probe_named("zsh"));
  out.push_back(probe_named("pwsh"));
  out.push_back(probe_named("powershell"));
  out.push_back(probe_named("cmd"));
  int order = 100;
  for (auto& s : out) {
    if (s.priority >= 0) s.priority = order--;
  }
  // 过滤掉不可用的
  std::vector<ShellInfo> usable;
  for (auto& s : out) if (s.priority >= 0) usable.push_back(s);
  // 按 priority 降序
  std::sort(usable.begin(), usable.end(),
            [](const ShellInfo& a, const ShellInfo& b) { return a.priority > b.priority; });
  return usable;
}

ShellInfo ShellDetector::resolve(const std::string& explicit_name,
                                 const std::vector<ShellInfo>& detected) {
  if (!explicit_name.empty()) {
    for (auto& s : detected) if (s.name == explicit_name) return s;
  }
  if (!detected.empty()) return detected[0];
  return probe_named("cmd");
}

ShellInfo ShellDetector::probe(const std::string& name) { return probe_named(name); }
ShellInfo ShellDetector::probe_bash() { return probe_named("bash"); }
ShellInfo ShellDetector::probe_zsh() { return probe_named("zsh"); }
ShellInfo ShellDetector::probe_pwsh() { return probe_named("pwsh"); }
ShellInfo ShellDetector::probe_powershell() { return probe_named("powershell"); }
ShellInfo ShellDetector::probe_cmd() { return probe_named("cmd"); }

Json ShellDetector::detect_all_json() {
  Json arr = Json::array();
  for (auto& s : detect_all()) {
    Json j = Json::object();
    j["name"] = s.name;
    j["path"] = s.path;
    j["priority"] = s.priority;
    arr.push_back(j);
  }
  return arr;
}

}  // namespace fs