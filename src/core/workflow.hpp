// workflow.hpp - 工作流后端支撑（addon2.md 长任务-工作流）
// 工作流 = 可定义流水线；每步可顺序/并行派子代理处理分模块。
// 后端职责：定义/实例落盘、步骤状态机、图结构数据（侧栏图/嵌套列表跳转用）、
// 健壮性改良（步骤级重试/跳过、失败不整体中断、用户定向引导到某子代理）。
#pragma once

#include <map>
#include <string>
#include <vector>

#include "fs_clock.hpp"
#include "fs_error.hpp"
#include "fs_json.hpp"

namespace fs {

// 步骤状态
enum class StepState {
  kPending,
  kRunning,
  kDone,
  kFailed,
  kSkipped,
};

const char* step_state_str(StepState s);

// 一步中的一个子代理任务
struct WorkflowAgent {
  std::string kind;    // 预置子代理名（local-research 等）
  std::string task;    // 任务描述
  std::string model;   // 空=继承
  std::string subagentId;  // 运行时分配（spawn 后回填）
};

// 流水线一步
struct WorkflowStep {
  std::string id;
  std::string name;
  bool parallel = false;         // true=并行派发本步 agents
  std::vector<WorkflowAgent> agents;
  StepState state = StepState::kPending;
  int attempts = 0;              // 重试次数（健壮性：步骤级重试）
  int64_t started_at = 0;
  int64_t ended_at = 0;
  std::string error;
  std::string guidance;          // 用户定向引导（发给本步子代理）

  Json to_json() const;
};

// 工作流定义
struct WorkflowDef {
  std::string id;
  std::string name;
  std::string description;
  std::vector<WorkflowStep> steps;

  Json to_json() const;
};

// 工作流运行实例
struct WorkflowRun {
  std::string id;
  std::string defId;
  std::string name;
  std::string sid;               // 所属会话
  std::vector<WorkflowStep> steps;
  std::string state = "pending"; // pending|running|done|failed
  int64_t created_at = 0;
  int64_t updated_at = 0;

  Json to_json() const;
  // 图结构（addon2：侧边栏以图和嵌套列表现实并跳转）
  Json graph_json() const;
};

// 工作流存储：定义 + 运行实例（内存 + 可选落盘由上层负责）
class WorkflowStore {
 public:
  explicit WorkflowStore(const Clock* clock = nullptr);

  // 定义
  FsResult define(const Json& def);        // 校验并注册（重名覆盖）
  Json list_defs() const;
  const WorkflowDef* find_def(const std::string& name) const;

  // 运行
  std::string new_run_id();
  WorkflowRun* create_run(const WorkflowDef& def, const std::string& sid);
  WorkflowRun* find_run(const std::string& runId);
  const WorkflowRun* find_run(const std::string& runId) const;
  std::vector<WorkflowRun*> runs_for_sid(const std::string& sid);
  // 按子代理 id 反查所属运行（子代理完成回调推进用）；未找到返回 nullptr
  WorkflowRun* find_run_by_subagent(const std::string& subagentId);
  // 全部运行（落盘/遍历用）
  std::vector<WorkflowRun*> all_runs();

  // 步骤推进（runner 调用）
  bool mark_step_running(const std::string& runId, const std::string& stepId,
                         int64_t now_ms);
  bool mark_step_done(const std::string& runId, const std::string& stepId,
                      int64_t now_ms);
  bool mark_step_failed(const std::string& runId, const std::string& stepId,
                        const std::string& error, int64_t now_ms);
  // 重试：failed -> pending，attempts+1
  bool retry_step(const std::string& runId, const std::string& stepId);
  // 跳过：-> skipped（健壮性：失败不整体中断，可跳步）
  bool skip_step(const std::string& runId, const std::string& stepId);
  // 定向引导
  bool steer_step(const std::string& runId, const std::string& stepId,
                  const std::string& text);
  // 更新整体状态（全 done -> done；有 failed 且无 running -> failed）
  void refresh_run_state(const std::string& runId);

 private:
  const Clock* clock_;
  WallClock default_clock_;
  std::map<std::string, WorkflowDef> defs_;   // name -> def
  std::map<std::string, WorkflowRun> runs_;   // runId -> run
  int next_run_ = 1;
};

// 内置示例工作流定义（供测试与演示）
std::vector<WorkflowDef> builtin_workflows();

}  // namespace fs