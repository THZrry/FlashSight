// ui_dom_whale.cpp - DOM Adapter over WhaleUI's C DOM API (native desktop build).
//
// The third backend next to WasmDom (browser) and NullDom (tests):
//   * WasmDom   : emscripten/val.h + EM_JS driving the browser DOM
//   * NullDom   : no-op + call log
//   * WhaleDom  : whaleui_dom_* (this file) - the desktop build
//
// Everything above this layer is untouched by the switch: the framework diffs
// VNodes and calls IDom; which DOM is underneath is the adapter's business.
//
// ---------------------------------------------------------------------------
// WHY THIS FILE IS BEHIND A MACRO
//
// WhaleUI is not finished yet (drag & drop is the remaining piece), so the
// desktop path is written and wired but NOT compiled in: build it with
// `xmake f --whaleui=y` once WhaleUI is ready. Until then the native build keeps
// using NullDom and the browser (wasm) build is unaffected. The gaps this
// adapter has to work around, and what must be checked on the first run, are
// listed in doc-internal/ui-whaleui.md.
//
// Two design points worth knowing before reading:
//
//  * Node identity: `Node::id` indexes `slots_`, it is NOT the element pointer.
//    The indirection is needed for TEXT nodes - WhaleUI has no "write the data
//    of this text node" call, so set_text() swaps the underlying lexbor text
//    node while the id (which is what the diff compares) stays the same.
//
//  * Keyboard: the browser build binds listeners to nodes. Here the window key
//    callback is used instead, because WhaleUI's programmatic focus does not
//    move the ENGINE focus (see doc-internal/ui-whaleui.md) - a node-scoped
//    keydown listener would only fire after the user clicked that node. The
//    window callback fires whatever has focus, so the adapter checks
//    document.activeElement itself.
// ---------------------------------------------------------------------------
#include "ui_dom.hpp"

#ifdef FS_UI_WHALEUI

#include <ghc/filesystem.hpp>
#include <whaleui.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <system_error>
#include <vector>

#include "ui_vdom.hpp"  // bus() - the framework's handler registry

namespace fs {
namespace ui {

namespace {

std::string str_of(const char* p) { return p ? std::string(p) : std::string(); }

// One Node handle -> the live WhaleUI object behind it.
struct Slot {
  whaleui_dom_element_t* el = nullptr;
  bool is_text = false;
};

// A bound DOM listener. WhaleUI hands back a plain C callback + userdata, so the
// C++ closure lives here and one trampoline unwraps it.
struct Listener {
  std::string type;  // DOM event name
  whaleui_dom_element_t* el = nullptr;
  std::function<void(whaleui_dom_event_t*)> fn;
};

void trampoline(whaleui_dom_event_t* ev, void* ud) {
  Listener* l = static_cast<Listener*>(ud);
  if (!l || !l->fn) return;
  l->fn(ev);
}

// DOM event name for an EventKind. kKeyEnter has no DOM event of its own: the
// framework wants "Enter was pressed in this control (no Shift)", which the
// browser glue synthesizes from keydown - here it is a keydown listener with a
// filter (see on()).
const char* dom_name(EventKind k) {
  switch (k) {
    case EventKind::kClick: return "click";
    case EventKind::kInput: return "input";
    case EventKind::kChange: return "change";
    case EventKind::kKeyDown: return "keydown";
    case EventKind::kKeyEnter: return "keydown";
    case EventKind::kSubmit: return "submit";
    case EventKind::kDblClick: return "dblclick";
    case EventKind::kContextMenu: return "contextmenu";
    case EventKind::kScroll: return "scroll";
    case EventKind::kMouseEnter: return "mouseenter";
    case EventKind::kMouseLeave: return "mouseleave";
    case EventKind::kBlur: return "blur";
  }
  return "click";
}

// WHALEUI_KEY_* -> the DOM KeyboardEvent.key name the app matches on. The
// terminal engine (fs_vt) and the app's Escape handling both key off these
// names, so they must match the browser glue's spelling exactly.
std::string key_name(int keycode) {
  switch (keycode) {
    case WHALEUI_KEY_ENTER: return "Enter";
    case WHALEUI_KEY_ESCAPE: return "Escape";
    case WHALEUI_KEY_TAB: return "Tab";
    case WHALEUI_KEY_BACKSPACE: return "Backspace";
    case WHALEUI_KEY_DELETE: return "Delete";
    case WHALEUI_KEY_HOME: return "Home";
    case WHALEUI_KEY_END: return "End";
    case WHALEUI_KEY_UP: return "ArrowUp";
    case WHALEUI_KEY_DOWN: return "ArrowDown";
    case WHALEUI_KEY_LEFT: return "ArrowLeft";
    case WHALEUI_KEY_RIGHT: return "ArrowRight";
    case WHALEUI_KEY_SPACE: return " ";
    default: break;
  }
  if (keycode >= 'a' && keycode <= 'z') {
    return std::string(1, static_cast<char>(keycode));
  }
  if (keycode >= 'A' && keycode <= 'Z') {
    return std::string(1, static_cast<char>(keycode));
  }
  if (keycode >= '0' && keycode <= '9') {
    return std::string(1, static_cast<char>(keycode));
  }
  if (keycode >= 1073741882 && keycode <= 1073741941) {  // F1..F24
    char buf[8];
    std::snprintf(buf, sizeof(buf), "F%d", keycode - 1073741881);
    return std::string(buf);
  }
  return std::string();
}

class WhaleDom : public IDom {
 public:
  WhaleDom(whaleui_app_t* app, whaleui_window_t* win) : app_(app), win_(win) {
    doc_ = whaleui_window_get_document(win_);
    slots_.resize(2);  // 0 unused; 1 = root
    // The shell HTML carries an empty <div id="fs-root">; the app renders the
    // whole UI inside it (same contract as the browser backend).
    root_el_ = whaleui_dom_get_element_by_id(doc_, "fs-root");
    if (!root_el_) root_el_ = whaleui_dom_body(doc_);
    slots_[1].el = root_el_;
    // One window key callback drives both the terminal keys and the global
    // (document-level) keys; see the header comment.
    whaleui_window_set_key_callback(win_, &WhaleDom::on_window_key, this);
  }

