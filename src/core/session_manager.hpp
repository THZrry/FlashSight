// session_manager.hpp - SessionManager (10-appcore)
#pragma once

#include <map>
#include <string>
#include <vector>

#include <ghc/filesystem.hpp>

#include "fs_clock.hpp"
#include "fs_error.hpp"
#include "fs_json.hpp"
#include "fs_random.hpp"
#include "lock_arbiter.hpp"
#include "project_manager.hpp"
#include "settings_store.hpp"
#include "storage/fs_patch.hpp"
#include "storage/fs_store.hpp"
#include "storage/session_store.hpp"

namespace fs {

// 会话管理：会话固化设置、turn 落库、回滚/分叉/遗忘、refreshConfig。
// 无 toolUse 模型硬拒绝进会话；切换会话即切换会话设置。
class SessionManager {
 public:
  SessionManager(ProjectManager* proj, const Clock* clock, Random* rng)
      : proj_(proj), clock_(clock), rng_(rng), locks_(clock) {}

  // ----- session CRUD -----
  FsResult new_session(const std::string& pid, const std::string& cwd);
  // open(sid)：打开并刷新 lastSession；返回 full.json
  FsResult open_session(const std::string& sid);
  FsResult get_session(const std::string& sid) const;
  FsResult list_sessions(const std::string& pid) const;
  FsResult close_session(const std::string& sid);
  // 删除会话（连同 patches/terminals 一起移除）。不可恢复：UI 负责先确认。
  // 会先释放锁，否则删完还留着一条锁记录，下次同名 sid 会被误判为占用。
  FsResult delete_session(const std::string& sid);
  // 重命名会话（写 meta.title；列表页读的就是它）
  FsResult rename_session(const std::string& sid, const std::string& title);
  // 重命名并标记为用户设置（自动标题不再覆盖）
  FsResult rename_session_by_user(const std::string& sid, const std::string& title);
  // Set arbitrary meta flags (titleSummarized, ...). Used by the automatic title
  // upkeep so it does not repeat the summary call on every turn.
  FsResult set_meta_flag(const std::string& sid, const Json& fields);

  // ----- 会话设置（即时生效 + 记 systemOps）-----
  FsResult set_model(const std::string& sid, const std::string& model);
  FsResult set_thinking(const std::string& sid, const std::string& thinking);
  FsResult set_permission(const std::string& sid, const std::string& perm);
  FsResult set_permission_confirm(const std::string& sid, const std::string& perm,
                                  const std::string& token);
  FsResult set_auto_mode(const std::string& sid, bool auto_mode);
  FsResult set_enter_send(const std::string& sid, const std::string& enter_send);

  // 低→高权限切换令牌（后端发令牌，UI 确认）
  std::string request_permission_change_token(const std::string& sid,
                                             const std::string& new_perm);

  FsResult refresh_config(const std::string& sid);

  // ----- turn 落库 -----
  FsResult append_turn(const std::string& sid, const Json& turn);
  FsResult append_system_op(const std::string& sid, const std::string& op,
                            const Json& detail);
  // Crash-safe snapshot of the turn in flight: written every ~2s while a turn
  // runs, cleared when it lands. A crashed process (the webview crash) would
  // otherwise lose the whole answer, because a turn is only appended to the
  // session when it FINISHES - which is what the user hit.
  void write_live_snapshot(const std::string& sid, const Json& live);
  // Read it back (empty when absent). `session.liveTurn` merges it with the
  // in-memory state so a reopened tab shows a crashed turn too.
  Json read_live_snapshot(const std::string& sid) const;

  // ----- 子代理记录落盘 -----
  // A subagent's record (task + summary + transcript) used to live ONLY in the
  // in-memory SubagentManager, so after a restart every "打开记录" button in the
  // transcript pointed at nothing - "子代理还是无法在历史记录里打开（也有可能是
  // 旧的历史记录没存）". Written to <session>/subagents/<id>.json next to the turns,
  // so it shares the session's lifetime and survives a restart.
  void write_subagent_record(const std::string& sid, const std::string& id,
                             const Json& record) const;
  // Empty object when absent.
  Json read_subagent_record(const std::string& sid, const std::string& id) const;

  // ----- 回滚 / 分叉 / 遗忘 -----
  FsResult rollback(const std::string& sid, const std::string& turn_id,
                    std::vector<std::string>* reverse_errors);
  // Drop this turn and everything after it from the TRANSCRIPT ONLY - the file
  // patches are left alone. Used by "edit the last message and resend": editing
  // what you asked must not undo the work that was already done.
  FsResult trim_history(const std::string& sid, const std::string& turn_id);
  FsResult fork(const std::string& sid, const std::string& turn_id);
  FsResult forget(const std::string& sid, const std::string& scope);  // "all" | turnId

  // 便捷访问
  std::string pid_of(const std::string& sid) const;
  FsResult load_full(const std::string& sid, Json* out) const;
  // full.json 的最后修改时间（ms）；0 = 取不到。用作"会话最后活动时间"：
  // 每次 append_turn 都会重写 full.json，所以 mtime 就是最后活动时刻，
  // 不需要额外在 meta 里维护一个 updatedAt 字段。
  int64_t full_mtime_ms(const std::string& sid) const;
  SessionFileStore make_store(const std::string& sid) const;
  ProjectStore project_store_of(const std::string& sid) const;

 private:
  ProjectManager* proj_;
  const Clock* clock_;
  Random* rng_;
  LockArbiter locks_;
  // 会话 → 项目 pid 缓存（未命中时会回查项目注册表并在此登记）
  mutable std::map<std::string, std::string> sid_pid_;
  // "记忆上次"：最近一次会话里用户手动选过的自动挡/权限。只在没有显式默认
  // 时用于新会话。写在 settings 的 session.last.autoMode / session.last.filePerm。
  void remember_last_session_choice(const std::string& sid);
  // 权限升级确认令牌
  std::map<std::string, std::string> upgrade_tokens_;

  std::string gen_sid_for(const std::string& pid);
  bool valid_sid(const std::string& sid) const;
  FsResult load_full_checked(const std::string& sid, Json* out) const;
  FsResult update_session_settings(const std::string& sid, const Json& fields);
};

}  // namespace fs