// ui_native_backend.cpp - in-process IUiBackend over FsApi.
#include "ui_native_backend.hpp"

#include "core/fs_api.hpp"

namespace fs {
namespace ui {

NativeBackend::NativeBackend(FsApi* api) : api_(new UiApi(api)) {
  if (api) {
    // FsApi fans events out through this sink; queue them for poll().
    api->set_event_sink([this](const std::string& e, const Json& p) {
      std::lock_guard<std::mutex> lk(mu_);
      queue_.push_back(std::make_pair(e, p));
    });
  }
}

NativeBackend::~NativeBackend() = default;

BackendResult NativeBackend::call(const std::string& method, const Json& args) {
  return to_backend_result(api_->call(method, args));
}

std::function<void()> NativeBackend::subscribe(BackendEvent fn) {
  subs_.push_back(std::move(fn));
  size_t id = subs_.size() - 1;
  return [this, id]() {
    if (id < subs_.size()) subs_[id] = nullptr;
  };
}

void NativeBackend::poll() {
  std::deque<std::pair<std::string, Json> > batch;
  {
    std::lock_guard<std::mutex> lk(mu_);
    batch.swap(queue_);
  }
  for (size_t i = 0; i < batch.size(); ++i) {
    for (size_t k = 0; k < subs_.size(); ++k) {
      if (subs_[k]) subs_[k](batch[i].first, batch[i].second);
    }
  }
}

}  // namespace ui
}  // namespace fs
