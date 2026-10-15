// ui_dom_wasm.cpp - DOM Adapter over the real browser DOM (wasm build only).
//
// Uses emscripten/val.h for node creation and mutation, and EM_JS for event
// binding (a JS closure has to be created on the JS side; EM_JS keeps that in
// this one file instead of a separate glue script, which also keeps the build
// to a single target with no extra static assets).
//
// Compiled only for the wasm target; the native build uses ui_dom_null.cpp.
#include "ui_dom.hpp"

#ifdef __EMSCRIPTEN__

#include <emscripten.h>
#include <emscripten/val.h>

#include <map>

namespace fs {
namespace ui {

using emscripten::val;

// ---- JS helpers -----------------------------------------------------------
// __fsNodes    : JS-side table id -> Element (so EM_JS event wrappers can reach
//                the element without C++ -> JS object passing).
// __fsHandlers : id -> { kind -> [eventName, listener] } so a rebind can remove
//                the previous listener (the framework mints new handler ids on
//                every render; without this, clicks would fire stale handlers).
EM_JS(void, fs_dom_boot, (), {
  if (!globalThis.__fsNodes) globalThis.__fsNodes = [null];
  if (!globalThis.__fsHandlers) globalThis.__fsHandlers = [];
});

EM_JS(void, fs_dom_off, (int node_id, int kind), {
  var h = globalThis.__fsHandlers[node_id];
  if (h && h[kind]) {
    var el = globalThis.__fsNodes[node_id];
    if (el) el.removeEventListener(h[kind][0], h[kind][1], false);
    delete h[kind];
  }
});

// drop a removed node from the JS-side tables (called after removeChild)
EM_JS(void, fs_dom_forget, (int node_id), {
  if (globalThis.__fsNodes) globalThis.__fsNodes[node_id] = undefined;
  if (globalThis.__fsHandlers) globalThis.__fsHandlers[node_id] = undefined;
});

EM_JS(void, fs_dom_bind, (int node_id, int kind, int hid), {
  var el = globalThis.__fsNodes[node_id];
  if (!el) return;
  if (!globalThis.__fsHandlers[node_id]) globalThis.__fsHandlers[node_id] = {};
  var store = globalThis.__fsHandlers[node_id];
  if (store[kind]) el.removeEventListener(store[kind][0], store[kind][1], false);
  var names = ['click', 'input', 'change', 'keydown', 'keyenter', 'submit',
               'dblclick', 'contextmenu', 'scroll', 'mouseenter', 'mouseleave'];
  var name = names[kind] || 'click';
  var evName = (name === 'keyenter') ? 'keydown' : name;
  var listener = function (evt) {
    // Enter in a textarea sends unless Shift is held; other keydowns are not
    // forwarded at all (so plain Enter-to-send still allows newlines).
    if (name === 'keyenter') {
      if (evt.key !== 'Enter' || evt.shiftKey) return;
      evt.preventDefault();
    }
    var v = "";
    try {
      var t = el.tagName;
      if (name === 'keydown') {
        // keydown carries the key name (not the element value) so handlers can
        // react to Escape / arrow keys; kKeyEnter handles Enter separately.
        v = evt.key === undefined ? "" : String(evt.key);
      } else if (t === 'INPUT' && el.type === 'checkbox') {
        v = el.checked ? '1' : '0';
      } else if (t === 'INPUT' || t === 'TEXTAREA' || t === 'SELECT') {
        v = el.value === undefined ? "" : String(el.value);
      } else {
        v = el.textContent || "";
      }
    } catch (e) { v = ""; }
    if (typeof Module !== 'undefined' && Module._fs_ui_fire) {
      var len = Module.lengthBytesUTF8(v) + 1;
      var ptr = Module._malloc(len);
      Module.stringToUTF8(v, ptr, len);
      Module._fs_ui_fire(hid, ptr);
      Module._free(ptr);
    }
  };
  store[kind] = [evName, listener];
  el.addEventListener(evName, listener, false);
});

// Bind a drag&drop zone. Three very different payloads arrive here:
//   * a folder  -> only its *name* is exposed by the browser (never the path),
//                  so the app asks the backend to resolve it
//   * a file    -> same, name only
//   * a link    -> text/uri-list (dragging a URL from another app)
//   * plain text
// `kind` tells C++ which one it got; the value is the name/url/text.
EM_JS(void, fs_dom_drop_zone, (int node_id, int cb_id), {
  var el = globalThis.__fsNodes[node_id];
  if (!el) return;
  // Rebinding (new render generation) just swaps the callback id; the
  // listeners are installed once and read the id at drop time.
  el.__fsDropCb = cb_id;
  if (el.__fsDropInit) return;
  el.__fsDropInit = true;
  el.addEventListener('dragover', function (ev) {
    ev.preventDefault();
    try { ev.dataTransfer.dropEffect = 'copy'; } catch (e) {}
    el.classList.add('drop-hover');
  });
  el.addEventListener('dragleave', function (ev) {
    // dragleave also fires when moving between children; ignore those so the
    // highlight does not flicker while dragging across the window
    if (ev.relatedTarget && el.contains(ev.relatedTarget)) return;
    el.classList.remove('drop-hover');
  });
  el.addEventListener('drop', function (ev) {
    ev.preventDefault();
    el.classList.remove('drop-hover');
    var kind = 'text', value = "";
    try {
      var dt = ev.dataTransfer;
      var items = dt.items;
      if (items && items.length && items[0].webkitGetAsEntry) {
        var entry = items[0].webkitGetAsEntry();
        if (entry) {
          kind = entry.isDirectory ? 'dir' : 'file';
          value = entry.name;
        }
      }
      if (!value && dt.files && dt.files.length) {
        kind = 'file';
        value = dt.files[0].name;
      }
      if (!value && dt.getData) {
        // a dragged link arrives as text/uri-list, one URI per line
        var uri = dt.getData('text/uri-list');
        if (uri) {
          var nl = uri.indexOf(String.fromCharCode(10));
          var first = (nl >= 0 ? uri.substring(0, nl) : uri).replace(
              String.fromCharCode(13), "");
          if (first && first.charAt(0) !== '#') { kind = 'url'; value = first; }
        }
      }
      if (!value && dt.getData) {
        value = dt.getData('text/plain') || "";
        kind = 'text';
      }
    } catch (e) { value = value || ""; }
    if (!value) return;
    var klen = Module.lengthBytesUTF8(kind) + 1;
    var vlen = Module.lengthBytesUTF8(value) + 1;
    var kptr = Module._malloc(klen), vptr = Module._malloc(vlen);
    Module.stringToUTF8(kind, kptr, klen);
    Module.stringToUTF8(value, vptr, vlen);
    Module._fs_ui_drop(el.__fsDropCb, kptr, vptr);
    Module._free(kptr);
    Module._free(vptr);
  });
});

// Drag the divider to resize `side` horizontally. Clamped to [minW, maxW].
// The new width is applied inline during the drag; on release the width is
// reported to C++ (fs_ui_split) so the app can persist it in state - otherwise
// the width would be lost as soon as the node is re-created.
//
// The collapse handle rides ON the divider, so a press there must not start a
// drag, and a drag must not be mistaken for a handle click (otherwise the
// panel would collapse the moment you let go).
EM_JS(void, fs_dom_splitter, (int split_node, int side_node, int cb_id), {
  var bar = globalThis.__fsNodes[split_node];
  var side = globalThis.__fsNodes[side_node];
  if (!bar || !side) return;
  // rebinding just swaps the callback id; listeners install once
  bar.__fsSplitCb = cb_id;
  if (bar.__fsSplitInit) return;
  bar.__fsSplitInit = true;
  bar.style.cursor = 'col-resize';
  bar.addEventListener('mousedown', function (down) {
    // a press on the handle is a click, not a drag
    var t = down.target;
    if (t && t.classList && t.classList.contains('splitter-handle')) return;
    down.preventDefault();
    var startX = down.clientX;
    var startW = side.getBoundingClientRect().width;
    var moved = false;
    function move(ev) {
      var dx = ev.clientX - startX;
      if (!moved && Math.abs(dx) > 3) moved = true;
      if (!moved) return;
      // The divider sits on the side panel's LEFT edge: dragging left must
      // widen the panel, so the delta is subtracted (not added).
      var w = startW - dx;
      if (w < 220) w = 220;
      if (w > 720) w = 720;
      side.style.width = w + 'px';
      side.style.flex = '0 0 auto';
    }
    function up() {
      document.removeEventListener('mousemove', move);
      document.removeEventListener('mouseup', up);
      if (!moved) return;  // a plain click on the divider: nothing to persist
      // swallow the click that follows a drag so it cannot toggle the panel
      var swallow = function (ev) { ev.stopPropagation(); ev.preventDefault(); };
      document.addEventListener('click', swallow, true);
      setTimeout(function () { document.removeEventListener('click', swallow, true); }, 0);
      var w = parseInt(side.style.width, 10);
      if (isFinite(w) && Module._fs_ui_split) Module._fs_ui_split(bar.__fsSplitCb, w);
    }
    document.addEventListener('mousemove', move);
    document.addEventListener('mouseup', up);
  });
});

// Drag the top edge of a bottom panel to set its height. Same shape as
// fs_dom_splitter but on the vertical axis: the handle sits on the panel's TOP
// edge, so dragging UP (negative dy) must make the panel TALLER.
EM_JS(void, fs_dom_vsplitter, (int split_node, int panel_node, int cb_id), {
  var bar = globalThis.__fsNodes[split_node];
  var panel = globalThis.__fsNodes[panel_node];
  if (!bar || !panel) return;
  bar.__fsVSplitCb = cb_id;
  if (bar.__fsVSplitInit) return;
  bar.__fsVSplitInit = true;
  bar.style.cursor = 'row-resize';
  bar.addEventListener('mousedown', function (down) {
    down.preventDefault();
    var startY = down.clientY;
    var startH = panel.getBoundingClientRect().height;
    var moved = false;
    function move(ev) {
      var dy = ev.clientY - startY;
      if (!moved && Math.abs(dy) > 3) moved = true;
      if (!moved) return;
      var h = startH - dy;
      if (h < 120) h = 120;
      var maxH = Math.round(window.innerHeight * 0.85);
      if (h > maxH) h = maxH;
      panel.style.height = h + 'px';
    }
    function up() {
      document.removeEventListener('mousemove', move);
      document.removeEventListener('mouseup', up);
      if (!moved) return;
      var swallow = function (ev) { ev.stopPropagation(); ev.preventDefault(); };
      document.addEventListener('click', swallow, true);
      setTimeout(function () { document.removeEventListener('click', swallow, true); }, 0);
      var h = parseInt(panel.style.height, 10);
      if (isFinite(h) && Module._fs_ui_vsplit) Module._fs_ui_vsplit(bar.__fsVSplitCb, h);
    }
    document.addEventListener('mousemove', move);
    document.addEventListener('mouseup', up);
  });
});

EM_JS(int, fs_dom_root, (), {
  var r = document.getElementById('fs-root');
  if (!r) {
    r = document.createElement('div');
    r.id = 'fs-root';
    document.body.appendChild(r);
  }
  return 1;
});

// ---- drop-zone callbacks --------------------------------------------------
// Written by WasmDom::bind_drop_zone, invoked from JS via fs_ui_drop below.
// File-level (not class members) because the EM_JS-exported entry point is a
// free function and needs to reach the same table.
std::map<int, std::function<void(const std::string&, const std::string&)> >& drop_cbs() {
  static std::map<int, std::function<void(const std::string&, const std::string&)> > m;
  return m;
}
int& drop_seq() {
  static int s = 0;
  return s;
}

// splitter resize callbacks (one app-wide divider; replaced on each bind)
std::map<int, std::function<void(int)> >& split_cbs() {
  static std::map<int, std::function<void(int)> > m;
  return m;
}
int& split_seq() {
  static int s = 0;
  return s;
}

// vertical (bottom panel height) resize callbacks
std::map<int, std::function<void(int)> >& vsplit_cbs() {
  static std::map<int, std::function<void(int)> > m;
  return m;
}
int& vsplit_seq() {
  static int s = 0;
  return s;
}

// global key callback (document-level Escape etc.)
std::map<int, std::function<void(const std::string&)> >& key_cbs() {
  static std::map<int, std::function<void(const std::string&)> > m;
  return m;
}
int& key_seq() {
  static int s = 0;
  return s;
}

// Listen on the document so Escape works whatever has focus. Installed once.
EM_JS(void, fs_dom_global_key, (int cb_id), {
  if (globalThis.__fsGlobalKey) {
    globalThis.__fsGlobalKeyCb = cb_id;
    return;
  }
  globalThis.__fsGlobalKey = true;
  globalThis.__fsGlobalKeyCb = cb_id;
  document.addEventListener('keydown', function (ev) {
    if (!Module._fs_ui_key) return;
    var len = Module.lengthBytesUTF8(ev.key) + 1;
    var ptr = Module._malloc(len);
    Module.stringToUTF8(ev.key, ptr, len);
    Module._fs_ui_key(globalThis.__fsGlobalKeyCb, ptr);
    Module._free(ptr);
  }, false);
});

// ---- WasmDom --------------------------------------------------------------
namespace {

class WasmDom : public IDom {
 public:
  WasmDom() {
    fs_dom_boot();
    doc_ = val::global("document");
    nodes_.resize(2);  // index 0 unused; 1 = root
    int rid = fs_dom_root();
    (void)rid;
    val r = doc_.call<val>("getElementById", std::string("fs-root"));
    nodes_[1] = r;
    val table = val::global("__fsNodes");
    table.set(1u, r);
    next_id_ = 2;
  }

