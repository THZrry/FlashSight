// test_ui.cpp - UI framework tests: DOM adapter, VNode diff/patch, components.
// All run on NullDom (no browser needed); the wasm build shares this code.
#include "test_framework.hpp"

#include "ui/ui_components.hpp"
#include "ui/ui_dom.hpp"
#include "ui/ui_vdom.hpp"

using namespace fs::ui;

// ---------- DOM adapter ----------

FS_TEST(nulldom_records_and_dumps) {
  NullDom dom;
  Node root = dom.root();
  Node div = dom.create_element("div");
  dom.set_attr(div, "class", "card");
  Node t = dom.create_text("hello");
  dom.append(div, t);
  dom.append(root, div);
  std::string d = dom.dump();
  EXPECT_TRUE(d.find("<div class=\"card\">") != std::string::npos);
  EXPECT_TRUE(d.find("\"hello\"") != std::string::npos);
  EXPECT_EQ(dom.count_of("create_element"), 1);
  EXPECT_EQ(dom.count_of("append"), 2);
}

FS_TEST(nulldom_remove_and_clear) {
  NullDom dom;
  Node root = dom.root();
  Node a = dom.create_element("div");
  Node b = dom.create_element("div");
  dom.append(root, a);
  dom.append(root, b);
  dom.remove(a);
  EXPECT_EQ(dom.count_of("remove"), 1);
  dom.clear(root);
  std::string d = dom.dump();
  EXPECT_TRUE(d.find("<div") == std::string::npos);
}

// ---------- State container ----------

FS_TEST(state_notifies_and_updates) {
  State<int> s(1);
  int got = 0;
  s.subscribe([&got](const int& v) { got = v; });
  s.set(5);
  EXPECT_EQ(got, 5);
  s.update([](int& v) { v += 10; });
  EXPECT_EQ(got, 15);
}

// ---------- VNode builder ----------

FS_TEST(vnode_builder) {
  H n = el("div", "card").id("x").attr("data-k", "v").style("color", "red");
  EXPECT_EQ(n.get()->tag, std::string("div"));
  EXPECT_EQ(n.get()->attrs.size(), size_t(3));
  EXPECT_EQ(n.get()->styles.size(), size_t(1));
  H t = txt("hi");
  EXPECT_TRUE(t.get()->is_text());
}

// ---------- patch: mount ----------

FS_TEST(patch_mount_basic) {
  NullDom dom;
  H tree = el("div", "root-card");
  tree.add(el("span").text("a"));
  tree.add(el("span").text("b"));
  mount(dom, tree, dom.root());
  std::string d = dom.dump();
  EXPECT_TRUE(d.find("<div class=\"root-card\">") != std::string::npos);
  EXPECT_TRUE(d.find("\"a\"") != std::string::npos);
  EXPECT_TRUE(d.find("\"b\"") != std::string::npos);
  EXPECT_EQ(dom.count_of("create_element"), 3);  // div + 2 spans
}

// ---------- patch: text update is minimal ----------

FS_TEST(patch_updates_text_only) {
  NullDom dom;
  H v1 = el("div").add(el("span").text("old"));
  mount(dom, v1, dom.root());
  dom.clear_calls();

  H v2 = el("div").add(el("span").text("new"));
  patch(dom, v1, v2, dom.root());
  EXPECT_EQ(dom.count_of("create_element"), 0);
  EXPECT_EQ(dom.count_of("set_text"), 1);
  std::string d = dom.dump();
  EXPECT_TRUE(d.find("\"new\"") != std::string::npos);
  EXPECT_TRUE(d.find("\"old\"") == std::string::npos);
}

// ---------- patch: attr add/change/remove ----------

FS_TEST(patch_attrs) {
  NullDom dom;
  H v1 = el("div").attr("class", "a").attr("data-x", "1");
  mount(dom, v1, dom.root());
  dom.clear_calls();

  H v2 = el("div").attr("class", "b").attr("data-y", "2");
  patch(dom, v1, v2, dom.root());
  EXPECT_EQ(dom.count_of("set_attr"), 2);
  EXPECT_EQ(dom.count_of("remove_attr"), 1);
  std::string d = dom.dump();
  EXPECT_TRUE(d.find("class=\"b\"") != std::string::npos);
  EXPECT_TRUE(d.find("data-y=\"2\"") != std::string::npos);
  EXPECT_TRUE(d.find("data-x") == std::string::npos);
}

