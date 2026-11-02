// ui_dom_webview.cpp - DOM Adapter over webview (native window + OS web engine).
//
// The fourth IDom backend:
//   * WasmDom     : emscripten/val.h in a browser tab (the Web build)
//   * NullDom     : no-op + call log (tests)
//   * WhaleDom    : WhaleUI's own C DOM (the WhaleUI desktop path)
//   * WebViewDom  : this file - webview (WebView2 / WebKitGTK / WKWebView)
//                   hosting the same HTML/CSS/JS the browser build uses
//
// ---------------------------------------------------------------------------
// WHY A SEPARATE ADAPTER (not the wasm one)
//
// WasmDom reaches the DOM through emscripten/val, which only exists inside an
// emscripten module. A desktop window is a plain C++ process, so every DOM
// operation crosses an engine boundary. webview offers exactly one useful
// primitive for that: `webview_eval(w, js)` (plus `webview_bind` for callbacks
// INTO C++). There is no synchronous "eval and return a value" - the JS side of
// a binding returns a Promise, and webview_eval returns nothing at all.
//
// That shapes the whole adapter:
//
//   1. NODE IDS ARE ALLOCATED IN C++. The JS glue stores every node in a table
//      under the id we hand it, so no call ever needs to return a value.
//
//   2. READS ARE MIRRORED. get_value(), is_focused(), scroll_top() and friends
//      must answer immediately, so the adapter keeps a mirror that is updated
//      (a) on every write we make and (b) by a `fs_ui_push` binding the JS side
//      calls when the USER changes something (input/change/focus/scroll). The
//      push is delivered before the event handler runs, so a handler that reads
//      get_value() sees the new value.
//
// Everything else is a one-way `webview_eval` call, exactly like the browser
// backend's EM_JS helpers - the JS bodies are the same code, retargeted.
// ---------------------------------------------------------------------------
#include "ui_dom.hpp"

#ifdef FS_UI_WEBVIEW

#include <webview.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "fs_json.hpp"
#include "ui_vdom.hpp"  // bus() - the framework's handler registry