  Node create_element(const std::string& tag) override {
    val el = doc_.call<val>("createElement", tag);
    return adopt(el);
  }

  Node create_text(const std::string& text) override {
    val el = doc_.call<val>("createTextNode", text);
    return adopt(el);
  }

  void append(Node parent, Node child) override {
    val p = at(parent), c = at(child);
    if (p.isNull() || p.isUndefined() || c.isNull() || c.isUndefined()) return;
    // guard: never append a node into itself or its own ancestor
    val n = p;
    while (!n.isNull() && !n.isUndefined()) {
      if (n == c) return;
      n = n["parentNode"];
    }
    p.call<void>("appendChild", c);
  }

  void insert_before(Node parent, Node child, Node ref) override {
    val p = at(parent), c = at(child), r = at(ref);
    if (p.isNull() || p.isUndefined() || c.isNull() || c.isUndefined()) return;
    if (r.isNull() || r.isUndefined()) {
      p.call<void>("appendChild", c);
      return;
    }
    p.call<void>("insertBefore", c, r);
  }

  void remove(Node node) override {
    val n = at(node);
    if (n.isNull() || n.isUndefined()) return;
    val p = n["parentNode"];
    if (!p.isNull() && !p.isUndefined()) p.call<void>("removeChild", n);
    nodes_[static_cast<size_t>(node.id)] = val::undefined();
    fs_dom_forget(static_cast<int>(node.id));
  }