  ~WhaleDom() override {
    for (std::map<uint64_t, std::map<int, Listener*> >::iterator it =
             kind_l_.begin();
         it != kind_l_.end(); ++it) {
      for (std::map<int, Listener*>::iterator jt = it->second.begin();
           jt != it->second.end(); ++jt) {
        drop_listener(jt->second);
      }
    }
    for (size_t i = 0; i < aux_l_.size(); ++i) drop_listener(aux_l_[i]);
    if (resize_obs_) whaleui_dom_resize_observer_destroy(resize_obs_);
    if (measure_canvas_) whaleui_dom_element_destroy(measure_canvas_);
  }

  // ---- structure ----------------------------------------------------------

  Node create_element(const std::string& tag) override {
    return adopt(whaleui_dom_create_element(doc_, tag.c_str()), false);
  }

  Node create_text(const std::string& text) override {
    return adopt(whaleui_dom_create_text_node(doc_, text.c_str()), true);
  }

  void append(Node parent, Node child) override {
    whaleui_dom_element_t* p = el_of(parent);
    whaleui_dom_element_t* c = el_of(child);
    if (!p || !c) return;
    // never append a node into itself or one of its ancestors (lexbor would
    // build a cycle and every later tree walk would spin forever)
    for (whaleui_dom_element_t* a = p; a; a = whaleui_dom_parent(a)) {
      if (a == c) return;
    }
    whaleui_dom_append_child(p, c);
  }

  void insert_before(Node parent, Node child, Node ref) override {
    whaleui_dom_element_t* p = el_of(parent);
    whaleui_dom_element_t* c = el_of(child);
    if (!p || !c) return;
    whaleui_dom_insert_before(p, c, el_of(ref));
  }

  void remove(Node node) override {
    Slot& s = slot(node);
    if (!s.el) return;
    // Detach + destroy. WhaleUI's element_destroy only frees a DETACHED node,
    // and the framework calls remove() exactly for subtrees it is dropping -
    // without the destroy every re-render would leak what it replaced (a chat
    // log replaces thousands of nodes). Nothing may hold the id afterwards:
    // patch_children only re-inserts nodes that survive the same patch.
    drop_kind_listeners(node);
    drop_aux_listeners_for(s.el);
    whaleui_dom_remove(s.el);
    whaleui_dom_element_destroy(s.el);
    s.el = nullptr;
    s.is_text = false;
  }

  void clear(Node parent) override {
    whaleui_dom_element_t* p = el_of(parent);
    if (!p) return;
    // Children are detached but NOT destroyed: the framework only calls clear()
    // for nodes whose children it still tracks.
    //
    // NOTE: WhaleUI's first_child is the first ELEMENT child (text nodes are
    // skipped), so a text-only parent looks empty here. The framework's diff
    // never calls clear() - it removes nodes individually - so this is a
    // contract gap rather than a live bug; see doc-internal/ui-whaleui.md.
    for (;;) {
      whaleui_dom_element_t* c = whaleui_dom_first_child(p);
      if (!c) break;
      whaleui_dom_remove_child(p, c);
    }
  }

  // ---- content / attributes ----------------------------------------------

  void set_text(Node node, const std::string& text) override {
    Slot& s = slot(node);
    if (!s.el) return;
    if (s.is_text) {
      replace_text_node(s, text);
      return;
    }
    whaleui_dom_set_text(s.el, text.c_str());
  }

  void set_attr(Node node, const std::string& key, const std::string& value) override {
    whaleui_dom_element_t* el = el_of(node);
    if (!el) return;
    // `value` is a PROPERTY on form controls, not an attribute: WhaleUI routes
    // it to the right storage (input attribute / textarea text / selected
    // option), which is what makes clearing the composer actually clear it.
    if (key == "value" && is_control(el)) {
      whaleui_dom_set_value(el, value.c_str());
      return;
    }
    // HTML boolean attributes carry no value, and :checked/:disabled match on
    // PRESENCE - so "0" must REMOVE the attribute rather than set it to "0"
    // (setting "0" would still match: the checkbox that will not turn off).
    if (key == "checked" || key == "disabled" || key == "selected") {
      bool on = value == "1" || value == "true" || value == "checked";
      if (on) {
        whaleui_dom_set_attribute(el, key.c_str(), "");
      } else {
        whaleui_dom_remove_attribute(el, key.c_str());
      }
      return;
    }
    whaleui_dom_set_attribute(el, key.c_str(), value.c_str());
  }

  void remove_attr(Node node, const std::string& key) override {
    whaleui_dom_element_t* el = el_of(node);
    if (!el) return;
    if (key == "value" && is_control(el)) {
      whaleui_dom_set_value(el, "");
      return;
    }
    whaleui_dom_remove_attribute(el, key.c_str());
  }

