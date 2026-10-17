// ui_vdom.cpp - Virtual DOM diff/patch implementation.
#include "ui_vdom.hpp"

#include <algorithm>
#include <string>

namespace fs {
namespace ui {

EventBus& bus() {
  static EventBus b;
  return b;
}

// ---------------- Animation registry ----------------
// One place that decides what each appearance looks like. Durations live in the
// CSS (keyframes + the .anim-* rules); this table only fixes the vocabulary, so
// the app cannot invent an animation the CSS does not implement.
namespace anim {

const char* const kView = "view";
const char* const kEnter = "enter";
const char* const kList = "list";
const char* const kPop = "pop";
const char* const kFade = "fade";
const char* const kSlideLeft = "slide-left";
const char* const kSlideRight = "slide-right";
const char* const kDrawer = "drawer";

namespace {
struct Entry {
  const char* name;
  const char* cls;
};
const Entry kTable[] = {
    {kView, "anim-view"},
    {kEnter, "anim-enter"},
    {kList, "anim-list"},
    {kPop, "anim-pop"},
    {kFade, "anim-fade"},
    {kSlideLeft, "anim-slide-left"},
    {kSlideRight, "anim-slide-right"},
    {kDrawer, "anim-drawer"},
};
const int kCount = static_cast<int>(sizeof(kTable) / sizeof(kTable[0]));
}  // namespace

bool known(const std::string& name) { return !css_class(name).empty(); }

std::string css_class(const std::string& name) {
  for (int i = 0; i < kCount; ++i) {
    if (name == kTable[i].name) return kTable[i].cls;
  }
  return std::string();
}

const std::vector<std::string>& all() {
  static std::vector<std::string> names = [] {
    std::vector<std::string> v;
    for (int i = 0; i < kCount; ++i) v.push_back(kTable[i].name);
    return v;
  }();
  return names;
}

}  // namespace anim

void EventBus::retain(const std::vector<int>& keep) {
  std::vector<int> sorted = keep;
  std::sort(sorted.begin(), sorted.end());
  for (std::map<int, EventFn>::iterator it = handlers_.begin(); it != handlers_.end();) {
    if (!std::binary_search(sorted.begin(), sorted.end(), it->first)) {
      it = handlers_.erase(it);
    } else {
      ++it;
    }
  }
}

void collect_handler_ids(const VNodePtr& v, std::vector<int>* out) {
  if (!v) return;
  for (size_t i = 0; i < v->events.size(); ++i) out->push_back(v->events[i].hid);
  for (size_t i = 0; i < v->children.size(); ++i) collect_handler_ids(v->children[i], out);
}

VNodePtr find_by_key(const VNodePtr& v, const std::string& key) {
  if (!v) return VNodePtr();
  if (v->key == key) return v;
  for (size_t i = 0; i < v->children.size(); ++i) {
    VNodePtr hit = find_by_key(v->children[i], key);
    if (hit) return hit;
  }
  return VNodePtr();
}

VNodePtr make_vnode(const std::string& tag) {
  VNodePtr n(new VNode());
  n->tag = tag;
  return n;
}

VNodePtr make_text(const std::string& text) {
  VNodePtr n(new VNode());
  n->text = text;
  return n;
}

namespace {

void patch_node(IDom& dom, const VNodePtr& oldv, const VNodePtr& neu);

const std::string& attr_get(const VNode& n, const std::string& key, bool* found) {
  static const std::string empty;
  for (size_t i = 0; i < n.attrs.size(); ++i) {
    if (n.attrs[i].key == key) {
      *found = true;
      return n.attrs[i].value;
    }
  }
  *found = false;
  return empty;
}

bool same_kind(const VNode& a, const VNode& b) {
  if (a.is_text() != b.is_text()) return false;
  if (a.is_text()) return true;
  return a.tag == b.tag && a.key == b.key;
}

// Create the DOM node for a VNode (children handled by the caller loop).
Node create_node(IDom& dom, const VNodePtr& v) {
  if (v->is_text()) {
    v->dom = dom.create_text(v->text);
    return v->dom;
  }
  v->dom = dom.create_element(v->tag);
  for (size_t i = 0; i < v->attrs.size(); ++i) {
    dom.set_attr(v->dom, v->attrs[i].key, v->attrs[i].value);
  }
  for (size_t i = 0; i < v->styles.size(); ++i) {
    dom.set_style(v->dom, v->styles[i].key, v->styles[i].value);
  }
  for (size_t i = 0; i < v->events.size(); ++i) {
    dom.on(v->dom, v->events[i].kind, v->events[i].hid);
  }
  return v->dom;
}

void patch_attrs(IDom& dom, const VNodePtr& oldv, const VNodePtr& neu) {
  // A focused control's value is the user's, not ours. The composer deliberately
  // does not re-render per keystroke (that would fight the caret), so a
  // background re-render would otherwise write the stale value back and reset
  // the caret mid-typing. Skip `value` while the node has focus.
  const bool focused = dom.is_focused(neu->dom);
  // set new / changed
  for (size_t i = 0; i < neu->attrs.size(); ++i) {
    if (focused && neu->attrs[i].key == "value") continue;
    bool found = false;
    const std::string& old_val = attr_get(*oldv, neu->attrs[i].key, &found);
    if (!found || old_val != neu->attrs[i].value) {
      dom.set_attr(neu->dom, neu->attrs[i].key, neu->attrs[i].value);
    }
  }
  // remove dropped
  for (size_t i = 0; i < oldv->attrs.size(); ++i) {
    if (focused && oldv->attrs[i].key == "value") continue;
    bool found = false;
    attr_get(*neu, oldv->attrs[i].key, &found);
    if (!found) dom.remove_attr(neu->dom, oldv->attrs[i].key);
  }
  // styles: re-apply changed, and CLEAR the ones the new tree no longer sets.
  // Without the clear, an inline style survives forever: the terminal drawer
  // sets height while open and, once collapsed, the stale inline height kept it
  // full-size (a node that stops setting a style must stop having it).
  for (size_t i = 0; i < neu->styles.size(); ++i) {
    dom.set_style(neu->dom, neu->styles[i].key, neu->styles[i].value);
  }
  for (size_t i = 0; i < oldv->styles.size(); ++i) {
    bool still = false;
    for (size_t j = 0; j < neu->styles.size(); ++j) {
      if (neu->styles[j].key == oldv->styles[i].key) {
        still = true;
        break;
      }
    }
    if (!still) dom.set_style(neu->dom, oldv->styles[i].key, "");
  }
}

void patch_events(IDom& dom, const VNodePtr& oldv, const VNodePtr& neu) {
  // A fresh render mints a new handler id for every event, so any node whose
  // handler changed must drop the old listener first (otherwise the old and new
  // handlers both fire on the next click).
  for (size_t i = 0; i < neu->events.size(); ++i) {
    const VEvent& e = neu->events[i];
    bool changed = true;
    for (size_t j = 0; j < oldv->events.size(); ++j) {
      if (oldv->events[j].kind == e.kind && oldv->events[j].hid == e.hid) {
        changed = false;
        break;
      }
    }
    if (changed) {
      dom.off(neu->dom, e.kind);
      dom.on(neu->dom, e.kind, e.hid);
    }
  }
  // unbind kinds the new node no longer listens for
  for (size_t j = 0; j < oldv->events.size(); ++j) {
    bool still = false;
    for (size_t i = 0; i < neu->events.size(); ++i) {
      if (neu->events[i].kind == oldv->events[j].kind) {
        still = true;
        break;
      }
    }
    if (!still) dom.off(neu->dom, oldv->events[j].kind);
  }
}

// Keyed child reconciliation.
void patch_children(IDom& dom, const VNodePtr& oldv, const VNodePtr& neu) {
  // index old children by key (or by positional order when unkeyed)
  std::map<std::string, VNodePtr> old_by_key;
  std::vector<VNodePtr> old_unkeyed;
  for (size_t i = 0; i < oldv->children.size(); ++i) {
    const VNodePtr& c = oldv->children[i];
    if (!c->key.empty()) {
      old_by_key[c->key] = c;
    } else {
      old_unkeyed.push_back(c);
    }
  }

  // 1) match / create every new child (fills each child's dom handle)
  std::vector<VNodePtr> result;
  size_t unkeyed_i = 0;
  for (size_t i = 0; i < neu->children.size(); ++i) {
    const VNodePtr& c = neu->children[i];
    VNodePtr match;
    if (!c->key.empty()) {
      std::map<std::string, VNodePtr>::iterator it = old_by_key.find(c->key);
      if (it != old_by_key.end()) {
        match = it->second;
        old_by_key.erase(it);
      }
    } else {
      while (unkeyed_i < old_unkeyed.size()) {
        VNodePtr cand = old_unkeyed[unkeyed_i++];
        if (same_kind(*cand, *c)) {
          match = cand;
          break;
        }
      }
    }
    if (match) {
      patch_node(dom, match, c);
    } else if (c->is_text()) {
      c->dom = dom.create_text(c->text);
    } else {
      c->dom = create_node(dom, c);
      for (size_t k = 0; k < c->children.size(); ++k) {
        patch(dom, VNodePtr(), c->children[k], c->dom);
      }
    }
    result.push_back(c);
  }

  std::vector<Node> desired;
  for (size_t i = 0; i < result.size(); ++i) desired.push_back(result[i]->dom);

  // 2) current DOM order = old children that still have live nodes
  std::vector<Node> cur;
  for (size_t i = 0; i < oldv->children.size(); ++i) {
    if (oldv->children[i]->dom.valid()) cur.push_back(oldv->children[i]->dom);
  }

  // 3) drop nodes no longer wanted
  std::vector<Node> live;
  for (size_t i = 0; i < cur.size(); ++i) {
    bool keep = false;
    for (size_t j = 0; j < desired.size(); ++j) {
      if (cur[i] == desired[j]) {
        keep = true;
        break;
      }
    }
    if (keep) {
      live.push_back(cur[i]);
    } else {
      dom.remove(cur[i]);
    }
  }

  // 4) if the remaining order already matches, nothing more to do
  bool same = live.size() == desired.size();
  if (same) {
    for (size_t i = 0; i < desired.size(); ++i) {
      if (!(live[i] == desired[i])) {
        same = false;
        break;
      }
    }
  }
  if (same) {
    neu->dom = oldv->dom;
    return;
  }

  // 5) reorder: walk back-to-front so each node's successor anchor is already
  //    in place (this is what makes insertBefore valid rather than a no-op).
  //
  // NOTE: neu->dom must be set FIRST. The reorder loop uses neu->dom as the
  // parent to insert into; assigning it after the loop (which is what this did)
  // meant every reorder inserted into Node{0} - a no-op - so a subtree whose
  // child list was fully replaced (home page section swap, tab page swap) was
  // created but never attached. The keyed fast path above hid it: it returns
  // early whenever the order already matches.
  neu->dom = oldv->dom;
  for (size_t i = desired.size(); i-- > 0;) {
    Node anchor = (i + 1 < desired.size()) ? desired[i + 1] : Node{0};
    dom.insert_before(neu->dom, desired[i], anchor);
  }
}

void patch_node(IDom& dom, const VNodePtr& oldv, const VNodePtr& neu) {
  neu->dom = oldv->dom;
  if (neu->is_text()) {
    if (oldv->text != neu->text) dom.set_text(neu->dom, neu->text);
    return;
  }
  patch_attrs(dom, oldv, neu);
  patch_events(dom, oldv, neu);
  patch_children(dom, oldv, neu);
  // Replay the declared animation when the token changed. A mount animation
  // fires on creation, so a node that survives the patch would never animate
  // again - which is how an animation got "swallowed" (it played while the
  // panel was still empty and was over by the time the content arrived). A part
  // bumps the token when its content is actually there.
  if (neu->anim_key != oldv->anim_key && !neu->anim_key.empty()) {
    std::string cls = anim::css_class(neu->anim);
    if (!cls.empty()) dom.play_animation(neu->dom, cls);
  }
}

}  // namespace

void patch(IDom& dom, const VNodePtr& oldv, const VNodePtr& neu, Node parent) {
  if (!neu) {
    if (oldv && oldv->dom.valid()) dom.remove(oldv->dom);
    return;
  }
  if (!oldv || !oldv->dom.valid() || !same_kind(*oldv, *neu)) {
    if (oldv && oldv->dom.valid()) dom.remove(oldv->dom);
    if (neu->is_text()) {
      neu->dom = dom.create_text(neu->text);
    } else {
      neu->dom = create_node(dom, neu);
      for (size_t i = 0; i < neu->children.size(); ++i) {
        patch(dom, VNodePtr(), neu->children[i], neu->dom);
      }
    }
    dom.append(parent, neu->dom);
    return;
  }
  patch_node(dom, oldv, neu);
}

void mount(IDom& dom, const VNodePtr& v, Node parent) {
  patch(dom, VNodePtr(), v, parent);
}

}  // namespace ui
}  // namespace fs