namespace fs {
namespace ui {

namespace {

std::string js_str(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 2);
  out += '"';
  for (size_t i = 0; i < s.size(); ++i) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  out += '"';
  return out;
}

// ---------------------------------------------------------------------------
// The JS side. One table + one dispatch function; the C++ adapter calls it
// through webview_eval with (op, node, node2, a, b).
//
// Installed through webview_init() rather than a one-shot eval: init scripts
// re-run after every navigation, so the glue survives the initial set_html.
// ---------------------------------------------------------------------------
const char* kGlueJs = R"JS(
(function () {
  if (globalThis.__fsDom) return;
  var S = {
    nodes: [null],   // id -> DOM node (id 1 = root)
    handlers: []     // id -> { kind -> [eventName, listener] }
  };
  globalThis.__fsDom = S;

  // The browser's own context menu is a document-level default, so a per-node
  // contextmenu listener never fires on the parts of the page that have no
  // handler: right-clicking anywhere (including empty space) popped the
  // browser's "Back / Reload / Save as / Inspect" menu. Swallow it globally and
  // let the app draw its own menu everywhere - including over text fields, so
  // the menu is the same wherever you click (cut/copy/paste are provided by the
  // app's items, see exec_edit / pasteClipboard below).
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

  function N(i) { return S.nodes[i] || null; }
  // Call a C++ binding. webview exposes bindings as plain JS functions taking
  // POSITIONAL arguments (it wraps them in a Promise internally), so `args`
  // must be spread - passing the array as one argument would deliver
  // `[[a,b,c]]` to the C side.
  function post(name, args) {
    var fn = globalThis[name];
    if (typeof fn !== 'function') return;
    try { fn.apply(null, args); } catch (e) {}
  }

  // Push state the C++ mirror needs. Called BEFORE the app handler runs.
  function push(what, id, v) { post('fs_ui_push', [what, id | 0, String(v == null ? '' : v)]); }

  var NAMES = ['click', 'input', 'change', 'keydown', 'keyenter', 'submit',
               'dblclick', 'contextmenu', 'scroll', 'mouseenter', 'mouseleave',
               'blur'];

  function eventValue(kind, el, evt) {
    var name = NAMES[kind] || 'click';
    if (name === 'keydown') return evt.key == null ? '' : String(evt.key);
    // A context menu has to open AT the pointer, so its value carries the
    // viewport coordinates. Sending the element's text (what every other event
    // does) gave the menu no position to open at.
    if (name === 'contextmenu') {
      return String(evt.clientX | 0) + '\t' + String(evt.clientY | 0);
    }
    // A markdown link reports its TARGET, not its label. The anchor carries
    // data-href and no real href (a real one would navigate the window away from
    // the session), so the click value has to come from the attribute.
    if (name === 'click' && evt && evt.target && evt.target.closest) {
      var a = evt.target.closest('[data-href]');
      if (a) {
        evt.preventDefault();
        return a.getAttribute('data-href') || '';
      }
    }
    var t = el.tagName;
    if (t === 'INPUT' && (el.type === 'checkbox' || el.type === 'radio')) {
      return el.checked ? '1' : '0';
    }
    if (t === 'INPUT' || t === 'TEXTAREA' || t === 'SELECT') {
      return el.value == null ? '' : String(el.value);
    }
    return el.textContent || '';
  }

  function bind(id, kind, hid) {
    var el = N(id);
    if (!el) return;
    if (!S.handlers[id]) S.handlers[id] = {};
    var store = S.handlers[id];
    if (store[kind]) el.removeEventListener(store[kind][0], store[kind][1], false);
    var name = NAMES[kind] || 'click';
    var evName = (name === 'keyenter') ? 'keydown' : name;
    var listener = function (evt) {
      if (name === 'keyenter') {
        // Enter-to-send is a per-element choice (data-enter-send).
        if (evt.key !== 'Enter') return;
        var mode = el.getAttribute && el.getAttribute('data-enter-send');
        var ctrl = evt.ctrlKey || evt.metaKey;
        if (mode === 'ctrlEnter') {
          if (!ctrl) return;
          evt.preventDefault();
        } else {
          if (evt.shiftKey || ctrl) return;
          evt.preventDefault();
        }
      }
      if (name === 'keydown') {
        // While the composer's completion menu is open, the app owns Tab and
        // the arrow keys. Otherwise the engine moves focus / scrolls before
        // the app can accept the highlighted suggestion.
        var k = evt.key == null ? '' : String(evt.key);
        var cap = el.getAttribute && el.getAttribute('data-keys-capture');
        if (cap && (k === 'Tab' || k === 'ArrowUp' || k === 'ArrowDown' ||
                    k === 'Escape')) {
          evt.preventDefault();
        }
      }
      if (name === 'input' || name === 'change') push('value', id, eventValue(kind, el, evt));
      if (name === 'focus') { push('focus', id, '1'); }
      if (name === 'blur') { push('focus', id, '0'); }
      post('fs_ui_fire', [hid, eventValue(kind, el, evt)]);
    };
    store[kind] = [evName, listener];
    el.addEventListener(evName, listener, false);
  }

  // Drag a divider. `axis` 0 = horizontal (side panel), 1 = vertical (drawer).
  // `sign` +1: dragging toward the panel grows it (side panel on the right),
  //         -1: dragging toward the panel shrinks it (nav rail on the left).
  function splitter(barId, panelId, cbName, cbId, axis, sign) {
    var bar = N(barId), panel = N(panelId);
    if (!bar || !panel) return;
    bar['__cb_' + cbName] = cbId;
    bar['__panel_' + cbName] = panel;
    if (!bar['__init_' + cbName]) {
      bar['__init_' + cbName] = 1;
      bar.style.cursor = axis ? 'row-resize' : 'col-resize';
      bar.addEventListener('mousedown', function (down) {
        var cb = bar['__cb_' + cbName];
        var panel = bar['__panel_' + cbName];
        if (!cb || !panel) return;
        var t = down.target;
        if (t && t.classList && t.classList.contains('splitter-handle')) return;
        down.preventDefault();
        var start = axis ? down.clientY : down.clientX;
        var rect = panel.getBoundingClientRect();
        var startSize = axis ? rect.height : rect.width;
        var moved = false;
        function move(ev) {
          var d = (axis ? ev.clientY : ev.clientX) - start;
          if (!moved && Math.abs(d) > 3) moved = true;
          if (!moved) return;
          var size = startSize + sign * d;
          if (axis) {
            if (size < 120) size = 120;
            var maxH = Math.round(window.innerHeight * 0.85);
            if (size > maxH) size = maxH;
            panel.style.height = size + 'px';
          } else {
            if (size < 220) size = 220;
            if (size > 720) size = 720;
            panel.style.width = size + 'px';
            panel.style.flex = '0 0 auto';
          }
        }
        function up() {
          document.removeEventListener('mousemove', move);
          document.removeEventListener('mouseup', up);
          if (!moved) return;
          var swallow = function (ev) { ev.stopPropagation(); ev.preventDefault(); };
          document.addEventListener('click', swallow, true);
          setTimeout(function () { document.removeEventListener('click', swallow, true); }, 0);
          var size = parseInt(axis ? panel.style.height : panel.style.width, 10);
          if (isFinite(size)) post(cbName, [bar['__cb_' + cbName], size]);
        }
        document.addEventListener('mousemove', move);
        document.addEventListener('mouseup', up);
      });
    }
  }

  globalThis.__fsDomCall = function (op, i, j, a, b) {
    var el = N(i), el2 = N(j);
    switch (op) {
      case 'root':
        S.nodes[1] = document.getElementById('fs-root') || document.body;
        return;
      case 'create':  S.nodes[i] = document.createElement(a); return;
      case 'text':    S.nodes[i] = document.createTextNode(a); return;
      case 'append':
        if (el && el2) {
          for (var p = el; p; p = p.parentNode) if (p === el2) return;
          el.appendChild(el2);
        }
        return;
      case 'insertBefore': {
        if (!el || !el2) return;
        var anchor = parseInt(a, 10) || 0;
        var ref = anchor ? N(anchor) : null;
        if (!ref) { if (el.lastChild !== el2) el.appendChild(el2); return; }
        if (el2.nextSibling === ref) return;   // already in place
        el.insertBefore(el2, ref);
        return;
      }
      case 'remove':
        if (el) {
          if (el.parentNode) el.parentNode.removeChild(el);
          S.nodes[i] = null;
          S.handlers[i] = null;
        }
        return;
      case 'clear':   if (el) el.innerHTML = ''; return;
      case 'textSet': if (el) el.textContent = a; return;
      case 'html':    if (el) el.innerHTML = a; return;
      case 'attr':
        if (!el) return;
        if (a === 'value') { el.value = b; push('value', i, b); }
        else if (a === 'checked') el.checked = (b === '1' || b === 'true');
        else if (a === 'disabled') el.disabled = (b === '1' || b === 'true');
        else el.setAttribute(a, b);
        return;
      case 'attrRemove':
        if (!el) return;
        if (a === 'value') { el.value = ''; push('value', i, ''); }
        else if (a === 'checked') el.checked = false;
        else el.removeAttribute(a);
        return;
      case 'style':
        if (!el) return;
        if (a.length > 2 && a[0] === '-' && a[1] === '-') el.style.setProperty(a, b);
        else el.style[a] = b;
        return;
      case 'visible': if (el) el.style.display = (a === '1') ? '' : 'none'; return;
      case 'bind':    bind(i, parseInt(a, 10), parseInt(b, 10)); return;
      case 'unbind': {
        var h = S.handlers[i];
        if (h && h[a]) {
          if (el) el.removeEventListener(h[a][0], h[a][1], false);
          delete h[a];
        }
        return;
      }
      case 'focus': if (el) { try { el.focus(); } catch (e) {} } return;
      // Apply the offset now AND once more in the frame's pre-paint callback.
      //
      // Unlike the browser build (EM_JS, one synchronous task), every call here
      // is a separate ExecuteScript round trip: the transcript patch and this
      // scroll are two different tasks, and the engine may paint the freshly
      // built page at scrollTop 0 in between - the "先到顶、再蹦到位置" report.
      // requestAnimationFrame runs after the patch but BEFORE that frame paints,
      // so the first painted frame is already at the remembered offset. It fires
      // even when the window is hidden; the immediate assignment stays as the
      // fallback for engines that skip rAF while minimized.
      case 'scrollTop': {
        if (!el) return;
        var t = parseInt(a, 10) || 0;
        el.style.scrollBehavior = 'auto';
        el.scrollTop = t;
        el.style.scrollBehavior = '';
        requestAnimationFrame(function () {
          el.style.scrollBehavior = 'auto';
          el.scrollTop = t;
          el.style.scrollBehavior = '';
        });
        return;
      }
      case 'scrollBottom': {
        if (!el) return;
        el.style.scrollBehavior = 'auto';
        el.scrollTop = el.scrollHeight;
        el.style.scrollBehavior = '';
        requestAnimationFrame(function () {
          el.style.scrollBehavior = 'auto';
          el.scrollTop = el.scrollHeight;
          el.style.scrollBehavior = '';
        });
        return;
      }
      case 'scrollPin':
        if (!el) return;
        // `a` is an ENABLE flag ("1"), not a callback id: the host keeps one
        // pin callback. Treating it as an id left it at 0 and the listener
        // bailed out on its own guard.
        el.__fsPinCb = 1;
        if (el.__fsPinInit) return;
        el.__fsPinInit = 1;
        el.addEventListener('scroll', function () {
          var atBottom = el.scrollTop + el.clientHeight >= el.scrollHeight - 2;
          post('fs_ui_scroll_pin', [0, atBottom ? 1 : 0]);
        }, { passive: true });
        return;
      case 'scrollEdges':
        if (!el) return;
        el.__fsEdgeCb = 1;
        if (el.__fsEdgeInit) return;
        el.__fsEdgeInit = 1;
        el.addEventListener('scroll', function () {
          // 24 px, the same margin the wasm adapter uses: the older page starts
          // loading just before the very top, not only on the exact pixel row.
          var top = el.scrollTop <= 24;
          var bot = el.scrollTop + el.clientHeight >= el.scrollHeight - 2;
          post('fs_ui_scroll_edges', [0, top ? 1 : 0, bot ? 1 : 0]);
          // The mirror is keyed by NODE index: on_push() writes slots_[id] and
          // scroll_top(node) reads it back. Passing the CALLBACK id here wrote
          // the offset into an unrelated slot, so the host's copy of the scroll
          // position stayed at 0 - which made every tab switch save 0 and
          // restore 0 ("切回会话就跑到最上面").
          push('scroll', i, el.scrollTop + '\u001f' + el.scrollHeight + '\u001f' + el.clientHeight);
        }, { passive: true });
        return;
      case 'animation':
        if (!el || !a) return;
        el.classList.remove(a);
        void el.offsetWidth;
        el.classList.add(a);
        return;
      case 'classAdd':    if (el) el.classList.add(a); return;
      case 'classRemove': if (el) el.classList.remove(a); return;
      case 'timeout':     setTimeout(function () { post('fs_ui_timeout', [parseInt(a, 10)]); }, parseInt(b, 10) || 0); return;
      // Sign = which way the panel grows relative to the pointer delta, and it
      // is fixed by WHICH EDGE the divider is on - the same geometry the wasm
      // adapter uses:
      //   split    (right side panel, divider = its LEFT edge): drag left = wider
      //   vsplit   (bottom panel, divider = its TOP edge):       drag up   = taller
      //   navsplit (nav rail, divider = its RIGHT edge):         drag right = wider
      // These were all negated here, so every divider grew the WRONG way in the
      // native window ("拖动方向和实际方向相反").
      case 'split':    splitter(i, j, 'fs_ui_split', parseInt(a, 10), 0, -1); return;
      case 'vsplit':   splitter(i, j, 'fs_ui_vsplit', parseInt(a, 10), 1, -1); return;
      case 'navsplit': splitter(i, j, 'fs_ui_nav_split', parseInt(a, 10), 0, 1); return;
      case 'outside':
        if (!el) return;
        el.__fsOutCb = parseInt(a, 10);
        if (el.__fsOutInit) return;
        el.__fsOutInit = 1;
        el.__fsOutHandler = function (ev) {
          // The node may have been removed from the DOM while this document
          // listener stayed installed. Firing then would dismiss whatever popup
          // is open NOW because of a dead one - so a detached popup is inert.
          if (!el.isConnected) return;
          if (!el.__fsOutCb) return;
          var t = ev.target;
          if (el.contains(t)) return;
          // Only the ANCHOR is exempt (the button that toggles this popup).
          // Exempting the whole parent container also swallowed presses on
          // siblings, which made the popup look undismissable.
          if (t && t.closest && t.closest('.model-field, .model-btn-wrap, [data-keep-popup]')) {
            return;
          }
          post('fs_ui_outside', [el.__fsOutCb]);
        };
        document.addEventListener('mousedown', el.__fsOutHandler, true);
        return;
      case 'placePopup': {
        if (!el || !el2) return;
        var ar = el2.getBoundingClientRect();
        var vw = window.innerWidth, vh = window.innerHeight;
        var gap = 8;
        var ph = el.offsetHeight || 420, pw = el.offsetWidth || 640;
        var cb = el.offsetParent || document.body;
        var cr = cb.getBoundingClientRect();
        var ax = ar.left - cr.left - cb.clientLeft;
        var ay = ar.top - cr.top - cb.clientTop;
        var below = vh - ar.bottom - gap;
        var above = ar.top - gap;
        el.style.bottom = 'auto'; el.style.top = 'auto';
        if (below >= ph || below >= above) {
          el.style.top = Math.round(ay + ar.height + gap) + 'px';
          el.classList.add('below');
        } else {
          el.style.top = Math.round(ay - ph - gap) + 'px';
          el.classList.remove('below');
        }
        var leftVp = ar.left;
        if (leftVp + pw > vw - 8) leftVp = vw - pw - 8;
        if (leftVp < 8) leftVp = 8;
        el.style.left = Math.round(leftVp - cr.left - cb.clientLeft) + 'px';
        el.style.right = 'auto';
        return;
      }
      case 'tabDrag':
        if (!el) return;
        el.__fsTabCb = parseInt(a, 10);
        if (el.__fsTabInit) return;
        el.__fsTabInit = 1;
        (function () {
          var fromEl = null, ghost = null, lastTarget = null;
          function isTab(t) {
            return t && t.classList &&
                   (t.classList.contains('tab') || t.classList.contains('ptab'));
          }
          el.addEventListener('mousedown', function (ev) {
            var t = ev.target;
            if (!isTab(t)) return;
            if (t.classList.contains('add')) return;
            fromEl = t;
          });
          el.addEventListener('mousemove', function (ev) {
            if (!fromEl) return;
            var dx = ev.clientX - fromEl.getBoundingClientRect().left;
            if (!ghost && Math.abs(dx) < 6) return;
            if (!ghost) {
              ghost = fromEl.cloneNode(true);
              ghost.classList.add('tab-ghost');
              fromEl.classList.add('dragging');
              document.body.appendChild(ghost);
            }
            ghost.style.position = 'fixed';
            ghost.style.left = ev.clientX + 8 + 'px';
            ghost.style.top = ev.clientY + 8 + 'px';
            ghost.style.pointerEvents = 'none';
            var t = document.elementFromPoint(ev.clientX, ev.clientY);
            while (t && !isTab(t)) t = t.parentElement;
            if (t && t.classList.contains('add')) t = null;
            if (t !== lastTarget) {
              if (lastTarget) lastTarget.classList.remove('drop-target');
              if (t) t.classList.add('drop-target');
              lastTarget = t;
            }
          });
          function finish() {
            if (ghost) { ghost.remove(); ghost = null; }
            if (lastTarget) { lastTarget.classList.remove('drop-target'); }
            if (fromEl && lastTarget && lastTarget !== fromEl &&
                el.__fsTabCb) {
              var f = fromEl.getAttribute('data-key');
              var t = lastTarget.getAttribute('data-key');
              if (f && t) post('fs_ui_tab_drag', [el.__fsTabCb, f + '\t' + t]);
            }
            if (fromEl) fromEl.classList.remove('dragging');
            fromEl = null; lastTarget = null;
          }
          el.addEventListener('mouseup', finish);
          document.addEventListener('mouseup', finish);
        })();
        return;
      case 'keyInput':
        if (!el) return;
        el.__fsKeyCb = parseInt(a, 10);
        if (el.__fsKeyInit) return;
        el.__fsKeyInit = 1;
        if (!el.hasAttribute('tabindex')) el.setAttribute('tabindex', '0');
        el.style.outline = 'none';
        function send(kind, key, text, ev) {
          post('fs_ui_key_input', [el.__fsKeyCb, JSON.stringify({
            key: key || '', text: text || '',
            ctrl: !!(ev && ev.ctrlKey), alt: !!(ev && ev.altKey),
            shift: !!(ev && ev.shiftKey), meta: !!(ev && ev.metaKey),
            paste: kind === 'paste'
          })]);
        }
        el.addEventListener('keydown', function (ev) {
          ev.preventDefault(); ev.stopPropagation();
          send('keydown', ev.key, (ev.key && ev.key.length > 1) ? '' : ev.key, ev);
        });
        el.addEventListener('keypress', function (ev) { ev.preventDefault(); ev.stopPropagation(); });
        el.addEventListener('paste', function (ev) {
          ev.preventDefault(); ev.stopPropagation();
          var text = '';
          try { text = (ev.clipboardData || window.clipboardData).getData('text') || ''; } catch (e) {}
          if (text) send('paste', '', text, ev);
        });
        el.addEventListener('mousedown', function () { try { el.focus(); } catch (e) {} });
        return;
      case 'cellSize':
        if (!el) return;
        el.__fsCellCb = parseInt(a, 10);
        if (!el.__fsCellInit) {
          el.__fsCellInit = 1;
          function report() {
            var probe = el.__fsProbe;
            if (!probe) {
              probe = document.createElement('span');
              probe.textContent = 'MMMMMMMMMM';
              probe.style.position = 'absolute';
              probe.style.visibility = 'hidden';
              probe.style.whiteSpace = 'pre';
              probe.style.pointerEvents = 'none';
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
            var cs2 = getComputedStyle(el);
            var pad = parseFloat(cs2.paddingLeft) + parseFloat(cs2.paddingRight);
            var box = el.getBoundingClientRect();
            var cols = Math.floor((box.width - pad) / w);
            var rows = Math.floor(box.height / lh);
            if (cols < 20) cols = 20;
            if (rows < 5) rows = 5;
            post('fs_ui_cell_size', [el.__fsCellCb, cols, rows]);
          }
          if (typeof ResizeObserver !== 'undefined') {
            el.__fsCellRO = new ResizeObserver(report);
            el.__fsCellRO.observe(el);
          }
          window.addEventListener('resize', report);
          requestAnimationFrame(report);
        }
        return;
      case 'filePicker':
        if (!el) return;
        el.__fsPickCb = parseInt(a, 10);
        el.__fsPickPid = b;
        if (el.__fsPickInit) return;
        el.__fsPickInit = 1;
        el.addEventListener('click', function (ev) {
          ev.preventDefault(); ev.stopPropagation();
          var inp = document.createElement('input');
          inp.type = 'file';
          inp.multiple = true;
          inp.style.display = 'none';
          document.body.appendChild(inp);
          inp.addEventListener('change', function () {
            var files = Array.prototype.slice.call(inp.files || []);
            document.body.removeChild(inp);
            // No server here: hand the names to C++, which copies the bytes
            // itself through the backend (the desktop build has real paths).
            var names = files.map(function (f) { return f.name; });
            post('fs_ui_file_pick', [el.__fsPickCb, JSON.stringify({ names: names })]);
          });
          inp.click();
        });
        return;
      case 'dropZone':
        if (!el) return;
        el.__fsDropCb = parseInt(a, 10);
        if (el.__fsDropInit) return;
        el.__fsDropInit = 1;
        el.addEventListener('dragover', function (ev) {
          ev.preventDefault();
          try { ev.dataTransfer.dropEffect = 'copy'; } catch (e) {}
          el.classList.add('drop-hover');
        });
        el.addEventListener('dragleave', function (ev) {
          if (ev.relatedTarget && el.contains(ev.relatedTarget)) return;
          el.classList.remove('drop-hover');
        });
        el.addEventListener('drop', function (ev) {
          ev.preventDefault();
          el.classList.remove('drop-hover');
          var kind = 'text', value = '';
          try {
            var dt = ev.dataTransfer;
            var items = dt.items;
            if (items && items.length && items[0].webkitGetAsEntry) {
              var entry = items[0].webkitGetAsEntry();
              if (entry) { kind = entry.isDirectory ? 'dir' : 'file'; value = entry.name; }
            }
            if (!value && dt.files && dt.files.length) { kind = 'file'; value = dt.files[0].name; }
            if (!value && dt.getData) {
              var uri = dt.getData('text/uri-list');
              if (uri) {
                var nl = uri.indexOf('\n');
                var first = (nl >= 0 ? uri.substring(0, nl) : uri).replace('\r', '');
                if (first && first.charAt(0) !== '#') { kind = 'url'; value = first; }
              }
            }
            if (!value && dt.getData) { value = dt.getData('text/plain') || ''; kind = 'text'; }
          } catch (e) {}
          if (!value) return;
          post('fs_ui_drop', [el.__fsDropCb, kind, value]);
        });
        return;
      case 'globalKey':
        if (globalThis.__fsGlobalKey) return;
        globalThis.__fsGlobalKey = 1;
        document.addEventListener('keydown', function (ev) {
          // Modifiers are part of the key: "Ctrl+W" is a different command from
          // "w", and the shortcut table defines its bindings with them.
          var k = ev.key || '';
          var mods = '';
          if (ev.ctrlKey) mods += 'Ctrl+';
          if (ev.metaKey) mods += 'Meta+';
          if (ev.altKey) mods += 'Alt+';
          if (ev.shiftKey) mods += 'Shift+';
          post('fs_ui_key', [mods + k]);
        });
        return;
      case 'copy':
        try { navigator.clipboard.writeText(a); } catch (e) {
          try {
            var ta = document.createElement('textarea');
            ta.value = a; document.body.appendChild(ta); ta.select();
            document.execCommand('copy'); document.body.removeChild(ta);
          } catch (e2) {}
        }
        return;
      case 'execEdit':
        // Runs a browser edit command against the focused field. The app draws
        // its own menu over text fields, so cut/copy/paste/select-all have to
        // come from here rather than from the (suppressed) native menu.
        try { document.execCommand(a, false, null); } catch (e) {}
        return;
      case 'pasteClip':
        // execCommand('paste') is refused by every modern engine; the async
        // clipboard API is the only way, and it needs a user gesture (the menu
        // click IS one). Insert at the caret and fire `input` so the app's
        // handler sees the new value.
        (function () {
          var el = document.activeElement;
          if (!el || !navigator.clipboard || !navigator.clipboard.readText) return;
          navigator.clipboard.readText().then(function (t) {
            if (!t) return;
            try {
              var s = el.selectionStart, e2 = el.selectionEnd;
              if (typeof s === 'number' && typeof e2 === 'number') {
                el.value = el.value.slice(0, s) + t + el.value.slice(e2);
                el.selectionStart = el.selectionEnd = s + t.length;
              } else {
                el.value += t;
              }
              el.dispatchEvent(new Event('input', { bubbles: true }));
            } catch (err) {}
          }).catch(function () {});
        })();
        return;
      case 'appearance': {
        var root = document.documentElement;
        var parts = b.split('\u001f');
        var pal = a, mode = parts[0] || 'auto', font = parts[1] || '';
        var mono = parts[2] || '', size = parseInt(parts[3], 10) || 0;
        var tabs = parts[4] || 'top', hl = parts[5] || '1';
        if (mode === 'auto' || !mode) {
          mode = (window.matchMedia && window.matchMedia('(prefers-color-scheme: dark)').matches)
                 ? 'dark' : 'light';
        }
        var cls = [];
        for (var ci = 0; ci < root.classList.length; ci++) {
          var c = root.classList[ci];
          if (c.indexOf('palette-') !== 0) cls.push(c);
        }
        if (pal && pal !== 'default') cls.push('palette-' + pal);
        root.className = cls.join(' ');
        root.setAttribute('data-mode', mode);
        root.setAttribute('data-tabs', tabs);
        root.setAttribute('data-code-highlight', hl === '1' ? '1' : '0');
        root.style.setProperty('--font-user', font ? ('"' + font + '", var(--font-base)') : '');
        root.style.setProperty('--mono-user', mono ? ('"' + mono + '", var(--mono-base)') : '');
        root.style.setProperty('--font-size-user', size > 0 ? (size + 'px') : '');
        return;
      }
      default: return;
    }
  };
})();
)JS";

// The glue above uses `post(name, args)` to call back into C++. webview exposes
// bindings as async functions, so `post` is fire-and-forget: it hands the
// payload to the named binding and ignores the returned promise.
const char* kPostJs = R"JS(
(function () {
  if (globalThis.__fsPostReady) return;
  globalThis.__fsPostReady = 1;
})();
)JS";

class WebViewDom : public IDom {
 public:
  WebViewDom(webview_t w) : w_(w) {
    g_dom = this;
    // The glue is already installed by webview_install_glue() (called before
    // the first document load); binding the callbacks is all that is left.
    install_bindings();
    call("root", 0, 0, "", "");
    slots_.resize(2);
    slots_[1].valid = true;
    probe();
  }