  void set_style(Node node, const std::string& key, const std::string& value) override {
    whaleui_dom_element_t* el = el_of(node);
    if (!el) return;
    // An empty value means "stop setting this": remove the declaration, so a
    // stale inline style cannot outlive the state that produced it (the
    // terminal drawer sets a height while open and must lose it when closed).
    if (value.empty()) {
      whaleui_dom_style_remove_property(el, key.c_str());
      return;
    }
    whaleui_dom_set_style(el, key.c_str(), value.c_str());
  }

  void set_visible(Node node, bool visible) override {
    set_style(node, "display", visible ? "" : "none");
  }

  std::string get_value(Node node) override {
    return str_of(whaleui_dom_get_value(el_of(node)));
  }

  bool is_focused(Node node) override {
    whaleui_dom_element_t* el = el_of(node);
    return el && whaleui_dom_active_element(doc_) == el;
  }

  // ---- events ------------------------------------------------------------

  void on(Node node, EventKind kind, int handler_id) override {
    off(node, kind);  // replace: the framework mints a new handler id per render
    whaleui_dom_element_t* el = el_of(node);
    if (!el) return;
    Listener* l = new Listener();
    l->type = dom_name(kind);
    l->el = el;
    l->fn = [handler_id, kind](whaleui_dom_event_t* ev) {
      if (kind == EventKind::kKeyEnter) {
        // Enter WITHOUT Shift means "send" in the composer; Shift+Enter must
        // stay a newline, so it is not reported at all.
        if (str_of(whaleui_dom_event_key(ev)) != "Enter") return;
        if (whaleui_dom_event_shift_key(ev)) return;
        whaleui_dom_event_prevent_default(ev);
        bus().fire(handler_id, std::string());
        return;
      }
      bus().fire(handler_id, event_value(kind, ev));
    };
    kind_l_[node.id][static_cast<int>(kind)] = l;
    whaleui_dom_add_event_listener(el, l->type.c_str(), &trampoline, l);
  }

  void off(Node node, EventKind kind) override {
    std::map<uint64_t, std::map<int, Listener*> >::iterator it =
        kind_l_.find(node.id);
    if (it == kind_l_.end()) return;
    std::map<int, Listener*>::iterator jt = it->second.find(static_cast<int>(kind));
    if (jt == it->second.end()) return;
    drop_listener(jt->second);
    it->second.erase(jt);
  }

  // ---- splitter (drag to resize) -----------------------------------------

  void bind_splitter(Node splitter, Node side,
                     std::function<void(int)> on_resize) override {
    bind_drag(hbar_, el_of(splitter), el_of(side), false, on_resize);
  }

  void bind_nav_splitter(Node splitter, Node nav,
                         std::function<void(int)> on_resize) override {
    // The nav rail is LEFT of its divider: the shared drag handler subtracts the
    // delta, so a rightward drag would shrink it. Flip the direction by wrapping
    // the callback is not enough (the drag itself is wrong), so the nav binding
    // gets its own drag state with `grow_with_delta = true`.
    bind_nav_drag(el_of(splitter), el_of(nav), on_resize);
  }

  void bind_tab_drag(Node strip, std::function<void(const std::string&)> cb) override {
    // WhaleUI has no HTML5 drag; a full pointer-based reorder there would need
    // its own hit-testing. The tabs still work, they just cannot be dragged.
    (void)strip;
    (void)cb;
  }

  void place_popup(Node popup, Node anchor) override {
    // Popups are absolutely positioned by the stylesheet; WhaleUI lays them out
    // the same way, so the upward-opening default is already correct there.
    // (No viewport flip: the desktop window is large enough for the picker.)
    (void)popup;
    (void)anchor;
  }

  void bind_outside_click(Node popup, std::function<void()> cb) override {
    // Close on a press anywhere else. The document-level listener is the same
    // mechanism the global-key binding uses.
    whaleui_dom_element_t* el = el_of(popup);
    if (!el) return;
    outside_cb_ = cb;
    bind_aux(outside_, reinterpret_cast<whaleui_dom_element_t*>(doc_), "mousedown",
             [this, el](whaleui_dom_event_t* ev) {
      if (!outside_cb_) return;
      whaleui_dom_element_t* t =
          reinterpret_cast<whaleui_dom_element_t*>(whaleui_dom_event_target(ev));
      while (t) {
        if (t == el) return;                 // inside the popup
        t = whaleui_dom_parent(t);
      }
      outside_cb_();
    });
  }

  void bind_vsplitter(Node splitter, Node panel,
                      std::function<void(int)> on_resize) override {
    bind_drag(vbar_, el_of(splitter), el_of(panel), true, on_resize);
  }

  // ---- drop zone ----------------------------------------------------------

  void bind_drop_zone(
      Node node,
      std::function<void(const std::string&, const std::string&)> cb) override {
    drop_cb_ = cb;
    whaleui_dom_element_t* el = el_of(node);
    if (!el) return;
    // dragover must be cancelled for the engine to treat this element as a drop
    // target (the browser contract: preventDefault in dragover accepts).
    bind_aux(drop_over_, el, "dragover", [](whaleui_dom_event_t* ev) {
      whaleui_dom_event_prevent_default(ev);
    });
    bind_aux(drop_drop_, el, "drop", [this](whaleui_dom_event_t* ev) {
      whaleui_dom_event_prevent_default(ev);
      if (!drop_cb_) return;
      whaleui_data_transfer_t* dt = whaleui_dom_event_data_transfer(ev);
      if (!dt) return;
      std::string kind, value;
      classify_drop(dt, &kind, &value);
      if (value.empty()) return;
      drop_cb_(kind, value);
    });
  }

