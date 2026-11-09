// steering_queue.hpp - 引导队列（addon1.md 中途引导）
// 分两种：本轮结束后传递（round_end）与当前传递（immediate，当前工具调用完成后输入）。
#pragma once

#include <string>
#include <vector>

#include "fs_json.hpp"

namespace fs {

enum class SteeringKind {
  kRoundEnd,    // 本轮结束后传递（需显式表达，与用户下一轮输入区分）
  kImmediate,   // 当前传递（工具调用完成后输入，不需要显式）
};

const char* steering_kind_str(SteeringKind k);

// Hard cap. The queue is rendered above the composer and drained into the next
// model call; without a bound a model that keeps talking plus a user who keeps
// typing grows it until the queue pushes the composer off the window.
constexpr size_t kMaxSteeringItems = 20;

// 队列条目（支持编辑/排序/删除；auto 模式每轮可加备注）
struct SteeringItem {
  int id = 0;
  SteeringKind kind = SteeringKind::kImmediate;
  std::string text;
};

class SteeringQueue {
 public:
  // 加入（用户手动"引导当前"放入当前队列）
  int push(SteeringKind kind, const std::string& text);
  // 编辑（原位）
  bool edit(int id, const std::string& text);
  // 删除
  bool erase(int id);
  // 排序：拖动改变优先级（new_order 为 id 顺序）
  bool reorder(const std::vector<int>& new_order);

  // 取出全部当前传递（合并输入，>1 合并）；清空 immediate
  std::string drain_immediate(char sep = '\n');
  // 下一次模型调用带上队列里的全部引导；清空队列。
  std::string drain_all(char sep = '\n');
  // Remove ONE item, returning its text (the "send now" action).
  bool take(int id, std::string* text);
  // 取出本轮结束传递（不清队列，由调用方显式清）
  std::vector<SteeringItem> round_end_items() const;
  void clear_kind(SteeringKind kind);

  // 查询
  std::vector<SteeringItem> items() const { return items_; }
  bool empty() const { return items_.empty(); }
  size_t immediate_count() const;

  Json to_json() const;

 private:
  std::vector<SteeringItem> items_;
  int next_id_ = 1;
};

}  // namespace fs