  ~WebViewDom() override {
    if (g_dom == this) g_dom = nullptr;
  }

  // ---- structure ---------------------------------------------------------

  Node create_element(const std::string& tag) override {
    uint64_t id = alloc();
    call("create", static_cast<int>(id), 0, tag, "");
    return Node{id};
  }
  Node create_text(const std::string& text) override {
    uint64_t id = alloc();
    call("text", static_cast<int>(id), 0, text, "");
    slots_[id].is_text = true;
    slots_[id].text = text;
    return Node{id};
  }
  void append(Node parent, Node child) override {
    call("append", idx(parent), idx(child), "", "");
  }
  void insert_before(Node parent, Node child, Node ref) override {
    // `ref` travels in slot `a` (the glue's signature is (op, i, j, a, b) and
    // `j` is the child). Dropping it here made every reorder append to the end
    // instead of inserting at the anchor - which reads as "the page went blank"
    // once the diff starts moving keyed children around.
    call("insertBefore", idx(parent), idx(child), std::to_string(idx(ref)), "");
  }
  void remove(Node node) override {
    if (!node.valid()) return;
    call("remove", idx(node), 0, "", "");
    if (node.id < slots_.size()) slots_[node.id].valid = false;
    if (focused_ == node.id) focused_ = 0;
  }
  void clear(Node parent) override { call("clear", idx(parent), 0, "", ""); }

