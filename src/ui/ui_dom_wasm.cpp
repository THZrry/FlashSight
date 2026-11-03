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

#include "fs_json.hpp"

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
  // The browser's context menu is a document-level default: a per-node
  // contextmenu listener never fires on nodes without one, so right-clicking
  // anywhere popped "Back / Reload / Save as / Inspect". Swallow it globally and
  // let the app draw its own menu everywhere - text fields included, so the menu
  // is identical wherever you click.
  if (!globalThis.__fsCtxMenuPatched) {
    globalThis.__fsCtxMenuPatched = true;
    document.addEventListener('contextmenu', function (ev) {
      ev.preventDefault();
    }, true);
    // Middle-click belongs to the app, not to Chromium: its default is
    // scroll-to-pan, which over a tab strip reads as "middle click did something
    // weird to the page". Swallow both the press and the aux-click.
    document.addEventListener('mousedown', function (ev) {
      if (ev.button === 1) ev.preventDefault();
    }, true);
    document.addEventListener('auxclick', function (ev) {
      if (ev.button === 1) ev.preventDefault();
    }, true);
    // A vertical wheel over the tab strip scrolls it SIDEWAYS: the strip is the
    // only horizontally scrolling bar in the app, and a wheel is all most mice
    // have. Horizontal wheels / trackpads keep their native behaviour.
    document.addEventListener('wheel', function (ev) {
      var el = ev.target;
      while (el && el !== document.body &&
             !(el.classList && el.classList.contains('tabs-scroll'))) {
        el = el.parentElement;
      }
      if (!el || el === document.body) return;
      if (Math.abs(ev.deltaY) <= Math.abs(ev.deltaX)) return;
      el.scrollLeft += ev.deltaY;
      ev.preventDefault();
    }, { passive: false });
  }
});

// Which message is at the top of the transcript: the first one whose bottom
// edge is below the scroller's top edge. Synchronous, like every wasm DOM read.
EM_JS(int, fs_dom_anchor, (int node_id), {
  var el = globalThis.__fsNodes[node_id];
  if (!el) return -1;
  var kids = el.querySelectorAll('.msg');
  if (!kids.length) return -1;
  var rect = el.getBoundingClientRect();
  var base = rect.top + (el.clientTop || 0);
  for (var i = 0; i < kids.length; i++) {
    var r = kids[i].getBoundingClientRect();
    if (r.bottom - base > 1) return i;
  }
  return kids.length - 1;
});

EM_JS(void, fs_dom_scroll_into_view, (int node_id), {
  var el = globalThis.__fsNodes[node_id];
  if (!el) return;
  try { el.scrollIntoView({ block: 'start', behavior: 'auto' }); }
  catch (e) { try { el.scrollIntoView(true); } catch (e2) {} }
});

