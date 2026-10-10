// plan_store.hpp - PlanStore (agent-loop.md §7)
#pragma once

#include <map>
#include <string>

#include <ghc/filesystem.hpp>

#include "fs_error.hpp"
#include "fs_json.hpp"

namespace fs {

// 规划树（PlanStore）：goal → subtargets → status；每 turn planDelta 落库。
// full.json 里直接落位 plan 字段（会话级），用 map<sid, plan>，内存缓存。
class PlanStore {
 public:
  explicit PlanStore() = default;

  // full.json 里存取 plan（会话级）
  Json get(const std::string& sid) const;
  Json update(const std::string& sid, const Json& plan_tree);
  // plan delta 记为一行 → 覆盖整棵树（v1 规划树整体替换；delta 合并简单合并）
  bool del(const std::string& sid);

  // 内存管理（由 SessionManager 读写 full.json 时经外部喂入）
  void set_for_sid(const std::string& sid, const Json& plan);
  Json get_cached(const std::string& sid) const;

 private:
  std::map<std::string, Json> plans_;
};

}  // namespace fs