  // ---- global key (document level) ---------------------------------------
  // Escape must work whatever has focus, so it rides the WINDOW key callback
  // rather than a DOM listener: the engine dispatches keydown to its own focus
  // element, which can be null (nothing clicked yet) and then no DOM listener
  // would ever see the key. The window callback always fires.
  void bind_global_key(std::function<void(const std::string&)> cb) override {
    global_key_cb_ = cb;  // delivered by on_window_key (installed in the ctor)
  }

  // ---- terminal keyboard input (node-scoped, with modifiers) -------------
  // A DOM keydown listener, NOT the window callback: a terminal needs the
  // modifier state (Ctrl+C is a byte, not the letter 'c', and Shift+Tab is not
  // Tab) and the window callback carries only the keycode. The engine
  // dispatches keydown to the element it considers focused, which a mouse press
  // on the surface sets - so the terminal must be clicked once before it takes
  // keys (WhaleUI's programmatic focus does not move the engine focus; see
  // doc-internal/ui-whaleui.md).
  void bind_key_input(Node node, std::function<void(const KeyPress&)> cb) override {
    key_input_cb_ = cb;
    whaleui_dom_element_t* el = el_of(node);
    if (!el) return;
    bind_aux(key_down_, el, "keydown", [this](whaleui_dom_event_t* ev) {
      if (!key_input_cb_) return;
      KeyPress k = key_press(ev);
      // Ctrl+V: the surface is not an editable control, so the engine's own
      // paste path never runs for it - read the system clipboard here.
      if (k.ctrl && (k.key == "v" || k.key == "V")) {
        k.key.clear();
        k.paste = true;
        k.text = str_of(whaleui_clipboard_get_text());
        if (k.text.empty()) return;
        whaleui_dom_event_prevent_default(ev);
      }
      key_input_cb_(k);
    });
  }

  void focus(Node node) override {
    whaleui_dom_element_t* el = el_of(node);
    if (el) whaleui_dom_focus(el);
  }

  // ---- character-cell size (terminal geometry) ---------------------------

  void bind_cell_size(Node node, std::function<void(int, int)> cb) override {
    cell_cb_ = cb;
    whaleui_dom_element_t* el = el_of(node);
    if (!el) return;
    // One observer per element: WhaleUI's observer carries a SINGLE callback
    // for every element it watches, so two terminals under one observer would
    // both report through the last-registered callback.
    if (cell_el_ == el && resize_obs_) return;
    if (resize_obs_) {
      whaleui_dom_resize_observer_destroy(resize_obs_);
      resize_obs_ = nullptr;
    }
    cell_el_ = el;
    resize_obs_ = whaleui_dom_create_resize_observer(doc_);
    if (resize_obs_ &&
        whaleui_dom_resize_observer_observe(resize_obs_, el,
                                            &WhaleDom::on_cell_resize, this) == 0) {
      // the observer reports the initial size on its first frame
      return;
    }
    // no observer: measure once, so the child process still gets a usable grid
    // (it just will not follow a later resize)
    report_cells(el, 0, 0);
  }

  // ---- scroll -------------------------------------------------------------

  void bind_scroll_pin(Node node, std::function<void(bool)> cb) override {
    scroll_pin_cb_ = cb;
    (void)node;
    // WhaleUI exposes no scroll-position read (see doc-internal/ui-whaleui.md),
    // so "is the view at the bottom?" cannot be answered yet. Reporting `true`
    // keeps the useful half of the contract: new output follows the view.
    if (scroll_pin_cb_) scroll_pin_cb_(true);
  }

  void scroll_to_bottom(Node node) override {
    (void)node;
    // Needs a scrollTop/scrollTo element API in WhaleUI; see the doc.
  }

  // ---- file picker (upload) ----------------------------------------------

  void bind_file_picker(Node node, const std::string& pid,
                        std::function<void(const std::string&)> cb) override {
    (void)node;
    (void)pid;
    (void)cb;
    // A native chooser needs an FsApi entry point (folder_picker has the FOLDER
    // dialog only). Until that exists the button does nothing rather than
    // reporting a fake success; see doc-internal/ui-whaleui.md.
  }

  // ---- animation (re)start ------------------------------------------------

  void play_animation(Node node, const std::string& cls) override {
    whaleui_dom_element_t* el = el_of(node);
    if (!el || cls.empty()) return;
    whaleui_dom_class_remove(el, cls.c_str());
    // Re-add on the NEXT loop iteration: dropping and re-adding inside one call
    // is coalesced by the style pass, so the animation would never replay.
    Pending* p = new Pending();
    p->el = el;
    p->cls = cls;
    whaleui_set_timeout(&WhaleDom::apply_pending_anim, p, 0);
  }

  // WhaleUI can run CSS animations, so the two-view exit transition works there
  // too; keeping the old subtree alive is the framework's decision, not the
  // backend's.
  bool supports_exit_animations() const override { return true; }

  // ---- appearance ---------------------------------------------------------

