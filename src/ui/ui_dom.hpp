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
#include <functional>
#include <map>
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
  kBlur = 11,
};

const char* event_kind_name(EventKind k);

// A key press with its modifiers. A terminal cannot work from a formatted
// string: Ctrl+C is not the character 'c', Shift+Tab is not Tab, and paste is
// not typing. The adapter reports the raw facts and the terminal engine
// (fs_vt) turns them into the escape sequence the child expects.
struct KeyPress {
  std::string key;   // "a" | "Enter" | "ArrowUp" | "F1" | "Escape" | "Tab" ...
  std::string text;  // paste payload when paste == true
  bool ctrl = false;
  bool alt = false;
  bool shift = false;
  bool meta = false;
  bool paste = false;
};

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
  // Is this node the focused element? A focused control's value belongs to the
  // USER - the app must not write over it (the composer deliberately does not
  // re-render per keystroke, so a background re-render would otherwise restore
  // the stale value and reset the caret mid-typing).
  virtual bool is_focused(Node node) {
    (void)node;
    return false;
  }

  // --- events ---
  // on() binds the handler for (node, kind), replacing any previous binding
  // for that pair. The framework re-renders the whole tree and mints a fresh
  // handler id each time, so the adapter MUST drop the old listener or every
  // click would fire all stale handlers in order.
  virtual void on(Node node, EventKind kind, int handler_id) = 0;
  virtual void off(Node node, EventKind kind) = 0;

  // --- splitter (drag to resize `side`; browser-only nicety) ---
  // Backends that cannot drag (NullDom) keep the default width.
  // on_resize (optional) reports the new width so the app can persist it in
  // state; without it the width would be lost when the node is re-created.
  virtual void bind_splitter(Node splitter, Node side,
                            std::function<void(int)> on_resize) {
    (void)splitter;
    (void)side;
    (void)on_resize;
  }

  // Same, but for a panel that sits to the LEFT of its divider (the project
  // navigation rail): dragging right must WIDEN it, the mirror of the side
  // panel. Kept separate from bind_splitter so both dividers can be live at the
  // same time and each one only changes its own pair of columns.
  virtual void bind_nav_splitter(Node splitter, Node nav,
                                 std::function<void(int)> on_resize) {
    (void)splitter;
    (void)nav;
    (void)on_resize;
  }

  // --- popover placement ---
  // Position a floating panel next to its anchor, inside the viewport: drop it
  // below the anchor when there is room, flip above when there is not, and keep
  // it horizontally inside the window. The app cannot know pixel geometry, so
  // the adapter measures.
  virtual void place_popup(Node popup, Node anchor) {
    (void)popup;
    (void)anchor;
  }

  // --- popup dismissal ---
  // Report a pointer press that lands outside `popup`. Unlike a full-screen
  // backdrop element this works for popovers rendered INSIDE a small field (a
  // settings row), where a fixed backdrop would cover the field itself.
  virtual void bind_outside_click(Node popup, std::function<void()> cb) {
    (void)popup;
    (void)cb;
  }

  // --- tab reordering (drag a tab onto another tab) ---
  // Reports "fromKey\ttoKey"; the app owns the tab order, the adapter only
  // reports which tab was dropped on which.
  virtual void bind_tab_drag(Node strip, std::function<void(const std::string&)> cb) {
    (void)strip;
    (void)cb;
  }

  // --- vertical splitter (drag the top edge of a bottom panel to set height) ---
  // Same contract as bind_splitter, on the vertical axis: `splitter` sits on
  // `panel`'s TOP edge and dragging up must make the panel taller.
  virtual void bind_vsplitter(Node splitter, Node panel,
                              std::function<void(int)> on_resize) {
    (void)splitter;
    (void)panel;
    (void)on_resize;
  }

  // --- drop zone (drag & drop onto `node`) ---
  // `kind` tells the app what was dropped, because the three cases have
  // completely different handling:
  //   "dir"  - a folder; browsers expose only the *name*, never the absolute
  //            path, so the backend resolves it (project.resolveDrop)
  //   "file" - a file; only the name is available
  //   "url"  - a link dragged from another app (text/uri-list)
  //   "text" - arbitrary dragged text
  virtual void bind_drop_zone(
      Node node, std::function<void(const std::string& kind,
                                    const std::string& value)> cb) {
    (void)node;
    (void)cb;
  }

  // --- global key (document-level) ---
  // Escape etc. must work regardless of focus, so this listens on the document
  // rather than a node (a keydown on <body> never reaches the app's root div).
  // The callback receives the key name ("Escape", "ArrowDown", ...).
  virtual void bind_global_key(std::function<void(const std::string&)> cb) {
    (void)cb;
  }

  // --- terminal keyboard input (node-scoped, with modifiers) ---
  // Unlike the generic kKeyDown binding this also prevents the browser's default
  // action (so Ctrl+W does not close the tab) and reports modifier state, which
  // is what a terminal needs. Paste arrives through the same callback.
  virtual void bind_key_input(Node node, std::function<void(const KeyPress&)> cb) {
    (void)node;
    (void)cb;
  }

  // Focus a node so it starts receiving keys (the terminal surface is focusable
  // but not an <input>: no caret, no browser text editing).
  virtual void focus(Node node) { (void)node; }

  // --- character-cell size (terminal geometry) ---
  // How many character cells fit inside `node`? Only the DOM knows its own pixel
  // size and its font metrics, so the adapter measures and reports; the child
  // process needs this (a full-screen program lays itself out by it, and a shell
  // wraps its prompt to it). Reports once immediately and again on every resize.
  virtual void bind_cell_size(Node node,
                              std::function<void(int cols, int rows)> cb) {
    (void)node;
    (void)cb;
  }

  // --- scroll pinning (terminals) ---
  // Reports whether `node` is scrolled to the bottom, whenever the user scrolls
  // it. The app uses it to decide whether new output should follow: yanking the
  // view back down while someone is reading history is worse than not following.
  virtual void bind_scroll_pin(Node node, std::function<void(bool at_bottom)> cb) {
    (void)node;
    (void)cb;
  }
  // Scroll `node` to the bottom (called after output when the view is pinned).
  virtual void scroll_to_bottom(Node node) { (void)node; }
  // Read / write the vertical scroll offset. The chat history needs both: the
  // offset is saved per session (so switching tabs does not lose your place) and
  // restored on the way back.
  virtual int scroll_top(Node node) {
    (void)node;
    return 0;
  }
  virtual void set_scroll_top(Node node, int top) {
    (void)node;
    (void)top;
  }

  // --- clipboard ---
  // Copying text is a platform action (navigator.clipboard in the browser,
  // Win32 clipboard on desktop), so it lives in the adapter like the file
  // picker: the UI only says "copy this".
  virtual void copy_text(const std::string& text) { (void)text; }

  // --- file picker (upload) ---
  // Clicking `node` opens the OS file chooser. The picked files are sent to the
  // backend (which stores them inside the project, where the model's file tools
  // can read them) and the callback reports the saved paths as JSON:
  //   {"files":[{"name":"a.png","path":"<abs>","size":1234}], "error":"..."}
  // The transfer itself stays on the JS side - the UI never carries file bytes.
  virtual void bind_file_picker(Node node, const std::string& pid,
                                std::function<void(const std::string&)> cb) {
    (void)node;
    (void)pid;
    (void)cb;
  }

  // --- animation (re)start ---
  // Mount-only animations are applied by the CSS class, but a node that already
  // exists gets PATCHED, not created - so an animation declared on a stable-keyed
  // node (a drawer opening, a panel appearing) never fires. This restarts it on
  // the live node: drop the class, force a style flush, add it back.
  virtual void play_animation(Node node, const std::string& cls) {
    (void)node;
    (void)cls;
  }

  // Run `cb` after `ms` milliseconds. Used to remove a subtree once its exit
  // animation has played: that deadline is wall-clock work, not a render, and
  // tying it to the render pump meant a node could stay in the DOM for as long
  // as nothing else re-rendered.
  virtual void schedule(int ms, std::function<void()> cb) {
    (void)ms;
    if (cb) cb();  // backends without timers run it now (no animations there)
  }

  // Can this backend keep a removed subtree alive for its exit animation? The
  // framework asks before delaying a removal: a backend without CSS animations
  // (NullDom, unit tests) must remove immediately, or a test that closes a tab
  // would still see the closed page in the dump.
  virtual bool supports_exit_animations() const { return false; }

  // --- appearance (palette / mode / fonts) ---
  // Applied to the document root, because these are the variables the whole
  // stylesheet is built from. `mode` is "auto" | "light" | "dark"; the adapter
  // resolves "auto" against the OS preference and writes the RESOLVED value, so
  // the CSS only needs two blocks instead of a media query plus overrides.
  // Empty strings mean "leave that part alone".
  struct Appearance {
    std::string palette;     // palette name ("default", "slate", ...)
    std::string mode;        // "auto" | "light" | "dark"
    std::string font;        // UI font family ("" = built-in stack)
    std::string mono;        // monospace family ("" = built-in stack)
    int font_size = 0;       // px (0 = built-in size)
    bool code_highlight = true;
    // Where the top-level tabs live. This is a LAYOUT switch (the strip becomes
    // a vertical side strip), so it has to reach the root element as a data
    // attribute rather than being a class on one container.
    std::string tabs;        // "top" | "left"
  };
  virtual void set_appearance(const Appearance& a) { (void)a; }

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
  bool is_focused(Node node) override;
  void on(Node node, EventKind kind, int handler_id) override;
  void off(Node node, EventKind kind) override;
  void bind_splitter(Node splitter, Node side,
                     std::function<void(int)> on_resize) override;
  void bind_nav_splitter(Node splitter, Node nav,
                         std::function<void(int)> on_resize) override;
  void bind_tab_drag(Node strip, std::function<void(const std::string&)> cb) override;
  void place_popup(Node popup, Node anchor) override;
  void bind_outside_click(Node popup, std::function<void()> cb) override;
  void schedule(int ms, std::function<void()> cb) override;
  void bind_vsplitter(Node splitter, Node panel,
                      std::function<void(int)> on_resize) override;
  void bind_drop_zone(Node node,
                      std::function<void(const std::string&, const std::string&)> cb) override;
  void bind_global_key(std::function<void(const std::string&)> cb) override;
  void bind_key_input(Node node, std::function<void(const KeyPress&)> cb) override;
  void focus(Node node) override;
  void bind_cell_size(Node node, std::function<void(int, int)> cb) override;
  void bind_file_picker(Node node, const std::string& pid,
                        std::function<void(const std::string&)> cb) override;
  void bind_scroll_pin(Node node, std::function<void(bool)> cb) override;
  void scroll_to_bottom(Node node) override;
  int scroll_top(Node node) override;
  void set_scroll_top(Node node, int top) override;
  void copy_text(const std::string& text) override;
  void play_animation(Node node, const std::string& cls) override;
  void set_appearance(const Appearance& a) override;
  // test hook: the last appearance the app applied
  const Appearance& appearance_for_test() const { return appearance_; }
  // test hook: run every pending scheduled callback (tests have no timers)
  void run_scheduled_for_test() {
    std::vector<std::function<void()> > pending = scheduled_;
    scheduled_.clear();
    for (size_t i = 0; i < pending.size(); ++i) pending[i]();
  }
  // test hook: animations the framework asked to (re)play
  const std::vector<std::string>& played_animations() const { return played_anims_; }
  // test hook: simulate the user scrolling the pinned node
  void fire_scroll_pin_for_test(bool at_bottom) {
    if (scroll_pin_cb_) scroll_pin_cb_(at_bottom);
  }
  // test hook: simulate a file picker completing
  void fire_file_pick_for_test(const std::string& reply) {
    if (file_pick_cb_) file_pick_cb_(reply);
  }
  // test hook: simulate a key press (with modifiers) on the bound terminal node
  void fire_key_input_for_test(const KeyPress& k) {
    if (key_input_cb_) key_input_cb_(k);
  }
  // test hook: simulate the adapter measuring the terminal surface
  void fire_cell_size_for_test(int cols, int rows) {
    if (cell_size_cb_) cell_size_cb_(cols, rows);
  }
  // test hook: simulate a drop on the bound zone
  void fire_drop_for_test(const std::string& kind, const std::string& value) {
    if (drop_cb_) drop_cb_(kind, value);
  }
  // test hook: simulate the end of a splitter drag
  void fire_split_for_test(int width) {
    if (split_cb_) split_cb_(width);
  }
  // test hook: simulate the end of a nav-rail drag
  void fire_nav_split_for_test(int width) {
    if (nav_split_cb_) nav_split_cb_(width);
  }
  // test hook: simulate dropping one tab onto another
  void fire_tab_drag_for_test(const std::string& from, const std::string& to) {
    if (tab_drag_cb_) tab_drag_cb_(from + "\t" + to);
  }
  // test hook: simulate a click outside the bound popup
  void fire_outside_click_for_test() {
    if (outside_cb_) outside_cb_();
  }
  // test hook: the last text the app asked to copy
  const std::string& last_copied_for_test() const { return last_copied_; }
  // test hook: simulate the end of a vertical splitter drag
  void fire_vsplit_for_test(int height) {
    if (vsplit_cb_) vsplit_cb_(height);
  }
  // test hook: simulate a global key press
  void fire_key_for_test(const std::string& key) {
    if (key_cb_) key_cb_(key);
  }
  Node root() override { return Node{1}; }
  const char* backend_name() const override { return "null"; }

  // test hook: simulate the user focusing a node (see is_focused)
  void set_focused_for_test(Node n) { focused_for_test_ = n.id; }

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
  std::function<void(const std::string&, const std::string&)> drop_cb_;
  std::function<void(int)> split_cb_;
  std::function<void(int)> nav_split_cb_;
  std::function<void(const std::string&)> tab_drag_cb_;
  std::function<void()> outside_cb_;
  std::vector<std::function<void()> > scheduled_;
  std::string last_copied_;
  // test hook: scroll offsets recorded per node id
  std::map<uint64_t, int> scroll_tops_;
  std::function<void(int)> vsplit_cb_;
  std::function<void(const std::string&)> key_cb_;
  std::function<void(const KeyPress&)> key_input_cb_;
  std::function<void(int, int)> cell_size_cb_;
  std::function<void(const std::string&)> file_pick_cb_;
  std::function<void(bool)> scroll_pin_cb_;
  std::vector<std::string> played_anims_;
  Appearance appearance_;
  // test hook: the node the app treats as focused
  uint64_t focused_for_test_ = 0;
};

}  // namespace ui
}  // namespace fs