  void clear(Node parent) override {
    val p = at(parent);
    if (p.isNull() || p.isUndefined()) return;
    p.set("innerHTML", std::string(""));
  }

  void set_text(Node node, const std::string& text) override {
    val n = at(node);
    if (n.isNull() || n.isUndefined()) return;
    n.set("textContent", text);
  }

  void set_attr(Node node, const std::string& key, const std::string& value) override {
    val n = at(node);
    if (n.isNull() || n.isUndefined()) return;
    if (key == "value") {
      n.set("value", value);
    } else if (key == "checked") {
      n.set("checked", value == "1" || value == "true");
    } else if (key == "disabled") {
      n.set("disabled", value == "1" || value == "true");
    } else {
      n.call<void>("setAttribute", key, value);
    }
  }

  void remove_attr(Node node, const std::string& key) override {
    val n = at(node);
    if (n.isNull() || n.isUndefined()) return;
    if (key == "value") {
      n.set("value", std::string(""));
    } else if (key == "checked") {
      n.set("checked", false);
    } else {
      n.call<void>("removeAttribute", key);
    }
  }

  void set_style(Node node, const std::string& key, const std::string& value) override {
    val n = at(node);
    if (n.isNull() || n.isUndefined()) return;
    n["style"].set(key, value);
  }

