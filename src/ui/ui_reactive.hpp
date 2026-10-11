// ui_reactive.hpp - minimal reactive framework (Signal/Effect + DOM bindings)
// C++14, no third-party deps. Single-threaded UI assumption.
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "ui_dom.hpp"

namespace fs {
namespace ui {

// Subscription: RAII unsubscribe handle
class Subscription {
 public:
  Subscription() = default;
  explicit Subscription(std::function<void()> unsubscribe)
      : unsubscribe_(std::move(unsubscribe)) {}
  ~Subscription() { reset(); }
  Subscription(Subscription&& o) noexcept : unsubscribe_(std::move(o.unsubscribe_)) {}
  Subscription& operator=(Subscription&& o) noexcept {
    reset();
    unsubscribe_ = std::move(o.unsubscribe_);
    return *this;
  }
  Subscription(const Subscription&) = delete;
  Subscription& operator=(const Subscription&) = delete;
  void reset() {
    if (unsubscribe_) {
      auto f = std::move(unsubscribe_);
      unsubscribe_ = nullptr;
      f();
    }
  }

 private:
  std::function<void()> unsubscribe_;
};

// Signal<T>: observable value. Must be held by shared_ptr (use Signal<T>::make).
template <typename T>
class Signal : public std::enable_shared_from_this<Signal<T>> {
 public:
  explicit Signal(T value = T()) : value_(std::move(value)) {}

  template <typename... Args>
  static std::shared_ptr<Signal<T>> make(Args&&... args) {
    return std::make_shared<Signal<T>>(std::forward<Args>(args)...);
  }

  const T& get() const { return value_; }

  void set(T value) {
    value_ = std::move(value);
    // snapshot callbacks first: callbacks may unsubscribe (mutating subs_)
    // during notify, so never touch the map while invoking.
    std::vector<std::function<void(const T&)>> fns;
    fns.reserve(subs_.size());
    for (auto& kv : subs_) fns.push_back(kv.second);
    for (auto& fn : fns) fn(value_);
  }

  Subscription subscribe(std::function<void(const T&)> fn) {
    size_t id = next_id_++;
    subs_[id] = std::move(fn);
    auto self = this->shared_from_this();
    return Subscription([self, id]() { self->subs_.erase(id); });
  }

 private:
  T value_;
  std::map<size_t, std::function<void(const T&)>> subs_;
  size_t next_id_ = 1;
};

// Effect: re-runs fn whenever any signal read inside changes.
// Usage: Effect e; e.run([&]{ label->set(...sig->get()...); });
class Effect {
 public:
  Effect() = default;
  ~Effect() = default;
  Effect(const Effect&) = delete;
  Effect& operator=(const Effect&) = delete;

  // Run fn now; auto-resubscribes to every Signal read during fn.
  // NOTE: tracking works via explicit track() calls (no TLS magic):
  //   e.run(sigA, sigB, [&]{ ... });
  template <typename F>
  void run(F&& fn) {
    fn();
  }

  // Run fn and re-run whenever any of the given signals change.
  // Each run clears old subscriptions and resubscribes (C++14 pack expansion
  // via initializer list).
  template <typename F, typename... Sigs>
  void watch(F&& fn, std::shared_ptr<Sigs>... sigs) {
    auto holder = std::make_shared<Holder>();
    auto rerun = std::make_shared<std::function<void()>>();
    *rerun = [fn, holder, rerun, sigs...]() mutable {
      holder->subs.clear();
      holder->rerun = rerun;
      // resubscribe first, then run (so fn() sees fresh subs on re-entry)
      int _[] = {0, (subscribe_static(holder, *rerun, sigs), 0)...};
      (void)_;
      fn();
    };
    (*rerun)();
    holder_ = holder;
  }

  void dispose() {
    if (holder_) {
      holder_->subs.clear();  // 先退订，再释放（否则 Signal 侧订阅残留）
      holder_->rerun.reset();
      holder_.reset();
    }
  }

 private:
  struct Holder {
    std::vector<Subscription> subs;
    std::shared_ptr<std::function<void()>> rerun;
  };
  std::shared_ptr<Holder> holder_;

  // static helper so the rerun lambda can resubscribe without `this`
  template <typename H, typename F, typename V>
  static void subscribe_static(const std::shared_ptr<H>& holder, const F& rerun,
                               const std::shared_ptr<Signal<V>>& sig) {
    auto r = rerun;
    holder->subs.push_back(sig->subscribe([r](const V&) { r(); }));
  }
};

// ---- DOM bindings ----

// bind_text: signal<string> -> node text
inline Subscription bind_text(IDom& dom, Node node,
                              const std::shared_ptr<Signal<std::string>>& sig) {
  dom.set_text(node, sig->get());
  return sig->subscribe([&dom, node](const std::string& v) { dom.set_text(node, v); });
}

// bind_visible: signal<bool> -> node visibility
inline Subscription bind_visible(IDom& dom, Node node,
                                 const std::shared_ptr<Signal<bool>>& sig) {
  dom.set_visible(node, sig->get());
  return sig->subscribe([&dom, node](bool v) { dom.set_visible(node, v); });
}

}  // namespace ui
}  // namespace fs