  void set_appearance(const Appearance& a) override {
    whaleui_dom_element_t* root = whaleui_dom_document_element(doc_);
    if (!root) return;
    // The stylesheet is variable-driven and reads these off :root, so this is
    // the whole theming surface (see the web/style.css header).
    std::string mode = a.mode;
    if (mode != "light" && mode != "dark") {
      mode = whaleui_app_resolved_theme(app_) == WHALEUI_THEME_DARK ? "dark"
                                                                   : "light";
    }
    // A palette is a class; the resolved mode is an attribute, so the two
    // compose (palette = which hues, mode = light/dark of that palette).
    std::string want = a.palette.empty() || a.palette == "default"
                           ? std::string()
                           : ("palette-" + a.palette);
    std::string classes;
    {
      std::string cur = str_of(whaleui_dom_get_attribute(root, "class"));
      size_t i = 0;
      while (i < cur.size()) {
        size_t sp = cur.find(' ', i);
        std::string tok = cur.substr(i, sp == std::string::npos ? sp : sp - i);
        if (!tok.empty() && tok.compare(0, 8, "palette-") != 0) {
          if (!classes.empty()) classes += " ";
          classes += tok;
        }
        if (sp == std::string::npos) break;
        i = sp + 1;
      }
      if (!want.empty()) classes = classes.empty() ? want : (classes + " " + want);
    }
    whaleui_dom_set_attribute(root, "class", classes.c_str());
    whaleui_dom_set_attribute(root, "data-mode", mode.c_str());
    // Tab placement is a layout switch (horizontal strip vs vertical side
    // strip), so it is an attribute on the root, like the mode.
    if (!a.tabs.empty()) {
      whaleui_dom_set_attribute(root, "data-tabs", a.tabs.c_str());
    }
    whaleui_dom_set_attribute(root, "data-code-highlight",
                              a.code_highlight ? "1" : "0");
    set_root_var(root, "--font-user", a.font, "--font-base");
    set_root_var(root, "--mono-user", a.mono, "--mono-base");
    set_root_var(root, "--font-size-user",
                 a.font_size > 0 ? (std::to_string(a.font_size) + "px")
                                 : std::string(),
                 nullptr);
  }

  // ---- root / identity ----------------------------------------------------

  Node root() override { return Node{1}; }
  const char* backend_name() const override { return "whaleui"; }

 private:
  // ---- value reporting (mirrors the browser glue) -------------------------

  static std::string event_value(EventKind kind, whaleui_dom_event_t* ev) {
    if (kind == EventKind::kKeyDown) {
      return str_of(whaleui_dom_event_key(ev));
    }
    whaleui_dom_element_t* el = whaleui_dom_event_target(ev);
    if (!el) return std::string();
    if (is_control(el)) {
      // a checkbox/radio reports its checkedness, not its value attribute
      if (str_of(whaleui_dom_tag_name(el)) == "input") {
        std::string type = str_of(whaleui_dom_get_attribute(el, "type"));
        if (type == "checkbox" || type == "radio") {
          return whaleui_dom_has_attribute(el, "checked") ? "1" : "0";
        }
      }
      return str_of(whaleui_dom_get_value(el));
    }
    return str_of(whaleui_dom_get_text(el));
  }

  static bool is_control(whaleui_dom_element_t* el) {
    std::string tag = str_of(whaleui_dom_tag_name(el));
    return tag == "input" || tag == "textarea" || tag == "select";
  }

  // What was dropped? WhaleUI hands over the REAL path for an OS drop (the
  // browser only ever had the file name), so a folder can be opened directly
  // instead of being searched for by name.
  static void classify_drop(whaleui_data_transfer_t* dt, std::string* kind,
                            std::string* value) {
    if (whaleui_data_transfer_file_count(dt) > 0) {
      const whaleui_file_t* f = whaleui_data_transfer_file_item(dt, 0);
      std::string uri = str_of(whaleui_file_uri(f));
      std::string name = str_of(whaleui_file_name(f));
      std::string path = uri;
      if (path.compare(0, 7, "file://") == 0) path = path.substr(7);
      std::error_code ec;
      if (!path.empty() && ghc::filesystem::is_directory(path, ec)) {
        *kind = "dir";
        *value = path;
        return;
      }
      // A file drop keeps its full path: the model's file tools can read it
      // directly, which a bare name could not.
      *kind = "file";
      *value = !path.empty() ? path : name;
      return;
    }
    std::string uri_list =
        str_of(whaleui_data_transfer_get_data(dt, "text/uri-list"));
    if (!uri_list.empty()) {
      size_t nl = uri_list.find('\n');
      std::string first =
          nl == std::string::npos ? uri_list : uri_list.substr(0, nl);
      if (!first.empty() && first[0] != '#') {
        *kind = "url";
        *value = first;
        return;
      }
    }
    std::string text = str_of(whaleui_data_transfer_get_text(dt));
    if (!text.empty()) {
      *kind = "text";
      *value = text;
    }
  }

  void set_root_var(whaleui_dom_element_t* root, const char* var,
                    const std::string& user, const char* fallback) {
    if (user.empty()) {
      whaleui_dom_style_remove_property(root, var);
      return;
    }
    // The stylesheet composes the user's family in front of the built-in stack
    // (`--font: var(--font-user, var(--font-base))`), so an empty setting must
    // remove the variable rather than set it to "".
    std::string v = fallback ? ("\"" + user + "\", var(" + fallback + ")") : user;
    whaleui_dom_set_style(root, var, v.c_str());
  }

  // ---- slot bookkeeping ---------------------------------------------------