  // ---- content / attributes ---------------------------------------------

  void set_text(Node node, const std::string& text) override {
    call("textSet", idx(node), 0, text, "");
    if (node.id < slots_.size()) slots_[node.id].text = text;
  }
  void set_html(Node node, const std::string& html) override {
    call("html", idx(node), 0, html, "");
  }
  void set_attr(Node node, const std::string& key, const std::string& value) override {
    call("attr", idx(node), 0, key, value);
    if (key == "value" && node.id < slots_.size()) slots_[node.id].value = value;
  }
  void remove_attr(Node node, const std::string& key) override {
    call("attrRemove", idx(node), 0, key, "");
    if (key == "value" && node.id < slots_.size()) slots_[node.id].value.clear();
  }
  void set_style(Node node, const std::string& key, const std::string& value) override {
    call("style", idx(node), 0, key, value);
  }
  void set_visible(Node node, bool visible) override {
    call("visible", idx(node), 0, visible ? "1" : "0", "");
  }
  std::string get_value(Node node) override {
    return node.id < slots_.size() ? slots_[node.id].value : std::string();
  }
  bool is_focused(Node node) override { return focused_ == node.id; }

  // ---- events ------------------------------------------------------------

  void on(Node node, EventKind kind, int handler_id) override {
    call("bind", idx(node), 0, std::to_string(static_cast<int>(kind)),
         std::to_string(handler_id));
  }
  void off(Node node, EventKind kind) override {
    call("unbind", idx(node), 0, std::to_string(static_cast<int>(kind)), "");
  }