EM_JS(void, fs_dom_off, (int node_id, int kind), {  var h = globalThis.__fsHandlers[node_id];
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
               'dblclick', 'contextmenu', 'scroll', 'mouseenter', 'mouseleave',
               'blur'];
  var name = names[kind] || 'click';
  var evName = (name === 'keyenter') ? 'keydown' : name;
  var listener = function (evt) {
    // A control INSIDE a clickable row (a card, a fold header) must not also
    // trigger the row: without this, pressing a subagent's 引导 or a tool row's
    // 打开记录 both ran its own action AND bubbled into the row's handler -
    // navigation on top of the action, or a fold toggling on every click.
    if (name === 'click' && evt.target && evt.target.closest &&
        evt.target.closest('[data-no-bubble]')) {
      return;
    }
    // Enter behaviour is a PER-ELEMENT choice, not a global constant: the
    // composer's "Enter 行为" setting says whether plain Enter sends or only
    // Ctrl/Cmd+Enter does. Without reading it here the setting could never take
    // effect (this handler is the only place the key is intercepted).
    if (name === 'keyenter') {
      if (evt.key !== 'Enter') return;
      var mode = el.getAttribute && el.getAttribute('data-enter-send');
      var ctrl = evt.ctrlKey || evt.metaKey;
      if (mode === 'ctrlEnter') {
        if (!ctrl) return;             // plain Enter must insert a newline
        evt.preventDefault();
      } else {
        if (evt.shiftKey || ctrl) return;  // Shift/Ctrl+Enter insert newlines
        evt.preventDefault();
      }
    }
    var v = "";
    try {
      var t = el.tagName;
      if (name === 'keydown') {
        // keydown carries the key name (not the element value) so handlers can
        // react to Escape / arrow keys; kKeyEnter handles Enter separately.
        v = evt.key === undefined ? "" : String(evt.key);
        // Elements that own these keys (the composer while its completion menu
        // is open) opt in, because otherwise the BROWSER acts first: Tab moves
        // focus and the arrow keys scroll the page before the app can accept a
        // suggestion. Opt-in rather than blanket so ordinary Tab navigation and
        // arrow scrolling elsewhere keep working.
        var cap = el.getAttribute && el.getAttribute('data-keys-capture');
        if (cap && (v === 'Tab' || v === 'ArrowUp' || v === 'ArrowDown' ||
                    v === 'Escape')) {
          evt.preventDefault();
        }
      } else if (name === 'contextmenu') {
        // The menu opens AT the pointer, so the value is "x\ty" in viewport
        // coordinates (every other event sends the element's text).
        v = String(evt.clientX | 0) + "\t" + String(evt.clientY | 0);
      } else if (name === 'click' && evt.target && evt.target.closest) {
        // A markdown link: report its TARGET, not the label. The anchor carries
        // data-href and no real href (a real one would navigate the app window
        // away from the session), so the shell needs the value from here.
        var a = evt.target.closest('[data-href]');
        if (a) {
          v = a.getAttribute('data-href') || "";
          evt.preventDefault();
        } else if (t === 'INPUT' && el.type === 'checkbox') {
          v = el.checked ? '1' : '0';
        } else {
          v = el.textContent || "";
        }
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
  if (!bar) return;
  // Rebinding just swaps the callback id (0 = no binding at all).
  bar.__fsSplitCb = cb_id;
  bar.__fsSplitSide = side || null;
  // Listeners install once and read the CURRENT id at press time: the divider
  // node survives the patch, so its old listeners would otherwise keep dragging
  // a panel that is no longer there.
  bar.style.cursor = cb_id ? 'col-resize' : 'default';
  if (bar.__fsSplitInit) return;
  bar.__fsSplitInit = true;
  bar.addEventListener('mousedown', function (down) {
    if (!bar.__fsSplitCb) return;          // not bound (panel collapsed)
    var side = bar.__fsSplitSide;
    if (!side) return;
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
      if (isFinite(w) && bar.__fsSplitCb && Module._fs_ui_split) {
        Module._fs_ui_split(bar.__fsSplitCb, w);
      }
    }
    document.addEventListener('mousemove', move);
    document.addEventListener('mouseup', up);
  });
});

// The mirror image: a panel on the LEFT of its divider (the project nav rail).
// Dragging right must WIDEN it, so the delta is ADDED. The right-hand side panel
// is a separate binding and keeps its width - dragging one divider never
// redistributes the other column.
EM_JS(void, fs_dom_nav_splitter, (int split_node, int nav_node, int cb_id), {
  var bar = globalThis.__fsNodes[split_node];
  var nav = globalThis.__fsNodes[nav_node];
  if (!bar) return;
  bar.__fsNavSplitCb = cb_id;
  bar.__fsNavSplitNav = nav || null;
  bar.style.cursor = cb_id ? 'col-resize' : 'default';
  if (bar.__fsNavSplitInit) return;
  bar.__fsNavSplitInit = true;
  bar.addEventListener('mousedown', function (down) {
    if (!bar.__fsNavSplitCb) return;
    var nav = bar.__fsNavSplitNav;
    if (!nav) return;
    var t = down.target;
    if (t && t.classList && t.classList.contains('splitter-handle')) return;
    down.preventDefault();
    var startX = down.clientX;
    var startW = nav.getBoundingClientRect().width;
    var moved = false;
    function move(ev) {
      var dx = ev.clientX - startX;
      if (!moved && Math.abs(dx) > 3) moved = true;
      if (!moved) return;
      var w = startW + dx;   // the nav is LEFT of the divider: right = wider
      if (w < 160) w = 160;
      if (w > 480) w = 480;
      nav.style.width = w + 'px';
      nav.style.flex = '0 0 auto';
    }
    function up() {
      document.removeEventListener('mousemove', move);
      document.removeEventListener('mouseup', up);
      if (!moved) return;
      var swallow = function (ev) { ev.stopPropagation(); ev.preventDefault(); };
      document.addEventListener('click', swallow, true);
      setTimeout(function () { document.removeEventListener('click', swallow, true); }, 0);
      var w = parseInt(nav.style.width, 10);
      if (isFinite(w) && bar.__fsNavSplitCb && Module._fs_ui_nav_split) {
        Module._fs_ui_nav_split(bar.__fsNavSplitCb, w);
      }
    }
    document.addEventListener('mousemove', move);
    document.addEventListener('mouseup', up);
  });
});

// Reorder top-level tabs by dragging one tab onto another. The tab elements are
// keyed by data-key; the app owns the order, so the adapter only reports the
// pair and the next render moves the real nodes.
//
// Feedback matters here: without it the drag is invisible (the tab you grabbed
// looks unchanged, and there is no indication of where it will land). So the
// dragged tab dims, a floating copy follows the cursor, and the tab under the
// pointer is highlighted. A 4px threshold separates a drag from a click.
EM_JS(void, fs_dom_tab_drag, (int strip_node, int cb_id), {
  var strip = globalThis.__fsNodes[strip_node];
  if (!strip) return;
  strip.__fsTabDragCb = cb_id;
  if (strip.__fsTabDragInit) return;
  strip.__fsTabDragInit = true;
  var fromEl = null, ghost = null, started = false, startX = 0, startY = 0;
  var lastTarget = null;
  // Both strips use this: the top tabs are `.tab`, the side panel's are `.ptab`.
  function isTab(el) {
    return el && el.classList &&
           (el.classList.contains('tab') || el.classList.contains('ptab'));
  }

  function tabAt(x, y) {
    var el = document.elementFromPoint(x, y);
    while (el && el !== strip && !isTab(el)) {
      el = el.parentElement;
    }
    return (el && el !== strip) ? el : null;
  }
  function clearTarget() {
    if (lastTarget) lastTarget.classList.remove('drop-target');
    lastTarget = null;
  }
  function cleanup() {
    if (fromEl) fromEl.classList.remove('dragging');
    if (ghost && ghost.parentNode) ghost.parentNode.removeChild(ghost);
    ghost = null;
    clearTarget();
    fromEl = null;
    started = false;
    document.removeEventListener('mousemove', move);
    document.removeEventListener('mouseup', up);
  }
  function move(ev) {
    if (!fromEl) return;
    if (!started) {
      if (Math.abs(ev.clientX - startX) < 4 && Math.abs(ev.clientY - startY) < 4) return;
      started = true;
      fromEl.classList.add('dragging');
      var r0 = fromEl.getBoundingClientRect();
      ghost = fromEl.cloneNode(true);
      ghost.classList.remove('dragging');
      ghost.classList.add('tab-ghost');
      ghost.style.width = r0.width + 'px';
      ghost.style.left = r0.left + 'px';
      ghost.style.top = r0.top + 'px';
      document.body.appendChild(ghost);
    }
    ev.preventDefault();
    if (ghost) {
      var r = fromEl.getBoundingClientRect();
      ghost.style.left = (ev.clientX - r.width / 2) + 'px';
      ghost.style.top = (ev.clientY - 14) + 'px';
    }
    var t = tabAt(ev.clientX, ev.clientY);
    if (t && (t === fromEl || t.classList.contains('add'))) t = null;    if (t !== lastTarget) {
      clearTarget();
      if (t) { t.classList.add('drop-target'); lastTarget = t; }
    }
  }
  function up(ev) {
    if (!fromEl) return;
    var from = fromEl.getAttribute('data-key');
    var to = started && lastTarget ? lastTarget.getAttribute('data-key') : null;
    var wasDrag = started;
    cleanup();
    if (!wasDrag) return;
    // swallow the click that follows a drag (it would switch tabs instead)
    var swallow = function (e) { e.stopPropagation(); e.preventDefault(); };
    document.addEventListener('click', swallow, true);
    setTimeout(function () { document.removeEventListener('click', swallow, true); }, 0);
    if (!from || !to || from === to) return;
    if (strip.__fsTabDragCb && Module._fs_ui_tab_drag) {
      var payload = from + '\t' + to;
      var len = Module.lengthBytesUTF8(payload) + 1;
      var ptr = Module._malloc(len);
      Module.stringToUTF8(payload, ptr, len);
      Module._fs_ui_tab_drag(strip.__fsTabDragCb, ptr);
      Module._free(ptr);
    }
  }
  strip.addEventListener('mousedown', function (down) {
    var t = down.target;
    while (t && t !== strip && !isTab(t)) t = t.parentElement;
    if (!t || t === strip) return;
    if (t.classList.contains('add')) return;      // the "+" is not a tab
    if (down.target.closest && down.target.closest('.tab-close')) return;
    fromEl = t;
    started = false;
    startX = down.clientX;
    startY = down.clientY;
    document.addEventListener('mousemove', move);
    document.addEventListener('mouseup', up);
  });
});

// Place a floating popup next to its anchor, inside the viewport.
//
// The popup is absolutely positioned by CSS (bottom: calc(100% + 8px)), which
// always opens UPWARD. In a settings page the field can sit near the top of the
// window, so "upward" runs off-screen and the list gets cut off. Here we
// measure: if there is more room above than below, keep it above; otherwise
// flip it below the anchor. Horizontal: keep it inside the window instead of
// letting a right-aligned popup hang off the left edge.
//
// NOTE the coordinate space: `top`/`left` on an absolutely positioned element
// are relative to its OFFSET PARENT (the .model-field row, which is
// position:relative), NOT the viewport. Mixing the two put the picker half a
// screen away from its button. Everything below is computed in the popup's own
// containing block.
// One-shot timer: run fs_ui_timeout(cb_id) after `ms`. Used for the cleanup
// that removes a subtree once its exit animation has played. Kept on the JS side
// because C++ has no event loop of its own here, and because a background tab
// throttles setTimeout (which is fine: the cleanup only needs to happen before
// the user looks again).
EM_JS(void, fs_dom_timeout, (int ms, int cb_id), {
  setTimeout(function () {
    if (typeof Module !== 'undefined' && Module._fs_ui_timeout) {
      Module._fs_ui_timeout(cb_id);
    }
  }, ms);
});

EM_JS(void, fs_dom_place_popup, (int popup_node, int anchor_node), {  var pop = globalThis.__fsNodes[popup_node];
  var anchor = globalThis.__fsNodes[anchor_node];
  if (!pop || !anchor) return;
  var a = anchor.getBoundingClientRect();
  var vw = window.innerWidth, vh = window.innerHeight;
  var gap = 8;
  var ph = pop.offsetHeight || 420;
  var pw = pop.offsetWidth || 640;

  // The containing block: the nearest positioned ancestor (or the body).
  var cb = pop.offsetParent || document.body;
  var c = cb.getBoundingClientRect();
  // Anchor position inside that block.
  var ax = a.left - c.left - cb.clientLeft;
  var ay = a.top - c.top - cb.clientTop;

  var above = a.top - gap;               // viewport space: room above
  var below = vh - a.bottom - gap;       // viewport space: room below
  pop.style.bottom = 'auto';
  pop.style.top = 'auto';
  if (below >= ph || below >= above) {
    pop.style.top = Math.round(ay + a.height + gap) + 'px';
    pop.classList.add('below');
  } else {
    // anchor-relative distance from the block's top to the popup's bottom edge
    pop.style.top = Math.round(ay - ph - gap) + 'px';
    pop.classList.remove('below');
  }
  // horizontal: keep the whole popup on screen (viewport clamp, then convert)
  var leftVp = a.left;
  if (leftVp + pw > vw - 8) leftVp = vw - pw - 8;
  if (leftVp < 8) leftVp = 8;
  pop.style.left = Math.round(leftVp - c.left - cb.clientLeft) + 'px';
  pop.style.right = 'auto';
});

// Report presses that land outside a popup (and outside its anchor, which is
// what opened it). This replaces the full-screen backdrop for popovers rendered
// inside a small field: a backdrop there covered the field itself, so the first
// click closed the popup AND was swallowed.
EM_JS(void, fs_dom_outside_click, (int popup_node, int cb_id), {
  var pop = globalThis.__fsNodes[popup_node];
  if (!pop) return;
  pop.__fsOutsideCb = cb_id;
  if (pop.__fsOutsideInit) return;
  pop.__fsOutsideInit = true;
  pop.__fsOutsideHandler = function (ev) {
    // A popup that has left the DOM must not dismiss whatever is open now: its
    // document listener outlives the node, and firing the (still-registered)
    // callback closed the NEXT popup on the first press anywhere.
    if (!pop.isConnected) return;
    if (!pop.__fsOutsideCb) return;
    var t = ev.target;
    if (pop.contains(t)) return;
    // The ANCHOR owns the toggle: a press on it is a toggle, not a dismiss -
    // closing here would reopen immediately on the following click. Only the
    // anchor is exempt. Testing the whole PARENT container instead (the old
    // `host.contains(t)`) also ignored every SIBLING of the popup inside that
    // container, so a press on the popup's own field wrapper or on a sibling
    // button did nothing and the popup looked impossible to dismiss.
    if (t && t.closest && t.closest('.model-field, .model-btn-wrap, [data-keep-popup]')) {
      return;
    }
    if (Module._fs_ui_outside_click) Module._fs_ui_outside_click(pop.__fsOutsideCb);
  };
  document.addEventListener('mousedown', pop.__fsOutsideHandler, true);
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

// ---- terminal key input ---------------------------------------------------
// A terminal cannot be driven through a normal text input: Ctrl+C is a control
// byte (not 'c'), Shift+Tab is not Tab, and every browser shortcut would fire
// while the user types. So the terminal surface is a focusable <div> and this
// listener (a) reports key + modifiers, (b) prevents the browser's default
// action, and (c) forwards paste as one chunk.
std::map<int, std::function<void(const KeyPress&)> >& key_input_cbs() {
  static std::map<int, std::function<void(const KeyPress&)> > m;
  return m;
}
int& key_input_seq() {
  static int s = 0;
  return s;
}

EM_JS(void, fs_dom_key_input, (int node_id, int cb_id), {
  var el = globalThis.__fsNodes[node_id];
  if (!el) return;
  el.__fsKeyCb = cb_id;
  if (el.__fsKeyInit) return;
  el.__fsKeyInit = true;
  // the surface must be focusable but must not behave like a text field
  if (!el.hasAttribute('tabindex')) el.setAttribute('tabindex', '0');
  el.style.outline = 'none';

  function send(kind, key, text, ev) {
    if (typeof Module === 'undefined' || !Module._fs_ui_key_input) return;
    var payload = JSON.stringify({
      key: key || '',
      text: text || '',
      ctrl: !!(ev && ev.ctrlKey),
      alt: !!(ev && ev.altKey),
      shift: !!(ev && ev.shiftKey),
      meta: !!(ev && ev.metaKey),
      paste: kind === 'paste',
    });
    var len = Module.lengthBytesUTF8(payload) + 1;
    var ptr = Module._malloc(len);
    Module.stringToUTF8(payload, ptr, len);
    Module._fs_ui_key_input(el.__fsKeyCb, ptr);
    Module._free(ptr);
  }

  el.addEventListener('keydown', function (ev) {
    // The browser must not act on anything typed into the terminal: Ctrl+W
    // would close the tab, Tab would move focus, F5 would reload.
    ev.preventDefault();
    ev.stopPropagation();
    send('keydown', ev.key, ev.key && ev.key.length > 1 ? '' : ev.key, ev);
  });
  el.addEventListener('keypress', function (ev) {
    ev.preventDefault();
    ev.stopPropagation();
  });
  el.addEventListener('paste', function (ev) {
    ev.preventDefault();
    ev.stopPropagation();
    var text = '';
    try {
      text = (ev.clipboardData || window.clipboardData).getData('text') || '';
    } catch (e) { text = ''; }
    if (text) send('paste', '', text, ev);
  });
  // clicking the surface focuses it, so keys go to the terminal
  el.addEventListener('mousedown', function () {
    try { el.focus(); } catch (e) {}
  });
  // a click can also carry a focus request from the app
  el.__fsFocus = function () { try { el.focus(); } catch (e) {} };
});

EM_JS(void, fs_dom_focus, (int node_id), {
  var el = globalThis.__fsNodes[node_id];
  if (!el) return;
  try { el.focus(); } catch (e) {}
});

// Measure how many character cells fit in `el`. The cell size is measured from
// the element's own font (a hidden probe span, so it works with any mono font
// and any zoom), and a ResizeObserver re-reports whenever the box changes - that
// is what keeps the child process's idea of the window in sync.
EM_JS(void, fs_dom_cell_size, (int node_id, int cb_id), {
  var el = globalThis.__fsNodes[node_id];
  if (!el) return;
  el.__fsCellCb = cb_id;
  function report() {
    if (typeof Module === 'undefined' || !Module._fs_ui_cell_size) return;
    var probe = el.__fsProbe;
    if (!probe) {
      probe = document.createElement('span');
      probe.textContent = 'MMMMMMMMMM';
      probe.style.position = 'absolute';
      probe.style.visibility = 'hidden';
      probe.style.whiteSpace = 'pre';
      probe.style.pointerEvents = 'none';
      // inherit the terminal's font so the measurement matches the real grid
      var cs = getComputedStyle(el);
      probe.style.font = cs.font;
      probe.style.fontFamily = cs.fontFamily;
      probe.style.fontSize = cs.fontSize;
      probe.style.lineHeight = cs.lineHeight;
      probe.style.letterSpacing = cs.letterSpacing;
      el.appendChild(probe);
      el.__fsProbe = probe;
    }
    var w = probe.getBoundingClientRect().width / 10;
    var lh = probe.getBoundingClientRect().height;
    if (!(w > 0) || !(lh > 0)) return;
    var pad = 0;
    var cs2 = getComputedStyle(el);
    pad = parseFloat(cs2.paddingLeft) + parseFloat(cs2.paddingRight);
    var box = el.getBoundingClientRect();
    var cols = Math.floor((box.width - pad) / w);
    var rows = Math.floor(box.height / lh);
    if (cols < 20) cols = 20;
    if (rows < 5) rows = 5;
    Module._fs_ui_cell_size(el.__fsCellCb, cols, rows);
  }
  if (!el.__fsCellInit) {
    el.__fsCellInit = true;
    if (typeof ResizeObserver !== 'undefined') {
      el.__fsCellRO = new ResizeObserver(function () { report(); });
      el.__fsCellRO.observe(el);
    }
    window.addEventListener('resize', report);
  }
  // report on the next frame: the element must be laid out before it can be
  // measured, and a fresh node often is not yet
  if (typeof requestAnimationFrame !== 'undefined') {
    requestAnimationFrame(function () { report(); });
  } else {
    setTimeout(report, 16);
  }
});

// File upload: clicking `el` opens the OS chooser; the picked files are POSTed
// to the local server (multipart) which stores them inside the project so the
// model's file tools can read them. The bytes never enter C++: the transfer and
// the reply are both handled here, and only the saved paths are handed back.
EM_JS(void, fs_dom_file_picker, (int node_id, int cb_id, const char* pid), {
  var el = globalThis.__fsNodes[node_id];
  if (!el) return;
  el.__fsPickCb = cb_id;
  el.__fsPickPid = UTF8ToString(pid);
  if (el.__fsPickInit) return;
  el.__fsPickInit = true;
  el.addEventListener('click', function (ev) {
    ev.preventDefault();
    ev.stopPropagation();
    var inp = document.createElement('input');
    inp.type = 'file';
    inp.multiple = true;
    // images and documents are what a chat attachment usually is, but "any" is
    // allowed - the backend decides what it can store
    inp.style.display = 'none';
    document.body.appendChild(inp);
    inp.addEventListener('change', function () {
      var files = Array.prototype.slice.call(inp.files || []);
      document.body.removeChild(inp);
      if (!files.length) {
        report({ files: [] });
        return;
      }
      var fd = new FormData();
      fd.append('pid', el.__fsPickPid || '');
      for (var i = 0; i < files.length; i++) fd.append('file', files[i], files[i].name);
      fetch('/api/upload', { method: 'POST', body: fd })
        .then(function (r) { return r.json(); })
        .then(function (j) { report(j && j.data ? j.data : j); })
        .catch(function (e) { report({ error: String(e) }); });
    });
    inp.click();
  });
  function report(obj) {
    if (typeof Module === 'undefined' || !Module._fs_ui_file_pick) return;
    var text = JSON.stringify(obj);
    var len = Module.lengthBytesUTF8(text) + 1;
    var ptr = Module._malloc(len);
    Module.stringToUTF8(text, ptr, len);
    Module._fs_ui_file_pick(el.__fsPickCb, ptr);
    Module._free(ptr);
  }
});

// Terminal scroll pinning: report whether the surface is scrolled to the bottom
// so the app knows whether new output should follow the view.
EM_JS(void, fs_dom_scroll_pin, (int node_id, int cb_id), {
  var el = globalThis.__fsNodes[node_id];
  if (!el) return;
  el.__fsScrollCb = cb_id;
  if (el.__fsScrollInit) return;
  el.__fsScrollInit = true;
  function report() {
    if (typeof Module === 'undefined' || !Module._fs_ui_scroll_pin) return;
    // 2px of slack: sub-pixel layout means "exactly at the bottom" is rare
    var atBottom = el.scrollTop + el.clientHeight >= el.scrollHeight - 2;
    Module._fs_ui_scroll_pin(el.__fsScrollCb, atBottom ? 1 : 0);
  }
  el.addEventListener('scroll', report, { passive: true });
  // a wheel/touch gesture can also mean "stop following"
  el.addEventListener('wheel', function () { setTimeout(report, 0); }, { passive: true });
});

EM_JS(void, fs_dom_scroll_bottom, (int node_id), {
  var el = globalThis.__fsNodes[node_id];
  if (!el) return;
  el.scrollTop = el.scrollHeight;
});

EM_JS(int, fs_dom_scroll_top, (int node_id), {
  var el = globalThis.__fsNodes[node_id];
  return el ? (el.scrollTop | 0) : 0;
});

EM_JS(void, fs_dom_set_scroll_top, (int node_id, int top), {
  var el = globalThis.__fsNodes[node_id];
  if (!el) return;
  // Assigning scrollTop animates when the element has `scroll-behavior: smooth`
  // (the history does). Restoring a position must be instant, or the user sees
  // the chat scroll by itself after every tab switch.
  var prev = el.style.scrollBehavior;
  el.style.scrollBehavior = 'auto';
  el.scrollTop = top;
  el.style.scrollBehavior = prev;
});

EM_JS(int, fs_dom_scroll_height, (int node_id), {
  var el = globalThis.__fsNodes[node_id];
  return el ? (el.scrollHeight | 0) : 0;
});

EM_JS(void, fs_dom_scroll_edges, (int node_id, int cb_id), {
  var el = globalThis.__fsNodes[node_id];
  if (!el) return;
  el.__fsEdgesCb = cb_id;
  if (el.__fsEdgesInit) return;
  el.__fsEdgesInit = true;
  function report() {
    if (typeof Module === 'undefined' || !Module._fs_ui_scroll_edges) return;
    // 24px of slack: "the top" has to be reachable by an ordinary wheel nudge,
    // and sub-pixel layout makes an exact 0 rare.
    var atTop = el.scrollTop <= 24;
    var atBottom = el.scrollTop + el.clientHeight >= el.scrollHeight - 2;
    Module._fs_ui_scroll_edges(el.__fsEdgesCb, atTop ? 1 : 0, atBottom ? 1 : 0);
  }
  el.addEventListener('scroll', report, { passive: true });
});

EM_JS(int, fs_dom_client_height, (int node_id), {
  var el = globalThis.__fsNodes[node_id];
  return el ? (el.clientHeight | 0) : 0;
});

EM_JS(void, fs_dom_copy_text, (const char* text), {
  var s = UTF8ToString(text);
  if (!s) return;
  // navigator.clipboard needs a secure context and a user gesture; the local
  // server is http://127.0.0.1, which counts as secure. Fall back to the old
  // textarea trick when the API is missing so copy never silently does nothing.
  if (navigator.clipboard && navigator.clipboard.writeText) {
    navigator.clipboard.writeText(s).catch(function () { legacyCopy(s); });
  } else {
    legacyCopy(s);
  }
  function legacyCopy(t) {
    var ta = document.createElement('textarea');
    ta.value = t;
    ta.style.position = 'fixed';
    ta.style.opacity = '0';
    document.body.appendChild(ta);
    ta.select();
    try { document.execCommand('copy'); } catch (e) {}
    document.body.removeChild(ta);
  }
});

// Run a browser edit command against the focused field. The app draws its own
// context menu over text fields (the native one is suppressed), so cut / copy /
// paste / select-all have to be reachable from a menu item.
EM_JS(void, fs_dom_exec_edit, (const char* command), {
  var c = UTF8ToString(command);
  if (!c) return;
  if (c === 'paste') {
    // execCommand('paste') is refused by every modern engine; the async
    // clipboard API is the only path, and a menu click is a valid user gesture.
    var el = document.activeElement;
    if (!el || !navigator.clipboard || !navigator.clipboard.readText) return;
    navigator.clipboard.readText().then(function (t) {
      if (!t) return;
      try {
        var s = el.selectionStart, e = el.selectionEnd;
        if (typeof s === 'number' && typeof e === 'number') {
          el.value = el.value.slice(0, s) + t + el.value.slice(e);
          el.selectionStart = el.selectionEnd = s + t.length;
        } else {
          el.value += t;
        }
        el.dispatchEvent(new Event('input', { bubbles: true }));
      } catch (err) {}
    }).catch(function () {});
    return;
  }
  try { document.execCommand(c, false, null); } catch (e) {}
});

// Restart a mount-only animation on a node that already exists. The CSS class is
// already on it (the app declares it every render), so simply leaving it there
// does nothing: the animation ran once when the node was created. Removing the
// class, forcing a reflow so the browser drops the animation, then re-adding it
// replays it. Without the reflow the browser coalesces the two changes and
// nothing happens.
EM_JS(void, fs_dom_play_animation, (int node_id, const char* cls), {
  var el = globalThis.__fsNodes[node_id];
  if (!el) return;
  var name = UTF8ToString(cls);
  if (!name) return;
  el.classList.remove(name);
  // reading offsetWidth flushes the style change, which is what makes the
  // re-add count as a NEW animation rather than a no-op
  void el.offsetWidth;
  el.classList.add(name);
});

// Apply the appearance to the document root. The stylesheet is built from CSS
// variables on :root, so setting them here re-themes the whole UI in one place;
// "auto" mode is resolved against the OS preference HERE, which is why the CSS
// needs no media query for the theme (only the default, unset case does).
//
// classList, not className string surgery: it is idempotent and cannot produce
// duplicates. Rebuilding the attribute by splitting and re-joining a string
// accumulated junk ("t palette-fore t palette-forest ...") because every call
// appended to whatever was already there.
EM_JS(void, fs_dom_appearance, (const char* palette, const char* mode,
                               const char* font, const char* mono,
                               int font_size, int code_highlight,
                               const char* tabs), {
  var root = document.documentElement;
  var pal = UTF8ToString(palette);
  var m = UTF8ToString(mode);
  var f = UTF8ToString(font);
  var mo = UTF8ToString(mono);
  var tb = UTF8ToString(tabs);
  if (m === 'auto' || !m) {
    m = (window.matchMedia && window.matchMedia('(prefers-color-scheme: dark)').matches)
        ? 'dark' : 'light';
  }
  // a palette is a class; the resolved mode is an attribute so the two compose
  // (palette = which hues, mode = light/dark of that palette)
  var stale = [];
  for (var i = 0; i < root.classList.length; i++) {
    var c = root.classList[i];
    if (c.indexOf('palette-') === 0) stale.push(c);
  }
  for (var k = 0; k < stale.length; k++) root.classList.remove(stale[k]);
  if (pal && pal !== 'default') root.classList.add('palette-' + pal);
  root.setAttribute('data-mode', m);
  // Tab placement is a layout switch driven by CSS (see [data-tabs="left"]).
  if (tb) root.setAttribute('data-tabs', tb); else root.removeAttribute('data-tabs');
  root.style.setProperty('--font-user', f ? ('"' + f + '", var(--font-base)') : '');
  root.style.setProperty('--mono-user', mo ? ('"' + mo + '", var(--mono-base)') : '');
  root.style.setProperty('--font-size-user', font_size > 0 ? (font_size + 'px') : '');
  root.setAttribute('data-code-highlight', code_highlight ? '1' : '0');
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

// nav-rail resize callbacks (independent of the side-panel divider: the two
// columns are resized against their OWN neighbour, so moving one never moves
// the other)
std::map<int, std::function<void(int)> >& nav_split_cbs() {
  static std::map<int, std::function<void(int)> > m;
  return m;
}
int& nav_split_seq() {
  static int s = 0;
  return s;
}

// tab drag/reorder callbacks
std::map<int, std::function<void(const std::string&)> >& tab_drag_cbs() {
  static std::map<int, std::function<void(const std::string&)> > m;
  return m;
}
int& tab_drag_seq() {
  static int s = 0;
  return s;
}

// click-outside callbacks (popovers)
std::map<int, std::function<void()> >& outside_cbs() {
  static std::map<int, std::function<void()> > m;
  return m;
}
int& outside_seq() {
  static int s = 0;
  return s;
}

// one-shot timer callbacks (post-animation cleanup)
std::map<int, std::function<void()> >& sched_cbs() {
  static std::map<int, std::function<void()> > m;
  return m;
}
int& sched_seq() {
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

// terminal cell-size callbacks (one surface at a time)
std::map<int, std::function<void(int, int)> >& cell_size_cbs() {
  static std::map<int, std::function<void(int, int)> > m;
  return m;
}
int& cell_size_seq() {
  static int s = 0;
  return s;
}

// file-picker callbacks
std::map<int, std::function<void(const std::string&)> >& file_pick_cbs() {
  static std::map<int, std::function<void(const std::string&)> > m;
  return m;
}
int& file_pick_seq() {
  static int s = 0;
  return s;
}

// scroll-pin callbacks (one terminal surface at a time)
std::map<int, std::function<void(bool)> >& scroll_pin_cbs() {
  static std::map<int, std::function<void(bool)> > m;
  return m;
}
int& scroll_pin_seq() {
  static int s = 0;
  return s;
}

// scroll-edge callbacks (the chat: "reached the top" / "reached the bottom")
std::map<int, std::function<void(bool, bool)> >& scroll_edges_cbs() {
  static std::map<int, std::function<void(bool, bool)> > m;
  return m;
}
int& scroll_edges_seq() {
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
    // Modifiers travel with the key: the shortcut table's bindings are
    // "Ctrl+W" / "Ctrl+T", and a bare key name cannot express them.
    var k = ev.key || '';
    var mods = '';
    if (ev.ctrlKey) mods += 'Ctrl+';
    if (ev.metaKey) mods += 'Meta+';
    if (ev.altKey) mods += 'Alt+';
    if (ev.shiftKey) mods += 'Shift+';
    var s = mods + k;
    var len = Module.lengthBytesUTF8(s) + 1;
    var ptr = Module._malloc(len);
    Module.stringToUTF8(s, ptr, len);
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
    // "Already there" must be a no-op. insertBefore on a node that is already in
    // position still detaches and re-attaches it, which resets the scroll
    // position of every scroll container inside and restarts CSS animations.
    if (r.isNull() || r.isUndefined()) {
      val last = p["lastChild"];
      if (!last.isNull() && !last.isUndefined() && last == c) return;
      p.call<void>("appendChild", c);
      return;
    }
    val next = c["nextSibling"];
    if (!next.isNull() && !next.isUndefined() && next == r) return;
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

  void set_html(Node node, const std::string& html) override {
    val n = at(node);
    if (n.isNull() || n.isUndefined()) return;
    n.set("innerHTML", html);
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
    // A CSS custom property (`--x`) is NOT reachable through the indexed
    // CSSStyleDeclaration: `style['--grow'] = '50%'` silently does nothing, and
    // `style.set(...)` becomes that same assignment. Only setProperty() takes
    // custom properties, which is exactly what the chart's --grow relies on.
    if (key.size() > 2 && key[0] == '-' && key[1] == '-') {
      n["style"].call<void>("setProperty", key, value);
    } else {
      n["style"].set(key, value);
    }
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

  bool is_focused(Node node) override {
    val n = at(node);
    if (n.isNull() || n.isUndefined()) return false;
    val active = val::global("document")["activeElement"];
    return !active.isNull() && !active.isUndefined() && active == n;
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

  void bind_nav_splitter(Node splitter, Node nav,
                         std::function<void(int)> on_resize) override {
    int cb_id = 0;
    if (on_resize) {
      cb_id = ++nav_split_seq();
      nav_split_cbs().clear();
      nav_split_cbs()[cb_id] = std::move(on_resize);
    }
    fs_dom_nav_splitter(static_cast<int>(splitter.id), static_cast<int>(nav.id),
                        cb_id);
  }

  void bind_tab_drag(Node strip, std::function<void(const std::string&)> cb) override {
    int cb_id = 0;
    if (cb) {
      cb_id = ++tab_drag_seq();
      tab_drag_cbs().clear();
      tab_drag_cbs()[cb_id] = std::move(cb);
    }
    fs_dom_tab_drag(static_cast<int>(strip.id), cb_id);
  }

  void place_popup(Node popup, Node anchor) override {
    fs_dom_place_popup(static_cast<int>(popup.id), static_cast<int>(anchor.id));
  }

  void bind_outside_click(Node popup, std::function<void()> cb) override {
    // One callback per popup. A single shared slot meant the newest binding won:
    // with a picker open and a context menu opened over it, the menu's callback
    // replaced the picker's, and the document listeners installed by the earlier
    // popup then fired the WRONG callback (or none) - "clicking elsewhere does
    // not close it" / "it closes the wrong thing".
    int cb_id = 0;
    if (cb) {
      cb_id = ++outside_seq();
      outside_cbs()[cb_id] = std::move(cb);
    }
    fs_dom_outside_click(static_cast<int>(popup.id), cb_id);
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

  void bind_key_input(Node node, std::function<void(const KeyPress&)> cb) override {
    int cb_id = ++key_input_seq();
    key_input_cbs().clear();
    key_input_cbs()[cb_id] = std::move(cb);
    fs_dom_key_input(static_cast<int>(node.id), cb_id);
  }

  void focus(Node node) override { fs_dom_focus(static_cast<int>(node.id)); }

  void bind_cell_size(Node node, std::function<void(int, int)> cb) override {
    int cb_id = ++cell_size_seq();
    cell_size_cbs().clear();
    cell_size_cbs()[cb_id] = std::move(cb);
    fs_dom_cell_size(static_cast<int>(node.id), cb_id);
  }

  void bind_file_picker(Node node, const std::string& pid,
                        std::function<void(const std::string&)> cb) override {
    int cb_id = ++file_pick_seq();
    file_pick_cbs().clear();
    file_pick_cbs()[cb_id] = std::move(cb);
    fs_dom_file_picker(static_cast<int>(node.id), cb_id, pid.c_str());
  }

  void bind_scroll_pin(Node node, std::function<void(bool)> cb) override {
    int cb_id = ++scroll_pin_seq();
    scroll_pin_cbs().clear();
    scroll_pin_cbs()[cb_id] = std::move(cb);
    fs_dom_scroll_pin(static_cast<int>(node.id), cb_id);
  }

  void scroll_to_bottom(Node node) override {
    fs_dom_scroll_bottom(static_cast<int>(node.id));
  }

  void bind_scroll_edges(Node node,
                         std::function<void(bool, bool)> cb) override {
    int cb_id = ++scroll_edges_seq();
    scroll_edges_cbs().clear();
    scroll_edges_cbs()[cb_id] = std::move(cb);
    fs_dom_scroll_edges(static_cast<int>(node.id), cb_id);
  }

  int scroll_top(Node node) override {
    return fs_dom_scroll_top(static_cast<int>(node.id));
  }
  int scroll_anchor(Node node) override {
    return fs_dom_anchor(static_cast<int>(node.id));
  }
  void scroll_into_view(Node node) override {
    fs_dom_scroll_into_view(static_cast<int>(node.id));
  }

  void set_scroll_top(Node node, int top) override {
    fs_dom_set_scroll_top(static_cast<int>(node.id), top);
  }

  int scroll_height(Node node) override {
    return fs_dom_scroll_height(static_cast<int>(node.id));
  }

  int client_height(Node node) override {
    return fs_dom_client_height(static_cast<int>(node.id));
  }

  void copy_text(const std::string& text) override {
    fs_dom_copy_text(text.c_str());
  }

  bool exec_edit(const std::string& command) override {
    fs_dom_exec_edit(command.c_str());
    return true;
  }

  void play_animation(Node node, const std::string& cls) override {
    fs_dom_play_animation(static_cast<int>(node.id), cls.c_str());
  }

  // The browser can animate a leaving subtree; keeping it alive for ~200ms is
  // what lets the old page and the new page coexist during a tab switch.
  bool supports_exit_animations() const override { return true; }

  void schedule(int ms, std::function<void()> cb) override {
    if (!cb) return;
    // Take the id ONCE. `sched_cbs()[++sched_seq()] = ...` followed by
    // `fs_dom_timeout(ms, sched_seq())` increments twice: the callback was
    // stored under N but the timer fired with N+1, so it never found its entry
    // and the removal never happened - leaving exited pages in the DOM forever.
    int id = ++sched_seq();
    sched_cbs()[id] = std::move(cb);
    fs_dom_timeout(ms, id);
  }

  void set_appearance(const Appearance& a) override {
    fs_dom_appearance(a.palette.c_str(), a.mode.c_str(), a.font.c_str(),
                      a.mono.c_str(), a.font_size, a.code_highlight ? 1 : 0,
                      a.tabs.c_str());
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

// Called from JS (fs_dom_nav_splitter) when the project rail drag ends.
extern "C" EMSCRIPTEN_KEEPALIVE void fs_ui_nav_split(int cb_id, int width) {
  std::map<int, std::function<void(int)> >::iterator it =
      nav_split_cbs().find(cb_id);
  if (it == nav_split_cbs().end() || !it->second) return;
  it->second(width);
}

// Called from JS (fs_dom_tab_drag) with "fromKey\ttoKey".
extern "C" EMSCRIPTEN_KEEPALIVE void fs_ui_tab_drag(int cb_id, const char* payload) {
  std::map<int, std::function<void(const std::string&)> >::iterator it =
      tab_drag_cbs().find(cb_id);
  if (it == tab_drag_cbs().end() || !it->second) return;
  it->second(std::string(payload ? payload : ""));
}

// Called from JS (fs_dom_outside_click) when a press lands outside the popup.
extern "C" EMSCRIPTEN_KEEPALIVE void fs_ui_outside_click(int cb_id) {
  std::map<int, std::function<void()> >::iterator it = outside_cbs().find(cb_id);
  if (it == outside_cbs().end() || !it->second) return;
  it->second();
}

// Called from JS (fs_dom_timeout) after the requested delay.
extern "C" EMSCRIPTEN_KEEPALIVE void fs_ui_timeout(int cb_id) {
  std::map<int, std::function<void()> >::iterator it = sched_cbs().find(cb_id);
  if (it == sched_cbs().end()) return;
  std::function<void()> fn = it->second;
  sched_cbs().erase(it);  // one-shot: never fires twice
  if (fn) fn();
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

// Called from JS (fs_dom_cell_size) with the measured character-cell grid.
extern "C" EMSCRIPTEN_KEEPALIVE void fs_ui_cell_size(int cb_id, int cols, int rows) {
  std::map<int, std::function<void(int, int)> >::iterator it =
      cell_size_cbs().find(cb_id);
  if (it == cell_size_cbs().end() || !it->second) return;
  it->second(cols, rows);
}

// Called from JS (fs_dom_file_picker) with the upload result as JSON.
extern "C" EMSCRIPTEN_KEEPALIVE void fs_ui_file_pick(int cb_id, const char* json) {
  std::map<int, std::function<void(const std::string&)> >::iterator it =
      file_pick_cbs().find(cb_id);
  if (it == file_pick_cbs().end() || !it->second) return;
  it->second(std::string(json ? json : "{}"));
}

// Called from JS (fs_dom_scroll_pin) when the terminal surface is scrolled.
extern "C" EMSCRIPTEN_KEEPALIVE void fs_ui_scroll_pin(int cb_id, int at_bottom) {
  std::map<int, std::function<void(bool)> >::iterator it =
      scroll_pin_cbs().find(cb_id);
  if (it == scroll_pin_cbs().end() || !it->second) return;
  it->second(at_bottom != 0);
}

// Called from JS (fs_dom_scroll_edges) when the chat scrolls.
extern "C" EMSCRIPTEN_KEEPALIVE void fs_ui_scroll_edges(int cb_id, int at_top,
                                                        int at_bottom) {
  std::map<int, std::function<void(bool, bool)> >::iterator it =
      scroll_edges_cbs().find(cb_id);
  if (it == scroll_edges_cbs().end() || !it->second) return;
  it->second(at_top != 0, at_bottom != 0);
}

// Called from JS (fs_dom_key_input) with a JSON key description.
extern "C" EMSCRIPTEN_KEEPALIVE void fs_ui_key_input(int cb_id, const char* json) {
  std::map<int, std::function<void(const KeyPress&)> >::iterator it =
      key_input_cbs().find(cb_id);
  if (it == key_input_cbs().end() || !it->second) return;
  KeyPress k;
  try {
    Json j = Json::parse(json ? json : "{}");
    k.key = jsonutil::get<std::string>(j, "key", "");
    k.text = jsonutil::get<std::string>(j, "text", "");
    k.ctrl = jsonutil::get<bool>(j, "ctrl", false);
    k.alt = jsonutil::get<bool>(j, "alt", false);
    k.shift = jsonutil::get<bool>(j, "shift", false);
    k.meta = jsonutil::get<bool>(j, "meta", false);
    k.paste = jsonutil::get<bool>(j, "paste", false);
  } catch (...) {
    return;
  }
  it->second(k);
}

}  // namespace ui
}  // namespace fs

#endif  // __EMSCRIPTEN__
