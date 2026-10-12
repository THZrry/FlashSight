// todo_store.hpp - 进程（todo）状态存储（addon2.md 长任务-进程）
// 每会话/每子代理一棵进程树：列出本轮任务，完成一项打勾。
// 子代理独立 todo（addon2：工作流子代理也提供进程工具，保证任务确实完成）。
#pragma once

#include <map>
#include <string>
#include <vector>

#include "fs_json.hpp"

namespace fs {

struct TodoItem {
  int id = 0;
  std::string task;
  bool done = false;
  int64_t done_at = 0;
};

class TodoStore {
 public:
  // owner：会话 sid 或子代理 id（各自独立进程树）
  int add(const std::string& owner, const std::string& task);
  bool check(const std::string& owner, int id, bool done, int64_t now_ms);
  bool erase(const std::string& owner, int id);
  void clear(const std::string& owner);

  std::vector<TodoItem> list(const std::string& owner) const;
  Json to_json(const std::string& owner) const;
  // 未完成数（"只要未完成，自动开启下一轮"判定用）
  int pending_count(const std::string& owner) const;
  // 全部 owner（落库用）
  std::vector<std::string> owners() const;

 private:
  std::map<std::string, std::vector<TodoItem>> items_;
  std::map<std::string, int> next_id_;
};

}  // namespace fs