  Slot& slot(Node n) {
    if (n.id >= slots_.size()) slots_.resize(static_cast<size_t>(n.id) + 1);
    return slots_[static_cast<size_t>(n.id)];
  }
  whaleui_dom_element_t* el_of(Node n) {
    if (!n.valid() || n.id >= slots_.size()) return nullptr;
    return slots_[static_cast<size_t>(n.id)].el;
  }
  Node adopt(whaleui_dom_element_t* el, bool is_text) {
    if (!el) return Node{0};
    Node n{next_id_++};
    Slot& s = slot(n);
    s.el = el;
    s.is_text = is_text;
    return n;
  }

  // A text node's content cannot be written in place through WhaleUI's public
  // API, so swap the node: the handle the diff holds (our id) stays valid.
  void replace_text_node(Slot& s, const std::string& text) {
    whaleui_dom_element_t* old = s.el;
    whaleui_dom_element_t* parent = whaleui_dom_parent(old);
    whaleui_dom_element_t* fresh = whaleui_dom_create_text_node(doc_, text.c_str());
    if (!fresh) return;
    if (parent) whaleui_dom_insert_before(parent, fresh, old);
    whaleui_dom_remove(old);
    whaleui_dom_element_destroy(old);
    s.el = fresh;
    s.is_text = true;
  }

  // ---- listener bookkeeping ----------------------------------------------

  void drop_listener(Listener* l) {
    if (!l) return;
    whaleui_dom_remove_event_listener(l->el, l->type.c_str(), &trampoline, l);
    delete l;
  }

  void drop_kind_listeners(Node node) {
    std::map<uint64_t, std::map<int, Listener*> >::iterator it =
        kind_l_.find(node.id);
    if (it == kind_l_.end()) return;
    for (std::map<int, Listener*>::iterator jt = it->second.begin();
         jt != it->second.end(); ++jt) {
      drop_listener(jt->second);
    }
    kind_l_.erase(it);
  }

  void drop_aux_listeners_for(whaleui_dom_element_t* el) {
    for (size_t i = 0; i < aux_l_.size();) {
      if (aux_l_[i]->el == el) {
        drop_listener(aux_l_[i]);
        aux_l_.erase(aux_l_.begin() + static_cast<long>(i));
      } else {
        ++i;
      }
    }
    Listener** slots[] = {&hbar_, &vbar_, &navbar_, &key_down_, &drop_over_,
                          &drop_drop_, &outside_};
    for (size_t i = 0; i < sizeof(slots) / sizeof(slots[0]); ++i) {
      if (*slots[i] && (*slots[i])->el == el) *slots[i] = nullptr;
    }
  }

  // Bind one auxiliary listener into a slot, replacing whatever was there.
  // Aux slots are the ones the app drives directly (drop zone, splitters)
  // rather than through an EventKind.
  void bind_aux(Listener*& slot_ref, whaleui_dom_element_t* el, const char* type,
                std::function<void(whaleui_dom_event_t*)> fn) {
    if (!el) return;
    if (slot_ref && slot_ref->el == el && slot_ref->type == type) {
      slot_ref->fn = fn;  // same target: swap the closure, keep the listener
      return;
    }
    if (slot_ref) {
      for (size_t i = 0; i < aux_l_.size(); ++i) {
        if (aux_l_[i] == slot_ref) {
          aux_l_.erase(aux_l_.begin() + static_cast<long>(i));
          break;
        }
      }
      drop_listener(slot_ref);
      slot_ref = nullptr;
    }
    Listener* l = new Listener();
    l->type = type;
    l->el = el;
    l->fn = fn;
    aux_l_.push_back(l);
    slot_ref = l;
    whaleui_dom_add_event_listener(el, type, &trampoline, l);
  }

  // ---- splitter drag ------------------------------------------------------

  void bind_drag(Listener*& slot_ref, whaleui_dom_element_t* bar,
                 whaleui_dom_element_t* target, bool vertical,
                 std::function<void(int)> on_resize) {
    if (!bar) return;
    Drag& d = vertical ? vdrag_ : hdrag_;
    d.vertical = vertical;
    d.grow_with_delta = false;
    d.target = target;
    d.cb = on_resize;
    bind_aux(slot_ref, bar, "mousedown", [this, vertical](whaleui_dom_event_t* ev) {
      Drag& dr = vertical ? vdrag_ : hdrag_;
      if (!dr.target || !dr.cb) return;
      whaleui_dom_rect_t r;
      if (whaleui_dom_get_bounding_client_rect(dr.target, &r) != 0) return;
      dr.active = true;
      dr.moved = false;
      dr.start = vertical ? whaleui_dom_event_mouse_y(ev)
                          : whaleui_dom_event_mouse_x(ev);
      dr.start_size = static_cast<int>(vertical ? r.height : r.width);
      dr.size = dr.start_size;
      drag_live_ = &dr;  // one session at a time; the doc handlers resolve it
      whaleui_dom_element_t* docel = reinterpret_cast<whaleui_dom_element_t*>(doc_);
      whaleui_dom_add_event_listener(docel, "mousemove", &WhaleDom::on_drag_move,
                                     this);
      whaleui_dom_add_event_listener(docel, "mouseup", &WhaleDom::on_drag_up, this);
    });
  }

