// ui_native_backend.hpp - in-process backend (WhaleUI desktop build).
//
// Calls FsApi directly; no HTTP, no serialization on the wire (rules: "桌面原生
// 不需要走 http，直接进程内用函数调用即可"). Events come from the FsApi event
// sink and are queued until the UI pumps them.
#pragma once

#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "ui_api.hpp"
#include "ui_backend.hpp"

namespace fs {

class FsApi;

namespace ui {

class NativeBackend : public IUiBackend {
 public:
  explicit NativeBackend(FsApi* api);
  ~NativeBackend() override;

  BackendResult call(const std::string& method, const Json& args) override;
  std::function<void()> subscribe(BackendEvent fn) override;
  // deliver queued events to subscribers (UI drives this on its own cadence)
  void poll() override;
  // FsApi runs the terminal poller on its own thread here
  bool polls_server_side() const override { return true; }

  UiApi& api() { return *api_; }

 private:
  std::unique_ptr<UiApi> api_;
  std::vector<BackendEvent> subs_;
  std::deque<std::pair<std::string, Json> > queue_;
  std::mutex mu_;
};

}  // namespace ui
}  // namespace fs
