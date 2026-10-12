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

  H strip = comp::tab_strip(tabs, "s1", nullptr, nullptr, nullptr);
  EXPECT_EQ(strip.get()->children.size(), size_t(3));
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
  EXPECT_EQ(blocks[3].get()->tag, std::string("pre"));
  bool has_strong = false;
  for (size_t i = 0; i < blocks[1].get()->children.size(); ++i) {
    if (blocks[1].get()->children[i]->tag == "strong") has_strong = true;
  }
  EXPECT_TRUE(has_strong);
}

// ---------- components: chat message roles ----------

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
  tb.name = "file.read";
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
