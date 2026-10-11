// ui_dom_null.cpp - NullDom 空实现
#include "ui_dom.hpp"

namespace fs {
namespace ui {

Node NullDom::create(const std::string& tag, const std::string& cls,
                     const std::string& text) {
  Node n{next_id_++};
  calls_.push_back(Call{"create", n.id, tag + "." + cls + "=" + text});
  return n;
}

void NullDom::append(Node parent, Node child) {
  calls_.push_back(Call{"append", parent.id, "child=" + std::to_string(child.id)});
}

void NullDom::remove(Node node) {
  calls_.push_back(Call{"remove", node.id, ""});
}

void NullDom::clear(Node parent) {
  calls_.push_back(Call{"clear", parent.id, ""});
}

void NullDom::set_text(Node node, const std::string& text) {
  calls_.push_back(Call{"set_text", node.id, text});
}

void NullDom::set_attr(Node node, const std::string& key, const std::string& value) {
  calls_.push_back(Call{"set_attr", node.id, key + "=" + value});
}

void NullDom::set_visible(Node node, bool visible) {
  calls_.push_back(Call{"set_visible", node.id, visible ? "1" : "0"});
}

void NullDom::on(Node node, EventKind kind, EventFn fn) {
  (void)fn;
  calls_.push_back(Call{"on", node.id, std::to_string(static_cast<int>(kind))});
}

}  // namespace ui
}  // namespace fs