// ---------- patch: a dropped inline style must be cleared ----------
// Regression: the terminal drawer sets an inline height while open. When the
// new tree stopped setting it, the old value stayed on the node, so a collapsed
// drawer kept the open height. Styles are not one-way.
FS_TEST(patch_clears_dropped_style) {
  NullDom dom;
  H v1 = el("div").style("height", "340px");
  mount(dom, v1, dom.root());
  dom.clear_calls();
  H v2 = el("div");
  patch(dom, v1, v2, dom.root());
  // the dropped style is explicitly reset to empty
  bool cleared = false;
  for (size_t i = 0; i < dom.calls().size(); ++i) {
    const NullDom::Call& c = dom.calls()[i];
    if (c.method == "set_style" && c.detail == "height=") cleared = true;
  }
  EXPECT_TRUE(cleared);
}

// ---------- animations are declared by intent, from a closed registry ----------

FS_TEST(anim_registry_is_closed) {
  using namespace fs::ui::anim;
  EXPECT_TRUE(known(kView));
  EXPECT_TRUE(known(kEnter));
  EXPECT_TRUE(known(kPop));
  EXPECT_TRUE(known(kFade));
  EXPECT_TRUE(known(kDrawer));
  EXPECT_TRUE(known(kList));
  EXPECT_FALSE(known("nope"));            // unknown intents are ignored
  EXPECT_TRUE(css_class("nope").empty());
  // every registered intent maps to a real class, and they are all distinct
  const std::vector<std::string>& names = all();
  EXPECT_TRUE(names.size() >= 8);
  for (size_t i = 0; i < names.size(); ++i) {
    EXPECT_FALSE(css_class(names[i]).empty());
    for (size_t j = i + 1; j < names.size(); ++j) {
      EXPECT_TRUE(css_class(names[i]) != css_class(names[j]));
    }
  }
}

FS_TEST(anim_unknown_intent_adds_nothing) {
  NullDom dom;
  // a typo'd intent must not produce a dead class on the element
  H v = el("div", "card").anim("not-a-real-animation");
  mount(dom, v, dom.root());
  std::string d = dom.dump();
  EXPECT_TRUE(d.find("card") != std::string::npos);
  EXPECT_TRUE(d.find("anim-not-a-real-animation") == std::string::npos);
}

FS_TEST(anim_merges_into_class) {
  NullDom dom;
  H v = el("div", "card").anim(fs::ui::anim::kEnter);
  mount(dom, v, dom.root());
  std::string d = dom.dump();
  // one class attribute carrying both (a second class attr would be ignored)
  EXPECT_TRUE(d.find("class=\"card anim-enter\"") != std::string::npos);
}

// ---------- patch: keyed list reorder moves, not recreates ----------

FS_TEST(patch_keyed_reorder) {
  NullDom dom;
  H v1 = el("ul");
  v1.add(el("li").key("a").text("A"));
  v1.add(el("li").key("b").text("B"));
  v1.add(el("li").key("c").text("C"));
  mount(dom, v1, dom.root());
  dom.clear_calls();

  H v2 = el("ul");
  v2.add(el("li").key("c").text("C"));
  v2.add(el("li").key("a").text("A"));
  v2.add(el("li").key("b").text("B"));
  patch(dom, v1, v2, dom.root());
  EXPECT_EQ(dom.count_of("create_element"), 0);
  EXPECT_EQ(dom.count_of("remove"), 0);
  EXPECT_TRUE(dom.count_of("insert_before") >= 1);
  std::string d = dom.dump();
  size_t pc = d.find("\"C\"");
  size_t pa = d.find("\"A\"");
  size_t pb = d.find("\"B\"");
  EXPECT_TRUE(pc < pa && pa < pb);
}

// ---------- patch: keyed insert/remove ----------

FS_TEST(patch_keyed_insert_remove) {
  NullDom dom;
  H v1 = el("ul");
  v1.add(el("li").key("a").text("A"));
  v1.add(el("li").key("c").text("C"));
  mount(dom, v1, dom.root());
  dom.clear_calls();

  H v2 = el("ul");
  v2.add(el("li").key("a").text("A"));
  v2.add(el("li").key("b").text("B"));
  v2.add(el("li").key("c").text("C"));
  patch(dom, v1, v2, dom.root());
  EXPECT_EQ(dom.count_of("create_element"), 1);  // only B created
  EXPECT_EQ(dom.count_of("remove"), 0);

  H v3 = el("ul");
  v3.add(el("li").key("b").text("B"));
  patch(dom, v2, v3, dom.root());
  EXPECT_EQ(dom.count_of("remove"), 2);  // A and C removed
  std::string d = dom.dump();
  EXPECT_TRUE(d.find("\"A\"") == std::string::npos);
  EXPECT_TRUE(d.find("\"B\"") != std::string::npos);
}

