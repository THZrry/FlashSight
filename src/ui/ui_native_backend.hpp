// ui_native_backend.hpp - in-process backend (desktop builds: webview, WhaleUI).
//
// Calls FsApi directly; no HTTP, no serialization on the wire (rules: "桌面原生
// 不需要走 http，直接进程内用函数调用即可"). Events come from the FsApi event
// sink and are queued until the UI pumps them.
//
// ---------------------------------------------------------------------------
// WHY CALLS RUN OFF THE UI THREAD
//
// A model turn is not a fast function call: session.send blocks for the whole
// turn (provider round trip, then every tool call, then the next round). Running
// it on the UI thread froze the window for the entire turn - the desktop build
// had no streaming at all, just a dead frame until the answer was complete and
// then one repaint. The browser build never showed this because HttpBackend is
// genuinely async.
//
// So calls run on worker threads and results are delivered by poll() on the UI
// thread. FsApi is not thread-safe, so UiApi::call takes FsApi's shared
// recursive mutex around every route; the turn releases it for the model call
// so control calls (suspend / approve / panel loads) still get through.
//
// RE-ENTRANCY: a turn callback can call back into the backend (a tool asking for
// settings). That must NOT be queued behind the turn's own worker, or it would
// deadlock waiting for itself. `thread_local` marks "this thread is already
// inside a backend call", and such nested calls run inline.
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
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

  // Synchronous call: runs on the calling thread. Used by tests and by nested
  // calls from inside a turn.
  BackendResult call(const std::string& method, const Json& args) override;

  // Asynchronous: queued for a worker, result delivered by poll().
  void call_async(const std::string& method, const Json& args,
                  BackendDone done) override;

  std::function<void()> subscribe(BackendEvent fn) override;

  // Called from the UI thread: delivers finished results and queued events.
  // Results and events share one queue, so their order is preserved.
  void poll() override;

  // FsApi runs the terminal poller on its own thread here.
  bool polls_server_side() const override { return true; }

  // Wait for in-flight calls (destructor, tests).
  void wait_idle();

  UiApi& api() { return *api_; }

 private:
  struct Item {
    bool is_event = false;
    std::string name;   // method (for a result) or event name
    Json payload;       // result data or event payload
    bool ok = true;
    std::string code;
    std::string message;
    BackendDone done;
  };

  struct Job {
    std::string method;
    Json args;
    BackendDone done;
  };

  void push(Item item);
  void spawn_worker();
  void worker_main();

  std::unique_ptr<UiApi> api_;
  std::vector<BackendEvent> subs_;

  // UI-facing queue (workers and the event sink -> UI thread).
  std::deque<Item> queue_;
  std::mutex mu_;

  // Work queue + worker pool.
  std::deque<Job> jobs_;
  std::mutex job_mu_;
  std::condition_variable job_cv_;
  std::vector<std::thread> workers_;
  int idle_workers_ = 0;  // guarded by job_mu_
  bool quit_ = false;
  std::atomic<int> active_jobs_{0};
};

}  // namespace ui
}  // namespace fs
