/* web/glue.js - wasm 版极薄胶水（C++ 经 EM_ASM 调用这里，全是 POD 参数）
 * 职责只有五件事：create / append / remove(+clear) / setText+attr+visible / on。
 * 事件回调用 Module._fs_ui_fire(handlerId, valuePtr) 回到 C++。
 * 本文件是静态原型与 wasm 版共用的 DOM 适配层。
 */
(function () {
  var nodes = {};   // id -> Element
  var rootId = 1;

  function el(id) { return nodes[id] || null; }

  window.FsUi = {
    ensureRoot: function () {
      var r = document.getElementById('fs-root');
      if (!r) {
        r = document.createElement('div');
        r.id = 'fs-root';
        document.body.appendChild(r);
      }
      nodes[rootId] = r;
    },
    rootId: function () { return rootId; },
    create: function (id, tag, cls, text) {
      var e = document.createElement(tag || 'div');
      if (cls) e.className = cls;
      if (text) e.textContent = text;
      nodes[id] = e;
    },
    append: function (parent, child) {
      var p = el(parent), c = el(child);
      if (!p || !c || p === c) return;
      // 自环/祖先环防护：child 不能是 parent 自身或其祖先
      var n = p;
      while (n) {
        if (n === c) return;
        n = n.parentNode;
      }
      p.appendChild(c);
    },
    remove: function (id) {
      var e = el(id);
      if (e && e.parentNode) e.parentNode.removeChild(e);
      delete nodes[id];
    },
    clear: function (id) {
      var e = el(id);
      if (e) e.innerHTML = '';
    },
    setText: function (id, text) {
      var e = el(id);
      if (e) e.textContent = text;
    },
    setAttr: function (id, key, value) {
      var e = el(id);
      if (!e) return;
      if (key === 'value' && (e.tagName === 'INPUT' || e.tagName === 'TEXTAREA' || e.tagName === 'SELECT')) {
        e.value = value;
      } else if (key === 'checked') {
        e.checked = !!value;
      } else if (key === 'disabled') {
        e.disabled = !!value;
      } else {
        e.setAttribute(key, value);
      }
    },
    setVisible: function (id, visible) {
      var e = el(id);
      if (e) e.style.display = visible ? '' : 'none';
    },
    on: function (id, kind, handlerId) {
      var e = el(id);
      if (!e) return;
      // kind: 0=click 1=input 2=change 3=keyEnter
      var evts = ['click', 'input', 'change', 'keydown'];
      var ev = evts[kind] || 'click';
      e.addEventListener(ev, function (evt) {
        var v = '';
        if (ev === 'keydown') {
          if (evt.key !== 'Enter') return;
          v = e.value || '';
        } else if (ev === 'change' && e.tagName === 'INPUT' && e.type === 'checkbox') {
          v = e.checked ? '1' : '0';
        } else if (ev === 'change' && e.tagName === 'SELECT') {
          v = e.value;
        } else {
          v = e.value !== undefined ? e.value : (e.textContent || '');
        }
        // 回调 C++：分配临时内存传字符串
        if (typeof Module !== 'undefined' && Module._fs_ui_fire) {
          var len = Module.lengthBytesUTF8(v) + 1;
          var ptr = Module._malloc(len);
          Module.stringToUTF8(v, ptr, len);
          Module._fs_ui_fire(handlerId, ptr);
          Module._free(ptr);
        }
      });
    }
  };
})();