  // ---- splitters / popovers / tab drag -----------------------------------

  void bind_splitter(Node splitter, Node side,
                     std::function<void(int)> on_resize) override {
    bind_drag("split", splitter, side, on_resize, split_cbs_);
  }
  void bind_vsplitter(Node splitter, Node panel,
                      std::function<void(int)> on_resize) override {
    bind_drag("vsplit", splitter, panel, on_resize, vsplit_cbs_);
  }
  void bind_nav_splitter(Node splitter, Node nav,
                         std::function<void(int)> on_resize) override {
    bind_drag("navsplit", splitter, nav, on_resize, nav_split_cbs_);
  }
  void bind_tab_drag(Node strip, std::function<void(const std::string&)> cb) override {
    tab_drag_cb_ = cb;
    call("tabDrag", idx(strip), 0, "", "");
  }
  void place_popup(Node popup, Node anchor) override {
    call("placePopup", idx(popup), idx(anchor), "", "");
  }
  void bind_outside_click(Node popup, std::function<void()> cb) override {
    // ONE callback per popup, keyed by the popup node. A single slot meant the
    // last binding won: with a model picker open and a context menu opened on
    // top, the picker's dismissal callback was overwritten by the menu's - and
    // the document listeners that earlier popups installed kept firing the
    // NEWEST callback, so a press outside closed the wrong thing (or nothing).
    int id = ++outside_seq_;
    if (cb) {
      outside_cbs_[id] = std::move(cb);
    }
    outside_by_node_[popup.id] = id;
    call("outside", idx(popup), 0, std::to_string(id), "");
  }

  // ---- drop zone ---------------------------------------------------------

  void bind_drop_zone(
      Node node,
      std::function<void(const std::string&, const std::string&)> cb) override {
    drop_cb_ = cb;
    call("dropZone", idx(node), 0, "", "");
  }

  // ---- keyboard ----------------------------------------------------------

  void bind_global_key(std::function<void(const std::string&)> cb) override {
    global_key_cb_ = cb;
    call("globalKey", 0, 0, "", "");
  }
  void bind_key_input(Node node, std::function<void(const KeyPress&)> cb) override {
    key_input_cb_ = cb;
    call("keyInput", idx(node), 0, "", "");
  }
  void focus(Node node) override { call("focus", idx(node), 0, "", ""); }

