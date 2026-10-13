// ui_backend.hpp - UI backend abstraction: how the UI reaches the core.
//
//   * NativeBackend : in-process FsApi calls (WhaleUI desktop build; no HTTP)
//   * HttpBackend   : POST /api/call to the local server (browser/wasm build)
//   * NullBackend   : no-op (unit tests, prototype)
//
// call() is synchronous and is what the desktop build uses. The browser build
// cannot block on a request, so it implements call_async() instead; the default
// call_async just runs call() and reports immediately, which keeps synchronous
// backends a single-method implementation.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "fs_error.hpp"
#include "fs_json.hpp"

namespace fs {
namespace ui {

// Result of a backend call (JSON payload + error info for the UI to display).
struct BackendResult {
  bool ok = true;
  Json data = Json::object();
  std::string code;     // machine-readable err code ("not-found", ...)
  std::string message;  // human-readable
};

inline BackendResult to_backend_result(const FsResult& r) {
  BackendResult b;
  b.ok = r.ok();
  b.data = r.data;
  if (!r.ok()) {
    b.code = err_code_name(r.code);
    b.message = r.message;
    if (r.denied()) {
      if (b.code.empty()) b.code = r.denial.reason;
      if (b.message.empty()) b.message = r.denial.reason;
      b.data = Json::object();
      b.data["allowed"] = false;
      b.data["reason"] = r.denial.reason;
      b.data["lockedBy"] = r.denial.lockedBy;
      b.data["howToProceed"] = r.denial.howToProceed;
    }
  }
  return b;
}

using BackendEvent = std::function<void(const std::string& event, const Json& payload)>;
using BackendDone = std::function<void(const BackendResult&)>;

class IUiBackend {
 public:
  virtual ~IUiBackend() = default;

  // Synchronous call (desktop/in-process).
  virtual BackendResult call(const std::string& method, const Json& args) = 0;
  // Asynchronous call (browser). Default: run sync and report at once.
  virtual void call_async(const std::string& method, const Json& args, BackendDone done) {
    BackendResult r = call(method, args);
    if (done) done(r);
  }
  // Subscribe to backend events; returns an unsubscribe function.
  virtual std::function<void()> subscribe(BackendEvent fn) = 0;
  // Pump events (browser polls on a timer; desktop is a no-op).
  virtual void poll() {}
};

// Null backend: succeeds with an empty payload, events fired manually.
class NullBackend : public IUiBackend {
 public:
  BackendResult call(const std::string& method, const Json& args) override;
  std::function<void()> subscribe(BackendEvent fn) override;
  void fire(const std::string& event, const Json& payload);

 private:
  std::vector<BackendEvent> subs_;
};

}  // namespace ui
}  // namespace fs
