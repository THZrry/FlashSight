// ui_dom_browser.cpp - BrowserDom 实现（仅 __EMSCRIPTEN__ 下编译）
#include "ui_dom_browser.hpp"

namespace fs {
namespace ui {

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/bind.h>

namespace {
// 事件回调表：JS 侧按 id 回调回来（见 web/glue.js FsUi.fire）
std::map<uint64_t, EventFn> g_handlers;
uint64_t g_handler_seq = 1;
}  // namespace

// 由 JS 调用：FsUi.fire(handlerId, value)
extern "C" void fs_ui_fire(int handler_id, const char* value);

void fs_ui_fire(int handler_id, const char* value) {
  auto it = g_handlers.find(static_cast<uint64_t>(handler_id));
  if (it != g_handlers.end() && it->second) {
    it->second(value ? value : "");
  }
}

BrowserDom::BrowserDom() {
  // 根容器挂到 document.body（glue.js 保证 #fs-root 存在）
  EM_ASM({ FsUi.ensureRoot(); });
}

BrowserDom::~BrowserDom() = default;

Node BrowserDom::create(const std::string& tag, const std::string& cls,
                        const std::string& text) {
  Node n{next_id_++};
  EM_ASM(
      {
        FsUi.create($0, UTF8ToString($1), UTF8ToString($2), UTF8ToString($3));
      },
      static_cast<int>(n.id), tag.c_str(), cls.c_str(), text.c_str());
  return n;
}

void BrowserDom::append(Node parent, Node child) {
  EM_ASM({ FsUi.append($0, $1); }, static_cast<int>(parent.id),
         static_cast<int>(child.id));
}

void BrowserDom::remove(Node node) {
  EM_ASM({ FsUi.remove($0); }, static_cast<int>(node.id));
}

void BrowserDom::clear(Node parent) {
  EM_ASM({ FsUi.clear($0); }, static_cast<int>(parent.id));
}

void BrowserDom::set_text(Node node, const std::string& text) {
  EM_ASM({ FsUi.setText($0, UTF8ToString($1)); }, static_cast<int>(node.id),
         text.c_str());
}

void BrowserDom::set_attr(Node node, const std::string& key, const std::string& value) {
  EM_ASM({ FsUi.setAttr($0, UTF8ToString($1), UTF8ToString($2)); },
         static_cast<int>(node.id), key.c_str(), value.c_str());
}

void BrowserDom::set_visible(Node node, bool visible) {
  EM_ASM({ FsUi.setVisible($0, $1); }, static_cast<int>(node.id), visible ? 1 : 0);
}

void BrowserDom::on(Node node, EventKind kind, EventFn fn) {
  uint64_t hid = g_handler_seq++;
  g_handlers[hid] = std::move(fn);
  EM_ASM({ FsUi.on($0, $1, $2); }, static_cast<int>(node.id), static_cast<int>(kind),
         static_cast<int>(hid));
}

Node BrowserDom::root() {
  int id = EM_ASM_INT({ return FsUi.rootId(); });
  return Node{static_cast<uint64_t>(id)};
}

#endif  // __EMSCRIPTEN__

}  // namespace ui
}  // namespace fs
