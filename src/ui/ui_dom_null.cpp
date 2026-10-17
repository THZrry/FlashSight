// ui_dom_null.cpp - NullDom: call-logging DOM used by the native build and tests.
#include "ui_dom.hpp"

#include <algorithm>
#include <map>
#include <sstream>

namespace fs {
namespace ui {

const char* event_kind_name(EventKind k) {
  switch (k) {
    case EventKind::kClick: return "click";
    case EventKind::kInput: return "input";
    case EventKind::kChange: return "change";
    case EventKind::kKeyDown: return "keydown";
    case EventKind::kKeyEnter: return "keyenter";
    case EventKind::kSubmit: return "submit";
    case EventKind::kDblClick: return "dblclick";
    case EventKind::kContextMenu: return "contextmenu";
    case EventKind::kScroll: return "scroll";
    case EventKind::kMouseEnter: return "mouseenter";
    case EventKind::kMouseLeave: return "mouseleave";
    case EventKind::kBlur: return "blur";
  }  return "?";
}

// ---- tree model (per instance, kept in Impl so tests are isolated) ----
struct NullDom::Impl {
  struct FakeNode {
    std::string tag;
    std::string text;
    std::vector<std::pair<std::string, std::string> > attrs;
    std::vector<uint64_t> children;
    uint64_t parent = 0;
    bool visible = true;
  };
  std::map<uint64_t, FakeNode> nodes;

  static void set_attr_f(FakeNode& n, const std::string& k, const std::string& v) {
    for (size_t i = 0; i < n.attrs.size(); ++i) {
      if (n.attrs[i].first == k) {
        n.attrs[i].second = v;
        return;
      }
    }
    n.attrs.push_back(std::make_pair(k, v));
  }

  static std::string attr_str(const FakeNode& n) {
    std::string s;
    for (size_t i = 0; i < n.attrs.size(); ++i) {
      if (n.attrs[i].second.empty()) continue;
      s += " " + n.attrs[i].first + "=\"" + n.attrs[i].second + "\"";
    }
    return s;
  }

