// fs_api.hpp - FsApi 统一入口（arch.md §5 方法表）
// 同进程函数调用；以后整体换 HTTP（UI 与后端不耦合）。
#pragma once

#include <memory>
#include <string>

#include "fs_clock.hpp"
#include "fs_error.hpp"
#include "fs_json.hpp"
#include "fs_random.hpp"
#include "core/deny_list.hpp"
#include "core/lock_arbiter.hpp"
#include "core/plan_store.hpp"
#include "core/project_manager.hpp"
#include "core/session_manager.hpp"
#include "core/settings_store.hpp"
#include "core/shell_detect.hpp"
#include "core/terminal_registry.hpp"
#include "core/usage_store.hpp"
#include "storage/fs_store.hpp"

namespace fs {

// P1 骨架：project/session/terminal-list/usage/settings/plan 全实现；
// tool.*/provider.*/mcp.* 返回 kNotSupported 桩（P2/P3 接真实现）。
class FsApi {
 public:
  // root_dir 可注入（单测用临时目录）；clock/rng 可注入（单测假时钟/固定种子）
  explicit FsApi(const ghc::filesystem::path& root_dir,
                 const Clock* clock = nullptr, Random* rng = nullptr);
  ~FsApi();

  // ----- project.* -----
  FsResult project_list();
  FsResult project_open(const std::string& path);
  FsResult project_create(const std::string& path);
  FsResult project_remove(const std::string& pid);
  FsResult project_get(const std::string& pid);

  // ----- session.* -----
  FsResult session_new(const std::string& pid, const std::string& cwd);
  FsResult session_open(const std::string& sid);
  FsResult session_close(const std::string& sid);
  FsResult session_fork(const std::string& sid, const std::string& turnId);
  FsResult session_rollback(const std::string& sid, const std::string& turnId);
  FsResult session_forget(const std::string& sid, const std::string& scope);
  FsResult session_list(const std::string& pid);
  FsResult session_set_model(const std::string& sid, const std::string& model);
  FsResult session_set_thinking(const std::string& sid, const std::string& thinking);
  FsResult session_set_permission(const std::string& sid, const std::string& perm);
  FsResult session_set_permission_confirm(const std::string& sid, const std::string& perm,
                                          const std::string& token);
  FsResult session_request_permission_token(const std::string& sid,
                                            const std::string& newPerm);
  FsResult session_set_auto_mode(const std::string& sid, bool autoMode);
  FsResult session_set_enter_send(const std::string& sid, const std::string& enterSend);
  FsResult session_refresh_config(const std::string& sid);
  FsResult session_append_turn(const std::string& sid, const Json& turn);
  FsResult session_get(const std::string& sid);
  // P3 接：send/append/suspend/resume/cancelWake
  FsResult session_send(const std::string& sid, const std::string& text,
                        const Json& attachments) {
    (void)sid; (void)text; (void)attachments;
    return FsResult::error(ErrCode::kNotSupported, "agent loop: P3");
  }

  // ----- terminal.*（注册表部分 P1；spawn/exec 实际执行 P3 reproc）-----
  FsResult terminal_list(const std::string& sid);
  FsResult terminal_show(const std::string& tid);
  FsResult terminal_kill(const std::string& tid);
  FsResult terminal_set_encoding(const std::string& tid, const std::string& enc);

  // ----- usage.* -----
  FsResult usage_query(const Json& filter);
  FsResult usage_export_csv(const Json& filter, const std::string& path);
  FsResult usage_record(const Json& row);

  // ----- settings.* -----
  FsResult settings_get(const std::string& key, const std::string& pid);
  FsResult settings_set(const std::string& key, const Json& value, const std::string& pid);
  FsResult settings_unset(const std::string& key, const std::string& pid);
  FsResult settings_dump(const std::string& pid);  // 全量（带来源标注）
  FsResult settings_detect_shells();
  FsResult settings_set_shell(const std::string& name);
  FsResult settings_set_ignore_global(const std::string& pid, bool ignore);

  // ----- plan.* -----
  FsResult plan_get(const std::string& sid);
  FsResult plan_update(const std::string& sid, const Json& planTree);

  // ----- 锁（供 P3 工具层过权限门用；P1 单测直接调）-----
  FsResult lock_acquire(const std::string& sid);  // 按会话权限档申请项目写锁
  FsResult lock_release(const std::string& sid);
  FsResult lock_holder(const std::string& pid);

  // ----- tool.* / provider.* / mcp.*：P1 桩 -----
  FsResult tool_stub(const std::string& name) {
    return FsResult::error(ErrCode::kNotSupported,
                           "tool not implemented until P3: " + name);
  }
  FsResult provider_list_providers();
  FsResult provider_list_models(const std::string& providerId) {
    (void)providerId;
    return FsResult::error(ErrCode::kNotSupported, "provider: P2");
  }
  FsResult provider_test_connection(const std::string& providerId) {
    (void)providerId;
    return FsResult::error(ErrCode::kNotSupported, "provider: P2");
  }
  FsResult mcp_stub() {
    return FsResult::error(ErrCode::kNotSupported, "mcp: P3");
  }

  // 内部组件访问（测试/上层组合用）
  ProjectManager& projects() { return *proj_; }
  SessionManager& sessions() { return *sessions_; }
  SettingsStore& settings() { return *settings_; }
  UsageStore& usage() { return *usage_; }
  LockArbiter& locks() { return *locks_; }
  TerminalRegistry& terminals() { return *terminals_; }
  DenyList& deny_list() { return *deny_list_; }
  PlanStore& plans() { return *plans_; }

 private:
  // 确保某个 pid 的项目级 SettingsStore 已构建（无 pid 用空路径占位 = 只有全局）
  SettingsStore& store_for(const std::string& pid);
  // 项目注册表里查 pid 的路径（未注册返回空串）
  std::string path_for(const std::string& pid) const;

  ghc::filesystem::path root_;
  WallClock default_clock_;
  const Clock* clock_;
  XorShiftRandom default_rng_;
  Random* rng_;

  std::unique_ptr<SettingsStore> settings_;          // 全局（空项目路径）
  std::unique_ptr<SettingsStore> project_settings_;  // 按 pid 懒构建
  std::string project_settings_pid_;
  std::unique_ptr<ProjectManager> proj_;
  std::unique_ptr<SessionManager> sessions_;
  std::unique_ptr<UsageStore> usage_;
  std::unique_ptr<LockArbiter> locks_;
  std::unique_ptr<TerminalRegistry> terminals_;
  std::unique_ptr<DenyList> deny_list_;
  std::unique_ptr<PlanStore> plans_;
};

}  // namespace fs