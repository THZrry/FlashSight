// ui_backend.cpp - NullBackend 实现
#include "ui_backend.hpp"

namespace fs {
namespace ui {

FsResult NullBackend::call(const std::string& method, const Json& args) {
  (void)args;
  Json data = Json::object();
  data["method"] = method;
  return FsResult::success(data);
}

std::function<void()> NullBackend::subscribe(BackendEvent fn) {
  subs_.push_back(std::move(fn));
  size_t id = subs_.size() - 1;
  // v1：退订置空（不压缩 vector，保证 id 稳定）
  return [this, id]() {
    if (id < subs_.size()) subs_[id] = nullptr;
  };
}

void NullBackend::fire(const std::string& event, const Json& payload) {
  for (auto& s : subs_) {
    if (s) s(event, payload);
  }
}

}  // namespace ui
}  // namespace fs