  // The nav rail sits LEFT of its divider, so it grows when the pointer moves
  // RIGHT. It has its own Drag state so the side panel's divider keeps working
  // at the same time and neither drag moves the other column.
  void bind_nav_drag(whaleui_dom_element_t* bar, whaleui_dom_element_t* nav,
                     std::function<void(int)> on_resize) {
    if (!bar) return;
    Drag& d = navdrag_;
    d.vertical = false;
    d.grow_with_delta = true;
    d.target = nav;
    d.cb = on_resize;
    bind_aux(navbar_, bar, "mousedown", [this](whaleui_dom_event_t* ev) {
      Drag& dr = navdrag_;
      if (!dr.target || !dr.cb) return;
      whaleui_dom_rect_t r;
      if (whaleui_dom_get_bounding_client_rect(dr.target, &r) != 0) return;
      dr.active = true;
      dr.moved = false;
      dr.start = whaleui_dom_event_mouse_x(ev);
      dr.start_size = static_cast<int>(r.width);
      dr.size = dr.start_size;
      drag_live_ = &dr;
      whaleui_dom_element_t* docel = reinterpret_cast<whaleui_dom_element_t*>(doc_);
      whaleui_dom_add_event_listener(docel, "mousemove", &WhaleDom::on_drag_move,
                                     this);
      whaleui_dom_add_event_listener(docel, "mouseup", &WhaleDom::on_drag_up, this);
    });
  }

  static void on_drag_move(whaleui_dom_event_t* ev, void* ud) {
    WhaleDom* self = static_cast<WhaleDom*>(ud);
    if (!self || !self->drag_live_) return;
    Drag& d = *self->drag_live_;
    if (!d.active || !d.target) return;
    int pos = d.vertical ? whaleui_dom_event_mouse_y(ev)
                         : whaleui_dom_event_mouse_x(ev);
    int delta = pos - d.start;
    if (delta > 3 || delta < -3) d.moved = true;
    if (!d.moved) return;
    // The handle sits on the panel's near edge, so dragging TOWARD the panel
    // (left on the side divider, up on the bottom drawer) grows it. The nav rail
    // is on the far side of its divider, so there the delta is added.
    int size = d.grow_with_delta ? (d.start_size + delta) : (d.start_size - delta);
    if (d.vertical) {
      if (size < 120) size = 120;
      int w = 0, h = 0;
      whaleui_window_get_size(self->win_, &w, &h);
      int maxh = static_cast<int>(h * 0.85);
      if (size > maxh) size = maxh;
      whaleui_dom_set_style(d.target, "height",
                            (std::to_string(size) + "px").c_str());
    } else if (d.grow_with_delta) {
      if (size < 160) size = 160;
      if (size > 480) size = 480;
      whaleui_dom_set_style(d.target, "width",
                            (std::to_string(size) + "px").c_str());
      whaleui_dom_set_style(d.target, "flex", "0 0 auto");
    } else {
      if (size < 220) size = 220;
      if (size > 720) size = 720;
      whaleui_dom_set_style(d.target, "width",
                            (std::to_string(size) + "px").c_str());
      whaleui_dom_set_style(d.target, "flex", "0 0 auto");
    }
    d.size = size;
  }

  static void on_drag_up(whaleui_dom_event_t* ev, void* ud) {
    (void)ev;
    WhaleDom* self = static_cast<WhaleDom*>(ud);
    if (!self) return;
    whaleui_dom_element_t* docel = reinterpret_cast<whaleui_dom_element_t*>(self->doc_);
    whaleui_dom_remove_event_listener(docel, "mousemove", &WhaleDom::on_drag_move,
                                      self);
    whaleui_dom_remove_event_listener(docel, "mouseup", &WhaleDom::on_drag_up, self);
    if (!self->drag_live_) return;
    Drag& d = *self->drag_live_;
    self->drag_live_ = nullptr;
    if (!d.active) return;
    d.active = false;
    if (!d.moved) return;  // a plain click on the divider: nothing to persist
    if (d.cb) d.cb(d.size);
  }

  // ---- terminal cell measurement -----------------------------------------

  static void on_cell_resize(whaleui_dom_element_t* el, float w, float h,
                             void* ud) {
    WhaleDom* self = static_cast<WhaleDom*>(ud);
    if (self) self->report_cells(el, w, h);
  }