  void set_visible(Node node, bool visible) override {
    val n = at(node);
    if (n.isNull() || n.isUndefined()) return;
    n["style"].set("display", visible ? std::string("") : std::string("none"));
  }

  std::string get_value(Node node) override {
    val n = at(node);
    if (n.isNull() || n.isUndefined()) return "";
    val v = n["value"];
    if (v.isUndefined() || v.isNull()) {
      val t = n["textContent"];
      return t.isUndefined() || t.isNull() ? std::string() : t.as<std::string>();
    }
    return v.as<std::string>();
  }

  void on(Node node, EventKind kind, int handler_id) override {
    fs_dom_bind(static_cast<int>(node.id), static_cast<int>(kind), handler_id);
  }

  void off(Node node, EventKind kind) override {
    fs_dom_off(static_cast<int>(node.id), static_cast<int>(kind));
  }

  void bind_splitter(Node splitter, Node side,
                     std::function<void(int)> on_resize) override {
    int cb_id = 0;
    if (on_resize) {
      cb_id = ++split_seq();
      split_cbs().clear();
      split_cbs()[cb_id] = std::move(on_resize);
    }
    fs_dom_splitter(static_cast<int>(splitter.id), static_cast<int>(side.id), cb_id);
  }

  void bind_vsplitter(Node splitter, Node panel,
                      std::function<void(int)> on_resize) override {
    int cb_id = 0;
    if (on_resize) {
      cb_id = ++vsplit_seq();
      vsplit_cbs().clear();
      vsplit_cbs()[cb_id] = std::move(on_resize);
    }
    fs_dom_vsplitter(static_cast<int>(splitter.id), static_cast<int>(panel.id), cb_id);
  }