// ---------- patch: tag change replaces node ----------

FS_TEST(patch_tag_change_replaces) {
  NullDom dom;
  H v1 = el("div").text("x");
  mount(dom, v1, dom.root());
  dom.clear_calls();

  H v2 = el("span").text("x");
  patch(dom, v1, v2, dom.root());
  EXPECT_EQ(dom.count_of("create_element"), 1);
  EXPECT_EQ(dom.count_of("remove"), 1);
}

// ---------- events: bus dispatch ----------

FS_TEST(event_bus_fire) {
  std::string got;
  H b = comp::button("ok", [&got](const std::string& v) { got = "clicked:" + v; });
  ASSERT_TRUE(b.get()->events.size() == 1);
  int hid = b.get()->events[0].hid;
  bus().fire(hid, "x");
  EXPECT_EQ(got, std::string("clicked:x"));
}

// ---------- components: button variants ----------

FS_TEST(comp_button_variants) {
  H b1 = comp::button("go", nullptr, "primary");
  bool has_primary = false;
  for (size_t i = 0; i < b1.get()->attrs.size(); ++i) {
    if (b1.get()->attrs[i].value.find("primary") != std::string::npos) has_primary = true;
  }
  EXPECT_TRUE(has_primary);

  H tb = comp::title_button("项目", true, nullptr);
  bool active = false;
  for (size_t i = 0; i < tb.get()->attrs.size(); ++i) {
    if (tb.get()->attrs[i].value.find("active") != std::string::npos) active = true;
  }
  EXPECT_TRUE(active);
}

// ---------- components: tab strip ----------

FS_TEST(comp_tab_strip) {
  std::vector<comp::TabSpec> tabs;
  comp::TabSpec t1;
  t1.key = "home";
  t1.title = "主页";
  tabs.push_back(t1);
  comp::TabSpec t2;
  t2.key = "s1";
  t2.title = "会话1";
  t2.closable = true;
  t2.dirty = true;
  tabs.push_back(t2);
  comp::TabSpec t3;
  t3.key = "s2";
  t3.title = "会话2";
  t3.closable = true;
  tabs.push_back(t3);

  H strip = comp::tab_strip(tabs, "s1", nullptr, nullptr, nullptr);
  // Two slots: the pinned one (主页) and the scrolling one (sessions + "+").
  ASSERT_TRUE(strip.get()->children.size() == size_t(2));
  // 主页 stays in the pinned slot, so it cannot be scrolled out of reach.
  EXPECT_EQ(strip.get()->children[0]->children.size(), size_t(1));
  // The session tabs and the "+" are in the scrolling slot.
  EXPECT_EQ(strip.get()->children[1]->children.size(), size_t(3));
}

// ---------- components: markdown ----------

