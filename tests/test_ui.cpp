// test_ui.cpp - UI 兼容层 + 响应式单测（NullDom 记录调用序列）
#include <string>

#include "test_framework.hpp"

#include "ui/ui_app.hpp"
#include "ui/ui_dom.hpp"
#include "ui/ui_reactive.hpp"

using namespace fs::ui;

// ---------- NullDom：记录调用序列 ----------
FS_TEST(nulldom_records_calls) {
  NullDom dom;
  Node root = dom.root();
  EXPECT_TRUE(root.valid());
  Node a = dom.create("div", "main", "hi");
  EXPECT_TRUE(a.valid());
  dom.append(root, a);
  dom.set_text(a, "hello");
  dom.set_visible(a, false);
  bool fired = false;
  dom.on(a, EventKind::kClick, [&fired](const std::string&) { fired = true; });
  const auto& calls = dom.calls();
  EXPECT_TRUE(calls.size() >= 5);
  EXPECT_EQ(calls[0].method, std::string("create"));
  // on 只记录不触发
  EXPECT_FALSE(fired);
}

// ---------- Signal：set 通知订阅者 ----------
FS_TEST(signal_notifies) {
  auto s = Signal<int>::make(1);
  int seen = 0;
  int count = 0;
  auto sub = s->subscribe([&seen, &count](const int& v) {
    seen = v;
    ++count;
  });
  EXPECT_EQ(s->get(), 1);
  s->set(2);
  EXPECT_EQ(seen, 2);
  EXPECT_EQ(count, 1);
  s->set(3);
  EXPECT_EQ(count, 2);
  // 退订后不再通知
  sub.reset();
  s->set(4);
  EXPECT_EQ(count, 2);
  EXPECT_EQ(seen, 3);
}

// ---------- Effect.watch：信号变则重跑 ----------
FS_TEST(effect_watch_reruns) {
  auto a = Signal<int>::make(1);
  auto b = Signal<std::string>::make("x");
  int runs = 0;
  int last = 0;
  Effect e;
  e.watch([&]() {
    ++runs;
    last = a->get();
  }, a, b);
  EXPECT_EQ(runs, 1);
  EXPECT_EQ(last, 1);
  b->set("y");  // 任一被 watch 的信号变都重跑
  EXPECT_EQ(runs, 2);
  a->set(42);
  EXPECT_EQ(runs, 3);
  EXPECT_EQ(last, 42);
  e.dispose();
  a->set(100);
  EXPECT_EQ(runs, 3);  // dispose 后不再跑
}

// ---------- bind_text / bind_visible ----------
FS_TEST(bind_text_visible) {
  NullDom dom;
  Node n = dom.create("span", "", "");
  auto t = Signal<std::string>::make("a");
  auto st = bind_text(dom, n, t);
  t->set("b");
  bool found_set_b = false;
  for (auto& c : dom.calls()) {
    if (c.method == "set_text" && c.detail == "b") found_set_b = true;
  }
  EXPECT_TRUE(found_set_b);

  Node m = dom.create("div", "", "");
  auto v = Signal<bool>::make(true);
  auto sv = bind_visible(dom, m, v);
  v->set(false);
  bool found_hide = false;
  for (auto& c : dom.calls()) {
    if (c.method == "set_visible" && c.node == m.id && c.detail == "0") found_hide = true;
  }
  EXPECT_TRUE(found_hide);
}

// ---------- UiApp：mount 建出四页骨架，切页重建 ----------
FS_TEST(uiapp_mount_and_pages) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st);
  app.mount();
  // mount 应有 create 调用（含 titlebar/tabstrip）
  EXPECT_TRUE(dom.calls().size() > 5);
  size_t before = dom.calls().size();
  app.show(Page::kSession);
  // 切页触发 clear + 重建
  bool has_clear = false;
  for (size_t i = before; i < dom.calls().size(); ++i) {
    if (dom.calls()[i].method == "clear") has_clear = true;
  }
  EXPECT_TRUE(has_clear);
  app.show(Page::kSettings);
  app.show(Page::kHome);
  // 主页子 tab 切换（用量统计现在是主页子页）
  st.home_tab->set(HomeTab::kUsage);
  st.home_tab->set(HomeTab::kProject);
}
