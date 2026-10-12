// todo_store.cpp - 进程（todo）状态存储实现（addon2.md）
#include "todo_store.hpp"

namespace fs {

int TodoStore::add(const std::string& owner, const std::string& task) {
  int id = ++next_id_[owner];
  TodoItem it;
  it.id = id;
  it.task = task;
  items_[owner].push_back(it);
  return id;
}

bool TodoStore::check(const std::string& owner, int id, bool done, int64_t now_ms) {
  auto it = items_.find(owner);
  if (it == items_.end()) return false;
  for (auto& t : it->second) {
    if (t.id == id) {
      t.done = done;
      t.done_at = done ? now_ms : 0;
      return true;
    }
  }
  return false;
}

bool TodoStore::erase(const std::string& owner, int id) {
  auto it = items_.find(owner);
  if (it == items_.end()) return false;
  for (size_t i = 0; i < it->second.size(); ++i) {
    if (it->second[i].id == id) {
      it->second.erase(it->second.begin() + static_cast<long>(i));
      return true;
    }
  }
  return false;
}

void TodoStore::clear(const std::string& owner) { items_.erase(owner); }

std::vector<TodoItem> TodoStore::list(const std::string& owner) const {
  auto it = items_.find(owner);
  return it == items_.end() ? std::vector<TodoItem>() : it->second;
}

Json TodoStore::to_json(const std::string& owner) const {
  Json arr = Json::array();
  auto it = items_.find(owner);
  if (it == items_.end()) return arr;
  for (auto& t : it->second) {
    Json j = Json::object();
    j["id"] = t.id;
    j["task"] = t.task;
    j["done"] = t.done;
    j["doneAt"] = t.done_at;
    arr.push_back(j);
  }
  return arr;
}

int TodoStore::pending_count(const std::string& owner) const {
  auto it = items_.find(owner);
  if (it == items_.end()) return 0;
  int n = 0;
  for (auto& t : it->second) {
    if (!t.done) ++n;
  }
  return n;
}

std::vector<std::string> TodoStore::owners() const {
  std::vector<std::string> v;
  for (auto& kv : items_) v.push_back(kv.first);
  return v;
}

}  // namespace fs