  void dump_rec(std::ostringstream& os, uint64_t id, int depth) const {
    std::map<uint64_t, FakeNode>::const_iterator it = nodes.find(id);
    if (it == nodes.end()) return;
    const FakeNode& n = it->second;
    os << std::string(static_cast<size_t>(depth) * 2, ' ');
    if (n.tag == "#text") {
      os << "\"" << n.text << "\"\n";
    } else {
      os << "<" << n.tag << attr_str(n) << ">";
      if (!n.visible) os << " (hidden)";
      os << "\n";
    }
    for (size_t i = 0; i < n.children.size(); ++i) dump_rec(os, n.children[i], depth + 1);
  }
};

NullDom::NullDom() : impl_(new Impl()) {
  // materialize the root (id 1) so dump() always has an anchor
  Impl::FakeNode r;
  r.tag = "#root";
  impl_->nodes[1] = r;
}

NullDom::~NullDom() {}

Node NullDom::create_element(const std::string& tag) {
  Node n{next_id_++};
  Impl::FakeNode f;
  f.tag = tag;
  impl_->nodes[n.id] = f;
  calls_.push_back(Call{"create_element", n.id, tag});
  return n;
}

Node NullDom::create_text(const std::string& text) {
  Node n{next_id_++};
  Impl::FakeNode f;
  f.tag = "#text";
  f.text = text;
  impl_->nodes[n.id] = f;
  calls_.push_back(Call{"create_text", n.id, text});
  return n;
}

void NullDom::append(Node parent, Node child) {
  std::map<uint64_t, Impl::FakeNode>::iterator p = impl_->nodes.find(parent.id);
  if (p != impl_->nodes.end()) p->second.children.push_back(child.id);
  std::map<uint64_t, Impl::FakeNode>::iterator c = impl_->nodes.find(child.id);
  if (c != impl_->nodes.end()) c->second.parent = parent.id;
  calls_.push_back(Call{"append", child.id, std::to_string(parent.id)});
}

void NullDom::insert_before(Node parent, Node child, Node ref) {
  std::map<uint64_t, Impl::FakeNode>::iterator p = impl_->nodes.find(parent.id);
  if (p != impl_->nodes.end()) {
    std::vector<uint64_t>& v = p->second.children;
    std::vector<uint64_t>::iterator it = std::find(v.begin(), v.end(), ref.id);
    if (it != v.end()) {
      v.insert(it, child.id);
    } else {
      v.push_back(child.id);
    }
  }
  std::map<uint64_t, Impl::FakeNode>::iterator c = impl_->nodes.find(child.id);
  if (c != impl_->nodes.end()) c->second.parent = parent.id;
  calls_.push_back(Call{"insert_before", child.id,
                        std::to_string(parent.id) + "<" + std::to_string(ref.id)});
}

void NullDom::remove(Node node) {
  std::map<uint64_t, Impl::FakeNode>::iterator it = impl_->nodes.find(node.id);
  if (it != impl_->nodes.end()) {
    std::map<uint64_t, Impl::FakeNode>::iterator p = impl_->nodes.find(it->second.parent);
    if (p != impl_->nodes.end()) {
      std::vector<uint64_t>& v = p->second.children;
      v.erase(std::remove(v.begin(), v.end(), node.id), v.end());
    }
  }
  calls_.push_back(Call{"remove", node.id, ""});
}

void NullDom::clear(Node parent) {
  std::map<uint64_t, Impl::FakeNode>::iterator it = impl_->nodes.find(parent.id);
  if (it != impl_->nodes.end()) it->second.children.clear();
  calls_.push_back(Call{"clear", parent.id, ""});
}

void NullDom::set_text(Node node, const std::string& text) {
  std::map<uint64_t, Impl::FakeNode>::iterator it = impl_->nodes.find(node.id);
  if (it != impl_->nodes.end()) it->second.text = text;
  calls_.push_back(Call{"set_text", node.id, text});
}

void NullDom::set_attr(Node node, const std::string& key, const std::string& value) {
  std::map<uint64_t, Impl::FakeNode>::iterator it = impl_->nodes.find(node.id);
  if (it != impl_->nodes.end()) Impl::set_attr_f(it->second, key, value);
  calls_.push_back(Call{"set_attr", node.id, key + "=" + value});
}

void NullDom::remove_attr(Node node, const std::string& key) {
  std::map<uint64_t, Impl::FakeNode>::iterator it = impl_->nodes.find(node.id);
  if (it != impl_->nodes.end()) {
    std::vector<std::pair<std::string, std::string> >& v = it->second.attrs;
    for (std::vector<std::pair<std::string, std::string> >::iterator i = v.begin();
         i != v.end();) {
      if (i->first == key) {
        i = v.erase(i);
      } else {
        ++i;
      }
    }
  }
  calls_.push_back(Call{"remove_attr", node.id, key});
}

void NullDom::set_style(Node node, const std::string& key, const std::string& value) {
  std::map<uint64_t, Impl::FakeNode>::iterator it = impl_->nodes.find(node.id);
  if (it != impl_->nodes.end()) Impl::set_attr_f(it->second, "style:" + key, value);
  calls_.push_back(Call{"set_style", node.id, key + "=" + value});
}

void NullDom::set_visible(Node node, bool visible) {
  std::map<uint64_t, Impl::FakeNode>::iterator it = impl_->nodes.find(node.id);
  if (it != impl_->nodes.end()) it->second.visible = visible;
  calls_.push_back(Call{"set_visible", node.id, visible ? "1" : "0"});
}

std::string NullDom::get_value(Node node) {
  std::map<uint64_t, Impl::FakeNode>::iterator it = impl_->nodes.find(node.id);
  if (it != impl_->nodes.end()) {
    for (size_t i = 0; i < it->second.attrs.size(); ++i) {
      if (it->second.attrs[i].first == "value") return it->second.attrs[i].second;
    }
    return it->second.text;
  }
  return "";
}

bool NullDom::is_focused(Node node) { return node.id == focused_for_test_; }

void NullDom::on(Node node, EventKind kind, int handler_id) {
  calls_.push_back(Call{"on", node.id,
                        std::string(event_kind_name(kind)) + "#" +
                            std::to_string(handler_id)});
}

void NullDom::off(Node node, EventKind kind) {
  calls_.push_back(Call{"off", node.id, event_kind_name(kind)});
}

void NullDom::bind_splitter(Node splitter, Node side,
                            std::function<void(int)> on_resize) {
  split_cb_ = on_resize;
  calls_.push_back(Call{"bind_splitter", splitter.id, std::to_string(side.id)});
}

void NullDom::bind_vsplitter(Node splitter, Node panel,
                             std::function<void(int)> on_resize) {
  vsplit_cb_ = on_resize;
  calls_.push_back(Call{"bind_vsplitter", splitter.id, std::to_string(panel.id)});
}

void NullDom::bind_drop_zone(
    Node node, std::function<void(const std::string&, const std::string&)> cb) {
  drop_cb_ = cb;
  calls_.push_back(Call{"bind_drop_zone", node.id, ""});
}

void NullDom::bind_global_key(std::function<void(const std::string&)> cb) {
  key_cb_ = cb;
  calls_.push_back(Call{"bind_global_key", 0, ""});
}

void NullDom::bind_key_input(Node node, std::function<void(const KeyPress&)> cb) {
  key_input_cb_ = cb;
  calls_.push_back(Call{"bind_key_input", node.id, ""});
}

void NullDom::focus(Node node) {
  calls_.push_back(Call{"focus", node.id, ""});
}

void NullDom::bind_cell_size(Node node, std::function<void(int, int)> cb) {
  cell_size_cb_ = cb;
  calls_.push_back(Call{"bind_cell_size", node.id, ""});
}

void NullDom::bind_file_picker(Node node, const std::string& pid,
                               std::function<void(const std::string&)> cb) {
  file_pick_cb_ = cb;
  calls_.push_back(Call{"bind_file_picker", node.id, pid});
}

void NullDom::bind_scroll_pin(Node node, std::function<void(bool)> cb) {
  scroll_pin_cb_ = cb;
  calls_.push_back(Call{"bind_scroll_pin", node.id, ""});
}

void NullDom::scroll_to_bottom(Node node) {
  calls_.push_back(Call{"scroll_to_bottom", node.id, ""});
}

void NullDom::play_animation(Node node, const std::string& cls) {
  if (cls.empty()) return;
  played_anims_.push_back(cls);
  calls_.push_back(Call{"play_animation", node.id, cls});
}

void NullDom::set_appearance(const Appearance& a) {
  appearance_ = a;
  calls_.push_back(Call{"set_appearance", 0,
                        "palette=" + a.palette + " mode=" + a.mode +
                            " font=" + a.font + " mono=" + a.mono +
                            " size=" + std::to_string(a.font_size) +
                            " tabs=" + a.tabs});
}

std::string NullDom::dump() const {
  std::ostringstream os;
  impl_->dump_rec(os, 1, 0);
  return os.str();
}

int NullDom::count_of(const std::string& method) const {
  int n = 0;
  for (size_t i = 0; i < calls_.size(); ++i) {
    if (calls_[i].method == method) ++n;
  }
  return n;
}

}  // namespace ui
}  // namespace fs