FS_TEST(comp_markdown_blocks) {
  std::string md =
      "# Title\n"
      "some **bold** text\n"
      "\n"
      "- item1\n"
      "- item2\n"
      "\n"
      "```cpp\n"
      "int x = 1;\n"
      "```\n";
  std::vector<H> blocks = comp::markdown_blocks(md);
  EXPECT_EQ(blocks.size(), size_t(4));
  EXPECT_EQ(blocks[0].get()->tag, std::string("h1"));
  EXPECT_EQ(blocks[1].get()->tag, std::string("p"));
  EXPECT_EQ(blocks[2].get()->tag, std::string("ul"));
  // A code block is a wrapper now: language label + gutter + copy button around
  // the <pre>. The <pre> is the second child (after the header).
  ASSERT_TRUE(blocks[3].get()->tag == std::string("div"));
  {
    const VNodePtr& box = blocks[3].get();
    bool has_pre = false;
    bool has_gutter = false;
    bool has_head = false;
    for (size_t i = 0; i < box->children.size(); ++i) {
      const std::string& c = box->children[i]->tag;
      if (c == "div") {
        for (size_t j = 0; j < box->children[i]->attrs.size(); ++j) {
          if (box->children[i]->attrs[j].key == "class") {
            if (box->children[i]->attrs[j].value.find("code-head") != std::string::npos) {
              has_head = true;
            }
            if (box->children[i]->attrs[j].value.find("code-lines") != std::string::npos) {
              has_gutter = true;
            }
          }
        }
      }
    }
    // the <pre> and the line-number gutter live one level deeper, inside
    // .code-body (a flex row: gutter | code)
    for (size_t i = 0; i < box->children.size(); ++i) {
      if (box->children[i]->tag != "div") continue;
      for (size_t k = 0; k < box->children[i]->children.size(); ++k) {
        const VNodePtr& inner = box->children[i]->children[k];
        if (inner->tag == "pre") has_pre = true;
        for (size_t j = 0; j < inner->attrs.size(); ++j) {
          if (inner->attrs[j].key == "class" &&
              inner->attrs[j].value.find("code-lines") != std::string::npos) {
            has_gutter = true;
          }
        }
      }
    }
    EXPECT_TRUE(has_head);
    EXPECT_TRUE(has_gutter);
    EXPECT_TRUE(has_pre);
  }
  bool has_strong = false;
  for (size_t i = 0; i < blocks[1].get()->children.size(); ++i) {
    if (blocks[1].get()->children[i]->tag == "strong") has_strong = true;
  }
  EXPECT_TRUE(has_strong);
}

// ---------- components: chat message roles ----------

// A streamed answer arrives one character at a time. `#` / `##` before the
// space used to fall through the heading branch, and the paragraph gather
// broke on a leading '#' WITHOUT advancing - markdown_blocks() spun forever
// and the UI froze on the first character of a `## ...` answer.
FS_TEST(comp_markdown_incomplete_heading_terminates) {
  const char* samples[] = {"#", "##", "###", "#foo", "## ", "## Title",
                           "# Title\n#", "# Title\n#foo\n# "};
  for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i) {
    std::vector<H> blocks = comp::markdown_blocks(samples[i], nullptr, nullptr);
    EXPECT_TRUE(!blocks.empty());
  }
  // Streaming prefixes of a real answer shape must all terminate too.
  const std::string stream = "## 先回答排序：我同意你的判断\n\n- L3\n- L2\n";
  for (size_t n = 1; n <= stream.size(); ++n) {
    std::vector<H> blocks = comp::markdown_blocks(stream.substr(0, n), nullptr,
                                                  nullptr);
    EXPECT_TRUE(!blocks.empty());
    if (blocks.empty()) break;
  }
}

FS_TEST(comp_chat_roles) {
  comp::ChatMsg user;
  user.role = "user";
  user.text = "hi";
  H u = comp::chat_message(user, nullptr, nullptr);
  bool has_bubble = false;
  for (size_t i = 0; i < u.get()->children.size(); ++i) {
    if (u.get()->children[i]->tag == "div") {
      for (size_t k = 0; k < u.get()->children[i]->attrs.size(); ++k) {
        if (u.get()->children[i]->attrs[k].value.find("bubble") != std::string::npos) {
          has_bubble = true;
        }
      }
    }
  }
  EXPECT_TRUE(has_bubble);

  comp::ChatMsg sys;
  sys.role = "system";
  sys.text = "compressed";
  H s = comp::chat_message(sys, nullptr, nullptr);
  bool has_sys = false;
  for (size_t i = 0; i < s.get()->children.size(); ++i) {
    if (s.get()->children[i]->tag == "div") has_sys = true;
  }
  EXPECT_TRUE(has_sys);
}

// ---------- components: tool block + summary ----------

FS_TEST(comp_tool_block_and_summary) {
  comp::ToolBlock tb;
  tb.name = "read_file";
  tb.summary = "120 lines";
  tb.detail = "code";
  tb.expanded = false;
  H closed = comp::tool_block(tb, nullptr);
  EXPECT_EQ(closed.get()->children.size(), size_t(1));

  tb.expanded = true;
  H open = comp::tool_block(tb, nullptr);
  EXPECT_EQ(open.get()->children.size(), size_t(2));

  H sum = comp::turn_summary("key points here");
  EXPECT_EQ(sum.get()->tag, std::string("div"));
  EXPECT_EQ(sum.get()->children.size(), size_t(2));
}

// ---------- components: forms ----------

