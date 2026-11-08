// plan_store.hpp - the PLAN as a DOCUMENT (addon2 长任务-计划)
//
// Three layers, named by the user (batch 50):
//   * 任务列表 todo   - the lightest process: the tasks of this turn, ticked off;
//   * 规划 plan       - ONE markdown document the model writes and keeps true:
//                       .flashsight/plans/<sid>.md. Each round the model derives
//                       the next todo list from it, and marks the document
//                       complete when the work is. Auto mode follows it.
//   * 工作流 workflow - the heaviest: a pipeline of subagent steps.
//
// The store therefore holds no step tree. It reads/writes the document and
// parses the one machine-readable fact the loop needs: its STATUS.
#pragma once

#include <functional>
#include <map>
#include <string>

#include <ghc/filesystem.hpp>

#include "fs_error.hpp"
#include "fs_json.hpp"

namespace fs {

class PlanStore {
 public:
  // Where this session's document lives. Injected by FsApi (it owns the
  // session -> project mapping); tests set a temp path.
  typedef std::function<ghc::filesystem::path(const std::string& sid)> PathFn;
  PlanStore() = default;

  void set_path_fn(PathFn fn) { path_fn_ = std::move(fn); }

  // {exists, path, status, content, updatedAt, seq, turnId, plans:[...]}
  // status: "active" | "done" | "" (no document).
  Json get(const std::string& sid) const;

  // Replace the document body. `status` empty = keep the current status.
  // `new_plan` starts the next numbered plan; `turn_id` is recorded in the
  // header so a plan can be traced to the turn that wrote it.
  FsResult update(const std::string& sid, const std::string& content,
                  const std::string& status, const std::string& turn_id = "",
                  bool new_plan = false);

  // Ordered plan documents of this session (oldest first).
  Json list(const std::string& sid) const;

  bool del(const std::string& sid);

  // Review is still a separate fact (addon2: approve / reject / revise + a
  // comment). It no longer renders a panel; the verdict is returned to the
  // caller and can be shown as a message/toast.
  Json review(const std::string& sid, const std::string& verdict,
              const std::string& comment, int64_t now_ms);

  // The parsed status of a document body ("done"/"active"/"").
  static std::string parse_status(const std::string& text);

 private:
  PathFn path_fn_;
  std::map<std::string, Json> reviews_;  // sid -> {verdict, comment, at}
};

}  // namespace fs