  void bind_drop_zone(
      Node node,
      std::function<void(const std::string&, const std::string&)> cb) override {
    int cb_id = ++drop_seq();
    // One app-wide drop zone (the window); replace the table so a long run of
    // re-renders cannot grow it without bound.
    drop_cbs().clear();
    drop_cbs()[cb_id] = std::move(cb);
    fs_dom_drop_zone(static_cast<int>(node.id), cb_id);
  }

  void bind_global_key(std::function<void(const std::string&)> cb) override {
    int cb_id = ++key_seq();
    key_cbs().clear();
    key_cbs()[cb_id] = std::move(cb);
    fs_dom_global_key(cb_id);
  }

 private:
  Node root() override { return Node{1}; }

  const char* backend_name() const override { return "wasm"; }

 private:
  val at(Node n) {
    if (n.id >= nodes_.size()) return val::undefined();
    return nodes_[static_cast<size_t>(n.id)];
  }
  Node adopt(val el) {
    Node n{next_id_++};
    if (nodes_.size() <= n.id) nodes_.resize(static_cast<size_t>(n.id) + 1);
    nodes_[static_cast<size_t>(n.id)] = el;
    val::global("__fsNodes").set(static_cast<unsigned>(n.id), el);
    return n;
  }

  val doc_;
  std::vector<val> nodes_;
  uint64_t next_id_ = 2;
};

}  // namespace

// Factory used by the wasm entry point.
IDom* make_platform_dom() { return new WasmDom(); }

// Called from JS (fs_dom_drop_zone) with what was dropped: kind is
// "dir" | "file" | "url" | "text", value is the name / url / text.
extern "C" EMSCRIPTEN_KEEPALIVE void fs_ui_drop(int cb_id, const char* kind,
                                                const char* value) {
  std::map<int, std::function<void(const std::string&, const std::string&)> >::iterator it =
      drop_cbs().find(cb_id);
  if (it == drop_cbs().end() || !it->second) return;
  it->second(std::string(kind ? kind : ""), std::string(value ? value : ""));
}

// Called from JS (fs_dom_splitter) when a drag ends, with the final width.
extern "C" EMSCRIPTEN_KEEPALIVE void fs_ui_split(int cb_id, int width) {
  std::map<int, std::function<void(int)> >::iterator it = split_cbs().find(cb_id);
  if (it == split_cbs().end() || !it->second) return;
  it->second(width);
}

// Called from JS (fs_dom_vsplitter) when a drag ends, with the final height.
extern "C" EMSCRIPTEN_KEEPALIVE void fs_ui_vsplit(int cb_id, int height) {
  std::map<int, std::function<void(int)> >::iterator it = vsplit_cbs().find(cb_id);
  if (it == vsplit_cbs().end() || !it->second) return;
  it->second(height);
}

// Called from JS (fs_dom_global_key) for document-level key presses.
extern "C" EMSCRIPTEN_KEEPALIVE void fs_ui_key(int cb_id, const char* key) {
  std::map<int, std::function<void(const std::string&)> >::iterator it = key_cbs().find(cb_id);
  if (it == key_cbs().end() || !it->second) return;
  it->second(std::string(key ? key : ""));
}

}  // namespace ui
}  // namespace fs

#endif  // __EMSCRIPTEN__
