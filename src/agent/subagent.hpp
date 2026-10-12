// subagent.hpp - 子代理管理（agent-loop.md 6, addon1.md）
// 预置：local-research / complete-step / return / round-plan / check-files
#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "fs_clock.hpp"
#include "fs_error.hpp"
#include "fs_json.hpp"

namespace fs {

// 子代理状态
enum class SubagentStatus {
  kRunning,
  kDone,
  kCancelled,
  kTimeout,
};

const char* subagent_status_str(SubagentStatus s);

// 子代理记录（全文保留在侧栏"子代理会话历史"，不进主会话历史 - addon1.md）
struct SubagentRecord {
  std::string id;
  std::string parent_sid;
  std::string kind;          // 预置代理名 / custom
  std::string task;
  std::string model;         // 空=继承父
  std::string cwd;
  int depth = 1;             // 深度（上限 3）
  SubagentStatus status = SubagentStatus::kRunning;
  int64_t started_at = 0;
  int64_t ended_at = 0;
  std::string summary;       // 回父的 summary（/return 自动执行后只给这个）
  Json full = Json::object();  // 全文（侧栏可查）
  bool timeout_at_ms = false;
  int64_t timeout_ms = 0;

  Json to_json() const;
};

using SubagentDoneFn = std::function<void(const SubagentRecord&)>;

class SubagentManager {
 public:
  struct Options {
    int max_parallel = 4;      // 单会话并行上限（默认 4，设置可调）
    int max_depth = 3;         // 递归深度上限（默认 3，设置可改 - addon1.md）
    int64_t default_timeout_ms = 0;  // 0=不限
  };

  explicit SubagentManager(const Clock* clock = nullptr);

  void set_options(Options opts) { opts_ = opts; }
  const Options& options() const { return opts_; }

  // spawn{task, model|inherit, cwd, permission<=父, timeout, depth+1}
  // permission 由调用方保证 <= 父（P3 v1 在 FsApi 层检查）
  FsResult spawn(const std::string& parent_sid, const std::string& kind,
                 const std::string& task, const std::string& model,
                 const std::string& cwd, int depth, int64_t timeout_ms,
                 std::string* out_id);

  // /return：子代理输出结束后自动执行（只给主代理返回摘要）
  FsResult complete_with_summary(const std::string& id, const std::string& summary,
                                 const Json& full);

  // cancel：取消与超时落库
  FsResult cancel(const std::string& id);

  // steer：向运行中的子代理发送引导（addon2 任务健壮性：打断后续接；
  // 子代理没有轮的概念，只需要当前引导）。文本进入该子代理的引导队列。
  FsResult steer(const std::string& id, const std::string& text);
  // 取出并清空该子代理的引导（子代理执行器在工具调用间隙消费）
  std::string drain_steering(const std::string& id);
  std::vector<std::string> steering_for(const std::string& id) const;

  // 查询
  const SubagentRecord* find(const std::string& id) const;
  std::vector<SubagentRecord> list_for_sid(const std::string& sid) const;
  int running_count(const std::string& sid) const;

  // 超时检查（时钟注入；返回超时的 id 列表）
  std::vector<std::string> check_timeouts(int64_t now_ms);

  void set_done_fn(SubagentDoneFn fn) { done_fn_ = std::move(fn); }

 private:
  Options opts_;
  const Clock* clock_;
  WallClock default_clock_;
  std::map<std::string, SubagentRecord> records_;
  std::map<std::string, std::vector<std::string>> steering_;
  int next_id_ = 1;
  SubagentDoneFn done_fn_;
};

// 预置代理定义（addon1.md 5 个 + research/explore）
struct PresetAgent {
  std::string name;         // 如 local-research
  std::string description;  // 模型可见的说明
  bool parallel = true;     // 是否支持并行（complete-step/round-plan 不支持）
  bool blocking = false;    // 是否阻塞主会话（round-plan 阻塞，主会话直接显示）
  bool read_only = false;   // local-research 只读
  bool enabled = true;      // 用户开关
  std::string model;        // 空=继承
};

// 内置预置表
std::vector<PresetAgent> builtin_preset_agents();

}  // namespace fs