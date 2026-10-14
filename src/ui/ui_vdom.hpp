// ui_vdom.hpp - Framework layer (rules/addon3.md "架构/Container + Framework")
//
// Container : State<T>   - observable value; set() notifies subscribers.
// Framework : VNode      - immutable-ish virtual node description
//             H          - fluent builder (el("div").cls("card").add(...))
//             EventBus   - integer handler ids shared with the DOM adapter
//             patch()    - keyed diff between two VNode trees -> minimal DOM ops
//
// Render model: rebuild the whole VNode tree from State, then patch it against
// the previous tree. The diff is the only thing that touches the DOM, so a
// re-render is cheap and the app never hand-manages elements.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "ui_dom.hpp"

namespace fs {
namespace ui {

using EventFn = std::function<void(const std::string& value)>;

// ---------------- EventBus ----------------
// Handlers live here, not in the DOM adapter: the adapter only knows integer
// ids, which keeps every backend trivial (and lets the wasm side stay val/EM_JS
// only). One UI per process -> singleton is enough.
//
// Ids are strictly monotonic and never reused, so a stale DOM listener can
// never accidentally resolve to a newer handler. After each render the app
// calls retain() with the ids the current tree references, dropping everything
// from the previous generation (otherwise every render would leak handlers).
class EventBus {
 public:
  int add(EventFn fn) {
    int id = ++seq_;
    handlers_[id] = std::move(fn);
    return id;
  }
  void fire(int id, const std::string& value) {
    std::map<int, EventFn>::iterator it = handlers_.find(id);
    if (it == handlers_.end() || !it->second) return;
    // Copy first: the handler usually calls update() -> render() -> retain(),
    // which erases this very entry. Invoking the map slot directly would
    // destroy the running closure (and its captures) mid-call.
    EventFn fn = it->second;
    fn(value);
  }
  // keep only the given ids; drop all others
  void retain(const std::vector<int>& keep);
  size_t size() const { return handlers_.size(); }
  int last_id() const { return seq_; }

 private:
  std::map<int, EventFn> handlers_;
  int seq_ = 0;
};

EventBus& bus();

// ---------------- KeyedStore ----------------
// Framework-level scoped state. A scope is anything the user switches between
// (tabs, sessions, pages, panels): the app keeps ONE live copy of T (the
// active scope) and this store keeps the stashed copies keyed by scope id.
// Switching = stash(live) then restore(other). Nothing here is UI-specific, so
// any screen with switchable scopes reuses it.
template <typename T>
class KeyedStore {
 public:
  // stash the live scope under `key`
  void stash(const std::string& key, const T& live) { map_[key] = live; }
  // restore `key` into `live`; false when the key was never stashed
  bool restore(const std::string& key, T* live) const {
    typename std::map<std::string, T>::const_iterator it = map_.find(key);
    if (it == map_.end()) return false;
    if (live) *live = it->second;
    return true;
  }
  // reference to `key`'s stashed value, creating a default when absent
  T& get_or_create(const std::string& key) { return map_[key]; }
  bool has(const std::string& key) const { return map_.find(key) != map_.end(); }
  void erase(const std::string& key) { map_.erase(key); }
  void clear() { map_.clear(); }
  size_t size() const { return map_.size(); }

 private:
  std::map<std::string, T> map_;
};

// ---------------- Container ----------------
// Observable value. set() always notifies (the app coalesces re-renders).
template <typename T>
class State {
 public:
  State() {}
  explicit State(const T& v) : value_(v) {}
  const T& get() const { return value_; }
  void set(const T& v) {
    value_ = v;
    for (size_t i = 0; i < subs_.size(); ++i) subs_[i](value_);
  }
  // mutate in place then notify
  void update(const std::function<void(T&)>& f) {
    f(value_);
    for (size_t i = 0; i < subs_.size(); ++i) subs_[i](value_);
  }
  void subscribe(const std::function<void(const T&)>& f) { subs_.push_back(f); }

