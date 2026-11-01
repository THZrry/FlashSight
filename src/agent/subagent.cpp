// subagent.cpp - SubagentManager implementation
#include "subagent.hpp"

#include <algorithm>

#include "fs_serialize.hpp"

namespace fs {

const char* subagent_status_str(SubagentStatus s) {
  switch (s) {
    case SubagentStatus::kRunning: return "running";
    case SubagentStatus::kDone: return "done";
    case SubagentStatus::kCancelled: return "cancelled";
    case SubagentStatus::kTimeout: return "timeout";
  }
  return "running";
}

Json SubagentRecord::to_json() const {
  Json j = Json::object();
  j["id"] = id;
  j["parentSid"] = parent_sid;
  j["kind"] = kind;
  j["task"] = task;
  j["model"] = model;
  j["cwd"] = cwd;
  j["depth"] = depth;
  j["status"] = subagent_status_str(status);
  j["startedAt"] = started_at;
  j["endedAt"] = ended_at;
  j["summary"] = summary;
  // `full` holds the whole transcript, INCLUDING each assistant message's system
  // prompt copy. It is what the side panel renders, but it must not ride along in
  // a LIST: three subagents made `subagent_list` exceed the tool-output cap and
  // the last entry was cut off entirely. Lists carry a size; the transcript is
  // fetched per agent (`subagent.transcript`).
  int messages = 0;
  if (full.is_object() && full.contains("transcript") && full["transcript"].is_array()) {
    messages = static_cast<int>(full["transcript"].size());
  }
  j["transcriptMessages"] = messages;
  return j;
}

// The full record (transcript included) for ONE agent, for the detail view.
Json SubagentRecord::to_json_full() const {
  Json j = to_json();
  j["full"] = full;
  return j;
}

SubagentManager::SubagentManager(const Clock* clock)
    : clock_(clock), default_clock_() {
  if (!clock_) clock_ = &default_clock_;
}

FsResult SubagentManager::spawn(const std::string& parent_sid, const std::string& kind,
                                const std::string& task, const std::string& model,
                                const std::string& cwd, int depth, int64_t timeout_ms,
                                std::string* out_id) {
  if (depth > opts_.max_depth) {
    return FsResult::error(ErrCode::kSubagentDepth, "subagent depth exceeded");
  }
  if (running_count(parent_sid) >= opts_.max_parallel) {
    return FsResult::error(ErrCode::kBusy, "subagent parallel limit reached");
  }
  std::string id = "sub_" + std::to_string(next_id_++);
  SubagentRecord r;
  r.id = id;
  r.parent_sid = parent_sid;
  r.kind = kind;
  r.task = task;
  r.model = model;
  r.cwd = cwd;
  r.depth = depth;
  r.status = SubagentStatus::kRunning;
  r.started_at = clock_->now_ms();
  r.timeout_ms = timeout_ms > 0 ? timeout_ms : opts_.default_timeout_ms;
  records_[id] = r;
  if (out_id) *out_id = id;
  return FsResult::success(Json{{"id", id}});
}

FsResult SubagentManager::complete_with_summary(const std::string& id,
                                               const std::string& summary,
                                               const Json& full) {
  auto it = records_.find(id);
  if (it == records_.end()) {
    return FsResult::error(ErrCode::kNotFound, "subagent not found: " + id);
  }
  it->second.status = SubagentStatus::kDone;
  it->second.ended_at = clock_->now_ms();
  it->second.summary = summary;
  it->second.full = full;
  if (done_fn_) done_fn_(it->second);
  return FsResult::success();
}

FsResult SubagentManager::cancel(const std::string& id) {
  auto it = records_.find(id);
  if (it == records_.end()) {
    return FsResult::error(ErrCode::kNotFound, "subagent not found: " + id);
  }
  // Report whether anything was actually cancelled. Returning a bare {ok:true}
  // for an already-finished agent made "cancel" look like it did something when
  // it was a no-op - and the caller cannot tell the two apart.
  bool was_running = it->second.status == SubagentStatus::kRunning;
  if (!was_running) {
    return FsResult::success(Json{{"id", id}, {"cancelled", false},
                                 {"status", subagent_status_str(it->second.status)},
                                 {"note", "already finished; nothing to cancel"}});
  }
  it->second.status = SubagentStatus::kCancelled;
  it->second.ended_at = clock_->now_ms();
  return FsResult::success(Json{{"id", id}, {"cancelled", true}, {"status", "cancelled"}});
}

const SubagentRecord* SubagentManager::find(const std::string& id) const {
  auto it = records_.find(id);
  return it == records_.end() ? nullptr : &it->second;
}

// steer：向子代理发引导（addon2：子代理只需当前引导，无轮概念）
FsResult SubagentManager::steer(const std::string& id, const std::string& text) {
  auto it = records_.find(id);
  if (it == records_.end()) {
    return FsResult::error(ErrCode::kNotFound, "subagent not found: " + id);
  }
  if (it->second.status != SubagentStatus::kRunning) {
    return FsResult::error(ErrCode::kBusy, "subagent not running: " + id);
  }
  steering_[id].push_back(text);
  return FsResult::success(Json{{"queued", (int)steering_[id].size()}});
}

// Re-open a finished record so a follow-up run can be started for the SAME id.
// The transcript is kept (transcript_for() hands it to the new run) and the
// summary is cleared, because it will be replaced when the follow-up finishes.
FsResult SubagentManager::reopen_for_follow_up(const std::string& id) {
  auto it = records_.find(id);
  if (it == records_.end()) {
    return FsResult::error(ErrCode::kNotFound, "subagent not found: " + id);
  }
  if (it->second.status == SubagentStatus::kRunning) {
    // Already running: a follow-up is just steering it.
    return FsResult::error(ErrCode::kBusy, "subagent still running: " + id);
  }
  it->second.status = SubagentStatus::kRunning;
  it->second.summary.clear();
  it->second.ended_at = 0;
  it->second.started_at = clock_->now_ms();
  return FsResult::success();
}

Json SubagentManager::transcript_for(const std::string& id) const {
  auto it = records_.find(id);
  if (it == records_.end()) return Json::array();
  if (it->second.full.is_object() && it->second.full.contains("transcript")) {
    return it->second.full["transcript"];
  }
  return Json::array();
}

std::string SubagentManager::drain_steering(const std::string& id) {  auto it = steering_.find(id);
  if (it == steering_.end() || it->second.empty()) return "";
  std::string out;
  for (size_t i = 0; i < it->second.size(); ++i) {
    if (i) out += "\n";
    out += it->second[i];
  }
  it->second.clear();
  return out;
}

std::vector<std::string> SubagentManager::steering_for(const std::string& id) const {
  auto it = steering_.find(id);
  return it == steering_.end() ? std::vector<std::string>() : it->second;
}

std::vector<SubagentRecord> SubagentManager::list_for_sid(const std::string& sid) const {
  std::vector<SubagentRecord> out;
  for (auto& kv : records_) {
    if (kv.second.parent_sid == sid) out.push_back(kv.second);
  }
  return out;
}

int SubagentManager::running_count(const std::string& sid) const {
  int n = 0;
  for (auto& kv : records_) {
    if (kv.second.parent_sid == sid && kv.second.status == SubagentStatus::kRunning) ++n;
  }
  return n;
}

std::vector<std::string> SubagentManager::check_timeouts(int64_t now_ms) {
  std::vector<std::string> out;
  for (auto& kv : records_) {
    auto& r = kv.second;
    if (r.status == SubagentStatus::kRunning && r.timeout_ms > 0 &&
        now_ms - r.started_at > r.timeout_ms) {
      r.status = SubagentStatus::kTimeout;
      r.ended_at = now_ms;
      out.push_back(r.id);
    }
  }
  return out;
}

int SubagentManager::prune_finished(const std::string& sid, size_t keep) {
  // Collect this session's finished records, oldest first.
  std::vector<std::pair<int64_t, std::string> > done;
  for (auto& kv : records_) {
    if (kv.second.parent_sid != sid) continue;
    if (kv.second.status == SubagentStatus::kRunning) continue;
    done.push_back(std::make_pair(kv.second.started_at, kv.first));
  }
  if (done.size() <= keep) return 0;
  std::sort(done.begin(), done.end());
  size_t drop = done.size() - keep;
  for (size_t i = 0; i < drop; ++i) {
    records_.erase(done[i].second);
    steering_.erase(done[i].second);
  }
  return static_cast<int>(drop);
}

std::vector<PresetAgent> builtin_preset_agents() {
  std::vector<PresetAgent> v;
  // addon1.md 5 个 + research/explore（agent-loop.md 6）
  v.push_back(PresetAgent{"local-research", "只读检查本地代码库（找 bug、代码分析、文件行数据）",
                          true, false, true, true, ""});
  v.push_back(PresetAgent{"complete-step", "分派完成本轮对话的 step，返回结果+摘要，不支持并行",
                          false, false, false, true, ""});
  v.push_back(PresetAgent{"return", "子代理多轮后返回摘要（输出结束自动执行，不给模型）",
                          true, false, false, true, ""});
  v.push_back(PresetAgent{"round-plan", "本轮规划（阻塞不并行，直接主会话显示，可对接 opencode 惯例）",
                          false, true, false, true, ""});
  v.push_back(PresetAgent{"check-files", "指定文件分析改错（重构用），返回摘要",
                          true, false, false, true, ""});
  v.push_back(PresetAgent{"research", "通用研究（预置）", true, false, false, true, ""});
  v.push_back(PresetAgent{"explore", "通用探索（预置）", true, false, false, true, ""});
  return v;
}

}  // namespace fs