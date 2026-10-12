// terminal_manager.hpp - 子进程管理（reproc 封装；terminal.md）
// TerminalRegistry 记录元数据，本类持有真实 reproc 句柄并做 spawn/poll/kill。
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/shell_detect.hpp"
#include "core/terminal_registry.hpp"
#include "fs_clock.hpp"
#include "fs_error.hpp"
#include "fs_json.hpp"
#include "fs_types.hpp"

namespace fs {

// 一个在跑的终端进程（reproc_t* 由本类拥有）
class TerminalManager {
 public:
  TerminalManager(TerminalRegistry* reg, const Clock* clock);
  ~TerminalManager();

  // args: {command, shell, cwd, encoding, background, wakeOnExit, timeoutMs}
  // -> {tid, status, output, exitCode}
  FsResult spawn(const Json& args, Json* out);
  // args: {tid} -> {status, output, exitCode}
  FsResult poll(const Json& args, Json* out);
  // args: {tid} -> {}
  FsResult kill(const Json& args, Json* out);

 private:
  struct Proc;
  TerminalRegistry* reg_;
  const Clock* clock_;
  std::map<std::string, std::unique_ptr<Proc> > procs_;

  // 组装 argv（shell + flag + command）；win cmd 用 /c，powershell 用 -Command
  static std::vector<std::string> build_argv(const ShellInfo& sh, const std::string& cmd);
  // 读走当前可用输出（非阻塞），追加到 proc.out
  static void drain(Proc& p);
};

}  // namespace fs
