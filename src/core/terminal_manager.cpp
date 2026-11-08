// terminal_manager.cpp - reproc 子进程封装实现。
#include "terminal_manager.hpp"

#include <cstring>
#include <system_error>
#include <vector>

#include <reproc/reproc.h>

#include <ghc/filesystem.hpp>

#include "fs_encoding.hpp"

namespace fs {

// 一个在跑的进程：reproc 句柄（管道模式）或 ConPty（真终端）+ 输出缓冲。
//
// 两种模式的区别只在"子进程看到什么"：管道模式给的是普通 stdio，ConPTY 模式
// 给的是一个真正的控制台（有窗口大小、有 ANSI 输出、能跑全屏程序）。用户终端
// 优先 ConPTY；不可用时退回管道，此时仍然把输出喂进同一个屏幕模型。
struct TerminalManager::Proc {
  reproc_t* h = nullptr;
  ConPty* pty = nullptr;  // non-null => real terminal
  std::string tid;
  std::string sid;
  std::string out;        // 累积的 stdout+stderr（已转成 UTF-8）
  std::string enc;        // 输出编码（空=系统默认，见 fs_encoding.hpp）
  std::string pending;    // 跨 read 边界的不完整多字节序列
  vt::Screen screen;      // 屏幕网格（VT 解析结果；UI 直接画它）
  int cols = 100;
  int rows = 30;
  bool screen_dirty = true;
  size_t reported = 0;    // out 中已上报给 UI 的字节数（tick 只发新增）
  bool exited = false;
  int exit_code = 0;
  // A foreground spawn is waiting on this process OUTSIDE mu_ (see spawn()).
  // tick() must still drain output for the live panel, but exit detection and
  // the registry transition belong to that waiter; otherwise both would mark
  // the terminal exited and the event would race the tool result.
  bool fg_wait = false;
  // A BACKGROUND model terminal used to ignore timeoutMs completely: only the
  // foreground waiter called reproc_wait with a deadline, so `terminal_spawn`
  // with timeoutMs=60000 stayed alive for half an hour. Keep the deadline here
  // and enforce it in tick() (the only clock a background process has).
  int64_t timeout_ms = 0;
  int64_t started_ms = 0;
  int64_t kill_deadline_ms = 0;
  bool timeout_sent = false;
  bool hard_killed = false;
  bool timed_out = false;
  // Which manager owns this process: feed_screen is static and needs it to reach
  // the per-instance output sink (a static sink outlived its FsApi and called
  // into a destroyed object in the unit test).
  TerminalManager* owner = nullptr;
};

TerminalManager::TerminalManager(TerminalRegistry* reg, const Clock* clock)
    : reg_(reg), clock_(clock) {}

TerminalManager::~TerminalManager() {
  for (std::map<std::string, std::unique_ptr<Proc> >::iterator it = procs_.begin();
       it != procs_.end(); ++it) {
    Proc& p = *it->second;
    if (p.pty) {
      p.pty->terminate();
      delete p.pty;
      p.pty = nullptr;
    }
    if (p.h) {
      if (!p.exited) {
        // 退出时清理：先温和终止，再强杀（避免留孤儿）
        reproc_stop(p.h, (reproc_stop_actions)REPROC_STOP_ACTIONS_NULL);
      }
      p.h = reproc_destroy(p.h);
    }
  }
}

std::vector<std::string> TerminalManager::build_argv(const ShellInfo& sh,
                                                     const std::string& cmd) {
  std::vector<std::string> argv;
  std::string exe = sh.path.empty() ? sh.name : sh.path;
  argv.push_back(exe);
  if (sh.name == "cmd") {
    argv.push_back("/c");
    argv.push_back(cmd);
  } else if (sh.name == "powershell" || sh.name == "pwsh") {
    argv.push_back("-NoProfile");
    argv.push_back("-Command");
    argv.push_back(cmd);
  } else {
    // bash / zsh / 其它 POSIX 风格
    argv.push_back("-c");
    argv.push_back(cmd);
  }
  return argv;
}

std::vector<std::string> TerminalManager::build_argv_interactive(const ShellInfo& sh) {
  std::vector<std::string> argv;
  std::string exe = sh.path.empty() ? sh.name : sh.path;
  argv.push_back(exe);
  if (sh.name == "cmd") {
    // cmd 无命令时就是交互式；/Q 关掉 echo 提示噪声
    argv.push_back("/Q");
  } else if (sh.name == "powershell" || sh.name == "pwsh") {
    argv.push_back("-NoLogo");
    argv.push_back("-NoProfile");
  } else {
    // bash / zsh：-i 强制交互（管道 stdin 时否则会退化）
    argv.push_back("-i");
  }
  return argv;
}

// 把一段（已解码成 UTF-8 的）子进程输出同时存进文本缓冲和屏幕网格。
// 网格是 UI 真正画的东西：escape 序列在解析时被吃掉，光标/颜色/擦除都落成
// 单元格状态。文本缓冲只用于"所有终端"概览和复制。
void TerminalManager::feed_screen(Proc& p, const std::string& text) {
  p.out += text;
  // Log it NOW: this is the only place every byte passes through, and tick()
  // may not run again (the app can close right after a command exits).
  if (p.owner && p.owner->output_sink_) p.owner->output_sink_(p.tid, text);
  p.screen.feed(text);
  p.screen_dirty = true;
  // 终端要回话（DSR：shell 问光标在哪，不回就卡住）时立刻写回子进程
  std::string reply = p.screen.take_reply();
  if (!reply.empty()) {
    if (p.pty) {
      p.pty->write(reply);
    } else if (p.h) {
      reproc_write(p.h, reinterpret_cast<const uint8_t*>(reply.data()),
                   reply.size());
    }
  }
  if (p.out.size() > (1u << 20)) {  // 上限 1MB，防失控
    size_t cut = p.out.size() - (1u << 20);
    // 不要切在 UTF-8 序列中间
    while (cut < p.out.size() &&
           (static_cast<unsigned char>(p.out[cut]) & 0xC0) == 0x80) {
      ++cut;
    }
    p.out.erase(0, cut);
    p.reported = p.reported > cut ? p.reported - cut : 0;
  }
}

void TerminalManager::drain(Proc& p) {
  if (p.pty) {
    std::string raw;
    if (!p.pty->read(&raw)) return;
    if (raw.empty()) return;
    // ConPTY 在 Windows 上吐的是 UTF-8（它内部做了一次转换），但仍可能被切在
    // 多字节中间，所以照样过一遍解码器
    feed_screen(p, encoding::decode_output(raw, "UTF-8", &p.pending));
    return;
  }
  if (!p.h) return;
  char buf[4096];
  for (;;) {
    int n = reproc_read(p.h, REPROC_STREAM_OUT, reinterpret_cast<uint8_t*>(buf),
                        sizeof(buf));
    if (n > 0) {
      // 子进程给的是字节，不是文本：cmd/pwsh 在中文 Windows 上按 OEM 代码页
      // 输出，直接塞进 JSON 会让 dump() 抛非法 UTF-8（HTTP 500）。
      feed_screen(p, encoding::decode_output(
                         std::string(buf, static_cast<size_t>(n)), p.enc,
                         &p.pending));
      continue;
    }
    // n <= 0：无更多数据 / 管道关闭 / 非阻塞
    break;
  }
}

FsResult TerminalManager::spawn(const Json& args, Json* out) {
  // A foreground command waits HERE for its whole run, so mu_ must not be held
  // across that wait: it is the manager-wide lock shared by tick(), every other
  // terminal's spawn/poll/kill, and (through tick) the wake scheduler. A
  // `terminal_exec sleep 60` used to block all of them for a full minute - the
  // two-session freeze where the plan and scheduled wakes never updated.
  std::unique_lock<std::mutex> lk(mu_);
  std::string command = jsonutil::get<std::string>(args, "command", "");
  bool interactive = jsonutil::get<bool>(args, "interactive", false);
  // 交互式终端没有命令：它就是"设置里的那个 shell"的一个会话
  if (command.empty() && !interactive) {
    return FsResult::error(ErrCode::kInvalidArg, "empty command");
  }
  std::string cwd = jsonutil::get<std::string>(args, "cwd", "");
  bool background = jsonutil::get<bool>(args, "background", true);
  bool wake_on_exit = jsonutil::get<bool>(args, "wakeOnExit", false);
  std::string shell_name = jsonutil::get<std::string>(args, "shell", "");
  std::string encoding = jsonutil::get<std::string>(args, "encoding", "");
  std::string label = jsonutil::get<std::string>(args, "label", "");
  int64_t timeout_ms = jsonutil::get_int(args, "timeoutMs", 60000);

  // shell 解析：显式指定 > 探测最优（cmd 兜底）
  ShellInfo sh = ShellDetector::resolve(shell_name, ShellDetector::detect_all());
  if (sh.path.empty() && sh.name.empty()) {
    return FsResult::error(ErrCode::kNotSupported, "no usable shell found");
  }

  std::vector<std::string> argv = interactive ? build_argv_interactive(sh)
                                              : build_argv(sh, command);
  int cols = static_cast<int>(jsonutil::get_int(args, "cols", 100));
  int rows = static_cast<int>(jsonutil::get_int(args, "rows", 30));
  if (cols < 20) cols = 20;
  if (rows < 5) rows = 5;

  // 交互式终端优先走 ConPTY：管道给不了窗口大小、提示符和全屏程序，而用户终端
  // 就是要一个真终端。不可用时（旧系统/非 Windows）退回管道，输出仍然进同一个
  // 屏幕模型，只是没有窗口尺寸、全屏程序会退化。
  std::unique_ptr<ConPty> pty;
  if (interactive && ConPty::available()) {
    std::unique_ptr<ConPty> candidate(new ConPty());
    std::string exe = sh.path.empty() ? sh.name : sh.path;
    if (candidate->start(exe, argv.size() > 1
                                  ? std::vector<std::string>(argv.begin() + 1, argv.end())
                                  : std::vector<std::string>(),
                         cwd, cols, rows)) {
      pty = std::move(candidate);
    }
  }

  reproc_t* h = nullptr;
  if (!pty) {
    std::vector<const char*> cargv;
    for (size_t i = 0; i < argv.size(); ++i) cargv.push_back(argv[i].c_str());
    cargv.push_back(nullptr);

    h = reproc_new();
    reproc_options opts;
    std::memset(&opts, 0, sizeof(opts));
    opts.working_directory = cwd.empty() ? nullptr : cwd.c_str();
    // 交互式终端要接管 stdin（用户在终端栏里敲命令）；模型终端不需要
    opts.redirect.in.type =
        interactive ? REPROC_REDIRECT_PIPE : REPROC_REDIRECT_DISCARD;
    opts.redirect.out.type = REPROC_REDIRECT_PIPE;     // 捕获 stdout
    opts.redirect.err.type = REPROC_REDIRECT_STDOUT;   // stderr 合并到 stdout
    opts.stop = (reproc_stop_actions)REPROC_STOP_ACTIONS_NULL;
    opts.nonblocking = true;                            // 读不阻塞（poll 语义）

    int rc = reproc_start(h, cargv.data(), opts);
    if (rc < 0) {
      std::string msg = reproc_strerror(rc);
      h = reproc_destroy(h);
      return FsResult::error(ErrCode::kIo, "spawn failed: " + msg);
    }
  }

  // 注册表：用户终端 kUser；模型终端按前台/后台区分
  std::string sid = jsonutil::get<std::string>(args, "sid", "");
  std::string kind_arg = jsonutil::get<std::string>(args, "kind", "");
  TermKind kind = interactive || kind_arg == "user"
                      ? TermKind::kUser
                      : (background ? TermKind::kModelBg : TermKind::kModelFg);
  TermEntry e = reg_->create(sid, kind, sh.name, cwd, encoding, wake_on_exit,
                             clock_->now_ms(), label);
  // Bind the transcript BEFORE the child starts: a foreground command's whole
  // output is drained inside this call, and the sink can only log to a path it
  // knows. (Assigning it afterwards left every model `terminal_exec` run with a
  // record but no log file - the "227 records, one log" session.)
  if (log_path_fn_) {
    std::string lp = log_path_fn_(sid, e.tid);
    if (!lp.empty()) {
      if (TermEntry* saved = reg_->find(e.tid)) {
        saved->log_path = lp;
        saved->log_attached = true;
      }
    }
  }

  std::unique_ptr<Proc> p(new Proc());
  p->h = h;
  p->pty = pty.release();
  p->tid = e.tid;
  p->sid = sid;
  p->owner = this;
  p->enc = encoding;
  p->cols = cols;
  p->rows = rows;
  p->screen.resize(cols, rows);
  std::string tid = e.tid;
  // Foreground commands already get a deadline from reproc_wait below. A
  // background command has no waiter, so only an EXPLICIT timeoutMs starts a
  // clock: an omitted value must mean "unlimited" (a long build must not die
  // because the model did not repeat the 60 s default).
  if (background && !interactive && args.contains("timeoutMs")) {
    p->timeout_ms = timeout_ms > 0 ? timeout_ms : 0;
    p->started_ms = e.started_at;
  }
  procs_[tid] = std::move(p);

  // 前台：等结果（超时终止并返回 timedOut，绝不转后台）
  // 交互式终端永远立即返回：它的生命周期由用户/UI 控制
  if (!background && !interactive) {
    Proc& pr = *procs_[tid];
    pr.fg_wait = true;
    reproc_t* handle = pr.h;
    int wait_ms = static_cast<int>(timeout_ms > 0 ? timeout_ms : REPROC_INFINITE);
    // Release the manager lock for the wait. tick() keeps draining output for
    // the live panel (and skips exit detection while fg_wait is set); other
    // sessions can spawn their own terminals in parallel.
    lk.unlock();
    int w = reproc_wait(handle, wait_ms);
    lk.lock();
    pr.fg_wait = false;
    drain(pr);
    if (w == REPROC_ETIMEDOUT) {
      // A foreground `terminal_exec` timeout is a TIMEOUT, not permission to
      // leave the process running forever. It used to be promoted to background;
      // a command with timeoutMs=60000 then stayed alive for half an hour and the
      // session looked stuck. Kill it, drain what it produced, and mark it exited.
      reproc_terminate(handle);
      int killed = reproc_wait(handle, 2000);
      if (killed == REPROC_ETIMEDOUT) {
        reproc_kill(handle);
        killed = reproc_wait(handle, 2000);
      }
      drain(pr);
      pr.exited = true;
      pr.exit_code = -1;
      reg_->mark_exited(tid, -1, clock_->now_ms());
      Json o = Json::object();
      o["tid"] = tid;
      o["status"] = "exited";
      o["exitCode"] = -1;
      o["output"] = pr.out;
      o["timedOut"] = true;
      o["summary"] = "timed out after " + std::to_string(wait_ms) + " ms (killed)";
      if (out) *out = o;
      return FsResult::success(o);
    }
    pr.exited = true;
    pr.exit_code = w;
    reg_->mark_exited(tid, w, clock_->now_ms());
    maybe_prune_log(tid);
    Json o = Json::object();
    o["tid"] = tid;
    o["status"] = "exited";
    o["output"] = pr.out;
    o["exitCode"] = w;
    if (out) *out = o;
    return FsResult::success(o);
  }

  Json o = Json::object();
  o["tid"] = tid;
  o["status"] = "running";
  o["shell"] = sh.name;
  if (!label.empty()) o["label"] = label;
  o["interactive"] = interactive;
  o["terminal"] = (procs_[tid]->pty != nullptr) ? "conpty" : "pipe";
  o["cols"] = cols;
  o["rows"] = rows;
  if (out) *out = o;
  return FsResult::success(o);
}

// 把用户输入写进终端。text 是 VT 编码后的原始字节（按键序列），不补换行——
// Enter 由调用方编码成 \r。ConPTY 下这就是一个真终端的键盘输入。
FsResult TerminalManager::write(const Json& args, Json* out) {
  std::lock_guard<std::mutex> lk(mu_);
  std::string tid = jsonutil::get<std::string>(args, "tid", "");
  std::string text = jsonutil::get<std::string>(args, "text", "");
  std::map<std::string, std::unique_ptr<Proc> >::iterator it = procs_.find(tid);
  if (it == procs_.end()) {
    return FsResult::error(ErrCode::kNotFound, "terminal not found: " + tid);
  }
  Proc& p = *it->second;
  if (p.exited) {
    return FsResult::error(ErrCode::kInvalidArg, "terminal already exited");
  }
  if (p.pty) {
    // ConPTY 直接吃 UTF-8 字节
    bool ok = p.pty->write(text);
    if (!ok) return FsResult::error(ErrCode::kIo, "write failed");
    Json o = Json::object();
    o["tid"] = tid;
    o["written"] = static_cast<int64_t>(text.size());
    if (out) *out = o;
    return FsResult::success(o);
  }
  if (!p.h) {
    return FsResult::error(ErrCode::kInvalidArg, "terminal has no input");
  }
  // 反向编码：cmd.exe 按 OEM 代码页读管道 stdin，UTF-8 的中文命令要转回去
  std::string bytes = encoding::encode_input(text, p.enc);
  int n = reproc_write(p.h, reinterpret_cast<const uint8_t*>(bytes.data()),
                       bytes.size());
  if (n < 0) {
    return FsResult::error(ErrCode::kIo, std::string("write failed: ") +
                                             reproc_strerror(n));
  }
  Json o = Json::object();
  o["tid"] = tid;
  o["written"] = n;
  if (out) *out = o;
  return FsResult::success(o);
}

// 交互式终端的窗口大小（ConPTY 会把它告诉子进程；全屏程序据此排版）。
FsResult TerminalManager::resize(const Json& args, Json* out) {
  std::lock_guard<std::mutex> lk(mu_);
  std::string tid = jsonutil::get<std::string>(args, "tid", "");
  std::map<std::string, std::unique_ptr<Proc> >::iterator it = procs_.find(tid);
  if (it == procs_.end()) {
    return FsResult::error(ErrCode::kNotFound, "terminal not found: " + tid);
  }
  Proc& p = *it->second;
  int cols = static_cast<int>(jsonutil::get_int(args, "cols", p.cols));
  int rows = static_cast<int>(jsonutil::get_int(args, "rows", p.rows));
  if (cols < 20) cols = 20;
  if (rows < 5) rows = 5;
  if (cols == p.cols && rows == p.rows) return FsResult::success();
  p.cols = cols;
  p.rows = rows;
  p.screen.resize(cols, rows);
  p.screen_dirty = true;
  if (p.pty) p.pty->resize(cols, rows);
  Json o = Json::object();
  o["tid"] = tid;
  o["cols"] = cols;
  o["rows"] = rows;
  if (out) *out = o;
  return FsResult::success(o);
}

FsResult TerminalManager::poll(const Json& args, Json* out) {
  std::lock_guard<std::mutex> lk(mu_);
  std::string tid = jsonutil::get<std::string>(args, "tid", "");
  std::map<std::string, std::unique_ptr<Proc> >::iterator it = procs_.find(tid);
  if (it == procs_.end()) {
    return FsResult::error(ErrCode::kNotFound, "terminal not found: " + tid);
  }
  Proc& p = *it->second;
  drain(p);
  if (!p.exited) {
    if (p.pty) {
      int code = 0;
      if (p.pty->exited(&code)) {
        p.exited = true;
        p.exit_code = code;
        reg_->mark_exited(tid, code, clock_->now_ms());
        maybe_prune_log(tid);
      }
    } else if (p.h) {
      int w = reproc_wait(p.h, 0);  // 非阻塞检查
      if (w != REPROC_ETIMEDOUT) {
        p.exited = true;
        p.exit_code = w;
        reg_->mark_exited(tid, w, clock_->now_ms());
        maybe_prune_log(tid);
      }
    }
  }
  Json o = Json::object();
  o["tid"] = tid;
  o["status"] = p.exited ? "exited" : "running";
  o["output"] = p.out;
  o["exitCode"] = p.exit_code;
  if (out) *out = o;
  return FsResult::success(o);
}

FsResult TerminalManager::kill(const Json& args, Json* out) {
  std::lock_guard<std::mutex> lk(mu_);
  std::string tid = jsonutil::get<std::string>(args, "tid", "");
  std::map<std::string, std::unique_ptr<Proc> >::iterator it = procs_.find(tid);
  if (it == procs_.end()) {
    return FsResult::error(ErrCode::kNotFound, "terminal not found: " + tid);
  }
  Proc& p = *it->second;
  if (p.pty) {
    drain(p);  // 收走最后的输出再关控制台
    p.pty->terminate();
    p.exited = true;
    p.exit_code = 0;
  } else if (p.h && !p.exited) {
    reproc_terminate(p.h);       // 先温和
    int w = reproc_wait(p.h, 2000);
    if (w == REPROC_ETIMEDOUT) {
      reproc_kill(p.h);          // 再强杀
      w = reproc_wait(p.h, 2000);
    }
    drain(p);
    p.exited = true;
    p.exit_code = (w == REPROC_ETIMEDOUT) ? -1 : w;
  }
  reg_->mark_exited(tid, p.exit_code, clock_->now_ms());
  Json o = Json::object();
  o["tid"] = tid;
  o["status"] = "killed";
  o["exitCode"] = p.exit_code;
  if (out) *out = o;
  return FsResult::success(o);
}

// 后台线程非阻塞轮询：把新增输出/退出状态汇总成事件列表交给宿主推送。
// UI 不再需要手动"轮询"按钮——桌面端宿主按回调、浏览器端经 /api/events 推送。
//
// 事件里带的是屏幕网格（按样式合并后的紧凑行），不是原始字节流：escape 序列
// 已经在这一层被解析掉，UI 直接画格子，不重复实现一个终端。
std::vector<Json> TerminalManager::tick() {
  std::lock_guard<std::mutex> lk(mu_);
  std::vector<Json> out;
  for (std::map<std::string, std::unique_ptr<Proc> >::iterator it = procs_.begin();
       it != procs_.end(); ++it) {
    Proc& p = *it->second;
    bool was_exited = p.exited;
    // Background timeout (no waiter exists). terminate first, then kill if the
    // process ignores it; both phases are non-blocking so tick() never parks the
    // terminal manager behind a hung child.
    int64_t now_ms = clock_->now_ms();
    if (!p.exited && !p.fg_wait && p.timeout_ms > 0 && p.started_ms > 0 &&
        now_ms - p.started_ms >= p.timeout_ms) {
      p.timed_out = true;
      if (!p.timeout_sent) {
        p.timeout_sent = true;
        p.kill_deadline_ms = now_ms + 1500;
        if (p.pty) p.pty->terminate();
        if (p.h) reproc_terminate(p.h);
      } else if (!p.hard_killed && p.kill_deadline_ms > 0 &&
                 now_ms >= p.kill_deadline_ms) {
        p.hard_killed = true;
        if (p.pty) p.pty->terminate();
        if (p.h) reproc_kill(p.h);
      }
    }
    size_t before = p.out.size();
    drain(p);
    bool new_out = p.out.size() > before;
    // The foreground waiter owns exit detection/finalization; a tick racing it
    // would mark the registry exited and emit terminal.exit before the tool
    // returns. Output still streams so the terminal panel stays live.
    if (!p.exited && !p.fg_wait) {
      if (p.pty) {
        int code = 0;
        if (p.pty->exited(&code)) {
          p.exited = true;
          p.exit_code = code;
          reg_->mark_exited(p.tid, code, clock_->now_ms());
        }
      } else if (p.h) {
        int w = reproc_wait(p.h, 0);  // 非阻塞检查
        if (w != REPROC_ETIMEDOUT) {
          p.exited = true;
          p.exit_code = w;
          reg_->mark_exited(p.tid, w, clock_->now_ms());
        }
      }
    }
    if (!new_out && p.exited == was_exited) continue;  // 无变化
    Json e = Json::object();
    e["tid"] = p.tid;
    e["sid"] = p.sid;
    // 只发新增文本（供"所有终端"概览/日志）；屏幕网格由 terminal.grid 全量取
    if (new_out) {
      e["output"] = p.out.substr(p.reported);
    }
    p.reported = p.out.size();
    e["status"] = p.exited ? "exited" : "running";
    e["exitCode"] = p.exit_code;
    e["justExited"] = (p.exited && !was_exited);
    if (p.timed_out) e["timedOut"] = true;
    // The UI must know whether this is a terminal the user should see. Without
    // the kind it registered EVERY model `terminal_exec` on its first output
    // chunk as a tab, gave it a number, and counted it in the session badge -
    // then terminal.list (which filters short model commands) returned only the
    // handful of real terminals, so "十几个" appeared to collapse into "几个".
    if (TermEntry* te = reg_->find(p.tid)) {
      e["kind"] = te->kind == TermKind::kUser
                      ? "user"
                      : (te->kind == TermKind::kModelBg ? "model-bg" : "model-fg");
      if (!te->label.empty()) e["label"] = te->label;
    }
    out.push_back(e);
  }
  return out;
}

// 屏幕网格 -> UI。按样式把连续单元格合并成 run，这样一屏通常只有几十个节点，
// 而不是 cols*rows 个。
//
// `from`（可选，默认 0）指定从第几行开始发：UI 要回滚历史时按需请求更早的
// 行，不必每次把 2000 行都传过去。
Json TerminalManager::screen_json(const Proc& p, int from) {
  const int hist = p.screen.scrollback_size();
  const int total = p.screen.total_lines();
  if (from < 0) from = 0;
  if (from >= total) from = total > 0 ? total - 1 : 0;

  Json lines = Json::array();
  for (int y = from; y < total; ++y) {
    Json runs = Json::array();
    int x = 0;
    while (x < p.screen.cols()) {
      const vt::Cell& c = p.screen.line_cell(y, x);
      if (c.flags & vt::kFlagWideTail) {
        ++x;
        continue;
      }
      std::string text;
      int32_t fg = c.fg, bg = c.bg;
      uint8_t attr = c.attr;
      int start = x;
      while (x < p.screen.cols()) {
        const vt::Cell& k = p.screen.line_cell(y, x);
        if (k.flags & vt::kFlagWideTail) {
          ++x;
          continue;
        }
        if (k.fg != fg || k.bg != bg || k.attr != attr) break;
        uint32_t cp = k.ch ? k.ch : ' ';
        // encode as UTF-8
        if (cp < 0x80) {
          text.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
          text.push_back(static_cast<char>(0xC0 | (cp >> 6)));
          text.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
          text.push_back(static_cast<char>(0xE0 | (cp >> 12)));
          text.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
          text.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
          text.push_back(static_cast<char>(0xF0 | (cp >> 18)));
          text.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
          text.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
          text.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        ++x;
      }
      if (start >= p.screen.cols()) break;
      // 整行末尾的空白不渲染（省节点；CSS 里右对齐的东西仍然正确）
      bool blank = true;
      for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != ' ') { blank = false; break; }
      }
      if (blank) continue;
      Json run = Json::object();
      run["x"] = start;
      run["t"] = text;
      if (fg != vt::kColorDefault) run["fg"] = fg;
      if (bg != vt::kColorDefault) run["bg"] = bg;
      if (attr) run["a"] = static_cast<int>(attr);
      runs.push_back(run);
    }
    lines.push_back(runs);
  }
  Json o = Json::object();
  o["cols"] = p.screen.cols();
  o["rows"] = p.screen.rows();
  o["from"] = from;
  o["scrollback"] = hist;
  o["totalLines"] = total;
  o["lines"] = lines;
  o["cursorX"] = p.screen.cursor_x();
  o["cursorY"] = p.screen.cursor_y();
  o["cursorVisible"] = p.screen.cursor_visible() && !p.exited;
  o["alt"] = p.screen.alt_screen();
  // DECCKM: the UI needs it to pick SS3 vs CSI when encoding an arrow key
  o["appKeys"] = p.screen.app_cursor_keys();
  o["bracketedPaste"] = p.screen.bracketed_paste();
  o["title"] = p.screen.title();
  return o;
}

