// terminal_registry.hpp - TerminalRegistry (terminal.md §2)
#pragma once

#include <map>
#include <string>
#include <vector>

#include "fs_clock.hpp"
#include "fs_error.hpp"
#include "fs_json.hpp"
#include "fs_types.hpp"

namespace fs {

// 终端注册表条目（terminal.md §2）
struct TermEntry {
  std::string tid;
  std::string sid;
  TermKind kind = TermKind::kUser;
  std::string shell;          // 探测/指定的 shell 名
  std::string label;          // 用户起的名字（可选；用户终端用，模型终端为空）
  std::string cwd;
  std::string encoding;       // 空=系统默认
  TermStatus status = TermStatus::kRunning;
  int exit_code = 0;
  int64_t started_at = 0;
  int64_t ended_at = 0;
  bool log_attached = false;
  std::string log_path;
  bool wake_on_exit = false;

  Json to_json() const;
  static TermEntry from_json(const Json& j);
};

class TerminalRegistry {
 public:
  TerminalRegistry(const Clock* clock = nullptr);

  // 创建：tid 自增（同项目内统一序列；用户与模型同一编号）
  TermEntry create(const std::string& sid, TermKind kind, const std::string& shell,
                   const std::string& cwd, const std::string& encoding,
                   bool wake_on_exit, int64_t now_ms,
                   const std::string& label = std::string());

  // 状态更新
  FsResult mark_exited(const std::string& tid, int exit_code, int64_t now_ms);

  // 查询
  TermEntry* find(const std::string& tid);
  const TermEntry* find(const std::string& tid) const;
  std::vector<TermEntry> list_for_session(const std::string& sid) const;
  // 把上一次运行留下的终端记录装回注册表（进程已不在 → status=exited）。
  // 同 id 的活动终端不会被覆盖，返回 false。
  bool add_saved(const Json& entry);

  // 前台超 5min 转后台（时钟注入）
  FsResult promote_to_background(const std::string& tid, int64_t now_ms);

  // 编码切换
  FsResult set_encoding(const std::string& tid, const std::string& enc);

  // 改用户给的名字（终端跑着也能改）
  FsResult set_label(const std::string& tid, const std::string& label);

  size_t count_running(const std::string& sid) const;
  size_t total_for_session(const std::string& sid) const;

 private:
  const Clock* clock_;
  WallClock default_clock_;
  int64_t next_tid_ = 1;
  std::map<std::string, TermEntry> entries_;  // tid → entry
};

}  // namespace fs