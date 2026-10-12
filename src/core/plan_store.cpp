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
  reviews_.erase(sid);
  return plans_.erase(sid) == 1;
}

// 计划评审（addon2）：approve/reject/revise + 评价建议；评价随 plan 返回
Json PlanStore::review(const std::string& sid, const std::string& verdict,
                       const std::string& comment, int64_t now_ms) {
  Json r = Json::object();
  r["verdict"] = verdict;
  r["comment"] = comment;
  r["at"] = now_ms;
  reviews_[sid] = r;
  Json plan = plans_.count(sid) ? plans_[sid] : Json::object();
  if (!plan.is_object()) plan = Json::object();
  plan["review"] = r;
  // revise：保留原计划待微调；reject：清掉计划让模型重新规划
  if (verdict == "reject") {
    plans_.erase(sid);
  } else {
    plans_[sid] = plan;
  }
  return plan;
}

}  // namespace fs