  // ---- terminal geometry -------------------------------------------------

  void bind_cell_size(Node node, std::function<void(int, int)> cb) override {
    cell_cb_ = cb;
    call("cellSize", idx(node), 0, "", "");
  }
  void bind_scroll_pin(Node node, std::function<void(bool)> cb) override {
    scroll_pin_cb_ = cb;
    // "1" is an ENABLE flag, not an id: this adapter keeps ONE callback per kind.
    // Passing "" made the glue's `parseInt(a)||0` produce 0, and the listener
    // bailed out on `if (!el.__fsPinCb) return` - so a terminal never reported
    // "at the end" in the desktop build.
    call("scrollPin", idx(node), 0, "1", "");
  }
  void scroll_to_bottom(Node node) override {
    call("scrollBottom", idx(node), 0, "", "");
  }
  void bind_scroll_edges(Node node,
                         std::function<void(bool, bool)> cb) override {
    scroll_edges_cb_ = cb;
    // Same flag contract as scrollPin. Without it the scroll listener returned
    // immediately: no edges, no mirrored offset, so 回到顶部/加载更早 and the
    // remember-the-position-on-tab-switch path were all dead in webview.
    call("scrollEdges", idx(node), 0, "1", "");
  }
  int scroll_top(Node node) override {
    return node.id < slots_.size() ? slots_[node.id].scroll_top : 0;
  }
  void set_scroll_top(Node node, int top) override {
    call("scrollTop", idx(node), 0, std::to_string(top), "");
    // Mirror the write HERE, synchronously. This adapter cannot read the live
    // offset back (the push is async), so without this the mirror stayed at the
    // previous value until the next scroll event arrived - and a tab switch
    // saves the offset by reading exactly this. Switching away right after a
    // restore therefore persisted the OLD position, which is the "滚动状态没保存"
    // report. The wasm adapter reads el.scrollTop directly and never had this.
    if (node.id < slots_.size()) slots_[node.id].scroll_top = top;
  }
  int scroll_height(Node node) override {
    return node.id < slots_.size() ? slots_[node.id].scroll_height : 0;
  }
  int client_height(Node node) override {
    return node.id < slots_.size() ? slots_[node.id].client_height : 0;
  }

  // ---- clipboard / files -------------------------------------------------

  void copy_text(const std::string& text) override {
    call("copy", 0, 0, text, "");
  }
  bool exec_edit(const std::string& command) override {
    // document.execCommand is deprecated but is the only synchronous way to
    // drive the browser's edit pipeline. "paste" is refused by the engine, so it
    // goes through the async clipboard API instead.
    call("execEdit", 0, 0, command, "");
    if (command == "paste") call("pasteClip", 0, 0, "", "");
    return true;
  }
  void bind_file_picker(Node node, const std::string& pid,
                        std::function<void(const std::string&)> cb) override {
    file_pick_cb_ = cb;
    call("filePicker", idx(node), 0, pid, "");
  }

  // ---- animation ---------------------------------------------------------

  void play_animation(Node node, const std::string& cls) override {
    call("animation", idx(node), 0, cls, "");
  }
  void schedule(int ms, std::function<void()> cb) override {
    if (!cb) return;
    int id = ++sched_seq_;
    sched_cbs_[id] = std::move(cb);
    call("timeout", 0, 0, std::to_string(id), std::to_string(ms));
  }
  bool supports_exit_animations() const override { return true; }

  // ---- appearance --------------------------------------------------------

  void set_appearance(const Appearance& a) override {
    std::string packed = a.mode + "\x1f" + a.font + "\x1f" + a.mono + "\x1f" +
                         std::to_string(a.font_size) + "\x1f" + a.tabs + "\x1f" +
                         (a.code_highlight ? "1" : "0");
    call("appearance", 0, 0, a.palette, packed);
  }

  // ---- root / identity ---------------------------------------------------

  Node root() override { return Node{1}; }
  const char* backend_name() const override { return "webview"; }

  // ---- callbacks from the JS glue (called by the trampolines below) -------

  void on_fire(int hid, const std::string& v) {
    if (hid <= -1) {
      // Answer to probe()/probe_deep(): only interesting when debugging, and
      // it would otherwise be noise on every normal start.
      if (getenv("FS_UI_WEBVIEW_DEBUG")) {
        std::fprintf(stderr, "[webview] probe %d: %s\n", hid, v.c_str());
      }
      return;
    }
    if (getenv("FS_UI_WEBVIEW_DEBUG")) {
      std::fprintf(stderr, "[webview] fire hid=%d value=%.60s\n", hid, v.c_str());
    }
    bus().fire(hid, v);
  }

  void on_push(const std::string& what, int id, const std::string& v) {
    if (id <= 0 || static_cast<size_t>(id) >= slots_.size()) return;
    Slot& s = slots_[static_cast<size_t>(id)];
    if (what == "value") {
      s.value = v;
    } else if (what == "focus") {
      if (v == "1") {
        focused_ = static_cast<uint64_t>(id);
      } else if (focused_ == static_cast<uint64_t>(id)) {
        focused_ = 0;
      }
    } else if (what == "scroll") {
      size_t p1 = v.find('\x1f');
      size_t p2 = p1 == std::string::npos ? p1 : v.find('\x1f', p1 + 1);
      if (p1 != std::string::npos) {
        s.scroll_top = std::atoi(v.substr(0, p1).c_str());
        if (p2 != std::string::npos) {
          s.scroll_height = std::atoi(v.substr(p1 + 1, p2 - p1 - 1).c_str());
          s.client_height = std::atoi(v.substr(p2 + 1).c_str());
        }
      }
    }
  }