FsResult TerminalManager::grid(const Json& args, Json* out) {
  std::lock_guard<std::mutex> lk(mu_);
  std::string tid = jsonutil::get<std::string>(args, "tid", "");
  std::map<std::string, std::unique_ptr<Proc> >::iterator it = procs_.find(tid);
  if (it == procs_.end()) {
    return FsResult::error(ErrCode::kNotFound, "terminal not found: " + tid);
  }
  Proc& p = *it->second;
  // 取网格前先把子进程还没吐完的输出收干净，UI 看到的才是最新的
  drain(p);
  int from = static_cast<int>(jsonutil::get_int(args, "from", 0));
  // Cap the reply: the UI shows a window of history, and shipping 2000 lines of
  // runs on every 50ms poll would be pure waste.
  const int kMaxLines = 1200;
  int total = p.screen.total_lines();
  if (total - from > kMaxLines) from = total - kMaxLines;
  Json o = screen_json(p, from);
  o["tid"] = tid;
  o["status"] = p.exited ? "exited" : "running";
  o["exitCode"] = p.exit_code;
  if (out) *out = o;
  return FsResult::success(o);
}

// A foreground MODEL command that finished quickly is noise, not a record. Its
// transcript is deleted on exit so the terminal list stays meaningful ("只需要
// 记录标记了的和运行时长非常长的"). Kept: user terminals, background terminals,
// labelled (marked) ones, and anything that ran at least a minute.
void TerminalManager::maybe_prune_log(const std::string& tid) {
  if (!reg_) return;
  const TermEntry* e = reg_->find(tid);
  if (!e || e->log_path.empty()) return;
  bool marked = e->kind == TermKind::kUser || e->kind == TermKind::kModelBg ||
                !e->label.empty();
  if (marked) return;
  const int64_t kKeepMs = 60 * 1000;
  int64_t dur = e->ended_at > e->started_at ? e->ended_at - e->started_at : 0;
  if (dur >= kKeepMs) return;
  std::error_code ec;
  ghc::filesystem::remove(e->log_path, ec);
  reg_->detach_log(tid);
}

}  // namespace fs
