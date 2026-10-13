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

  // ----- 回滚 / 分叉 / 遗忘 -----
  FsResult rollback(const std::string& sid, const std::string& turn_id,
                    std::vector<std::string>* reverse_errors);
  FsResult fork(const std::string& sid, const std::string& turn_id);
  FsResult forget(const std::string& sid, const std::string& scope);  // "all" | turnId

  // 便捷访问
  std::string pid_of(const std::string& sid) const;
  FsResult load_full(const std::string& sid, Json* out) const;
  SessionFileStore make_store(const std::string& sid) const;
  ProjectStore project_store_of(const std::string& sid) const;

 private:
  ProjectManager* proj_;
  const Clock* clock_;
  Random* rng_;
  LockArbiter locks_;
  // 会话 → 项目 pid 缓存（未命中时会回查项目注册表并在此登记）
  mutable std::map<std::string, std::string> sid_pid_;
  // 权限升级确认令牌
  std::map<std::string, std::string> upgrade_tokens_;

  std::string gen_sid_for(const std::string& pid);
  bool valid_sid(const std::string& sid) const;
  FsResult load_full_checked(const std::string& sid, Json* out) const;
  FsResult update_session_settings(const std::string& sid, const Json& fields);
};

}  // namespace fs