  void report_cells(whaleui_dom_element_t* el, float w, float h) {
    if (!cell_cb_ || !el) return;
    if (w <= 0 || h <= 0) {
      whaleui_dom_rect_t r;
      if (whaleui_dom_get_bounding_client_rect(el, &r) != 0) return;
      w = r.width;
      h = r.height;
    }
    if (w <= 0 || h <= 0) return;
    // Measure one character with the surface's own font. A hidden <canvas> is
    // the only text-measuring API WhaleUI exposes (there is no
    // measureText-on-an-element call), so the font string is read off the
    // surface's computed style and handed to the canvas.
    if (!measure_ctx_) {
      measure_canvas_ = whaleui_dom_create_element(doc_, "canvas");
      measure_ctx_ = whaleui_canvas_get_context(measure_canvas_, "2d");
      if (!measure_ctx_) return;
    }
    std::string font = "12px monospace";
    double pad = 0.0;
    whaleui_style_declaration_t* cs = whaleui_dom_get_computed_style(el, nullptr);
    if (cs) {
      char fs_buf[32] = {0}, fam_buf[192] = {0};
      char padl[32] = {0}, padr[32] = {0};
      bool have_fs = whaleui_style_declaration_get(cs, "font-size", fs_buf,
                                                   sizeof(fs_buf)) == 0;
      bool have_fam = whaleui_style_declaration_get(cs, "font-family", fam_buf,
                                                    sizeof(fam_buf)) == 0;
      if (have_fs && have_fam) font = std::string(fs_buf) + " " + fam_buf;
      if (whaleui_style_declaration_get(cs, "padding-left", padl, sizeof(padl)) == 0) {
        pad += std::atof(padl);
      }
      if (whaleui_style_declaration_get(cs, "padding-right", padr, sizeof(padr)) == 0) {
        pad += std::atof(padr);
      }
      whaleui_style_declaration_destroy(cs);
    }
    if (whaleui_canvas_set_font(measure_ctx_, font.c_str()) != 0) return;
    int w10 = 0, lh = 0;
    if (whaleui_canvas_measure_text(measure_ctx_, "MMMMMMMMMM", &w10, &lh) != 0) return;
    if (w10 <= 0 || lh <= 0) return;
    double adv = static_cast<double>(w10) / 10.0;
    int cols = static_cast<int>((static_cast<double>(w) - pad) / adv);
    int rows = static_cast<int>(static_cast<double>(h) / lh);
    if (cols < 20) cols = 20;
    if (rows < 5) rows = 5;
    cell_cb_(cols, rows);
  }

  // ---- keyboard (window level) -------------------------------------------
  // Only Escape is delivered here (the app's global key contract): everything
  // else the app cares about is bound to a node. The engine gives this callback
  // no modifiers, and the app's global handler only matches "Escape", so that
  // is all this reports.
  static void on_window_key(whaleui_app_t* app, int keycode, int pressed,
                            void* ud) {
    (void)app;
    WhaleDom* self = static_cast<WhaleDom*>(ud);
    if (!self || !pressed || !self->global_key_cb_) return;
    std::string key = key_name(keycode);
    if (key.empty()) return;
    self->global_key_cb_(key);
  }

  // The key + modifiers of a DOM keydown, in the shape the terminal engine
  // (fs_vt) expects.
  static KeyPress key_press(whaleui_dom_event_t* ev) {
    KeyPress k;
    k.key = str_of(whaleui_dom_event_key(ev));
    if (k.key.empty()) {
      // fall back to the keycode when the engine has no name for it
      k.key = key_name(whaleui_dom_event_key_code(ev));
    }
    int mods = whaleui_dom_event_mods(ev);
    k.ctrl = (mods & WHALEUI_MOD_CTRL) != 0;
    k.alt = (mods & WHALEUI_MOD_ALT) != 0;
    k.shift = (mods & WHALEUI_MOD_SHIFT) != 0;
    k.meta = (mods & WHALEUI_MOD_META) != 0;
    return k;
  }

  // ---- deferred animation replay -----------------------------------------

  struct Pending {
    whaleui_dom_element_t* el = nullptr;
    std::string cls;
  };

  static void apply_pending_anim(double ts, void* ud) {
    (void)ts;
    Pending* p = static_cast<Pending*>(ud);
    if (!p) return;
    whaleui_dom_class_add(p->el, p->cls.c_str());
    delete p;
  }

  whaleui_app_t* app_ = nullptr;
  whaleui_window_t* win_ = nullptr;
  whaleui_dom_document_t* doc_ = nullptr;
  whaleui_dom_element_t* root_el_ = nullptr;
  std::vector<Slot> slots_;
  uint64_t next_id_ = 2;

  std::map<uint64_t, std::map<int, Listener*> > kind_l_;
  std::vector<Listener*> aux_l_;
  // auxiliary listener slots (one live listener each)
  Listener* hbar_ = nullptr;      // side-panel divider
  Listener* vbar_ = nullptr;      // bottom-drawer grip
  Listener* navbar_ = nullptr;    // project nav rail divider
  Listener* outside_ = nullptr;   // document-level press outside a popup
  Listener* key_down_ = nullptr;  // terminal keydown
  Listener* drop_over_ = nullptr;
  Listener* drop_drop_ = nullptr;
  std::function<void(const std::string&, const std::string&)> drop_cb_;
  std::function<void()> outside_cb_;  // press outside an open popup
  std::function<void(const std::string&)> global_key_cb_;
  std::function<void(const KeyPress&)> key_input_cb_;
  std::function<void(int, int)> cell_cb_;
  std::function<void(bool)> scroll_pin_cb_;

  struct Drag {
    bool active = false;
    bool moved = false;
    bool vertical = false;
    bool grow_with_delta = false;  // nav rail: it is LEFT of the divider
    whaleui_dom_element_t* target = nullptr;
    int start = 0;
    int start_size = 0;
    int size = 0;
    std::function<void(int)> cb;
  };
  Drag hdrag_;
  Drag vdrag_;
  Drag navdrag_;
  Drag* drag_live_ = nullptr;  // the session the doc handlers resolve

  whaleui_resize_observer_t* resize_obs_ = nullptr;
  whaleui_dom_element_t* cell_el_ = nullptr;
  whaleui_dom_element_t* measure_canvas_ = nullptr;
  whaleui_canvas_context_t* measure_ctx_ = nullptr;
};

}  // namespace

// Factory used by the desktop entry point (ui_main_whale.cpp).
IDom* make_whale_dom(whaleui_app_t* app, whaleui_window_t* win) {
  return new WhaleDom(app, win);
}

}  // namespace ui
}  // namespace fs

#endif  // FS_UI_WHALEUI
