// test_ui2.cpp - Widget 库单测（NullDom 断言渲染与交互）
#include <string>
#include <vector>

#include "test_framework.hpp"

#include "ui/ui_backend.hpp"
#include "ui/ui_dom.hpp"
#include "ui/ui_reactive.hpp"
#include "ui/ui_widgets.hpp"

using namespace fs;
using namespace fs::ui;

// helper：在 calls 里找 method+detail
static bool has_call(const NullDom& dom, const std::string& method,
                     const std::string& detail_part = "") {
  for (auto& c : dom.calls()) {
    if (c.method == method && (detail_part.empty() || c.detail.find(detail_part) != std::string::npos))
      return true;
  }
  return false;
}

// ---------- Button：渲染 + 点击回调 ----------
FS_TEST(widget_button) {
  NullDom dom;
  bool clicked = false;
  Button b(dom, "发送", [&clicked](const std::string&) { clicked = true; }, true);
  b.mount(dom.root());
  EXPECT_TRUE(has_call(dom, "create", "button"));
  EXPECT_TRUE(has_call(dom, "set_text", "发送"));
  EXPECT_TRUE(has_call(dom, "on", ""));
  EXPECT_FALSE(clicked);  // NullDom 不触发，只记录
  b.set_disabled(true);
  EXPECT_TRUE(has_call(dom, "set_attr", "disabled=1"));
}

// ---------- Select：选项渲染 + change 回调写回信号 ----------
FS_TEST(widget_select) {
  NullDom dom;
  auto sel = Signal<int>::make(0);
  std::vector<std::string> opts = {"a", "b", "c"};
  Select s(dom, opts, sel);
  s.mount(dom.root());
  // 3 个 option
  int n_opt = 0;
  for (auto& c : dom.calls()) {
    if (c.method == "create" && c.detail.find("option") != std::string::npos) ++n_opt;
  }
  EXPECT_EQ(n_opt, 3);
  // 信号变 -> 同步 value 属性
  sel->set(2);
  EXPECT_TRUE(has_call(dom, "set_attr", "value=2"));
}

// ---------- Checkbox / Toggle ----------
FS_TEST(widget_checkbox_toggle) {
  NullDom dom;
  auto c = Signal<bool>::make(false);
  Checkbox cb(dom, "记住我", c);
  cb.mount(dom.root());
  EXPECT_TRUE(has_call(dom, "create", "input"));
  c->set(true);
  EXPECT_TRUE(has_call(dom, "set_attr", "checked=1"));

  auto t = Signal<bool>::make(false);
  Toggle tg(dom, t);
  tg.mount(dom.root());
  EXPECT_TRUE(has_call(dom, "set_text", "关"));
  t->set(true);
  EXPECT_TRUE(has_call(dom, "set_text", "开"));
}

// ---------- TextInput 双向绑定 ----------
FS_TEST(widget_textinput) {
  NullDom dom;
  auto v = Signal<std::string>::make("init");
  TextInput ti(dom, v, "占位");
  ti.mount(dom.root());
  EXPECT_TRUE(has_call(dom, "set_attr", "placeholder=占位"));
  EXPECT_TRUE(has_call(dom, "set_attr", "value=init"));
  EXPECT_TRUE(has_call(dom, "on", ""));
}

// ---------- Tabs：切页重建 ----------
FS_TEST(widget_tabs) {
  NullDom dom;
  auto cur = Signal<int>::make(0);
  int built = -1;
  Tabs::PageFn p0 = [&built](IDom&, Node) { built = 0; };
  Tabs::PageFn p1 = [&built](IDom&, Node) { built = 1; };
  Tabs tabs(dom, {"一", "二"}, cur, {p0, p1});
  tabs.mount(dom.root());
  EXPECT_EQ(built, 0);
  cur->set(1);
  EXPECT_EQ(built, 1);
}

// ---------- Accordion：展开/折叠 ----------
FS_TEST(widget_accordion) {
  NullDom dom;
  auto open = Signal<bool>::make(false);
  bool body_built = false;
  Accordion acc(dom, "标题", open, [&body_built](IDom&, Node) { body_built = true; });
  acc.mount(dom.root());
  EXPECT_FALSE(body_built);  // 默认折叠不建内容
  open->set(true);
  EXPECT_TRUE(body_built);
}

// ---------- ListView：按 count 渲染行 ----------
FS_TEST(widget_listview) {
  NullDom dom;
  auto n = Signal<int>::make(2);
  int rows = 0;
  ListView lv(dom, n, 10, [&rows](IDom& d, Node p, int) {
    ++rows;
    Node r = d.create("div", "row", "");
    d.append(p, r);
  });
  lv.mount(dom.root());
  EXPECT_EQ(rows, 2);
  n->set(5);
  EXPECT_EQ(rows, 7);  // refresh 累加（NullDom 不删旧节点，只验证调用）
}

// ---------- Modal：显隐 ----------
FS_TEST(widget_modal) {
  NullDom dom;
  auto v = Signal<bool>::make(false);
  Modal m(dom, v, "确认", [](IDom&, Node) {}, [](const std::string&) {});
  m.mount(dom.root());
  EXPECT_TRUE(has_call(dom, "set_visible", "0"));
  v->set(true);
  EXPECT_TRUE(has_call(dom, "set_visible", "1"));
}

// ---------- Progress / Spinner / Badge / CodeBlock ----------
FS_TEST(widget_display) {
  NullDom dom;
  auto pct = Signal<int>::make(30);
  Progress pr(dom, pct);
  pr.mount(dom.root());
  EXPECT_TRUE(has_call(dom, "set_attr", "width:30%"));
  pct->set(150);  // 钳制 100
  EXPECT_TRUE(has_call(dom, "set_attr", "width:100%"));

  auto vis = Signal<bool>::make(true);
  Spinner sp(dom, vis);
  sp.mount(dom.root());
  vis->set(false);
  EXPECT_TRUE(has_call(dom, "set_visible", "0"));

  Badge b(dom, "3");
  b.mount(dom.root());
  b.set_text("5");
  EXPECT_TRUE(has_call(dom, "set_text", "5"));

  CodeBlock cb(dom, "int x = 1;", "cpp");
  cb.mount(dom.root());
  cb.set_code("int y = 2;");
  EXPECT_TRUE(has_call(dom, "set_text", "int y = 2;"));
}

// ---------- NullBackend：调用与事件 ----------
FS_TEST(nullbackend_call_fire) {
  NullBackend be;
  Json args = Json::object();
  args["text"] = "hi";
  FsResult r = be.call("session.send", args);
  EXPECT_TRUE(r.ok());
  bool got = false;
  auto unsub = be.subscribe([&got](const std::string& e, const Json&) {
    if (e == "stream.delta") got = true;
  });
  be.fire("stream.delta", Json::object());
  EXPECT_TRUE(got);
  unsub();  // 退订不断言（v1 置空实现）
}
