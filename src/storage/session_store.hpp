// session_store.hpp - session files read/write (full.json, runtime.json, patches/)
#pragma once

#include <string>
#include <vector>

#include <ghc/filesystem.hpp>

#include "fs_error.hpp"
#include "fs_json.hpp"
#include "fs_store.hpp"

namespace fs {

// full.json 按 session-history.md §2 schema 追加；常规追加+回滚裁剪都走原子写
class SessionFileStore {
 public:
  explicit SessionFileStore(const ProjectStore& ps) : ps_(ps) {}

  // 建会话目录 + 初始 full.json（meta + sessionSettings + 空 turns/systemOps/snapshots）
  FsResult create(const std::string& sid, const Json& meta,
                  const Json& session_settings);

  // 读 full.json（宽容：缺失/坏 parse 返回 kNotFound / kParse）。
  // Sharded: the returned object always carries a complete `turns` array (the
  // shards are read and concatenated), so every caller sees the same shape it
  // always did.
  FsResult load_full(const std::string& sid, Json* out) const;
  // Write a full.json-shaped object back: `turns` goes to the shards, everything
  // else to full.json. Used by the paths that replace the whole history
  // (rollback, fork, forget).
  bool save_full(const std::string& sid, const Json& full, std::string* err) const;
  // 是否存在
  bool exists(const std::string& sid) const;
  bool full_exists(const std::string& sid) const;

  // 追加一个 turn（读-改-写原子）
  FsResult append_turn(const std::string& sid, const Json& turn);
  // 追加 systemOps 条目
  FsResult append_system_op(const std::string& sid, const Json& op);
  // 回滚：裁剪到只保留前 keep 个 turn（并同步裁剪 snapshots），原子写
  FsResult trim_turns(const std::string& sid, size_t keep);

  // 删除整个会话目录（full.json + patches/ + terminals/）。不可恢复，所以由调用
  // 方负责先确认；这里只做文件操作。
  FsResult remove(const std::string& sid);

  // ----- patches/<turnId>.json -----
  FsResult write_patch(const std::string& sid, const std::string& turn_id,
                       const Json& patch);
  FsResult read_patch(const std::string& sid, const std::string& turn_id,
                      Json* out) const;
  std::vector<std::string> list_patches(const std::string& sid) const;

  // ----- runtime.json（HistoryBuilder 缓存，可删可重建）-----
  FsResult write_runtime(const std::string& sid, const Json& runtime);
  FsResult read_runtime(const std::string& sid, Json* out) const;

  // ----- terminals/<tid>.log -----
  ghc::filesystem::path terminal_log_path(const std::string& sid,
                                          const std::string& tid) const;
  // 列出某会话的终端日志文件（已有日志才列出；缺失不报错）
  std::vector<std::string> list_terminal_logs(const std::string& sid) const;

  // ----- seen.at：该会话最后一次被用户查看的时刻 -----
  // A tiny side file, NOT a field of full.json: marking a session seen must not
  // touch full.json's mtime, because the status dot compares that mtime with
  // this timestamp ("anything newer than when you last looked").
  bool mark_seen(const std::string& sid, int64_t ms) const;
  int64_t read_seen_ms(const std::string& sid) const;
  // The task list (todo) of this session, persisted as its own small file so a
  // reopen/restart starts from the checklist the task already had.
  ghc::filesystem::path todos_path(const std::string& sid) const;

 private:
  ProjectStore ps_;
  ghc::filesystem::path full_path(const std::string& sid) const {
    return ps_.full_json(sid);
  }
};

// full.json 默认初始 sessionSettings（新建会话缺省值）
Json default_session_settings(const Json& defaults);

}  // namespace fs