 private:
  T value_{};
  std::vector<std::function<void(const T&)> > subs_;
};

// ---------------- VNode ----------------
struct VNode;
typedef std::shared_ptr<VNode> VNodePtr;

struct VEvent {
  EventKind kind;
  int hid;
};

struct VAttr {
  std::string key;
  std::string value;
};

struct VStyle {
  std::string key;
  std::string value;
};

struct VNode {
  std::string tag;   // empty => text node
  std::string text;  // text content (text node, or element shortcut)
  std::string key;   // diff identity for lists
  std::vector<VAttr> attrs;
  std::vector<VStyle> styles;
  std::vector<VEvent> events;
  std::vector<VNodePtr> children;
  Node dom{0};       // real node after patch (filled by patch())
  bool is_text() const { return tag.empty(); }
};

VNodePtr make_vnode(const std::string& tag);
VNodePtr make_text(const std::string& text);

// Fluent builder. Each call returns a new H sharing the same VNode, so chains
// are safe on temporaries.
class H {
 public:
  explicit H(VNodePtr n) : n_(n) {}
  H cls(const std::string& c) {
    for (size_t i = 0; i < n_->attrs.size(); ++i) {
      if (n_->attrs[i].key == "class") {  // merge so anim() can append
        if (!n_->attrs[i].value.empty()) n_->attrs[i].value += " ";
        n_->attrs[i].value += c;
        return *this;
      }
    }
    n_->attrs.push_back(VAttr{"class", c});
    return *this;
  }
  H id(const std::string& v) {
    n_->attrs.push_back(VAttr{"id", v});
    return *this;
  }
  H attr(const std::string& k, const std::string& v) {
    n_->attrs.push_back(VAttr{k, v});
    return *this;
  }
  H style(const std::string& k, const std::string& v) {
    n_->styles.push_back(VStyle{k, v});
    return *this;
  }
  H key(const std::string& k) {
    n_->key = k;
    return *this;
  }
  // Mount animation: adds class "anim-<name>" (CSS defines the keyframes).
  // Fires when the element is created, i.e. on first mount and whenever the
  // diff has to replace the node (key/tag change). See web/style.css.
  // Merges into the existing class attribute - a second class attr would be
  // ignored by the browser and silently drop the element's real styling.
  H anim(const std::string& name) { return cls("anim-" + name); }
  H on(EventKind k, EventFn fn) {
    n_->events.push_back(VEvent{k, bus().add(fn)});
    return *this;
  }
  H add(VNodePtr child) {
    if (child) n_->children.push_back(child);
    return *this;
  }
  H add(const H& child) { return add(child.get()); }
  H text(const std::string& t) {
    n_->children.push_back(make_text(t));
    return *this;
  }
  VNodePtr get() const { return n_; }
  operator VNodePtr() const { return n_; }

 private:
  VNodePtr n_;
};

inline H el(const std::string& tag) { return H(make_vnode(tag)); }
inline H el(const std::string& tag, const std::string& cls) {
  return H(make_vnode(tag)).cls(cls);
}
inline H txt(const std::string& t) { return H(make_text(t)); }

// ---------------- patch ----------------
// Diff `neu` against `oldv` (may be null) and apply the minimal DOM changes
// under `parent`. After patching, `neu`'s nodes hold their real Node handles.
void patch(IDom& dom, const VNodePtr& oldv, const VNodePtr& neu, Node parent);

// Mount a fresh tree into parent (oldv == null path of patch).
void mount(IDom& dom, const VNodePtr& v, Node parent);

// Collect every event handler id referenced by the tree (used by EventBus::retain).
void collect_handler_ids(const VNodePtr& v, std::vector<int>* out);

// Find a node by key (depth-first). Returns null when absent.
VNodePtr find_by_key(const VNodePtr& v, const std::string& key);

}  // namespace ui
}  // namespace fs