  void on_scroll_pin(int at_bottom) {
    if (scroll_pin_cb_) scroll_pin_cb_(at_bottom != 0);
  }
  void on_scroll_edges(int at_top, int at_bottom) {
    if (scroll_edges_cb_) scroll_edges_cb_(at_top != 0, at_bottom != 0);
  }
  void on_timeout(int id) {
    std::map<int, std::function<void()> >::iterator it = sched_cbs_.find(id);
    if (it == sched_cbs_.end()) return;
    std::function<void()> fn = it->second;
    sched_cbs_.erase(it);
    fn();
  }
  void on_key_input(const std::string& json) {
    if (!key_input_cb_) return;
    KeyPress k;
    try {
      Json j = Json::parse(json);
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
    key_input_cb_(k);
  }
  void on_cell_size(int cols, int rows) {
    if (cell_cb_) cell_cb_(cols, rows);
  }
  void on_drop(const std::string& kind, const std::string& value) {
    if (drop_cb_) drop_cb_(kind, value);
  }
  void on_split(int cb_id, int size, bool vertical) {
    std::map<int, std::function<void(int)> >& m =
        vertical ? vsplit_cbs_ : split_cbs_;
    std::map<int, std::function<void(int)> >::iterator it = m.find(cb_id);
    if (it != m.end() && it->second) it->second(size);
  }
  void on_nav_split(int cb_id, int size) {
    std::map<int, std::function<void(int)> >::iterator it =
        nav_split_cbs_.find(cb_id);
    if (it != nav_split_cbs_.end() && it->second) it->second(size);
  }
  void on_tab_drag(const std::string& pair) {
    if (tab_drag_cb_) tab_drag_cb_(pair);
  }
  void on_outside(int cb_id) {
    std::map<int, std::function<void()> >::iterator it = outside_cbs_.find(cb_id);
    if (it == outside_cbs_.end() || !it->second) return;
    std::function<void()> fn = it->second;
    // Erase BEFORE calling: the callback tears the popup down, and a second
    // press arriving (or the same press reaching another listener) must not run
    // it twice.
    outside_cbs_.erase(it);
    fn();
  }
  void on_file_pick(const std::string& json) {
    if (file_pick_cb_) file_pick_cb_(json);
  }
  void on_global_key(const std::string& key) {
    if (global_key_cb_) global_key_cb_(key);
  }

  // Each JS `post('fs_ui_x', [a,b,c])` arrives as a JSON array.
  void dispatch(const std::string& name, const std::string& json);

  webview_t handle() const { return w_; }

  static WebViewDom* g_dom;

 private:
  struct Slot {
    bool valid = false;
    bool is_text = false;
    std::string value;
    std::string text;
    int scroll_top = 0;
    int scroll_height = 0;
    int client_height = 0;
  };

  uint64_t alloc() {
    uint64_t id = next_id_++;
    if (slots_.size() <= id) slots_.resize(static_cast<size_t>(id) + 1);
    slots_[id].valid = true;
    return id;
  }
  int idx(Node n) {
    return (n.valid() && n.id < slots_.size() && slots_[n.id].valid)
               ? static_cast<int>(n.id)
               : 0;
  }

  void call(const char* op, int i, int j, const std::string& a,
            const std::string& b) {
    std::string js = "globalThis.__fsDomCall(";
    js += js_str(op);
    js += "," + std::to_string(i) + "," + std::to_string(j) + ",";
    js += js_str(a) + "," + js_str(b) + ")";
    webview_eval(w_, js.c_str());
  }

  // Same, but reports whether the glue is actually present. Used once at
  // startup: a blank window is otherwise indistinguishable from a broken
  // adapter, and the only way to see inside is a script that writes back.
  void probe() {
    webview_eval(w_,
                 "try{fs_ui_fire(-1, typeof __fsDomCall);}catch(e){"
                 "try{fs_ui_fire(-2, 'ERR '+e);}catch(e2){}}");
  }

  // Diagnostics: report how many nodes/listeners the glue holds, and whether a
  // programmatic click reaches the app. Run with FS_UI_WEBVIEW_DEBUG=1.
  void probe_deep() {
    webview_eval(w_,
                 "(function(){"
                 "var S=globalThis.__fsDom;"
                 "if(!S){fs_ui_fire(-3,'no __fsDom');return;}"
                 "var nodes=0,binds=0;"
                 "for(var k in S.handlers){var h=S.handlers[k];"
                 "for(var q in h){if(h[q])binds++;}}"
                 "for(var i=1;i<S.nodes.length;i++){if(S.nodes[i])nodes++;}"
                 "fs_ui_fire(-4,'nodes='+nodes+' binds='+binds);"
                 "var tabs=document.querySelectorAll('.title-btn');"
                 "fs_ui_fire(-5,'titleBtns='+tabs.length);"
                 "if(tabs.length>1){tabs[0].click();"
                 "fs_ui_fire(-6,'clicked 0 -> active='+"
                 "(document.querySelector('.title-btn.active')||{}).textContent);}"
                 "})()");
  }

  void bind_drag(const char* op, Node splitter, Node target,
                 std::function<void(int)> cb,
                 std::map<int, std::function<void(int)> >& table) {
    if (!cb) return;
    int id = ++split_seq_;
    table.clear();  // one live binding per axis, like the browser backend
    table[id] = std::move(cb);
    call(op, idx(splitter), idx(target), std::to_string(id), "");
  }

  void install_bindings();

  webview_t w_ = nullptr;
  std::vector<Slot> slots_;
  uint64_t next_id_ = 2;
  uint64_t focused_ = 0;
  int split_seq_ = 0;
  int sched_seq_ = 0;
  std::map<int, std::function<void(int)> > split_cbs_;
  std::map<int, std::function<void(int)> > vsplit_cbs_;
  std::map<int, std::function<void(int)> > nav_split_cbs_;
  std::map<int, std::function<void()> > sched_cbs_;
  std::function<void(const std::string&, const std::string&)> drop_cb_;
  std::function<void(const std::string&)> global_key_cb_;
  std::function<void(const KeyPress&)> key_input_cb_;
  std::function<void(int, int)> cell_cb_;
  std::function<void(bool)> scroll_pin_cb_;
  std::function<void(bool, bool)> scroll_edges_cb_;
  std::function<void(const std::string&)> tab_drag_cb_;
  // Outside-click dismissal, one entry per live popup (see bind_outside_click).
  std::map<int, std::function<void()> > outside_cbs_;
  std::map<uint64_t, int> outside_by_node_;  // popup node id -> callback id
  int outside_seq_ = 0;
  std::function<void(const std::string&)> file_pick_cb_;
};

WebViewDom* WebViewDom::g_dom = nullptr;

// ---------------------------------------------------------------------------
// Trampolines. webview needs plain C function pointers; each parses the JSON
// payload and forwards to the adapter.
//
// WIRE FORMAT: webview's `bind` exposes a JS function that takes POSITIONAL
// arguments (it does `params = [name].concat(arguments)` internally), so the
// C callback receives a JSON ARRAY of the arguments, e.g. `fs_ui_fire(hid, v)`
// arrives as `[hid, "v"]`. Do NOT pass one array as a single argument.
// ---------------------------------------------------------------------------
std::string arg_str(const Json& j, size_t i) {
  if (j.size() <= i) return std::string();
  if (j[i].is_string()) return j[i].get<std::string>();
  if (j[i].is_number_integer()) return std::to_string(j[i].get<long long>());
  return std::string();
}
int arg_int(const Json& j, size_t i) {
  if (j.size() <= i) return 0;
  if (j[i].is_number()) return j[i].get<int>();
  if (j[i].is_string()) return std::atoi(j[i].get<std::string>().c_str());
  return 0;
}

void cb_fire(const char* /*id*/, const char* req, void* /*arg*/) {
  WebViewDom* d = WebViewDom::g_dom;
  if (!d) return;
  try {
    Json j = Json::parse(req ? req : "[]");
    d->on_fire(arg_int(j, 0), arg_str(j, 1));
  } catch (...) {
  }
}

void cb_push(const char* /*id*/, const char* req, void* /*arg*/) {
  WebViewDom* d = WebViewDom::g_dom;
  if (!d) return;
  try {
    Json j = Json::parse(req ? req : "[]");
    d->on_push(arg_str(j, 0), arg_int(j, 1), arg_str(j, 2));
  } catch (...) {
  }
}

void cb_scroll_pin(const char* /*id*/, const char* req, void* /*arg*/) {
  WebViewDom* d = WebViewDom::g_dom;
  if (!d) return;
  try {
    Json j = Json::parse(req ? req : "[]");
    d->on_scroll_pin(arg_int(j, 1));
  } catch (...) {
  }
}

void cb_scroll_edges(const char* /*id*/, const char* req, void* /*arg*/) {
  WebViewDom* d = WebViewDom::g_dom;
  if (!d) return;
  try {
    Json j = Json::parse(req ? req : "[]");
    d->on_scroll_edges(arg_int(j, 1), arg_int(j, 2));
  } catch (...) {
  }
}

void cb_timeout(const char* /*id*/, const char* req, void* /*arg*/) {
  WebViewDom* d = WebViewDom::g_dom;
  if (!d) return;
  try {
    Json j = Json::parse(req ? req : "[]");
    d->on_timeout(arg_int(j, 0));
  } catch (...) {
  }
}

void cb_key_input(const char* /*id*/, const char* req, void* /*arg*/) {
  WebViewDom* d = WebViewDom::g_dom;
  if (!d) return;
  try {
    Json j = Json::parse(req ? req : "[]");
    d->on_key_input(arg_str(j, 1));
  } catch (...) {
  }
}

void cb_cell_size(const char* /*id*/, const char* req, void* /*arg*/) {
  WebViewDom* d = WebViewDom::g_dom;
  if (!d) return;
  try {
    Json j = Json::parse(req ? req : "[]");
    d->on_cell_size(arg_int(j, 1), arg_int(j, 2));
  } catch (...) {
  }
}

void cb_drop(const char* /*id*/, const char* req, void* /*arg*/) {
  WebViewDom* d = WebViewDom::g_dom;
  if (!d) return;
  try {
    Json j = Json::parse(req ? req : "[]");
    d->on_drop(arg_str(j, 1), arg_str(j, 2));
  } catch (...) {
  }
}

void cb_split(const char* /*id*/, const char* req, void* /*arg*/) {
  WebViewDom* d = WebViewDom::g_dom;
  if (!d) return;
  try {
    Json j = Json::parse(req ? req : "[]");
    d->on_split(arg_int(j, 0), arg_int(j, 1), false);
  } catch (...) {
  }
}

void cb_vsplit(const char* /*id*/, const char* req, void* /*arg*/) {
  WebViewDom* d = WebViewDom::g_dom;
  if (!d) return;
  try {
    Json j = Json::parse(req ? req : "[]");
    d->on_split(arg_int(j, 0), arg_int(j, 1), true);
  } catch (...) {
  }
}

void cb_nav_split(const char* /*id*/, const char* req, void* /*arg*/) {
  WebViewDom* d = WebViewDom::g_dom;
  if (!d) return;
  try {
    Json j = Json::parse(req ? req : "[]");
    d->on_nav_split(arg_int(j, 0), arg_int(j, 1));
  } catch (...) {
  }
}

void cb_tab_drag(const char* /*id*/, const char* req, void* /*arg*/) {
  WebViewDom* d = WebViewDom::g_dom;
  if (!d) return;
  try {
    Json j = Json::parse(req ? req : "[]");
    d->on_tab_drag(arg_str(j, 1));
  } catch (...) {
  }
}

void cb_outside(const char* /*id*/, const char* req, void* /*arg*/) {
  WebViewDom* d = WebViewDom::g_dom;
  if (!d) return;
  try {
    Json j = Json::parse(req ? req : "[]");
    d->on_outside(arg_int(j, 0));
  } catch (...) {
  }
}

void cb_file_pick(const char* /*id*/, const char* req, void* /*arg*/) {
  WebViewDom* d = WebViewDom::g_dom;
  if (!d) return;
  try {
    Json j = Json::parse(req ? req : "[]");
    d->on_file_pick(arg_str(j, 1));
  } catch (...) {
  }
}

void cb_global_key(const char* /*id*/, const char* req, void* /*arg*/) {
  WebViewDom* d = WebViewDom::g_dom;
  if (!d) return;
  try {
    Json j = Json::parse(req ? req : "[]");
    d->on_global_key(arg_str(j, 0));
  } catch (...) {
  }
}

void WebViewDom::install_bindings() {
  webview_bind(w_, "fs_ui_fire", cb_fire, nullptr);
  webview_bind(w_, "fs_ui_push", cb_push, nullptr);
  webview_bind(w_, "fs_ui_scroll_pin", cb_scroll_pin, nullptr);
  webview_bind(w_, "fs_ui_scroll_edges", cb_scroll_edges, nullptr);
  webview_bind(w_, "fs_ui_timeout", cb_timeout, nullptr);
  webview_bind(w_, "fs_ui_key_input", cb_key_input, nullptr);
  webview_bind(w_, "fs_ui_cell_size", cb_cell_size, nullptr);
  webview_bind(w_, "fs_ui_drop", cb_drop, nullptr);
  webview_bind(w_, "fs_ui_split", cb_split, nullptr);
  webview_bind(w_, "fs_ui_vsplit", cb_vsplit, nullptr);
  webview_bind(w_, "fs_ui_nav_split", cb_nav_split, nullptr);
  webview_bind(w_, "fs_ui_tab_drag", cb_tab_drag, nullptr);
  webview_bind(w_, "fs_ui_outside", cb_outside, nullptr);
  webview_bind(w_, "fs_ui_file_pick", cb_file_pick, nullptr);
  webview_bind(w_, "fs_ui_key", cb_global_key, nullptr);
}

}  // namespace

// Install the JS glue. MUST be called BEFORE the first set_html/navigate:
// webview_init() registers a script that runs when a document is created, so
// a document loaded earlier never sees it (every call would then silently hit
// an undefined `__fsDomCall` and the page stays blank).
void webview_install_glue(webview_t w) {
  webview_init(w, kGlueJs);
  webview_eval(w, kPostJs);
}

IDom* make_webview_dom(webview_t w) { return new WebViewDom(w); }

// Diagnostics entry (FS_UI_WEBVIEW_DEBUG=1): dump the glue's view of the page
// and simulate a click on the first title button.
void webview_debug_probe(webview_t w, void* /*dom*/) {
  webview_eval(w,
      "(function(){"
      "var S=globalThis.__fsDom;"
      "if(!S){fs_ui_fire(-3,'no __fsDom');return;}"
      "var nodes=0,binds=0;"
      "for(var k in S.handlers){var h=S.handlers[k];"
      "for(var q in h){if(h[q])binds++;}}"
      "for(var i=1;i<S.nodes.length;i++){if(S.nodes[i])nodes++;}"
      "fs_ui_fire(-4,'nodes='+nodes+' binds='+binds);"
      "var t=document.querySelectorAll('.title-btn');"
      "fs_ui_fire(-5,'titleBtns='+t.length);"
      "if(t.length>1){t[0].click();"
      "var a=document.querySelector('.title-btn.active');"
      "fs_ui_fire(-6,'after click active='+(a?a.textContent:'?'));}"
      "})()");
}

}  // namespace ui
}  // namespace fs

#endif  // FS_UI_WEBVIEW
