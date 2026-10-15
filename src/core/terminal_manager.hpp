// terminal_manager.hpp - 子进程管理（reproc 封装；terminal.md）
// TerminalRegistry 记录元数据，本类持有真实 reproc 句柄并做 spawn/poll/kill。
#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/conpty.hpp"
#include "core/shell_detect.hpp"
#include "core/terminal_registry.hpp"
#include "fs_clock.hpp"
#include "fs_error.hpp"
#include "fs_json.hpp"
#include "fs_types.hpp"
#include "term/fs_vt.hpp"

namespace fs {

// 一个在跑的终端进程（reproc_t* 由本类拥有）
class TerminalManager {
 public:
  TerminalManager(TerminalRegistry* reg, const Clock* clock);
  ~TerminalManager();

  // args: {command, shell, cwd, encoding, background, wakeOnExit, timeoutMs,
  //        kind, label, interactive, cols, rows}
  // interactive = 起一个交互式 shell（无命令、stdin 接管），用户终端用；
  // 非交互 = shell -c command，模型终端用。
  // -> {tid, status, output, exitCode}
  FsResult spawn(const Json& args, Json* out);
  // args: {tid} -> {status, output, exitCode}
  FsResult poll(const Json& args, Json* out);
  // args: {tid} -> {}
  FsResult kill(const Json& args, Json* out);
  // args: {tid, text} -> {written}；把用户输入写进交互式终端的 stdin。
  // text 是终端要的原始字节（VT 编码后的按键），不再补换行。
  FsResult write(const Json& args, Json* out);
  // args: {tid, cols, rows} -> {}；交互式终端（ConPTY）改变窗口大小
  FsResult resize(const Json& args, Json* out);
  // args: {tid} -> {cols, rows, cursor, lines}；把屏幕网格给 UI 画
  FsResult grid(const Json& args, Json* out);

  // One non-blocking pass over every live terminal: drain output and detect
  // exits. Returns one entry per terminal with NEW output or that just exited,
  // so the host can push it (events/SSE) instead of the UI polling by hand.
  // Entry: {tid, sid, output (new bytes), status, exitCode, started}
  //
  // Callers: the desktop poller thread AND (in the browser build) the UI's
  // timer both reach this, so every entry point locks mu_ - reproc handles
  // are not safe to touch from two threads at once.
  std::vector<Json> tick();

 private:
  struct Proc;
  TerminalRegistry* reg_;
  const Clock* clock_;
  std::map<std::string, std::unique_ptr<Proc> > procs_;
  // serializes spawn/poll/kill/write/tick: the desktop poller thread and the
  // browser UI timer can both drive tick() at the same time
  std::mutex mu_;

  // 组装 argv（shell + flag + command）；win cmd 用 /c，powershell 用 -Command
  static std::vector<std::string> build_argv(const ShellInfo& sh, const std::string& cmd);
  // 交互式 shell 的 argv（无命令；bash/zsh 加 -i，PowerShell 只去 logo）
  static std::vector<std::string> build_argv_interactive(const ShellInfo& sh);
  // 把子进程输出喂进屏幕（VT 解析）；UTF-8 解码由调用方先做
  static void feed_screen(Proc& p, const std::string& text);
  // 屏幕 -> UI 用的紧凑 JSON（按样式合并连续单元格）
  static Json screen_json(const Proc& p);
  // 读走当前可用输出（非阻塞），追加到 proc.out
  static void drain(Proc& p);
};

}  // namespace fs
