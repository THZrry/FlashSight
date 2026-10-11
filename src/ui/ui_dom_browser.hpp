// ui_dom_browser.hpp - wasm 分支的 BrowserDom（经极薄 JS 胶水操作浏览器 DOM）
// 约束：本机无 llvm/emscripten，只写不编。全部实现包在 #ifdef __EMSCRIPTEN__ 里，
// 桌面构建不受影响。等有环境后：emcc + --bind + web/glue.js 即得浏览器 wasm 版。
#pragma once

#include "ui_dom.hpp"

#include <string>

namespace fs {
namespace ui {

#ifdef __EMSCRIPTEN__
// BrowserDom：每个 Node 对应浏览器端一个 Element（id 映射存在 JS 侧）。
// 胶水（web/glue.js）只做五件事：create/append/remove/setText+attr/visible/on。
// C++ 侧经 EM_ASM_INT/EM_ASM 传参，全是 POD（int/string），无复杂绑定。
class BrowserDom : public IDom {
 public:
  BrowserDom();
  ~BrowserDom() override;

  Node create(const std::string& tag, const std::string& cls,
              const std::string& text) override;
  void append(Node parent, Node child) override;
  void remove(Node node) override;
  void clear(Node parent) override;
  void set_text(Node node, const std::string& text) override;
  void set_attr(Node node, const std::string& key, const std::string& value) override;
  void set_visible(Node node, bool visible) override;
  void on(Node node, EventKind kind, EventFn fn) override;
  Node root() override;

 private:
  uint64_t next_id_ = 1;
};
#else
// 非 emscripten 构建：BrowserDom 不可用（编译期阻断误用）
class BrowserDom : public IDom {
 public:
  Node create(const std::string&, const std::string&, const std::string&) override {
    return Node{0};
  }
  void append(Node, Node) override {}
  void remove(Node) override {}
  void clear(Node) override {}
  void set_text(Node, const std::string&) override {}
  void set_attr(Node, const std::string&, const std::string&) override {}
  void set_visible(Node, bool) override {}
  void on(Node, EventKind, EventFn) override {}
  Node root() override { return Node{0}; }
};
#endif

}  // namespace ui
}  // namespace fs