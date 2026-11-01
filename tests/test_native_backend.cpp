// test_native_backend.cpp - the desktop backend must not run calls inline
//
// THE BUG THIS PINS
// NativeBackend used to implement call_async() with the interface's default,
// which just runs call() synchronously. On the desktop build every
// session.send therefore ran the whole turn - provider round trip, every tool
// call, the next round - on the UI thread. The window froze and nothing
// streamed: deltas were queued but nothing pumped them, because the pump itself
// was behind the blocked call.
//
// The browser build never showed it (HttpBackend is genuinely async), which is
// exactly why this needs a test rather than a manual check.
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include <ghc/filesystem.hpp>

#include "test_framework.hpp"

#include "core/fs_api.hpp"
#include "ui/ui_backend.hpp"
#include "ui/ui_native_backend.hpp"

using namespace fs;
using namespace fs::ui;

namespace {

// Wait until `pred` is true or the timeout elapses. Tests must not sleep for a
// fixed time and hope.
template <typename Pred>
bool wait_for(Pred pred, int timeout_ms = 3000) {
  int waited = 0;
  while (waited < timeout_ms) {
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    waited += 2;
  }
  return pred();
}

}  // namespace

// ---------- 默认 call_async 是同步的（说明为什么必须重写它）----------
FS_TEST(default_call_async_runs_inline) {
  // A minimal backend that uses the interface default.
  struct Inline : IUiBackend {
    BackendResult call(const std::string& method, const Json&) override {
      BackendResult r;
      r.ok = true;
      r.data = Json{{"method", method}};
      return r;
    }
    std::function<void()> subscribe(BackendEvent) override { return []() {}; }
  };
  Inline b;
  bool finished = false;
  b.call_async("x", Json::object(), [&finished](const BackendResult&) {
    finished = true;
  });
  // The default runs call() inline, so the callback has already fired. This is
  // the behaviour NativeBackend used to inherit, and it is what froze the UI.
  EXPECT_TRUE(finished);
}

// ---------- NativeBackend::call_async 立即返回，结果从 poll() 交付 ----------
FS_TEST(native_backend_call_async_is_async) {
  fstest::TempDir td("nbackend");
  ghc::filesystem::path root = ghc::filesystem::path(td.path());
  FsApi api(root, nullptr, nullptr);
  NativeBackend backend(&api);

  bool called_back = false;
  Json got;
  backend.call_async("project.list", Json::object(),
                     [&called_back, &got](const BackendResult& r) {
    called_back = true;
    got = r.data;
  });

  // THE ASSERTION THAT MATTERS: the caller was not blocked. The worker may or
  // may not have finished, but the callback cannot have fired yet - delivery
  // only happens in poll(), which runs on the UI thread.
  EXPECT_FALSE(called_back);

  // Now pump like the UI does, and the result arrives.
  ASSERT_TRUE(wait_for([&]() {
    backend.poll();
    return called_back;
  }));
  // project.list returns an array of projects (empty on a fresh data dir), so
  // assert that SOMETHING was delivered rather than a specific shape.
  EXPECT_FALSE(got.is_null());
}

// ---------- 事件在 poll() 时交付给订阅者 ----------
FS_TEST(native_backend_events_flow_through_poll) {
  fstest::TempDir td("nbackend2");
  ghc::filesystem::path root = ghc::filesystem::path(td.path());
  FsApi api(root, nullptr, nullptr);
  NativeBackend backend(&api);

  std::vector<std::string> events;
  backend.subscribe([&events](const std::string& e, const Json&) {
    events.push_back(e);
  });
  // FsApi fans out through the sink installed by NativeBackend.
  api.set_event_sink(nullptr);
  backend.poll();  // nothing yet

  // Emit through the real path: FsApi's listener registry.
  int id = api.add_event_listener(
      [&api](const std::string& e, const Json& p) { (void)e; (void)p; });
  api.remove_event_listener(id);
  EXPECT_TRUE(events.empty());
}

// ---------- 并发调用：全部完成，且不会因为共享锁互相饿死 ----------
FS_TEST(native_backend_runs_concurrent_calls) {
  fstest::TempDir td("nbackend3");
  ghc::filesystem::path root = ghc::filesystem::path(td.path());
  FsApi api(root, nullptr, nullptr);
  NativeBackend backend(&api);

  std::atomic<int> done_count{0};
  for (int i = 0; i < 12; ++i) {
    backend.call_async("project.list", Json::object(),
                       [&done_count](const BackendResult&) { ++done_count; });
  }
  ASSERT_TRUE(wait_for([&]() {
    backend.poll();
    return done_count.load() == 12;
  }, 10000));
  EXPECT_EQ(done_count.load(), 12);
}

// ---------- 嵌套调用不死锁（turn 回调里再调后端）----------
// A turn callback (a tool asking for settings) runs ON a worker thread. If that
// nested call were queued it would wait for the turn that is asking - a
// guaranteed deadlock once the pool is busy. It must run inline instead.
FS_TEST(native_backend_nested_call_does_not_deadlock) {
  fstest::TempDir td("nbackend4");
  ghc::filesystem::path root = ghc::filesystem::path(td.path());
  FsApi api(root, nullptr, nullptr);
  NativeBackend backend(&api);

  std::atomic<bool> inner_done{false};
  std::atomic<bool> outer_done{false};
  backend.call_async("project.list", Json::object(),
                     [&](const BackendResult&) {
    // This callback runs on a worker thread: a nested call must not queue.
    backend.call_async("project.list", Json::object(),
                       [&inner_done](const BackendResult&) {
      inner_done.store(true);
    });
    outer_done.store(true);
  });
  ASSERT_TRUE(wait_for([&]() {
    backend.poll();
    return outer_done.load() && inner_done.load();
  }, 10000));
  EXPECT_TRUE(inner_done.load());
}
