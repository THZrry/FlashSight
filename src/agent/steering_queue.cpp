// steering_queue.cpp - SteeringQueue implementation
#include "steering_queue.hpp"

namespace fs {

const char* steering_kind_str(SteeringKind k) {
  return k == SteeringKind::kRoundEnd ? "round-end" : "immediate";
}

int SteeringQueue::push(SteeringKind kind, const std::string& text) {
  SteeringItem item;
  item.id = next_id_++;
  item.kind = kind;
  item.text = text;
  items_.push_back(item);
  return item.id;
}

bool SteeringQueue::edit(int id, const std::string& text) {
  for (auto& it : items_) {
    if (it.id == id) {
      it.text = text;
      return true;
    }
  }
  return false;
}

bool SteeringQueue::erase(int id) {
  for (auto it = items_.begin(); it != items_.end(); ++it) {
    if (it->id == id) {
      items_.erase(it);
      return true;
    }
  }
  return false;
}

bool SteeringQueue::reorder(const std::vector<int>& new_order) {
  if (new_order.size() != items_.size()) return false;
  std::vector<SteeringItem> next;
  for (int id : new_order) {
    bool found = false;
    for (auto& it : items_) {
      if (it.id == id) {
        next.push_back(it);
        found = true;
        break;
      }
    }
    if (!found) return false;
  }
  items_.swap(next);
  return true;
}

size_t SteeringQueue::immediate_count() const {
  size_t n = 0;
  for (auto& it : items_) {
    if (it.kind == SteeringKind::kImmediate) ++n;
  }
  return n;
}

std::string SteeringQueue::drain_immediate(char sep) {
  std::string out;
  std::vector<SteeringItem> keep;
  for (auto& it : items_) {
    if (it.kind == SteeringKind::kImmediate) {
      if (!out.empty()) out.push_back(sep);
      out += it.text;
    } else {
      keep.push_back(it);
    }
  }
  items_.swap(keep);
  return out;
}

std::vector<SteeringItem> SteeringQueue::round_end_items() const {
  std::vector<SteeringItem> out;
  for (auto& it : items_) {
    if (it.kind == SteeringKind::kRoundEnd) out.push_back(it);
  }
  return out;
}

void SteeringQueue::clear_kind(SteeringKind kind) {
  std::vector<SteeringItem> keep;
  for (auto& it : items_) {
    if (it.kind != kind) keep.push_back(it);
  }
  items_.swap(keep);
}

Json SteeringQueue::to_json() const {
  Json arr = Json::array();
  for (auto& it : items_) {
    Json j = Json::object();
    j["id"] = it.id;
    j["kind"] = steering_kind_str(it.kind);
    j["text"] = it.text;
    arr.push_back(j);
  }
  return arr;
}

}  // namespace fs