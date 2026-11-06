// ui_native_backend.cpp - in-process IUiBackend over FsApi.
//
// See the header for the two decisions that matter:
//   * calls run OFF the UI thread (otherwise a model turn freezes the window),
//   * a nested call from inside a turn runs INLINE (otherwise it would queue
//     behind the turn and deadlock).
#include "ui_native_backend.hpp"

#include <chrono>

#include "core/fs_api.hpp"

namespace fs {
namespace ui {

namespace {
// "This thread is already inside a backend call." A turn callback that calls
// back into the backend must not be queued behind the turn itself.
thread_local int g_in_backend_call = 0;

// Two concurrent turns (multi-session auto mode) must not starve the UI's own
// panels: each turn holds a worker for its whole round trip, and a balance probe
// or a terminal grid fetch can hold another. 4 left no headroom, so every panel
// call queued behind the turns and the window showed stale state.
constexpr int kMaxWorkers = 8;
// The UI queue is bounded. The UI thread drains it every 50 ms, but a long
// tool-heavy turn can emit hundreds of events per second, and if the UI thread
// is stuck for a while (a big patch, a slow machine, a compositor stall) the
// queue used to grow without any limit - tool payloads and deltas accumulating
// in memory for as long as the stall lasts. That is the tool <-> UI seam the
// user suspected: the backend keeps working, the window is stuck, and memory
// climbs with every event.
constexpr size_t kMaxQueuedItems = 20000;

bool droppable_event(const std::string& name) {
  // Display-only events: the turn itself is persisted by the loop, and the
  // final state is re-read on turn.done / tab switch. Lifecycle events
  // (turn.done, loop.interrupted, permission.prompt, ...) are never dropped.
  return name == "stream.delta" || name == "tool.update" ||
         name == "assistant.interim" || name == "terminal.output" ||
         name == "subagent.started";
}
}  // namespace

NativeBackend::NativeBackend(FsApi* api) : api_(new UiApi(api)) {
  if (api) {
    // FsApi fans events out through this sink. Called from ANY thread (the turn
    // thread, a subagent, the terminal poller), so it only appends to the queue;
    // delivery happens in poll() on the UI thread.
    api->set_event_sink([this](const std::string& e, const Json& p) {
      Item item;
      item.is_event = true;
      item.name = e;
      item.payload = p;
      push(std::move(item));
    });
  }
}

NativeBackend::~NativeBackend() {
  {
    std::lock_guard<std::mutex> lk(job_mu_);
    quit_ = true;
  }
  job_cv_.notify_all();
  for (size_t i = 0; i < workers_.size(); ++i) {
    if (workers_[i].joinable()) workers_[i].join();
  }
  workers_.clear();
}

void NativeBackend::push(Item item) {
  std::lock_guard<std::mutex> lk(mu_);
  queue_.push_back(std::move(item));
  if (queue_.size() <= kMaxQueuedItems) return;
  // Over the cap: drop the OLDEST droppable event. Scanning from the front
  // keeps the newest content (what the user will see when the UI recovers) and
  // never drops a lifecycle event. O(n) only while overloaded.
  for (std::deque<Item>::iterator it = queue_.begin(); it != queue_.end(); ++it) {
    if (it->is_event && droppable_event(it->name)) {
      queue_.erase(it);
      return;
    }
  }
  // A flood of lifecycle events: hard drop the oldest so the queue is bounded
  // no matter what.
  if (queue_.size() > kMaxQueuedItems + 1024) queue_.pop_front();
}

BackendResult NativeBackend::call(const std::string& method, const Json& args) {
  ++g_in_backend_call;
  BackendResult r;
  try {
    r = to_backend_result(api_->call(method, args));
  } catch (const std::exception& e) {
    r.ok = false;
    r.code = "internal";
    r.message = e.what();
  } catch (...) {
    r.ok = false;
    r.code = "internal";
    r.message = "unknown error";
  }
  --g_in_backend_call;
  return r;
}

void NativeBackend::call_async(const std::string& method, const Json& args,
                               BackendDone done) {
  // Re-entrant call (a turn callback asking for something): run it here. If it
  // were queued it would wait for the very turn that is asking, forever.
  if (g_in_backend_call > 0) {
    BackendResult r = call(method, args);
    if (done) done(r);
    return;
  }
  bool need_worker = false;
  {
    std::lock_guard<std::mutex> lk(job_mu_);
    Job j;
    j.method = method;
    j.args = args;
    j.done = done;
    jobs_.push_back(std::move(j));
    ++active_jobs_;
    // Grow the pool on demand: a turn occupies one worker for minutes, and the
    // UI still needs to load panels while it runs. Idle workers are reused.
    if (idle_workers_ == 0 && static_cast<int>(workers_.size()) < kMaxWorkers) {
      need_worker = true;
    }
  }
  if (need_worker) spawn_worker();
  job_cv_.notify_one();
}

void NativeBackend::spawn_worker() {
  // Spawned under job_mu_ by the caller's bookkeeping; the thread itself only
  // reads the queue.
  std::thread t([this]() { worker_main(); });
  std::lock_guard<std::mutex> lk(job_mu_);
  workers_.push_back(std::move(t));
}

void NativeBackend::worker_main() {
  for (;;) {
    Job job;
    {
      std::unique_lock<std::mutex> lk(job_mu_);
      ++idle_workers_;
      job_cv_.wait(lk, [this]() { return quit_ || !jobs_.empty(); });
      --idle_workers_;
      if (quit_ && jobs_.empty()) return;
      job = std::move(jobs_.front());
      jobs_.pop_front();
    }
    BackendResult r = call(job.method, job.args);
    Item item;
    item.is_event = false;
    item.name = job.method;
    item.payload = r.data;
    item.ok = r.ok;
    item.code = r.code;
    item.message = r.message;
    item.done = job.done;
    push(std::move(item));
    --active_jobs_;
  }
}

std::function<void()> NativeBackend::subscribe(BackendEvent fn) {
  subs_.push_back(std::move(fn));
  size_t id = subs_.size() - 1;
  return [this, id]() {
    if (id < subs_.size()) subs_[id] = nullptr;
  };
}

void NativeBackend::poll() {
  std::deque<Item> batch;
  {
    std::lock_guard<std::mutex> lk(mu_);
    batch.swap(queue_);
  }
  for (size_t i = 0; i < batch.size(); ++i) {
    Item& item = batch[i];
    if (item.is_event) {
      for (size_t k = 0; k < subs_.size(); ++k) {
        if (subs_[k]) subs_[k](item.name, item.payload);
      }
      continue;
    }
    if (item.done) {
      BackendResult r;
      r.ok = item.ok;
      r.data = item.payload;
      r.code = item.code;
      r.message = item.message;
      item.done(r);
    }
  }
}

void NativeBackend::wait_idle() {
  for (;;) {
    {
      std::lock_guard<std::mutex> lk(job_mu_);
      if (jobs_.empty() && active_jobs_.load() == 0) break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}

}  // namespace ui
}  // namespace fs
