// shell_detect.hpp - shell detection (terminal.md §4, 10-appcore SettingsStore)
#pragma once

#include <string>
#include <vector>

#include "fs_json.hpp"

namespace fs {

// 探测结果缓存；探测顺序 bash > zsh > pwsh7+ > PowerShell > cmd（cmd 兜底）。
struct ShellInfo {
  std::string name;   // "pwsh"|"powershell"|"cmd"|"bash"|"zsh"
  std::string path;   // 可执行路径
  int priority = -1;  // 越高越优先；-1 = 不可用
};

class ShellDetector {
 public:
  // 全量探测（一次缓存）
  static std::vector<ShellInfo> detect_all();

  // 择最优：显式指定优先；否则自动最佳
  static ShellInfo resolve(const std::string& explicit_name,
                           const std::vector<ShellInfo>& detected);

  // 简单探测一个名（未命中返回 priority=-1 的占位）
  static ShellInfo probe(const std::string& name);

  // 全量探测结果（JSON 数组，便于 FsApi settings.detectShells）
  static Json detect_all_json();

  // 在 PATH 上探测常见的开发 CLI 工具，返回 {name: path}（只含找到的）。
  // 用途：设置-环境 里"自动探测"。只列找到的——列一堆没装的工具路径没意义。
  static Json detect_cli_tools_json();

 private:
  static ShellInfo probe_bash();
  static ShellInfo probe_zsh();
  static ShellInfo probe_pwsh();
  static ShellInfo probe_powershell();
  static ShellInfo probe_cmd();
};

}  // namespace fs