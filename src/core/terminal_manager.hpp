// terminal_manager.hpp - 子进程管理（reproc 封装；terminal.md）
// TerminalRegistry 记录元数据，本类持有真实 reproc 句柄并做 spawn/poll/kill。
#pragma once

#include <functional>
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
  FsResult spawn(const Json& args, Json* out,
                 const std::function<bool()>& cancel = nullptr);
  // args: {tid} -> {status, output, exitCode}
  FsResult poll(const Json& args, Json* out);
  // args: {tid} -> {}
  FsResult kill(const Json& args, Json* out);
  // args: {tid, text} -> {written}；把用户输入写进交互式终端的 stdin。
  // text 是终端要的原始字节（VT 编码后的按键），不再补换行。
  FsResult write(const Json& args, Json* out);
  // args: {tid, cols, rows} -> {}；交互式终端（ConPTY）改变窗口大小
  FsResult resize(const Json& args, Json* out);
  // args: {tid} -> {cols, rows, cursor, lines, scrollback, totalLines}
  // `lines` covers the visible screen plus up to `scrollback` history lines
  // ABOVE it, so the UI can scroll back; `scrollback` is how many of those
  // leading lines are history (the rest are the live screen).
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

  // Thread-safe registry access. TerminalRegistry itself has no lock; the
  // manager's mutex serialises every mutation, so reads from FsApi (session
  // list, terminal drawer, transcript lookup) must take the same lock and work
  // on copies. Returning a raw TermEntry* here was a two-session data race.
  bool entry_snapshot(const std::string& tid, TermEntry* out);
  std::vector<TermEntry> entries_for_session(const std::string& sid);
  bool add_saved_entry(const Json& j);
  // For the few single registry updates FsApi performs itself.
  std::recursive_mutex& mutex() { return mu_; }

  // Every drained chunk of output, the moment it is produced (tid + text). The
  // host writes the terminal's transcript from here - NOT from tick(), which may
  // never run again if the app closes right after a command exits. That gap is
  // why the tail of a session was missing on reopen. Called with the manager's
  // mutex held, so the sink must not call back into the manager.
  typedef std::function<void(const std::string&, const std::string&)> OutputSink;
  // Per-INSTANCE, not static: a static sink outlived its FsApi and a bare
  // TerminalManager (the unit test) then called into a destroyed object.
  void set_output_sink(OutputSink fn) { output_sink_ = std::move(fn); }

  // Where this terminal's transcript lives. Called RIGHT AFTER the registry
  // entry is created and BEFORE the child starts, so a FOREGROUND command's
  // output is logged too: the path used to be assigned only after spawn()
  // returned, by which time the whole run had already been drained - that is why
  // model `terminal_exec` runs left hundreds of records with no log file.
  typedef std::function<std::string(const std::string& sid,
                                    const std::string& tid)> LogPathFn;
  void set_log_path_fn(LogPathFn fn) { log_path_fn_ = std::move(fn); }

 private:
  struct Proc;
  OutputSink output_sink_;
  LogPathFn log_path_fn_;
  TerminalRegistry* reg_;
  const Clock* clock_;
  std::map<std::string, std::unique_ptr<Proc> > procs_;
  // Serializes spawn/poll/kill/write/tick AND every TerminalRegistry access.
  // Recursive so a manager method called while FsApi already holds it (a
  // snapshot helper) cannot self-deadlock.
  std::recursive_mutex mu_;

  // 组装 argv（shell + flag + command）；win cmd 用 /c，powershell 用 -Command
  static std::vector<std::string> build_argv(const ShellInfo& sh, const std::string& cmd);
  // 交互式 shell 的 argv（无命令；bash/zsh 加 -i，PowerShell 只去 logo）
  static std::vector<std::string> build_argv_interactive(const ShellInfo& sh);
  // 把子进程输出喂进屏幕（VT 解析）；UTF-8 解码由调用方先做
  static void feed_screen(Proc& p, const std::string& text);
  // 屏幕 -> UI 用的紧凑 JSON（按样式合并连续单元格）。from = 起始行（0 =
  // 最早的历史行；可见屏幕从 scrollback 处开始）。
  static Json screen_json(const Proc& p, int from = 0);
  // 读走当前可用输出（非阻塞），追加到 proc.out
  static void drain(Proc& p);
  // Non-blocking process exit check. MUST NOT use reproc_wait(handle, 0) here:
  // its Windows path calls WaitForSingleObject(INFINITE) once the exit PIPE is
  // readable, and a shell that closes the inherited pipe before exiting (BusyBox)
  // makes that pipe readable while the shell is still alive. Caller holds mu_.
  static bool try_wait(Proc& p, int* exit_code);
  // Wait for an exit without holding mu_ across the sleep. Returns a reproc code
  // (REPROC_ETIMEDOUT on deadline). Caller MUST NOT hold mu_.
  int wait_for_exit(Proc& p, int timeout_ms);
  // A foreground MODEL command that finished quickly is noise, not a record: its
  // transcript is deleted on exit. Kept: user terminals, background terminals,
  // labelled ("marked") ones, and anything that ran longer than a minute.
  void maybe_prune_log(const std::string& tid);
};

}  // namespace fs