FS_TEST(comp_forms) {
  H sel = comp::select(std::vector<std::string>{"a", "b", "c"}, 1, nullptr);
  EXPECT_EQ(sel.get()->tag, std::string("select"));
  EXPECT_EQ(sel.get()->children.size(), size_t(3));

  H cb = comp::checkbox("on", true, nullptr);
  EXPECT_EQ(cb.get()->tag, std::string("label"));

  H tg = comp::toggle(true, "auto", nullptr);
  EXPECT_EQ(tg.get()->children.size(), size_t(2));

  H f = comp::field("Name", comp::text_input("v", "ph", nullptr), "hint");
  EXPECT_EQ(f.get()->children.size(), size_t(3));
}

// ---------- components: modal ----------

FS_TEST(comp_modal) {
  std::vector<H> body;
  body.push_back(el("div").text("content"));
  H m = comp::modal("Title", body, "OK", nullptr, nullptr);
  EXPECT_EQ(m.get()->tag, std::string("div"));
  ASSERT_TRUE(m.get()->children.size() == 1);
  VNodePtr box = m.get()->children[0];
  EXPECT_EQ(box->children.size(), size_t(3));
}

// ---------- patch: listener identity is stable across renders ----------

FS_TEST(vdom_surviving_node_keeps_its_listener_id) {
  // The bridged desktop adapters run DOM scripts asynchronously, so the browser
  // can still hold the previous render's listener while C++ has already built a
  // new tree. Rebinding every render made that listener's id dead (retain had
  // dropped it) - a click/keystroke during startup then did nothing. A node that
  // survives a patch must keep its id, and the id must dispatch the NEW closure.
  NullDom dom;
  Node root = dom.root();
  int calls = 0;
  VNodePtr first = el("button", "icon-btn")
                       .key("btn")
                       .on(EventKind::kClick, [&calls](const std::string&) { ++calls; })
                       .get();
  patch(dom, VNodePtr(), first, root);
  const int old_hid = first->events[0].hid;

  VNodePtr second = el("button", "icon-btn")
                        .key("btn")
                        .on(EventKind::kClick, [&calls](const std::string&) { calls += 10; })
                        .get();
  patch(dom, first, second, root);
  EXPECT_EQ(second->events[0].hid, old_hid);
  // The listener was not torn down and re-added...
  int rebinds = 0;
  for (size_t i = 0; i < dom.calls().size(); ++i) {
    const NullDom::Call& c = dom.calls()[i];
    if (c.method == "on" || c.method == "off") ++rebinds;
  }
  EXPECT_EQ(rebinds, 1);  // only the initial bind
  // ...and the id the DOM still holds resolves to the fresh closure.
  bus().fire(old_hid, "");
  EXPECT_EQ(calls, 10);
  // The bus is process-wide; do not leave a closure that captures a stack local.
  bus().retain(std::vector<int>());
}

FS_TEST(vdom_removed_kind_is_unbound) {
  NullDom dom;
  Node root = dom.root();
  VNodePtr first = el("div").key("n").on(EventKind::kClick, [](const std::string&) {}).get();
  patch(dom, VNodePtr(), first, root);
  dom.clear_calls();
  VNodePtr second = el("div").key("n").get();
  patch(dom, first, second, root);
  int offs = 0;
  for (size_t i = 0; i < dom.calls().size(); ++i) {
    if (dom.calls()[i].method == "off") ++offs;
  }
  EXPECT_EQ(offs, 1);
}

// Parsed markdown is cached: a finished answer is not re-parsed when its page is
// rebuilt. Each use is a CLONE (fresh DOM handles), while the cached handlers
// stay registered so the copy button still works.
FS_TEST(markdown_parse_cache_clones_and_keeps_handlers) {
  std::string md = "# Title\n\n```cpp\nint x;\n```\n\n[a](https://example.com)";
  EventFn cp = [](const std::string&) {};
  EventFn lk = [](const std::string&) {};
  std::vector<H> a = comp::markdown_blocks(md, cp, lk);
  std::vector<H> b = comp::markdown_blocks(md, cp, lk);
  ASSERT_TRUE(a.size() == b.size());
  ASSERT_TRUE(!a.empty());
  // A clone, not the same VNode: reusing one pointer would share its DOM handle.
  EXPECT_TRUE(a[0].get() != b[0].get());
  std::vector<int> ids;
  comp::markdown_cache_handler_ids(&ids);
  EXPECT_TRUE(!ids.empty());
  comp::markdown_cache_clear();
  bus().retain(std::vector<int>());
}
