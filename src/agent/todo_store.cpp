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
      removed_[owner][id] = it->second[i].done ? "done" : "dismissed";
      it->second.erase(it->second.begin() + static_cast<long>(i));
      while (removed_[owner].size() > 200) {
        removed_[owner].erase(removed_[owner].begin());
      }
      return true;
    }
  }
  return false;
}

void TodoStore::clear(const std::string& owner) {
  std::map<std::string, std::vector<TodoItem>>::iterator it = items_.find(owner);
  if (it != items_.end()) {
    for (size_t i = 0; i < it->second.size(); ++i) {
      removed_[owner][it->second[i].id] =
          it->second[i].done ? "done" : "dismissed";
    }
    while (removed_[owner].size() > 200) {
      removed_[owner].erase(removed_[owner].begin());
    }
  }
  items_.erase(owner);
}

void TodoStore::restore(const std::string& owner, const Json& items) {
  if (!items.is_array()) return;
  std::vector<TodoItem>& dst = items_[owner];
  for (size_t i = 0; i < items.size(); ++i) {
    const Json& e = items[i];
    if (!e.is_object()) continue;
    TodoItem t;
    t.id = static_cast<int>(jsonutil::get_int(e, "id", 0));
    t.task = jsonutil::get<std::string>(e, "task", "");
    t.done = jsonutil::get<bool>(e, "done", false);
    t.done_at = jsonutil::get_int(e, "doneAt", 0);
    if (t.id <= 0 || t.task.empty()) continue;
    if (t.id > next_id_[owner]) next_id_[owner] = t.id;
    dst.push_back(t);
  }
}

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

bool TodoStore::last_status(const std::string& owner, int id,
                            std::string* status) const {
  std::map<std::string, std::map<int, std::string>>::const_iterator it =
      removed_.find(owner);
  if (it == removed_.end()) return false;
  std::map<int, std::string>::const_iterator sit = it->second.find(id);
  if (sit == it->second.end()) return false;
  if (status) *status = sit->second;
  return true;
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