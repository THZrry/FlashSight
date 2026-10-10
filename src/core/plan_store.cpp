// plan_store.cpp - PlanStore (agent-loop.md §7)
#include "plan_store.hpp"

namespace fs {

void PlanStore::set_for_sid(const std::string& sid, const Json& plan) {
  plans_[sid] = plan;
}

Json PlanStore::get_cached(const std::string& sid) const {
  auto it = plans_.find(sid);
  if (it == plans_.end()) return Json();
  return it->second;
}

Json PlanStore::get(const std::string& sid) const { return get_cached(sid); }

Json PlanStore::update(const std::string& sid, const Json& plan_tree) {
  // 简单整体替换
  plans_[sid] = plan_tree;
  return plan_tree;
}

bool PlanStore::del(const std::string& sid) {
  return plans_.erase(sid) == 1;
}

}  // namespace fs