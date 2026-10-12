// ui_dom.hpp - DOM Adapter (rules/addon3.md "架构/DOM Adapter")
//
// One unified DOM API over two very different backends:
//   * WasmDom   : emscripten/val.h driving the real browser DOM (wasm build)
//   * NullDom   : no-op + call log (native build until WhaleUI's C API lands;
//                 unit tests assert on the call log)
//   * WhaleDom  : to be added once WhaleUI exposes its C DOM API. UI logic
//                 (Container / Framework / Composers / user) never changes.
//
// The adapter is deliberately dumb: it creates/mutates nodes and forwards
// events. Reactivity, diffing and event dispatch live in the framework
// (ui_vdom.hpp), so this layer stays trivial to reimplement per backend.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace fs {
namespace ui {

// Opaque node handle. 0 = invalid. Each backend interprets `id` its own way
// (browser: key into the JS node table; WhaleUI: its own node pointer).
struct Node {
  uint64_t id = 0;
  bool valid() const { return id != 0; }
  bool operator==(const Node& o) const { return id == o.id; }
  bool operator!=(const Node& o) const { return id != o.id; }
};

// Event kinds the framework can bind. Keep in sync with the wasm glue table.
enum class EventKind {
  kClick = 0,
  kInput = 1,
  kChange = 2,
  kKeyDown = 3,
  kKeyEnter = 4,
  kSubmit = 5,
  kDblClick = 6,
  kContextMenu = 7,
  kScroll = 8,
  kMouseEnter = 9,
  kMouseLeave = 10,
};

const char* event_kind_name(EventKind k);

// Full DOM surface the framework needs. Handlers are passed as integer ids
// into the framework's own registry (see EventBus in ui_vdom.hpp) so the
// adapter never owns C++ closures.
class IDom {
 public:
  virtual ~IDom() = default;

  // --- structure ---
  virtual Node create_element(const std::string& tag) = 0;
  virtual Node create_text(const std::string& text) = 0;
  virtual void append(Node parent, Node child) = 0;
  virtual void insert_before(Node parent, Node child, Node ref) = 0;
  virtual void remove(Node node) = 0;
  virtual void clear(Node parent) = 0;

  // --- content / attributes ---
  virtual void set_text(Node node, const std::string& text) = 0;
  virtual void set_attr(Node node, const std::string& key, const std::string& value) = 0;
  virtual void remove_attr(Node node, const std::string& key) = 0;
  virtual void set_style(Node node, const std::string& key, const std::string& value) = 0;
  virtual void set_visible(Node node, bool visible) = 0;
  // read back a form control's value (used to skip redundant writes)
  virtual std::string get_value(Node node) = 0;

  // --- events ---
  // on() binds the handler for (node, kind), replacing any previous binding
  // for that pair. The framework re-renders the whole tree and mints a fresh
  // handler id each time, so the adapter MUST drop the old listener or every
  // click would fire all stale handlers in order.
  virtual void on(Node node, EventKind kind, int handler_id) = 0;
  virtual void off(Node node, EventKind kind) = 0;

  // --- root container the app renders into ---
  virtual Node root() = 0;

  // Backend identity (diagnostics / tests)
  virtual const char* backend_name() const = 0;
};

// NullDom: records every call so tests can assert render output. Also the
// placeholder backend for the native build until WhaleUI is wired.
class NullDom : public IDom {
 public:
  NullDom();
  ~NullDom() override;

  Node create_element(const std::string& tag) override;
  Node create_text(const std::string& text) override;
  void append(Node parent, Node child) override;
  void insert_before(Node parent, Node child, Node ref) override;
  void remove(Node node) override;
  void clear(Node parent) override;
  void set_text(Node node, const std::string& text) override;
  void set_attr(Node node, const std::string& key, const std::string& value) override;
  void remove_attr(Node node, const std::string& key) override;
  void set_style(Node node, const std::string& key, const std::string& value) override;
  void set_visible(Node node, bool visible) override;
  std::string get_value(Node node) override;
  void on(Node node, EventKind kind, int handler_id) override;
  void off(Node node, EventKind kind) override;
  Node root() override { return Node{1}; }
  const char* backend_name() const override { return "null"; }

  // ---- inspection (tests) ----
  struct Call {
    std::string method;
    uint64_t node = 0;
    std::string detail;
  };
  const std::vector<Call>& calls() const { return calls_; }
  void clear_calls() { calls_.clear(); }
  // serialize the recorded tree as an indented text dump (test assertions)
  std::string dump() const;
  // number of recorded calls with a given method
  int count_of(const std::string& method) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  uint64_t next_id_ = 2;  // id 1 is the root
  std::vector<Call> calls_;
};

}  // namespace ui
}  // namespace fs
