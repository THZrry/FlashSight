// terminal_manager.cpp - reproc 子进程封装实现。
#include "terminal_manager.hpp"

#include <cstring>
#include <vector>

#include <reproc/reproc.h>

namespace fs {

// 一个在跑的进程：reproc 句柄 + 累积输出 + 状态
struct TerminalManager::Proc {
  reproc_t* h = nullptr;
  std::string tid;
  std::string sid;
  std::string out;        // 累积的 stdout+stderr（UTF-8 尽力）
  size_t reported = 0;    // out 中已上报给 UI 的字节数（tick 只发新增）
  bool exited = false;
  int exit_code = 0;
};

TerminalManager::TerminalManager(TerminalRegistry* reg, const Clock* clock)
    : reg_(reg), clock_(clock) {}

TerminalManager::~TerminalManager() {
  for (std::map<std::string, std::unique_ptr<Proc> >::iterator it = procs_.begin();
       it != procs_.end(); ++it) {
    Proc& p = *it->second;
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

void TerminalManager::drain(Proc& p) {
  if (!p.h) return;
  char buf[4096];
  for (;;) {
    int n = reproc_read(p.h, REPROC_STREAM_OUT, reinterpret_cast<uint8_t*>(buf),
                        sizeof(buf));
    if (n > 0) {
      p.out.append(buf, static_cast<size_t>(n));
      if (p.out.size() > (1u << 20)) {  // 上限 1MB，防失控
        p.out.erase(0, p.out.size() - (1u << 20));
      }
      continue;
    }
    // n <= 0：无更多数据 / 管道关闭 / 非阻塞
    break;
  }
}

FsResult TerminalManager::spawn(const Json& args, Json* out) {
  std::string command = jsonutil::get<std::string>(args, "command", "");
  if (command.empty()) {
    return FsResult::error(ErrCode::kInvalidArg, "empty command");
  }
  std::string cwd = jsonutil::get<std::string>(args, "cwd", "");
  bool background = jsonutil::get<bool>(args, "background", true);
  bool wake_on_exit = jsonutil::get<bool>(args, "wakeOnExit", false);
  std::string shell_name = jsonutil::get<std::string>(args, "shell", "");
  std::string encoding = jsonutil::get<std::string>(args, "encoding", "");
  int64_t timeout_ms = jsonutil::get_int(args, "timeoutMs", 60000);

  // shell 解析：显式指定 > 探测最优（cmd 兜底）
  ShellInfo sh = ShellDetector::resolve(shell_name, ShellDetector::detect_all());
  if (sh.path.empty() && sh.name.empty()) {
    return FsResult::error(ErrCode::kNotSupported, "no usable shell found");
  }

  std::vector<std::string> argv = build_argv(sh, command);
  std::vector<const char*> cargv;
  for (size_t i = 0; i < argv.size(); ++i) cargv.push_back(argv[i].c_str());
  cargv.push_back(nullptr);

  reproc_t* h = reproc_new();
  reproc_options opts;
  std::memset(&opts, 0, sizeof(opts));
  opts.working_directory = cwd.empty() ? nullptr : cwd.c_str();
  opts.redirect.in.type = REPROC_REDIRECT_DISCARD;   // 模型终端不需要 stdin
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

  // 注册表：前台 exec 用 model-fg，后台 spawn 用 model-bg
  std::string sid = jsonutil::get<std::string>(args, "sid", "");
  TermKind kind = background ? TermKind::kModelBg : TermKind::kModelFg;
  TermEntry e = reg_->create(sid, kind, sh.name, cwd, encoding, wake_on_exit,
                             clock_->now_ms());

  std::unique_ptr<Proc> p(new Proc());
  p->h = h;
  p->tid = e.tid;
  p->sid = sid;
  std::string tid = e.tid;
  procs_[tid] = std::move(p);

  // 前台：等结果（超时返回已收集输出 + timeout 标记，宿主可转后台）
  if (!background) {
    Proc& pr = *procs_[tid];
    int w = reproc_wait(pr.h, static_cast<int>(timeout_ms > 0 ? timeout_ms : REPROC_INFINITE));
    drain(pr);
    if (w == REPROC_ETIMEDOUT) {
      // 转后台继续跑（terminal.md：前台超时由宿主转后台）
      reg_->promote_to_background(tid, clock_->now_ms());
      Json o = Json::object();
      o["tid"] = tid;
      o["status"] = "running";
      o["output"] = pr.out;
      o["timedOut"] = true;
      if (out) *out = o;
      return FsResult::success(o);
    }
    pr.exited = true;
    pr.exit_code = w;
    reg_->mark_exited(tid, w, clock_->now_ms());
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
  if (out) *out = o;
  return FsResult::success(o);
}

FsResult TerminalManager::poll(const Json& args, Json* out) {
  std::string tid = jsonutil::get<std::string>(args, "tid", "");
  std::map<std::string, std::unique_ptr<Proc> >::iterator it = procs_.find(tid);
  if (it == procs_.end()) {
    return FsResult::error(ErrCode::kNotFound, "terminal not found: " + tid);
  }
  Proc& p = *it->second;
  drain(p);
  if (!p.exited) {
    int w = reproc_wait(p.h, 0);  // 非阻塞检查
    if (w != REPROC_ETIMEDOUT) {
      p.exited = true;
      p.exit_code = w;
      reg_->mark_exited(tid, w, clock_->now_ms());
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
  std::string tid = jsonutil::get<std::string>(args, "tid", "");
  std::map<std::string, std::unique_ptr<Proc> >::iterator it = procs_.find(tid);
  if (it == procs_.end()) {
    return FsResult::error(ErrCode::kNotFound, "terminal not found: " + tid);
  }
  Proc& p = *it->second;
  if (p.h && !p.exited) {
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
std::vector<Json> TerminalManager::tick() {
  std::vector<Json> out;
  for (std::map<std::string, std::unique_ptr<Proc> >::iterator it = procs_.begin();
       it != procs_.end(); ++it) {
    Proc& p = *it->second;
    if (!p.h) continue;
    bool was_exited = p.exited;
    size_t before = p.out.size();
    drain(p);
    bool new_out = p.out.size() > before;
    if (!p.exited) {
      int w = reproc_wait(p.h, 0);  // 非阻塞检查
      if (w != REPROC_ETIMEDOUT) {
        p.exited = true;
        p.exit_code = w;
        reg_->mark_exited(p.tid, w, clock_->now_ms());
      }
    }
    if (!new_out && p.exited == was_exited) continue;  // 无变化
    Json e = Json::object();
    e["tid"] = p.tid;
    e["sid"] = p.sid;
    // 只发新增字节；累积内容仍可由 terminal.show 全量取
    if (new_out) {
      e["output"] = p.out.substr(p.reported);
    }
    p.reported = p.out.size();
    e["status"] = p.exited ? "exited" : "running";
    e["exitCode"] = p.exit_code;
    e["justExited"] = (p.exited && !was_exited);
    out.push_back(e);
  }
  return out;
}

}  // namespace fs
