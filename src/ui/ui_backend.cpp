// ui_backend.cpp - NullBackend (no-op backend for tests / prototypes).
#include "ui_backend.hpp"

namespace fs {
namespace ui {

BackendResult NullBackend::call(const std::string& method, const Json& args) {
  (void)args;
  BackendResult r;
  r.ok = true;
  r.data = Json::object();
  r.data["method"] = method;
  return r;
}

std::function<void()> NullBackend::subscribe(BackendEvent fn) {
  subs_.push_back(std::move(fn));
  size_t id = subs_.size() - 1;
  return [this, id]() {
    if (id < subs_.size()) subs_[id] = nullptr;
  };
}

void NullBackend::fire(const std::string& event, const Json& payload) {
  for (size_t i = 0; i < subs_.size(); ++i) {
    if (subs_[i]) subs_[i](event, payload);
  }
}

}  // namespace ui
}  // namespace fs
