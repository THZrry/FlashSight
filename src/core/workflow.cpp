// workflow.cpp - 工作流后端实现（addon2.md 长任务-工作流）
#include "workflow.hpp"

namespace fs {

const char* step_state_str(StepState s) {
  switch (s) {
    case StepState::kPending: return "pending";
    case StepState::kRunning: return "running";
    case StepState::kDone: return "done";
    case StepState::kFailed: return "failed";
    case StepState::kSkipped: return "skipped";
  }
  return "pending";
}

Json WorkflowStep::to_json() const {
  Json j = Json::object();
  j["id"] = id;
  j["name"] = name;
  j["parallel"] = parallel;
  j["state"] = step_state_str(state);
  j["attempts"] = attempts;
  j["startedAt"] = started_at;
  j["endedAt"] = ended_at;
  j["error"] = error;
  j["guidance"] = guidance;
  Json ags = Json::array();
  for (auto& a : agents) {
    Json aj = Json::object();
    aj["kind"] = a.kind;
    aj["task"] = a.task;
    aj["model"] = a.model;
    aj["subagentId"] = a.subagentId;
    ags.push_back(aj);
  }
  j["agents"] = ags;
  return j;
}

Json WorkflowDef::to_json() const {
  Json j = Json::object();
  j["id"] = id;
  j["name"] = name;
  j["description"] = description;
  Json ss = Json::array();
  for (auto& s : steps) ss.push_back(s.to_json());
  j["steps"] = ss;
  return j;
}

Json WorkflowRun::to_json() const {
  Json j = Json::object();
  j["id"] = id;
  j["defId"] = defId;
  j["name"] = name;
  j["sid"] = sid;
  j["state"] = state;
  j["createdAt"] = created_at;
  j["updatedAt"] = updated_at;
  Json ss = Json::array();
  for (auto& s : steps) ss.push_back(s.to_json());
  j["steps"] = ss;
  return j;
}

// 图结构：节点 = 步骤 + 其子代理；边 = 顺序依赖 + 步骤到子代理的包含关系
Json WorkflowRun::graph_json() const {
  Json nodes = Json::array();
  Json edges = Json::array();
  std::string prev_step;
  for (size_t i = 0; i < steps.size(); ++i) {
    const WorkflowStep& s = steps[i];
    Json n = Json::object();
    n["id"] = s.id;
    n["type"] = "step";
    n["label"] = s.name;
    n["state"] = step_state_str(s.state);
    n["parallel"] = s.parallel;
    n["index"] = (int)i;
    nodes.push_back(n);
    // 顺序边：上一步 -> 本步（嵌套列表/图跳转用）
    if (!prev_step.empty()) {
      edges.push_back(Json{{"from", prev_step}, {"to", s.id}, {"kind", "sequence"}});
    }
    prev_step = s.id;
    for (size_t k = 0; k < s.agents.size(); ++k) {
      const WorkflowAgent& a = s.agents[k];
      Json an = Json::object();
      an["id"] = s.id + "#" + std::to_string(k);
      an["type"] = "agent";
      an["label"] = a.kind;
      an["task"] = a.task;
      an["subagentId"] = a.subagentId;
      an["parent"] = s.id;
      an["state"] = step_state_str(s.state);
      nodes.push_back(an);
      edges.push_back(Json{{"from", s.id},
                           {"to", s.id + "#" + std::to_string(k)},
                           {"kind", "contains"}});
    }
  }
  Json g = Json::object();
  g["nodes"] = nodes;
  g["edges"] = edges;
  g["runId"] = id;
  g["state"] = state;
  return g;
}

WorkflowStore::WorkflowStore(const Clock* clock) : clock_(clock), default_clock_() {
  if (!clock_) clock_ = &default_clock_;
}

FsResult WorkflowStore::define(const Json& def) {
  if (!def.is_object()) return FsResult::error(ErrCode::kInvalidArg, "def must be object");
  WorkflowDef d;
  d.id = jsonutil::get<std::string>(def, "id", "");
  d.name = jsonutil::get<std::string>(def, "name", "");
  d.description = jsonutil::get<std::string>(def, "description", "");
  if (d.name.empty()) return FsResult::error(ErrCode::kInvalidArg, "def.name required");
  if (d.id.empty()) d.id = "wf_" + d.name;
  Json steps = def.value("steps", Json::array());
  if (!steps.is_array()) return FsResult::error(ErrCode::kInvalidArg, "def.steps must be array");
  int idx = 0;
  for (auto& sj : steps) {
    WorkflowStep s;
    s.id = jsonutil::get<std::string>(sj, "id", "");
    if (s.id.empty()) s.id = "step_" + std::to_string(++idx);
    s.name = jsonutil::get<std::string>(sj, "name", s.id);
    s.parallel = jsonutil::get<bool>(sj, "parallel", false);
    Json ags = sj.value("agents", Json::array());
    if (ags.is_array()) {
      for (auto& aj : ags) {
        WorkflowAgent a;
        a.kind = jsonutil::get<std::string>(aj, "kind", "");
        a.task = jsonutil::get<std::string>(aj, "task", "");
        a.model = jsonutil::get<std::string>(aj, "model", "");
        if (!a.kind.empty()) s.agents.push_back(a);
      }
    }
    d.steps.push_back(s);
  }
  defs_[d.name] = d;
  return FsResult::success(d.to_json());
}

Json WorkflowStore::list_defs() const {
  Json arr = Json::array();
  for (auto& kv : defs_) arr.push_back(kv.second.to_json());
  return arr;
}

const WorkflowDef* WorkflowStore::find_def(const std::string& name) const {
  auto it = defs_.find(name);
  return it == defs_.end() ? nullptr : &it->second;
}

std::string WorkflowStore::new_run_id() { return "run_" + std::to_string(next_run_++); }

WorkflowRun* WorkflowStore::create_run(const WorkflowDef& def, const std::string& sid) {
  WorkflowRun r;
  r.id = new_run_id();
  r.defId = def.id;
  r.name = def.name;
  r.sid = sid;
  r.steps = def.steps;  // 复制定义（含 agents），运行时改状态
  r.state = "running";
  r.created_at = clock_->now_ms();
  r.updated_at = r.created_at;
  runs_[r.id] = r;
  return &runs_[r.id];
}

WorkflowRun* WorkflowStore::find_run(const std::string& runId) {
  auto it = runs_.find(runId);
  return it == runs_.end() ? nullptr : &it->second;
}

const WorkflowRun* WorkflowStore::find_run(const std::string& runId) const {
  auto it = runs_.find(runId);
  return it == runs_.end() ? nullptr : &it->second;
}

std::vector<WorkflowRun*> WorkflowStore::runs_for_sid(const std::string& sid) {
  std::vector<WorkflowRun*> v;
  for (auto& kv : runs_) {
    if (kv.second.sid == sid) v.push_back(&kv.second);
  }
  return v;
}

WorkflowRun* WorkflowStore::find_run_by_subagent(const std::string& subagentId) {
  for (auto& kv : runs_) {
    for (auto& s : kv.second.steps) {
      for (auto& a : s.agents) {
        if (a.subagentId == subagentId) return &kv.second;
      }
    }
  }
  return nullptr;
}

std::vector<WorkflowRun*> WorkflowStore::all_runs() {
  std::vector<WorkflowRun*> v;
  for (auto& kv : runs_) v.push_back(&kv.second);
  return v;
}

namespace {

WorkflowStep* find_step(WorkflowRun* r, const std::string& stepId) {
  if (!r) return nullptr;
  for (auto& s : r->steps) {
    if (s.id == stepId) return &s;
  }
  return nullptr;
}

}  // namespace

bool WorkflowStore::mark_step_running(const std::string& runId,
                                      const std::string& stepId, int64_t now_ms) {
  WorkflowRun* r = find_run(runId);
  WorkflowStep* s = find_step(r, stepId);
  if (!s) return false;
  s->state = StepState::kRunning;
  s->started_at = now_ms;
  r->updated_at = now_ms;
  return true;
}

bool WorkflowStore::mark_step_done(const std::string& runId,
                                   const std::string& stepId, int64_t now_ms) {
  WorkflowRun* r = find_run(runId);
  WorkflowStep* s = find_step(r, stepId);
  if (!s) return false;
  s->state = StepState::kDone;
  s->ended_at = now_ms;
  r->updated_at = now_ms;
  refresh_run_state(runId);
  return true;
}

bool WorkflowStore::mark_step_failed(const std::string& runId,
                                     const std::string& stepId,
                                     const std::string& error, int64_t now_ms) {
  WorkflowRun* r = find_run(runId);
  WorkflowStep* s = find_step(r, stepId);
  if (!s) return false;
  s->state = StepState::kFailed;
  s->error = error;
  s->ended_at = now_ms;
  r->updated_at = now_ms;
  refresh_run_state(runId);
  return true;
}

bool WorkflowStore::retry_step(const std::string& runId, const std::string& stepId) {
  WorkflowRun* r = find_run(runId);
  WorkflowStep* s = find_step(r, stepId);
  if (!s) return false;
  if (s->state != StepState::kFailed) return false;
  s->state = StepState::kPending;
  s->attempts += 1;
  s->error.clear();
  s->ended_at = 0;
  r->updated_at = clock_->now_ms();
  refresh_run_state(runId);
  return true;
}

bool WorkflowStore::skip_step(const std::string& runId, const std::string& stepId) {
  WorkflowRun* r = find_run(runId);
  WorkflowStep* s = find_step(r, stepId);
  if (!s) return false;
  s->state = StepState::kSkipped;
  s->ended_at = clock_->now_ms();
  r->updated_at = s->ended_at;
  refresh_run_state(runId);
  return true;
}

bool WorkflowStore::steer_step(const std::string& runId, const std::string& stepId,
                               const std::string& text) {
  WorkflowRun* r = find_run(runId);
  WorkflowStep* s = find_step(r, stepId);
  if (!s) return false;
  s->guidance = text;
  r->updated_at = clock_->now_ms();
  return true;
}

void WorkflowStore::refresh_run_state(const std::string& runId) {
  WorkflowRun* r = find_run(runId);
  if (!r) return;
  bool any_running = false, any_failed = false, any_pending = false;
  for (auto& s : r->steps) {
    if (s.state == StepState::kRunning) any_running = true;
    if (s.state == StepState::kFailed) any_failed = true;
    if (s.state == StepState::kPending) any_pending = true;
  }
  if (any_running) {
    r->state = "running";
  } else if (any_failed) {
    // 失败且无运行中：整个运行标记 failed（后续 pending 步骤无法推进；
    // 用户可 retry_step 或 skip_step 后继续）
    r->state = "failed";
  } else if (any_pending) {
    r->state = "running";
  } else {
    r->state = "done";
  }
}

std::vector<WorkflowDef> builtin_workflows() {
  std::vector<WorkflowDef> v;
  WorkflowDef d;
  d.id = "wf_research_implement";
  d.name = "research-implement";
  d.description = "研究->实现->校验 三步流水线（示例）";
  {
    WorkflowStep s;
    s.id = "step_research";
    s.name = "研究";
    s.parallel = false;
    s.agents.push_back(WorkflowAgent{"local-research", "调研相关代码与方案", "", ""});
    d.steps.push_back(s);
  }
  {
    WorkflowStep s;
    s.id = "step_implement";
    s.name = "实现";
    s.parallel = true;
    s.agents.push_back(WorkflowAgent{"complete-step", "实现模块 A", "", ""});
    s.agents.push_back(WorkflowAgent{"complete-step", "实现模块 B", "", ""});
    d.steps.push_back(s);
  }
  {
    WorkflowStep s;
    s.id = "step_check";
    s.name = "校验";
    s.parallel = false;
    s.agents.push_back(WorkflowAgent{"check-files", "分析改动并改错", "", ""});
    d.steps.push_back(s);
  }
  v.push_back(d);
  return v;
}

}  // namespace fs