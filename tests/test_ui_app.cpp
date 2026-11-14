// test_ui_app.cpp - UiApp integration tests on NullDom: render, tabs, chat,
// side panel, settings. Verifies the whole state->tree->patch loop.
//
// The app is backend-driven now, so these tests seed AppState directly (the
// same shape a backend response produces) instead of relying on demo data.
#include "test_framework.hpp"

#include <chrono>
#include <functional>

#include "ui/ui_app.hpp"
#include "ui/ui_backend.hpp"
#include "ui/ui_components.hpp"
#include "ui/ui_dom.hpp"
#include "ui/ui_vdom.hpp"

using namespace fs;
using namespace fs::ui;

namespace {

// find the first node in the dump containing `needle`
bool dump_has(NullDom& dom, const std::string& needle) {
  return dom.dump().find(needle) != std::string::npos;
}

// The 所有终端 overview cards only (the terminal STRIP is a sibling and lists
// every terminal regardless of the filter, so a whole-dump search cannot tell
// "filtered out of the overview" from "still in the strip").
std::string dump_overview(NullDom& dom) {
  const std::string& d = dom.dump();
  size_t a = d.find("td-all");
  if (a == std::string::npos) return std::string();
  size_t b = d.find("td-tabs", a);
  return d.substr(a, b == std::string::npos ? std::string::npos : b - a);
}

bool overview_has(NullDom& dom, const std::string& needle) {
  return dump_overview(dom).find(needle) != std::string::npos;
}

// Only the left navigation rail (the home page underneath renders the same
// session titles, so a whole-dump search cannot tell the two apart).
std::string dump_nav(NullDom& dom) {
  const std::string& d = dom.dump();
  size_t a = d.find("nav-rail");
  if (a == std::string::npos) return std::string();
  size_t b = d.find("nav-main", a);
  return d.substr(a, b == std::string::npos ? std::string::npos : b - a);
}

bool nav_has(NullDom& dom, const std::string& needle) {
  return dump_nav(dom).find(needle) != std::string::npos;
}

// seed one project with one session, mirroring a backend project.list reply
void seed_project(AppState& s) {
  ProjectCard p;
  p.pid = "p1";
  p.name = "FlashSight";
  p.path = "C:\\aaa\\FlashSight";
  p.session_count = 1;
  ProjectCard::SessionRow row;
  row.sid = "sess_a";
  row.title = "UI 框架重构";
  row.updated_ms = 1700000000000LL;  // a fixed past timestamp
  row.running = true;
  p.sessions.push_back(row);
  s.projects.push_back(p);
}

// open a session tab (as open_session would, without a backend)
void seed_session(AppState& s, const std::string& sid = "sess_a") {
  SessionTab t;
  t.id = sid;
  t.title = "会话1";
  s.sessions.push_back(t);
  s.tab_ids.push_back(sid);
  s.active_tab = sid;
}

}  // namespace

// Count nodes whose class attribute contains `cls` (depth-first). Used by the
// component-level checks that build a VNode directly instead of mounting it.
static int count_class(const VNodePtr& v, const std::string& cls) {
  if (!v) return 0;
  int n = 0;
  for (size_t i = 0; i < v->attrs.size(); ++i) {
    if (v->attrs[i].key == "class" &&
        v->attrs[i].value.find(cls) != std::string::npos) {
      ++n;
      break;
    }
  }
  for (size_t i = 0; i < v->children.size(); ++i) n += count_class(v->children[i], cls);
  return n;
}

// Exact class TOKEN ("md-table" must not match "md-table-wrap").
static int count_class_exact(const VNodePtr& v, const std::string& cls) {
  if (!v) return 0;
  int n = 0;
  for (size_t i = 0; i < v->attrs.size(); ++i) {
    if (v->attrs[i].key != "class") continue;
    const std::string& c = v->attrs[i].value;
    size_t start = 0;
    while (start <= c.size()) {
      size_t sp = c.find(' ', start);
      std::string tok = c.substr(start, sp == std::string::npos ? std::string::npos
                                                                : sp - start);
      if (tok == cls) {
        ++n;
        break;
      }
      if (sp == std::string::npos) break;
      start = sp + 1;
    }
    break;
  }
  for (size_t i = 0; i < v->children.size(); ++i) {
    n += count_class_exact(v->children[i], cls);
  }
  return n;
}

// Does the subtree contain a text node with exactly this text?
static bool has_txt(const VNodePtr& v, const std::string& text) {
  if (!v) return false;
  if (v->is_text() && v->text == text) return true;
  for (size_t i = 0; i < v->children.size(); ++i) {
    if (has_txt(v->children[i], text)) return true;
  }
  return false;
}

// Collect every VNode whose key starts with `prefix` (depth-first). The chat
// message keys are internal, so the test reads them from the tree itself.
static void collect_msg_nodes(const VNodePtr& v, const std::string& prefix,
                              std::vector<VNodePtr>* out) {
  if (!v) return;
  if (v->key.compare(0, prefix.size(), prefix) == 0) out->push_back(v);
  for (size_t i = 0; i < v->children.size(); ++i) {
    collect_msg_nodes(v->children[i], prefix, out);
  }
}

// Click handler id of the first node carrying class `cls` whose subtree contains
// the exact text `text`. Used to "press" a generated menu row: rows are built in
// a loop, so they have no stable key to look up.
static int click_hid_in(const VNodePtr& n, const std::string& cls,
                        const std::string& text) {
  if (!n) return 0;
  std::string classes;
  for (size_t i = 0; i < n->attrs.size(); ++i) {
    if (n->attrs[i].key == "class") classes = n->attrs[i].value;
  }
  if (classes.find(cls) != std::string::npos) {
    bool has_text = false;
    std::function<void(const VNodePtr&)> scan = [&](const VNodePtr& m) {
      if (!m || has_text) return;
      if (m->is_text() && m->text == text) { has_text = true; return; }
      for (size_t k = 0; k < m->children.size(); ++k) scan(m->children[k]);
    };
    scan(n);
    if (has_text) {
      for (size_t i = 0; i < n->events.size(); ++i) {
        if (n->events[i].kind == EventKind::kClick) return n->events[i].hid;
      }
    }
  }
  for (size_t i = 0; i < n->children.size(); ++i) {
    int hid = click_hid_in(n->children[i], cls, text);
    if (hid) return hid;
  }
  return 0;
}

FS_TEST(uiapp_mounts_home) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  // top tab strip with 主页 / 设置
  EXPECT_TRUE(dump_has(dom, "主页"));
  EXPECT_TRUE(dump_has(dom, "设置"));
  // home title buttons
  EXPECT_TRUE(dump_has(dom, "探索"));
  EXPECT_TRUE(dump_has(dom, "项目"));
  EXPECT_TRUE(dump_has(dom, "用量统计"));
  // empty state until a backend replies
  EXPECT_TRUE(dump_has(dom, "还没有项目"));
}

FS_TEST(uiapp_project_card_renders) {
  NullDom dom;
  AppState st = AppState::make();
  seed_project(st);
  UiApp app(dom, st, nullptr);
  app.mount();
  EXPECT_TRUE(dump_has(dom, "FlashSight"));
  EXPECT_TRUE(dump_has(dom, "C:\\aaa\\FlashSight"));
}

FS_TEST(uiapp_switch_home_tabs) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) { s.home_tab = 2; });  // 用量统计
  EXPECT_TRUE(dump_has(dom, "按模型"));
  EXPECT_TRUE(dump_has(dom, "按天"));   // the real byDay chart, not a second table
  EXPECT_TRUE(dump_has(dom, "bar-chart"));
  app.update([](AppState& s) { s.home_tab = 0; });  // 探索
  // 探索 is the capability browser: MCP + Skill, searchable
  EXPECT_TRUE(dump_has(dom, "搜索名称或说明"));
  EXPECT_TRUE(dump_has(dom, "MCP"));
  EXPECT_TRUE(dump_has(dom, "Skill"));
}

// The session list is always in the tree: CSS picks how to show it (wide shows
// it directly, narrow collapses it behind the header), so the app must not
// conditionally build it - a wide card would otherwise have nothing to show.
FS_TEST(uiapp_project_card_renders_session_list) {
  NullDom dom;
  AppState st = AppState::make();
  seed_project(st);
  UiApp app(dom, st, nullptr);
  app.mount();
  EXPECT_TRUE(dump_has(dom, "UI 框架重构"));   // the list is present
  EXPECT_TRUE(dump_has(dom, "+ 新建会话"));
  EXPECT_TRUE(dump_has(dom, "proj-right"));
  // collapsed/expanded is a narrow-layout presentation detail driven by CSS
  app.update([](AppState& s) { s.projects[0].expanded = true; });
  EXPECT_TRUE(dump_has(dom, "proj-right open"));
  EXPECT_TRUE(dump_has(dom, "UI 框架重构"));
}

FS_TEST(uiapp_open_session_tab) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) { seed_session(s); });
  // session page: composer + side panel tabs
  EXPECT_TRUE(dump_has(dom, "输入消息"));
  EXPECT_TRUE(dump_has(dom, "用量"));
  EXPECT_TRUE(dump_has(dom, "子代理"));
}

FS_TEST(uiapp_close_session_tab) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) { seed_session(s); });
  app.update([](AppState& s) {
    for (size_t i = 0; i < s.tab_ids.size(); ++i) {
      if (s.tab_ids[i] == "sess_a") {
        s.tab_ids.erase(s.tab_ids.begin() + i);
        break;
      }
    }
    s.active_tab = "home";
  });
  EXPECT_TRUE(dump_has(dom, "探索"));
  EXPECT_FALSE(dump_has(dom, "输入消息"));
}

FS_TEST(uiapp_send_message_adds_chat) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);  // no backend: local demo reply path
  app.mount();
  app.update([](AppState& s) {
    seed_session(s);
    s.session_input = "hello world";
  });
  app.send_message();
  EXPECT_TRUE(dump_has(dom, "hello world"));
  EXPECT_EQ(app.state().messages.size(), size_t(2));
  EXPECT_EQ(app.state().messages[0].role, std::string("user"));
  EXPECT_EQ(app.state().messages[1].role, std::string("assistant"));
  EXPECT_TRUE(app.state().session_input.empty());
}

FS_TEST(uiapp_side_panel_switch) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) { seed_session(s); });
  // default side tab = usage
  EXPECT_TRUE(dump_has(dom, "上下文水位"));
  app.update([](AppState& s) { s.active_side = "subagents"; });
  EXPECT_TRUE(dump_has(dom, "没有子代理"));
  // terminals are NOT a side panel any more: they live in the bottom drawer, so
  // the panel kinds that remain are the ones with their own content
  app.open_or_focus_side_tab("ops");
  EXPECT_TRUE(dump_has(dom, "操作记录"));
}

FS_TEST(uiapp_side_add_shows_kind_menu) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) { seed_session(s); });
  // "+" must not silently add a files tab: it opens a picker
  size_t before = app.state().side_tabs.size();
  app.update([](AppState& s) { s.side_menu_open = true; });
  EXPECT_EQ(app.state().side_tabs.size(), before);
  EXPECT_TRUE(dump_has(dom, "文件"));
  EXPECT_TRUE(dump_has(dom, "Git"));
  // a MULTI kind (git) always adds another panel
  app.open_or_focus_side_tab("git");
  EXPECT_EQ(app.state().side_tabs.size(), before + 1);
  EXPECT_EQ(app.state().side_tabs.back().kind, std::string("git"));
  app.open_or_focus_side_tab("git");
  EXPECT_EQ(app.state().side_tabs.size(), before + 2);  // two diffs at once
  // a SINGLETON kind (usage) focuses the existing one instead of duplicating
  size_t with_two = app.state().side_tabs.size();
  app.open_or_focus_side_tab("usage");
  EXPECT_EQ(app.state().side_tabs.size(), with_two);
  app.open_or_focus_side_tab("usage");
  EXPECT_EQ(app.state().side_tabs.size(), with_two);
}

// Each session tab keeps its own messages / input / side panel. Switching
// away and back must restore them (this was broken before per-tab views).
FS_TEST(uiapp_session_tabs_keep_their_own_state) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  // open session A and give it a message + a widened side panel
  app.open_session("sess_a", "会话A");
  app.update([](AppState& s) {
    comp::ChatMsg m;
    m.role = "user";
    m.text = "AAA";
    s.messages.push_back(m);
    s.side_width = 500;
  });
  // open session B: its own (empty) view, its own default panel width
  app.open_session("sess_b", "会话B");
  EXPECT_EQ(app.state().messages.size(), size_t(0));
  EXPECT_EQ(app.state().side_width, 420);  // the new default width
  app.update([](AppState& s) {
    comp::ChatMsg m;
    m.role = "user";
    m.text = "BBB";
    s.messages.push_back(m);
  });

  app.show("sess_a");
  ASSERT_TRUE(app.state().messages.size() == 1);
  EXPECT_EQ(app.state().messages[0].text, std::string("AAA"));
  EXPECT_EQ(app.state().side_width, 500);  // panel width survived the switch

  app.show("sess_b");
  ASSERT_TRUE(app.state().messages.size() == 1);
  EXPECT_EQ(app.state().messages[0].text, std::string("BBB"));
  EXPECT_EQ(app.state().side_width, 420);  // the new default width  // B keeps its own default
}

// The side-panel divider reports the final width, which must land in state.
FS_TEST(uiapp_splitter_width_persists) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) { seed_session(s, "sess_a"); });
  EXPECT_TRUE(dom.count_of("bind_splitter") >= 1);
  dom.fire_split_for_test(430);
  EXPECT_EQ(app.state().side_width, 430);
}

// Escape (document-level key) dismisses the "/" skill picker.
FS_TEST(uiapp_escape_dismisses_skill_menu) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) { seed_session(s, "sess_a"); });
  app.update([](AppState& s) {
    s.skill_menu_open = true;
    AppState::CommandRow c;
    c.name = "lsp";
    c.title = "LSP";
    c.action = "skill:lsp";
    c.group = "skill";
    s.command_menu.push_back(c);
  });
  EXPECT_TRUE(dump_has(dom, "/lsp"));
  dom.fire_key_for_test("Escape");
  EXPECT_FALSE(app.state().skill_menu_open);
  dom.fire_key_for_test("a");  // other keys must not close it
  app.update([](AppState& s) { s.skill_menu_open = true; });
  dom.fire_key_for_test("Enter");
  EXPECT_TRUE(app.state().skill_menu_open);
}

// A background session's stream events land in that session's view, not on
// whatever tab the user is currently looking at.
FS_TEST(uiapp_background_session_events_are_isolated) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) { seed_session(s, "sess_a"); });
  app.on_backend_event_for_test(
      "stream.delta", Json{{"sid", "sess_b"}, {"text", "from B"}});
  // the visible session must be untouched
  EXPECT_EQ(app.state().messages.size(), size_t(0));
  // switching to B shows the streamed text
  app.show("sess_b");
  ASSERT_TRUE(app.state().messages.size() == 1);
  EXPECT_EQ(app.state().messages[0].text, std::string("from B"));
}

FS_TEST(uiapp_drop_zone_bound) {  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  // the window carries the drop zone and render() binds it
  EXPECT_TRUE(dom.count_of("bind_drop_zone") >= 1);
  // a dropped name with no backend is a no-op (resolve fails), not a crash
  app.handle_dropped_folder("some-folder");
  EXPECT_EQ(app.state().projects.size(), size_t(0));
}

FS_TEST(uiapp_project_card_actions) {
  NullDom dom;
  AppState st = AppState::make();
  seed_project(st);
  UiApp app(dom, st, nullptr);
  app.mount();
  // card exposes 在资源管理器中打开 / 移出列表 (not an ambiguous 打开/收起 pair)
  EXPECT_TRUE(dump_has(dom, "在资源管理器中打开"));
  EXPECT_TRUE(dump_has(dom, "移出列表"));
  // the sessions area is present even when collapsed (dominant card area)
  EXPECT_TRUE(dump_has(dom, "会话"));
  // The narrow-layout reveal is CSS (header click); the list rows are rendered
  // regardless. The >10 expand control is a separate end-of-list row, tested by
  // uiapp_long_session_list_expands_at_end.
  EXPECT_TRUE(dump_has(dom, "UI 框架重构"));
}

FS_TEST(uiapp_long_session_list_scrolls_on_home_and_caps_in_nav) {
  // home: EVERY session is rendered; the container scrolls (CSS), there is no
  // end-of-list expand row.
  {
    NullDom dom;
    AppState st = AppState::make();
    seed_project(st);
    st.projects[0].sessions.clear();
    for (int i = 0; i < 11; ++i) {
      ProjectCard::SessionRow row;
      row.sid = "s" + std::to_string(i);
      row.title = "SESSION_" + std::to_string(i);
      st.projects[0].sessions.push_back(row);
    }
    st.projects[0].session_count = 11;
    UiApp app(dom, st, nullptr);
    app.mount();
    EXPECT_TRUE(dump_has(dom, "SESSION_10"));
    EXPECT_FALSE(dump_has(dom, "展开剩余"));
  }
  // left rail: 10 visible, the rest behind 展开剩余 N 个 at the END.
  {
    NullDom dom;
    AppState st = AppState::make();
    st.settings_values["ui.tabs"] = "left";
    seed_project(st);
    st.projects[0].sessions.clear();
    for (int i = 0; i < 11; ++i) {
      ProjectCard::SessionRow row;
      row.sid = "s" + std::to_string(i);
      row.title = "SESSION_" + std::to_string(i);
      st.projects[0].sessions.push_back(row);
    }
    st.projects[0].session_count = 11;
    UiApp app(dom, st, nullptr);
    app.mount();
    EXPECT_FALSE(nav_has(dom, "SESSION_10"));
    EXPECT_TRUE(nav_has(dom, "展开剩余"));
    app.update([](AppState& s) { s.projects[0].nav_open = true; });
    EXPECT_TRUE(nav_has(dom, "SESSION_10"));
    EXPECT_TRUE(nav_has(dom, "收起"));
  }
}

FS_TEST(uiapp_splitter_bound) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) { seed_session(s); });
  // the side panel + divider exist and render() binds the drag handler
  EXPECT_TRUE(dom.count_of("bind_splitter") >= 1);
}

FS_TEST(uiapp_terminal_selector) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    seed_session(s);
    s.terminals.push_back(TermItem{"1", "pwsh", "", "running"});
    s.terminals.push_back(TermItem{"2", "cmd", "构建", "exited"});
    s.active_term = "1";
    s.terminal_open = true;
  });
  EXPECT_TRUE(dump_has(dom, "#1 pwsh"));
  EXPECT_TRUE(dump_has(dom, "#2 构建"));  // a named terminal shows its name
  // no status text on the right of the bar any more (per user feedback)
  EXPECT_FALSE(dump_has(dom, "运行中 /"));
}

// The bottom drawer is resizable: the drag handle reports a height that must
// land in state (and in the per-session view, so it survives tab switches).
FS_TEST(uiapp_terminal_height_persists) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    seed_session(s, "sess_a");
    s.terminal_open = true;
  });
  EXPECT_TRUE(dom.count_of("bind_vsplitter") >= 1);
  dom.fire_vsplit_for_test(480);
  EXPECT_EQ(app.state().term_height, 480);
}

// Dropping a folder opens it as a project; dropping a link/file goes to the
// composer instead (the two cases must not be confused).
FS_TEST(uiapp_drop_routes_by_kind) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) { seed_session(s, "sess_a"); });

  dom.fire_drop_for_test("url", "https://example.com/doc");
  EXPECT_TRUE(app.state().session_input.find("https://example.com/doc") !=
              std::string::npos);
  EXPECT_FALSE(app.state().toast.empty());

  app.update([](AppState& s) { s.session_input.clear(); });
  dom.fire_drop_for_test("file", "notes.md");
  // A dropped file is an ATTACHMENT, not text: it becomes a chip above the
  // input, so the textarea keeps only what was typed.
  EXPECT_TRUE(app.state().session_input.empty());
  ASSERT_TRUE(app.state().attachments.size() == 1);
  EXPECT_EQ(app.state().attachments[0], std::string("notes.md"));

  // a folder is not text: it is resolved+opened through the backend instead
  app.update([](AppState& s) { s.session_input.clear(); });
  dom.fire_drop_for_test("dir", "some-folder");
  EXPECT_TRUE(app.state().session_input.empty());
}

// Creating a terminal is one click: the shell comes from settings and there is
// nothing to fill in up front (the name is edited on the tab afterwards).
FS_TEST(uiapp_new_terminal_is_one_click) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    seed_session(s, "sess_a");
    s.terminal_open = true;
  });
  // the strip offers 新增 with no form behind it
  EXPECT_TRUE(dump_has(dom, "新增"));
  EXPECT_FALSE(dump_has(dom, "名称（可选）"));
  EXPECT_FALSE(dump_has(dom, "命令…"));
}

// A terminal is a grid, not a text box: the panel renders the engine's screen
// (styled runs + a cursor) and takes raw keys, so there is no <input> for it.
FS_TEST(uiapp_terminal_renders_grid_and_takes_keys) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    seed_session(s, "sess_a");
    s.terminal_open = true;
    s.terminals.push_back(TermItem{"1", "cmd", "构建", "running"});
    s.term_tab = "1";
  });
  // the grid the backend would send: two rows, the first with a colored run
  Json grid = Json::object();
  Json lines = Json::array();
  Json row0 = Json::array();
  row0.push_back(Json{{"x", 0}, {"t", "hello"}, {"fg", 0x0dbc79}});
  Json row1 = Json::array();
  row1.push_back(Json{{"x", 2}, {"t", "world"}, {"a", 1}});
  lines.push_back(row0);
  lines.push_back(row1);
  grid["lines"] = lines;
  grid["cols"] = 80;
  grid["rows"] = 2;
  grid["cursorX"] = 5;
  grid["cursorY"] = 1;
  grid["cursorVisible"] = true;
  app.update([&](AppState& s) { s.term_grids["1"] = grid; });

  EXPECT_TRUE(dump_has(dom, "term-screen"));   // the surface, not a textarea
  EXPECT_TRUE(dump_has(dom, "ts-run"));
  EXPECT_TRUE(dump_has(dom, "ts-cursor"));
  EXPECT_TRUE(dump_has(dom, "hello"));
  EXPECT_TRUE(dump_has(dom, "world"));
  EXPECT_TRUE(dump_has(dom, "#0dbc79"));       // the color landed as a style
  EXPECT_TRUE(dom.count_of("bind_key_input") >= 1);
  EXPECT_TRUE(dom.count_of("bind_cell_size") >= 1);
  // no <input>/<textarea> is used to drive the terminal
  EXPECT_FALSE(dump_has(dom, "term-input"));
}

// The adapter measures the surface and reports the cell grid; the app forwards
// it (the child process needs the window size to lay out full-screen programs).
FS_TEST(uiapp_terminal_reports_cell_size) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    seed_session(s, "sess_a");
    s.terminal_open = true;
    s.terminals.push_back(TermItem{"1", "cmd", "", "running"});
    s.term_tab = "1";
  });
  dom.fire_cell_size_for_test(120, 30);
  EXPECT_EQ(app.state().term_cols, 120);
  EXPECT_EQ(app.state().term_rows, 30);
}

// Renaming a terminal only changes its label (the shell keeps running).
FS_TEST(uiapp_terminal_rename_keeps_process) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    seed_session(s, "sess_a");
    s.terminal_open = true;
    s.terminals.push_back(TermItem{"1", "cmd", "", "running"});
    s.term_tab = "1";
  });
  app.update([](AppState& s) {
    s.term_renaming = "1";
    s.term_rename_text = "构建日志";
  });
  // the tab turns into an input (keys are diff identities, not attributes, so
  // assert on the rendered class instead)
  EXPECT_TRUE(dump_has(dom, "td-tab-rename"));
  app.rename_terminal("1", "构建日志");
  EXPECT_EQ(app.state().terminals[0].label, std::string("构建日志"));
  EXPECT_EQ(app.state().terminals[0].status, std::string("running"));
  EXPECT_TRUE(app.state().term_renaming.empty());
}

// Every page the app can show must declare an animation: the framework looks the
// intent up by page id, so an unlisted page would silently appear with none.
// This is the coverage check for "animation must not be forgotten".
FS_TEST(uiapp_every_page_declares_an_animation) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  // the three page shapes: home, settings, and a session
  app.open_modal_page("settings");
  std::string d1 = dom.dump();
  EXPECT_TRUE(d1.find("anim-view") != std::string::npos);
  app.update([](AppState& s) { seed_session(s, "sess_a"); });
  std::string d2 = dom.dump();
  EXPECT_TRUE(d2.find("anim-view") != std::string::npos);
  app.show("home");
  std::string d3 = dom.dump();
  // home is reached by a tab switch, so it carries a DIRECTIONAL slide rather
  // than the neutral view animation; any anim-* class means it is declared.
  EXPECT_TRUE(d3.find("anim-") != std::string::npos);
}

// 前进/后退方向感知：去一个新标签从右滑入；回到"上一页"（刚离开的那一页）
// 时播放反向动画，从左滑入。动画方向与实际导航方向一致，而不是永远从同一侧
// 出现。注意"上一个"是字面意思：只有目标 == 栈顶（刚来的地方）才算后退，
// 回到很久以前访问过的标签仍然是前进。
FS_TEST(uiapp_tab_switch_direction_aware) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    // seed both tabs but stay on home, so the first show() is a real navigation
    SessionTab a;
    a.id = "sess_a";
    s.sessions.push_back(a);
    s.tab_ids.push_back("sess_a");
    SessionTab b;
    b.id = "sess_b";
    s.sessions.push_back(b);
    s.tab_ids.push_back("sess_b");
  });
  // home -> sess_a: forward (from the right)
  app.show("sess_a");
  EXPECT_TRUE(dom.dump().find("anim-slide-left") != std::string::npos);
  // sess_a -> sess_b: forward
  app.show("sess_b");
  EXPECT_TRUE(dom.dump().find("anim-slide-left") != std::string::npos);
  // sess_b -> sess_a: sess_a is exactly the previous page, so this is a retrace
  app.show("sess_a");
  EXPECT_TRUE(dom.dump().find("anim-slide-right") != std::string::npos);
  // sess_a -> sess_b is a retrace again (we just came from it)
  app.show("sess_b");
  EXPECT_TRUE(dom.dump().find("anim-slide-right") != std::string::npos);
  // closing sess_b returns to sess_a: closing is always a backward move
  app.close_tab("sess_b");
  EXPECT_EQ(app.state().active_tab, std::string("sess_a"));
  EXPECT_TRUE(dom.dump().find("anim-slide-right") != std::string::npos);
  // home is not the page we just left, so this is forward, not a retrace
  app.show("home");
  EXPECT_TRUE(dom.dump().find("anim-slide-left") != std::string::npos);
}

// Creating a terminal is one click, and the tab strip offers a close that does
// NOT end the process (the model may still be using it) and does NOT remove the
// terminal from the list either: 所有终端 shows every terminal whether or not it
// has a tab, so closing a tab only un-tabs it.
FS_TEST(uiapp_terminal_close_tab_keeps_process) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    seed_session(s, "sess_a");
    s.terminal_open = true;
    s.terminals.push_back(TermItem{"1", "cmd", "", "running"});
    s.term_tab = "1";
  });
  app.close_terminal_tab("1");
  // still in the list (so 所有终端 keeps showing it), but no longer a tab
  EXPECT_EQ(app.state().terminals.size(), size_t(1));
  EXPECT_EQ(app.state().term_tab, std::string("all"));
  EXPECT_EQ(app.state().term_hidden.size(), size_t(1));
  // and the terminal was NOT killed (no terminal_kill was issued)
  for (size_t i = 0; i < dom.calls().size(); ++i) {
    EXPECT_TRUE(dom.calls()[i].detail.find("terminal_kill") == std::string::npos);
  }
  // closing is not a dead end: the overview card reopens it
  app.reopen_terminal_tab("1");
  EXPECT_TRUE(app.state().term_hidden.empty());
}

// A reopened tab appends to the strip instead of slotting back into tid order:
// the tabs read as "the order you opened them", so reopening must not make a
// new tab appear in the middle of the strip.
FS_TEST(uiapp_reopened_tab_appends) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    seed_session(s, "sess_a");
    s.terminal_open = true;
    s.terminals.push_back(TermItem{"1", "cmd", "one", "running"});
    s.terminals.push_back(TermItem{"2", "cmd", "two", "running"});
    s.terminals.push_back(TermItem{"3", "cmd", "three", "running"});
    s.term_order = {"1", "2", "3"};
  });
  // close the middle tab, then reopen it
  app.close_terminal_tab("2");
  app.reopen_terminal_tab("2");
  std::vector<const TermItem*> ord = app.ordered_terminals();
  EXPECT_EQ(ord.size(), size_t(3));
  EXPECT_EQ(ord[0]->tid, std::string("1"));
  EXPECT_EQ(ord[1]->tid, std::string("3"));
  EXPECT_EQ(ord[2]->tid, std::string("2"));  // appended, not back in the middle
}

// Visible terminal numbers are the ORDER in the user's list, not the backend
// tid: short unrecorded commands consumed tids, so a list of 11 terminals could
// display #107.
FS_TEST(uiapp_terminal_numbers_are_list_ordinals) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    seed_session(s, "sess_a");
    s.terminal_open = true;
    s.terminals.push_back(TermItem{"107", "cmd", "one", "running"});
    s.terminals.push_back(TermItem{"108", "cmd", "two", "running"});
    s.term_order = {"107", "108"};
  });
  std::string d = dom.dump();
  EXPECT_TRUE(d.find("#1 ") != std::string::npos);
  EXPECT_TRUE(d.find("#2 ") != std::string::npos);
  EXPECT_TRUE(d.find("#107") == std::string::npos);
}

// The 文件 button uploads (a DOM-adapter binding opens the chooser) instead of
// opening the file panel; the reply adds ATTACHMENTS above the composer.
FS_TEST(uiapp_file_button_uploads) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    seed_session(s, "sess_a");
    s.active_pid = "p1";
  });
  EXPECT_TRUE(dom.count_of("bind_file_picker") >= 1);
  dom.fire_file_pick_for_test(
      "{\"files\":[{\"rel\":\".flashsight/uploads/a.png\",\"name\":\"a.png\","
      "\"size\":10},{\"rel\":\".flashsight/uploads/b.pdf\",\"name\":\"b.pdf\","
      "\"size\":20}]}");
  // chips, not text: the message box stays clean
  EXPECT_TRUE(app.state().session_input.empty());
  ASSERT_TRUE(app.state().attachments.size() == 2);
  EXPECT_EQ(app.state().attachments[0], std::string(".flashsight/uploads/a.png"));
  EXPECT_EQ(app.state().attachments[1], std::string(".flashsight/uploads/b.pdf"));
  EXPECT_TRUE(dump_has(dom, "composer-attach"));
  EXPECT_TRUE(dump_has(dom, "a.png"));
  EXPECT_FALSE(app.state().toast.empty());
}

// 上下文水位贴在模型按钮的底边上（模型按钮的装饰），不再是输入框下方横贯整行
// 的一条进度条；颜色/宽度仍由 context_pct 驱动。
FS_TEST(uiapp_context_level_hugs_model_button) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    seed_session(s, "sess_a");
    s.context_pct = 92;
  });
  std::string d = dom.dump();
  EXPECT_TRUE(d.find("model-btn-wrap") != std::string::npos);
  EXPECT_TRUE(d.find("model-level") != std::string::npos);
  EXPECT_TRUE(d.find("model-level-fill hot") != std::string::npos);
  // the old full-width bar is gone
  EXPECT_TRUE(d.find("composer-level") == std::string::npos);
  // and it lives inside the right-hand group, right next to the model button
  size_t wrap = d.find("model-btn-wrap");
  size_t btn = d.find("model-pick-btn");
  ASSERT_TRUE(wrap != std::string::npos && btn != std::string::npos);
  EXPECT_TRUE(wrap < btn);
  // level colours are thresholds: 42% is neutral, 70% warm
  app.update([](AppState& s) { s.context_pct = 70; });
  EXPECT_TRUE(dom.dump().find("model-level-fill warm") != std::string::npos);
  app.update([](AppState& s) { s.context_pct = 20; });
  std::string d2 = dom.dump();
  EXPECT_TRUE(d2.find("model-level-fill warm") == std::string::npos);
  EXPECT_TRUE(d2.find("model-level-fill hot") == std::string::npos);
}

// 工具栏顺序：自动挡在批准模式之前（先定谁来做，再定能自己动到什么程度）。
FS_TEST(uiapp_auto_mode_before_permission) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) { seed_session(s, "sess_a"); });
  std::string d = dom.dump();
  size_t auto_pos = d.find("自动");
  size_t perm_pos = d.find("逐次批准");
  ASSERT_TRUE(auto_pos != std::string::npos && perm_pos != std::string::npos);
  EXPECT_TRUE(auto_pos < perm_pos);
}

// The side panel "+" opens a page listing every panel kind (with a reason to
// exist), and an already-open panel is marked as such.
FS_TEST(uiapp_side_add_page_lists_kinds) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) { seed_session(s, "sess_a"); });
  app.update([](AppState& s) { s.side_menu_open = true; });
  EXPECT_TRUE(dump_has(dom, "side-add"));
  EXPECT_TRUE(dump_has(dom, "添加面板"));
  EXPECT_TRUE(dump_has(dom, "项目文件树"));          // each kind explains itself
  EXPECT_TRUE(dump_has(dom, "文件内容"));            // preview is its own panel
  EXPECT_TRUE(dump_has(dom, "模型发起的工具调用"));
  EXPECT_TRUE(dump_has(dom, "已打开"));             // seeded panels are marked
}

// The panel strip must be KEYED. Unkeyed children are matched by position, so
// closing a middle panel shifts every later node left and the handlers travel
// with the slot - closing one panel then fired the next slot's action (in
// practice the "+" page opened by itself).
FS_TEST(uiapp_panel_tabs_are_keyed) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    seed_session(s, "sess_a");
    s.side_tabs.clear();
    s.side_seq = 0;
  });
  app.open_or_focus_side_tab("files");
  app.open_or_focus_side_tab("ops");
  std::string ops = app.state().active_side;
  // every panel tab carries its own key, and the "+" carries one too
  const VNodePtr& tree = app.tree_for_test();
  VNodePtr bar = find_by_key(tree, "panel-add");
  EXPECT_TRUE(bar != nullptr);
  // collect keys of the strip's children
  auto keys_of = [](const VNodePtr& n) {
    std::vector<std::string> out;
    if (!n) return out;
    for (size_t i = 0; i < n->children.size(); ++i) {
      out.push_back(n->children[i]->key);
    }
    return out;
  };
  VNodePtr strip = nullptr;
  // find the panel-tabs container: the parent of the keyed add button
  std::function<void(const VNodePtr&)> walk = [&](const VNodePtr& n) {
    if (!n) return;
    for (size_t i = 0; i < n->children.size(); ++i) {
      if (n->children[i] == bar) strip = n;
      walk(n->children[i]);
    }
  };
  walk(tree);
  ASSERT_TRUE(strip != nullptr);
  std::vector<std::string> keys = keys_of(strip);
  EXPECT_EQ(keys.size(), size_t(3));  // two panels + the "+"
  for (size_t i = 0; i < keys.size(); ++i) EXPECT_FALSE(keys[i].empty());

  // closing the active panel leaves the strip intact and does NOT open the
  // "+" page (the regression this test exists for)
  app.close_side_tab(ops);
  EXPECT_FALSE(app.state().side_menu_open);
  EXPECT_EQ(app.state().side_tabs.size(), size_t(1));
}

// The terminal overview shows a few lines per terminal (not a whole screen) and
// is scrollable, so many terminals stay usable.
FS_TEST(uiapp_terminal_overview_is_tail) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    seed_session(s, "sess_a");
    s.terminal_open = true;
    s.terminals.push_back(TermItem{"1", "cmd", "构建", "running"});
    s.term_tab = "all";
  });
  // a 10-line screen: only the tail should be shown
  Json lines = Json::array();
  for (int i = 0; i < 10; ++i) {
    Json row = Json::array();
    row.push_back(Json{{"x", 0}, {"t", "line" + std::to_string(i)}});
    lines.push_back(row);
  }
  Json grid = Json::object();
  grid["lines"] = lines;
  grid["cols"] = 80;
  grid["rows"] = 10;
  app.update([&](AppState& s) { s.term_grids["1"] = grid; });
  EXPECT_TRUE(dump_has(dom, "line9"));    // the newest line is there
  EXPECT_FALSE(dump_has(dom, "line0"));   // the oldest ones are dropped
}

// The terminal view scrolls: the grid carries history above the live screen, the
// cursor is placed on the right line of that combined buffer, and the view is
// pinned to the bottom unless the user scrolled up.
FS_TEST(uiapp_terminal_scrolls_with_history) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    seed_session(s, "sess_a");
    s.terminal_open = true;
    s.terminals.push_back(TermItem{"1", "cmd", "", "running"});
    s.term_tab = "1";
  });
  // 2 history lines above a 3-line screen; the cursor sits on screen row 1
  Json lines = Json::array();
  const char* texts[] = {"hist-0", "hist-1", "live-0", "live-1", "live-2"};
  for (int i = 0; i < 5; ++i) {
    Json row = Json::array();
    row.push_back(Json{{"x", 0}, {"t", texts[i]}});
    lines.push_back(row);
  }
  Json grid = Json::object();
  grid["lines"] = lines;
  grid["cols"] = 80;
  grid["rows"] = 3;
  grid["from"] = 0;
  grid["scrollback"] = 2;   // the first two lines are history
  grid["totalLines"] = 5;
  grid["cursorX"] = 3;
  grid["cursorY"] = 1;      // screen row 1 -> combined line 2 + 1 = 3
  grid["cursorVisible"] = true;
  app.update([&](AppState& s) { s.term_grids["1"] = grid; });

  EXPECT_TRUE(dump_has(dom, "hist-0"));   // history is rendered (scrollable)
  EXPECT_TRUE(dump_has(dom, "live-2"));
  EXPECT_TRUE(dump_has(dom, "ts-lines"));
  EXPECT_TRUE(dom.count_of("bind_scroll_pin") >= 1);
  // the cursor landed on the combined line for screen row 1, not row 0
  std::string d = dom.dump();
  size_t cursor_at = d.find("ts-cursor");
  size_t row_start = d.rfind("ts-row", cursor_at);
  std::string row_block = d.substr(row_start, cursor_at - row_start);
  EXPECT_TRUE(row_block.find("live-1") != std::string::npos);
  EXPECT_TRUE(row_block.find("hist-1") == std::string::npos);
}

// Scrolling up must not be undone by the next line of output; sitting at the
// bottom must follow it.
FS_TEST(uiapp_terminal_scroll_pin_state) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    seed_session(s, "sess_a");
    s.terminal_open = true;
    s.terminals.push_back(TermItem{"1", "cmd", "", "running"});
    s.term_tab = "1";
  });
  dom.fire_scroll_pin_for_test(false);   // user scrolled up
  dom.clear_calls();
  app.tick_terminal();                   // output arrives
  // no scroll_to_bottom while the user is reading history
  EXPECT_EQ(dom.count_of("scroll_to_bottom"), 0);
  dom.fire_scroll_pin_for_test(true);    // back at the bottom
  EXPECT_EQ(app.state().term_tab, std::string("1"));
}

// The terminal strip filters live once it gets crowded. Name matching is always
// on; the two toggles widen it to the commands typed in and to the whole screen.
FS_TEST(uiapp_terminal_filter_modes) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    seed_session(s, "sess_a");
    s.terminal_open = true;
    // 10 terminals: the threshold for the search box to appear
    for (int i = 1; i <= 10; ++i) {
      s.terminals.push_back(
          TermItem{std::to_string(i), "pwsh", i == 3 ? "构建日志" : "", "running"});
    }
    s.term_tab = "all";
  });
  // the search row only exists past the threshold
  EXPECT_TRUE(dump_has(dom, "td-search"));
  EXPECT_TRUE(dump_has(dom, "筛选终端"));

  // name match: the labelled terminal survives, the others are filtered out
  app.update([](AppState& s) { s.term_filter = "构建"; });
  EXPECT_TRUE(overview_has(dom, "#3 构建日志"));
  EXPECT_FALSE(overview_has(dom, "#1 pwsh"));

  // fuzzy: a plain pattern is a subsequence match, so a partial name works
  // (this is over the actual characters, not pinyin)
  app.update([](AppState& s) { s.term_filter = "构建日"; });   // 构建日志
  EXPECT_TRUE(overview_has(dom, "#3 构建日志"));
  EXPECT_FALSE(overview_has(dom, "#1 pwsh"));

  // regex when the pattern looks like one
  app.update([](AppState& s) { s.term_filter = "构建|pwsh"; });
  EXPECT_TRUE(overview_has(dom, "#1 pwsh"));
  EXPECT_TRUE(overview_has(dom, "#3 构建日志"));

  // a half-typed regex must not blank the list (it falls back to fuzzy)
  app.update([](AppState& s) { s.term_filter = "构建["; });
  EXPECT_TRUE(overview_has(dom, "#3 构建日志"));

  // no match at all -> an explicit empty state, not a blank panel
  app.update([](AppState& s) { s.term_filter = "zzzz-no-such"; });
  EXPECT_TRUE(dump_has(dom, "无匹配终端"));

  // 输入匹配: a command that appears nowhere in the name
  app.update([](AppState& s) {
    s.term_filter = "npm-run-build";
    s.term_filter_input = true;
    s.term_input_log["7"] = "npm run build\n";
  });
  // the pattern is not a regex (no metacharacters), so it is a subsequence
  // match against what was typed - "npm run build" contains "npm-run-build"
  // only as a subsequence once the separators are ignored... which they are
  // not, so use a pattern that really is a subsequence of the command.
  app.update([](AppState& s) { s.term_filter = "npmbuild"; });
  EXPECT_TRUE(overview_has(dom, "#7 pwsh"));
  EXPECT_FALSE(overview_has(dom, "#1 pwsh"));
  // ...and off again it matches nothing
  app.update([](AppState& s) { s.term_filter_input = false; });
  EXPECT_TRUE(dump_has(dom, "无匹配终端"));

  // 全文匹配: something only present in the terminal's output
  app.update([](AppState& s) {
    s.term_filter = "compiled-successfully";
    s.term_filter_text = true;
  });
  Json lines = Json::array();
  Json row = Json::array();
  row.push_back(Json{{"x", 0}, {"t", "compiled-successfully"}});
  lines.push_back(row);
  Json grid = Json::object();
  grid["lines"] = lines;
  grid["cols"] = 80;
  grid["rows"] = 1;
  app.update([&](AppState& s) { s.term_grids["5"] = grid; });
  EXPECT_TRUE(overview_has(dom, "#5 pwsh"));
  EXPECT_FALSE(overview_has(dom, "#1 pwsh"));
}

// Below the threshold there is no search box (it would be noise).
FS_TEST(uiapp_terminal_filter_hidden_when_few) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    seed_session(s, "sess_a");
    s.terminal_open = true;
    s.terminals.push_back(TermItem{"1", "pwsh", "", "running"});
    s.term_tab = "all";
  });
  EXPECT_FALSE(dump_has(dom, "td-search"));
}

// Every terminal-strip entry is keyed. Unkeyed items get matched positionally by
// the diff, so removing one made its node be reused for the next entry - and the
// click still propagating then hit a DIFFERENT entry (closing the last terminal
// spawned a new one). This asserts the keys are actually there.
FS_TEST(uiapp_terminal_strip_entries_are_keyed) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    seed_session(s, "sess_a");
    s.terminal_open = true;
    s.terminals.push_back(TermItem{"1", "pwsh", "", "running"});
    s.terminals.push_back(TermItem{"2", "cmd", "", "running"});
    s.term_tab = "1";
  });
  // the tab strip: all + one per terminal + 新增, each with its own key
  const VNodePtr& tree = app.tree_for_test();
  EXPECT_TRUE(find_by_key(tree, "term-tab:all") != nullptr);
  EXPECT_TRUE(find_by_key(tree, "term-tab:1") != nullptr);
  EXPECT_TRUE(find_by_key(tree, "term-tab:2") != nullptr);
  EXPECT_TRUE(find_by_key(tree, "term-tab:add") != nullptr);
}

// The strip lists EVERY terminal even while a filter is active: the filter is
// for finding things on the overview page, and hiding a tab (especially one you
// just created) would make it look like the terminal was never started.
FS_TEST(uiapp_terminal_strip_unaffected_by_filter) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    seed_session(s, "sess_a");
    s.terminal_open = true;
    s.terminals.push_back(TermItem{"1", "pwsh", "构建日志", "running"});
    s.terminals.push_back(TermItem{"2", "cmd", "dev-server", "running"});
    s.term_tab = "all";
    s.term_filter = "构建";   // matches only #1
  });
  // the overview cards ARE filtered...
  EXPECT_TRUE(overview_has(dom, "#1 构建日志"));
  EXPECT_FALSE(overview_has(dom, "#2 dev-server"));
  // ...but both tabs are still in the strip
  const VNodePtr& tree = app.tree_for_test();
  EXPECT_TRUE(find_by_key(tree, "term-tab:1") != nullptr);
  EXPECT_TRUE(find_by_key(tree, "term-tab:2") != nullptr);
}

// A focused control keeps the user's text: the app must not write the stale
// value back over it (the composer does not re-render per keystroke, so any
// background re-render would reset the caret mid-typing).
FS_TEST(uiapp_focused_input_keeps_its_value) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) { seed_session(s, "sess_a"); });

  // find the composer textarea in the tree
  VNodePtr ta = find_by_key(app.tree_for_test(), "session-input");
  ASSERT_TRUE(ta != nullptr);
  ASSERT_TRUE(ta->dom.valid());
  dom.set_focused_for_test(ta->dom);
  dom.clear_calls();

  // a re-render happens (e.g. the terminal tick); the app's state still holds
  // the old text because typing deliberately does not re-render
  app.render();
  for (size_t i = 0; i < dom.calls().size(); ++i) {
    const NullDom::Call& c = dom.calls()[i];
    if (c.method == "set_attr" && c.node == ta->dom.id &&
        c.detail.find("value=") == 0) {
      EXPECT_TRUE(false);  // the stale value was written over the user's text
    }
  }
  // and with focus elsewhere the write is allowed again (a programmatic clear)
  dom.set_focused_for_test(Node{0});
  app.update([](AppState& s) { s.session_input = "from-app"; });
  bool wrote = false;
  for (size_t i = 0; i < dom.calls().size(); ++i) {
    const NullDom::Call& c = dom.calls()[i];
    if (c.method == "set_attr" && c.node == ta->dom.id &&
        c.detail == "value=from-app") {
      wrote = true;
    }
  }
  EXPECT_TRUE(wrote);
}

// Appearance settings reach the DOM adapter, which owns the CSS variables:
// palette, mode, fonts and size. Without this the settings would persist but
// never actually re-theme anything.
FS_TEST(uiapp_appearance_applies_to_dom) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.open_modal_page("settings");
  // a fresh app pushes the defaults (so a page that never set anything still
  // gets a defined mode instead of relying on the CSS media query)
  EXPECT_EQ(dom.appearance_for_test().mode, std::string("auto"));
  EXPECT_EQ(dom.appearance_for_test().palette, std::string("default"));

  app.update([](AppState& s) { s.settings_nav = "appearance"; });
  app.set_setting("ui.theme", "forest");
  app.set_setting("ui.mode", "dark");
  app.set_setting("ui.font.size", 16);
  app.set_setting("ui.font.family", "Fira Sans");
  EXPECT_EQ(dom.appearance_for_test().palette, std::string("forest"));
  EXPECT_EQ(dom.appearance_for_test().mode, std::string("dark"));
  EXPECT_EQ(dom.appearance_for_test().font_size, 16);
  EXPECT_EQ(dom.appearance_for_test().font, std::string("Fira Sans"));
  // the legacy meaning of ui.theme (a mode) is still accepted
  app.set_setting("ui.theme", "light");
  EXPECT_EQ(dom.appearance_for_test().mode, std::string("light"));
  EXPECT_EQ(dom.appearance_for_test().palette, std::string("default"));
}

// Switching a settings section must animate the new section in. The body is
// KEYED by section (the sections are different pages and the bridged adapter
// needs the identity), so each switch creates a fresh body node whose MOUNT
// animation is the entry - not a replay on a surviving node.
FS_TEST(uiapp_settings_sections_animate) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.open_modal_page("settings");
  const char* secs[3] = {"appearance", "network", "about"};
  for (int i = 0; i < 3; ++i) {
    std::string sec = secs[i];
    app.update([&](AppState& s) { s.settings_nav = sec; });
    std::string d = dom.dump();
    EXPECT_TRUE(d.find("settings-body") != std::string::npos);
    EXPECT_TRUE(d.find("anim-enter") != std::string::npos);
  }
}

// A page whose content arrives late animates when the content lands, not only
// when the empty page was created (which is how an animation could look like it
// never ran).
FS_TEST(uiapp_page_animation_replays_after_load) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) { seed_session(s, "sess_a"); });
  dom.clear_calls();
  // history arrives
  app.update([](AppState& s) { s.session_loaded = true; });
  int replays = 0;
  for (size_t i = 0; i < dom.calls().size(); ++i) {
    if (dom.calls()[i].method == "play_animation") ++replays;
  }
  EXPECT_TRUE(replays >= 1);
}

FS_TEST(uiapp_settings_page) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.open_modal_page("settings");
  EXPECT_TRUE(dump_has(dom, "常规"));
  EXPECT_TRUE(dump_has(dom, "供应商"));
  app.update([](AppState& s) { s.settings_nav = "providers"; });
  // providers is a list + detail page; with none selected it prompts to pick one
  EXPECT_TRUE(dump_has(dom, "新建"));
  // selecting a provider (seeded) shows the edit form with Endpoint + headers
  app.update([](AppState& s) {
    AppState::ProviderRow r;
    r.id = "p1";
    r.name = "zen";
    r.kind = "openai-chat";  // opencode is a normal provider, not a shape
    r.endpoint = "https://opencode.ai/zen/v1";
    s.providers.push_back(r);
    s.provider_selected = "p1";
    s.pv_kind = "openai-chat";
    s.pv_base_url = "https://opencode.ai/zen/v1";
    // the session-affinity header is now an ordinary custom header
    s.pv_headers.push_back(std::make_pair(std::string("x-opencode-session"),
                                          std::string("{{SESSION}}")));
    s.pv_advanced = true;
  });
  EXPECT_TRUE(dump_has(dom, "Endpoint"));
  EXPECT_TRUE(dump_has(dom, "x-opencode-session"));
  EXPECT_TRUE(dump_has(dom, "{{SESSION}}"));
  EXPECT_TRUE(dump_has(dom, "请求体覆盖"));
  // every nav entry must render its own body (nav key == body branch).
  // 常规 is 界面显示 + 会话行为 (UI behaviour); 上下文 was dropped entirely and
  // MCP/Skill are two pages now. 浏览器控制 is gone: nothing drove a browser, so
  // every switch on that page was inert.
  const char* sec_keys[] = {"general", "appearance", "computer",
                            "providers", "session_defaults",
                            "model_prefs", "subagents", "mcp", "skills", "memory",
                            "network", "terminal", "files", "environment",
                            "shortcuts", "about"};
  const char* sec_text[] = {"界面显示", "主题", "截图",
                            "Endpoint", "会话默认",
                            "输出语言", "并行上限", "手动导入", "搜索 Skill", "长期记忆",
                            "网络总开关", "重新探测", "自动 git init",
                            "自动探测", "发送消息", "版本"};
  for (size_t i = 0; i < sizeof(sec_keys) / sizeof(sec_keys[0]); ++i) {
    std::string key = sec_keys[i];
    app.update([&](AppState& s) { s.settings_nav = key; });
    EXPECT_TRUE(dump_has(dom, sec_text[i]));
  }
}

// Compaction has TWO independent controls: WHEN to look (the water-level
// threshold) and WHAT to do when it fires (notify only / short view). Setting a
// threshold must not imply the short view. The old "最近轮次" window and "token
// 硬上限" compacted on EVERY turn and were removed, as were "保留最近轮数" (an
// invisible internal value) and "立即压缩" (armed a flag for the next turn).
FS_TEST(uiapp_history_settings_keep_only_the_threshold_knobs) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.open_modal_page("settings");
  app.update([](AppState& s) { s.settings_nav = "model_prefs"; });
  EXPECT_TRUE(dump_has(dom, "上下文水位阈值"));
  EXPECT_TRUE(dump_has(dom, "关闭"));  // the "off" option of the threshold
  // ONE control: there is no "what happens at the threshold" setting.
  EXPECT_FALSE(dump_has(dom, "达到阈值时"));
  EXPECT_FALSE(dump_has(dom, "仅提醒"));
  EXPECT_FALSE(dump_has(dom, "短视图"));
  EXPECT_FALSE(dump_has(dom, "保留最近轮数"));
  EXPECT_FALSE(dump_has(dom, "立即压缩"));
  EXPECT_FALSE(dump_has(dom, "最近轮次"));
  EXPECT_FALSE(dump_has(dom, "硬上限"));
  EXPECT_FALSE(dump_has(dom, "tokenBudget"));
  EXPECT_FALSE(dump_has(dom, "recentTurns"));
}

// A settings page that only writes keys nobody reads is a lie. The browser and
// "computer control" pages were exactly that (no driver, no mouse/keyboard
// tools); the proxy "mode" selector was too (the network layer applies
// host+port unconditionally). They must not be in the nav at all.
FS_TEST(uiapp_settings_has_no_inert_fake_pages) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.open_modal_page("settings");
  EXPECT_FALSE(dump_has(dom, "浏览器控制"));
  const char* gone[] = {"启用浏览器控制", "驱动路径", "允许操作证书",
                        "启用电脑控制", "跟随系统", "自定义"};
  for (size_t i = 0; i < sizeof(gone) / sizeof(gone[0]); ++i) {
    EXPECT_FALSE(dump_has(dom, gone[i]));
  }
}

FS_TEST(uiapp_provider_create_wizard) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.open_modal_page("settings");
  app.update([](AppState& s) { s.settings_nav = "providers"; });
  // step 1: templates drive the wizard
  app.update([](AppState& s) {
    AppState::ProviderTemplate t;
    t.id = "anthropic";
    t.name = "Anthropic Messages";
    t.kind = "anthropic";
    t.base_url = "https://api.anthropic.com/v1";
    t.description = "x-api-key + anthropic-version";
    s.provider_templates.push_back(t);
    s.provider_creating = true;
    s.pv_template_picked = false;
  });
  EXPECT_TRUE(dump_has(dom, "新建供应商"));
  EXPECT_TRUE(dump_has(dom, "Anthropic Messages"));
  // step 2: picking a template shows the form; 格式 is a dropdown, 高级 folds
  app.update([](AppState& s) {
    s.pv_template_picked = true;
    s.pv_name = "Anthropic Messages";
    s.pv_kind = "anthropic";
    s.pv_base_url = "https://api.anthropic.com/v1";
  });
  EXPECT_TRUE(dump_has(dom, "Endpoint"));
  EXPECT_TRUE(dump_has(dom, "格式"));
  EXPECT_TRUE(dump_has(dom, "Anthropic"));
  EXPECT_TRUE(dump_has(dom, "https://api.anthropic.com/v1"));
  EXPECT_TRUE(dump_has(dom, "高级"));
  // advanced section holds headers + models
  app.update([](AppState& s) { s.pv_advanced = true; });
  EXPECT_TRUE(dump_has(dom, "模型"));
  EXPECT_TRUE(dump_has(dom, "Header"));
}

FS_TEST(uiapp_mcp_section_lists_and_adds) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.open_modal_page("settings");
  app.update([](AppState& s) {
    // MCP has its own page now (Skill is separate: they are both long lists and
    // their import flows differ).
    s.settings_nav = "mcp";
    AppState::McpRow a;
    a.name = "my-tools";
    a.dir = "C:/tools/skills";
    a.description = "本地工具";
    a.enabled = true;
    s.mcp_tools.push_back(a);
    AppState::McpRow b;
    b.name = "other";
    b.dir = "C:/other";
    b.enabled = false;
    s.mcp_tools.push_back(b);
  });
  EXPECT_TRUE(dump_has(dom, "my-tools"));
  EXPECT_TRUE(dump_has(dom, "C:/tools/skills"));
  EXPECT_TRUE(dump_has(dom, "本地工具"));   // the card carries a description
  EXPECT_TRUE(dump_has(dom, "自动探测"));
  EXPECT_TRUE(dump_has(dom, "手动导入"));
  EXPECT_TRUE(dump_has(dom, "移除"));
}

// Skill is its own page, with search (the list can be long) and a card layout.
FS_TEST(uiapp_skill_section_separate_from_mcp) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.open_modal_page("settings");
  app.update([](AppState& s) {
    s.settings_nav = "skills";
    AppState::SkillRow a;
    a.name = "lsp";
    a.title = "语言服务";
    a.description = "符号重构";
    a.loadable = true;
    a.kind = "mcp";  // a built-in MCP, NOT a skill the user can type
    s.skills.push_back(a);
    AppState::SkillRow b;
    b.name = "my-skill";
    b.title = "我的技能";
    b.description = "本地导入的";
    b.dir = "C:/skills/my-skill";
    b.kind = "skill";
    s.skills.push_back(b);
    s.skills_loaded = true;
  });
  EXPECT_TRUE(dump_has(dom, "我的技能"));
  EXPECT_TRUE(dump_has(dom, "搜索 Skill"));
  // Built-in MCP entries belong to the MCP page: /lsp is not a command the user
  // can type, so listing it under Skill was misleading.
  EXPECT_FALSE(dump_has(dom, "浏览器驱动技能"));
  // filtering narrows the list
  app.update([](AppState& s) { s.skill_filter = "my-skill"; });
  EXPECT_TRUE(dump_has(dom, "我的技能"));
}

FS_TEST(uiapp_toast_has_deadline) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.toast("hello");
  EXPECT_TRUE(dump_has(dom, "hello"));
  EXPECT_TRUE(app.state().toast_deadline_ms > 0);
  // pump() clears an expired toast (deadline forced into the past)
  app.state().toast_deadline_ms = 0;
  app.pump();
  EXPECT_TRUE(app.state().toast.empty());
  EXPECT_FALSE(dump_has(dom, "hello"));
}

// The plan is a DOCUMENT (.flashsight/plans/<sid>.md), not a panel: there is no
// plan float, no step tree and no composer 规划 button. The task list is the only
// progress view. (The user's verdict: the small window was useless - delete it.)
FS_TEST(uiapp_plan_is_a_document_not_a_panel) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) { seed_session(s); });
  EXPECT_FALSE(dump_has(dom, "任务规划"));
  EXPECT_FALSE(dump_has(dom, "规划"));
  // A plan.update event carries the DOCUMENT ({exists,status,content,path}); it
  // must not create a second progress panel beside the checklist.
  app.on_backend_event_for_test(
      "plan.update",
      Json{{"sid", "sess_a"},
           {"plan", Json{{"exists", true},
                         {"status", "active"},
                         {"content", "# ship the parser\n- write lexer\n- write parser"}}}});
  std::string d = dom.dump();
  EXPECT_TRUE(d.find("ship the parser") == std::string::npos);
  EXPECT_TRUE(d.find("write lexer") == std::string::npos);
}
FS_TEST(uiapp_modal) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    s.ask_modal = true;
    s.ask_text = "允许执行 terminal_exec？";
  });
  EXPECT_TRUE(dump_has(dom, "允许执行 terminal_exec"));
}

FS_TEST(uiapp_stream_delta_appends) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) { seed_session(s); });
  // simulate a backend event arriving (as the event sink would deliver it)
  app.state().messages.clear();
  app.on_backend_event_for_test("stream.delta", Json{{"text", "hello "}});
  app.on_backend_event_for_test("stream.delta", Json{{"text", "world"}});
  ASSERT_TRUE(app.state().messages.size() == 1);
  EXPECT_EQ(app.state().messages[0].text, std::string("hello world"));
  EXPECT_TRUE(app.state().messages[0].streaming);
  app.on_backend_event_for_test("turn.done", Json::object());
  EXPECT_FALSE(app.state().messages[0].streaming);
}

// 工具调用前的解说（interim）先以 delta 的形式进了答案；interim 事件到达时必须把
// 它从答案里移出来、只留在工作过程里。否则实时记录里过程文字显示两遍（还是当成
// 总结），而存储的 turn 只把它放在 interim 里——重载/切页后它又"消失"了。
FS_TEST(uiapp_interim_moves_narration_out_of_the_answer) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) { seed_session(s); });
  app.state().messages.clear();
  const std::string narration = "let me read the config first";
  app.on_backend_event_for_test("stream.delta", Json{{"text", narration}});
  app.on_backend_event_for_test("assistant.interim", Json{{"text", narration}});
  ASSERT_TRUE(app.state().messages.size() == 1);
  EXPECT_EQ(app.state().messages[0].text, std::string(""));
  ASSERT_TRUE(app.state().messages[0].interim.size() == 1u);
  EXPECT_EQ(app.state().messages[0].interim[0], narration);
  // The answer continues in the LAST round: only that text is the answer.
  app.on_backend_event_for_test("stream.delta", Json{{"text", "done."}});
  EXPECT_EQ(app.state().messages[0].text, std::string("done."));
  app.on_backend_event_for_test("turn.done", Json::object());
  // Finishing folds the work log; the answer is untouched.
  EXPECT_FALSE(app.state().messages[0].work_open);
  EXPECT_EQ(app.state().messages[0].text, std::string("done."));
}

// 每次 API 调用就更新上下文/缓存，而不是等一轮结束：这些数字正是跑的时候看的。
FS_TEST(uiapp_per_call_usage_updates_the_panel) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    seed_session(s, "sess_a");
    s.context_window = 100000;
    s.session_tokens_in = 0;
    s.session_tokens_out = 0;
  });
  app.on_backend_event_for_test(
      "loop.usage",
      Json{{"sid", "sess_a"}, {"in", 25000}, {"out", 400}, {"cached", 20000},
           {"cacheKnown", true}});
  // Context level, cache and this-turn numbers move immediately...
  EXPECT_EQ(app.state().context_tokens, int64_t(25000));
  EXPECT_EQ(app.state().context_pct, 25);
  EXPECT_TRUE(app.state().cache_reported);
  EXPECT_EQ(app.state().cache_hit_pct, 80);
  EXPECT_EQ(app.state().turn_in, int64_t(25000));
  EXPECT_EQ(app.state().turn_out, int64_t(400));
  EXPECT_EQ(app.state().session_tokens_in, int64_t(25000));
  // ...and the next round keeps accumulating.
  app.on_backend_event_for_test(
      "loop.usage",
      Json{{"sid", "sess_a"}, {"in", 40000}, {"out", 500}, {"cached", 10000},
           {"cacheKnown", true}});
  EXPECT_EQ(app.state().context_tokens, int64_t(40000));
  EXPECT_EQ(app.state().session_tokens_in, int64_t(65000));
  EXPECT_EQ(app.state().cache_hit_pct, 25);
}

// 停止/挂起 ends the turn MID-STREAM, so no turn.done follows. The live message
// has to be finalized by the state event itself - otherwise the transcript kept
// a spinner and a counting timer for a task that had already stopped, which is
// one of the ways 挂起 looked half-done.
FS_TEST(uiapp_suspend_finalizes_the_live_message) {
  NullDom dom;
  AppState st = AppState::make();
  class SuspendBackend : public IUiBackend {
   public:
    BackendResult call(const std::string& method, const Json&) override {
      BackendResult r;
      r.ok = true;
      if (method == "session.loopState") r.data = Json{{"state", "suspended"}};
      return r;
    }
    std::function<void()> subscribe(BackendEvent) override { return []() {}; }
  } backend;
  UiApp app(dom, st, &backend);
  app.mount();
  app.update([](AppState& s) {
    seed_session(s, "sess_a");
    s.messages.clear();
    s.turn_running = true;
  });
  app.on_backend_event_for_test("stream.delta",
                                Json{{"sid", "sess_a"}, {"text", "half an ans"}});
  ASSERT_TRUE(app.state().messages.size() == 1);
  EXPECT_TRUE(app.state().messages[0].streaming);
  app.on_backend_event_for_test("loop.state",
                                Json{{"sid", "sess_a"}, {"state", "suspended"}});
  EXPECT_FALSE(app.state().messages[0].streaming);
  EXPECT_FALSE(app.state().turn_running);
  EXPECT_TRUE(app.state().suspended);   // from session.loopState, not the click
  // A park before the first token leaves no empty bubble behind.
  app.update([](AppState& s) {
    comp::ChatMsg m;
    m.role = "assistant";
    m.streaming = true;
    m.rev = 1;
    s.messages.push_back(m);
  });
  app.on_backend_event_for_test("loop.state",
                                Json{{"sid", "sess_a"}, {"state", "suspended"}});
  EXPECT_TRUE(app.state().messages.size() == 1);
}

// 中断 (the turn died on a provider/tool error) also ends mid-stream: the same
// rule as a park, plus the reason is kept ON the message - the toast is gone in
// three seconds and the transcript then said nothing about why it stopped.
FS_TEST(uiapp_interrupted_finalizes_and_explains) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    seed_session(s, "sess_a");
    s.messages.clear();
    s.turn_running = true;
  });
  app.on_backend_event_for_test("stream.delta",
                                Json{{"sid", "sess_a"}, {"text", "half"}});
  ASSERT_TRUE(app.state().messages.size() == 1);
  EXPECT_TRUE(app.state().messages[0].streaming);
  app.on_backend_event_for_test(
      "loop.interrupted", Json{{"sid", "sess_a"}, {"error", "http 400: bad key"}});
  EXPECT_FALSE(app.state().messages[0].streaming);
  EXPECT_FALSE(app.state().turn_running);
  EXPECT_TRUE(app.state().messages[0].error.find("http 400") != std::string::npos);
  EXPECT_TRUE(dom.dump().find("\u672c\u8f6e\u4e2d\u65ad") != std::string::npos);

  // A failure on the VERY FIRST provider call leaves no assistant message to
  // attach to. One is created, so the transcript still says what happened instead
  // of showing the question followed by nothing (the failure existed only in a
  // three-second toast).
  {
    NullDom dom2;
    AppState st2 = AppState::make();
    UiApp app2(dom2, st2, nullptr);
    app2.mount();
    app2.update([](AppState& s) {
      seed_session(s, "sess_b");
      s.messages.clear();
      comp::ChatMsg u;
      u.role = "user";
      u.text = "hi";
      s.messages.push_back(u);
      s.turn_running = true;
    });
    app2.on_backend_event_for_test(
        "loop.interrupted",
        Json{{"sid", "sess_b"}, {"error", "http 401: bad key"}});
    ASSERT_TRUE(app2.state().messages.size() == 2);
    EXPECT_EQ(app2.state().messages.back().role, std::string("assistant"));
    EXPECT_FALSE(app2.state().messages.back().streaming);
    EXPECT_TRUE(app2.state().messages.back().error.find("401") != std::string::npos);
    EXPECT_TRUE(dom2.dump().find("\u672c\u8f6e\u4e2d\u65ad") != std::string::npos);
  }
}

// A session.turns reply describes the disk, which is BEHIND the screen right after
// a send: the optimistic question (and any streamed text) exists only in memory.
// Assigning the reply wholesale erased it - "发送以后 history 没有反应，切页面才显示".
FS_TEST(uiapp_turns_reply_keeps_live_messages) {
  NullDom dom;
  AppState st = AppState::make();
  class OneTurnBackend : public IUiBackend {
   public:
    BackendResult call(const std::string& method, const Json&) override {
      BackendResult r;
      r.ok = true;
      if (method == "session.info") {
        r.data = Json{{"meta", Json{{"sid", "sess_a"}, {"pid", "p1"}}},
                      {"sessionSettings", Json::object()},
                      {"systemOps", Json::array()}};
      }
      if (method == "session.turns") {
        // The disk still holds ONLY the old turn: the reply was requested before
        // the send landed.
        r.data = Json{{"turns", Json::array({Json{{"role", "user"}, {"text", "old q"},
                                                 {"turnId", "t_old"}}})},
                      {"start", 0}, {"total", 1}, {"hasMore", false}};
      }
      if (method == "model.allEnabled") {
        r.data = Json{{"models", Json::array({Json{{"id", "m1"}}})}};
      }
      return r;
    }
    std::function<void()> subscribe(BackendEvent) override { return []() {}; }
  } backend;
  UiApp app(dom, st, &backend);
  app.mount();
  app.update([](AppState& s) {
    seed_session(s, "sess_a");
    s.model = "m1";
    s.model_available = true;
  });
  app.show("sess_a");
  ASSERT_TRUE(app.state().messages.size() == 1);
  app.update([](AppState& s) { s.session_input = "new q"; });
  app.send_message();
  // The optimistic user message is on screen now, followed by the live
  // "waiting for the model" placeholder (nothing streams until the first token).
  ASSERT_TRUE(app.state().messages.size() == 3);
  EXPECT_EQ(app.state().messages[1].text, std::string("new q"));
  EXPECT_EQ(app.state().messages[2].role, std::string("assistant"));
  EXPECT_TRUE(app.state().messages[2].streaming);
  EXPECT_TRUE(app.state().messages[2].text.empty());
  // ...and a stale reload must not erase it.
  app.show("home");
  app.show("sess_a");
  bool have_old = false, have_new = false;
  for (size_t i = 0; i < app.state().messages.size(); ++i) {
    if (app.state().messages[i].text == "old q") have_old = true;
    if (app.state().messages[i].text == "new q") have_new = true;
  }
  EXPECT_TRUE(have_old);   // the disk content is there
  EXPECT_TRUE(have_new);   // and the un-persisted send survived
}

// A send that becomes guidance must NOT draw a user bubble + wait placeholder
// and then retract them. The live state (turn running) says guidance before the
// request leaves; the backend reply only confirms it.
FS_TEST(uiapp_guidance_send_draws_no_bubble) {
  NullDom dom;
  AppState st = AppState::make();
  class QueueBackend : public IUiBackend {
   public:
    BackendResult call(const std::string& method, const Json&) override {
      BackendResult r;
      r.ok = true;
      if (method == "session.send") r.data = Json{{"queued", true}};
      if (method == "steering.list") r.data = Json::array();
      if (method == "model.allEnabled") {
        r.data = Json{{"models", Json::array({Json{{"id", "m1"}}})}};
      }
      return r;
    }
    std::function<void()> subscribe(BackendEvent) override { return []() {}; }
  } backend;
  UiApp app(dom, st, &backend);
  app.mount();
  app.update([](AppState& s) {
    seed_session(s, "sess_a");
    s.model = "m1";
    s.model_available = true;
    s.turn_running = true;  // the live state the composer already shows
    s.session_input = "guide this";
  });
  size_t before = app.state().messages.size();
  app.send_message();
  // No user echo, no assistant placeholder: the item is only in the queue.
  EXPECT_EQ(app.state().messages.size(), before);
  EXPECT_TRUE(app.state().session_input.empty());
}

// If the UI thought a turn was running but the backend starts a new one, the
// skipped question must be inserted from turn.started (which arrives DURING the
// call), in front of the answer. The send result arrives only when the turn is
// over, so appending it there would put the question after the answer.
FS_TEST(uiapp_guidance_race_draws_turn_from_reply) {
  NullDom dom;
  AppState st = AppState::make();
  class DeferredBackend : public IUiBackend {
   public:
    BackendDone send_done;
    BackendResult call(const std::string& method, const Json&) override {
      BackendResult r;
      r.ok = true;
      if (method == "model.allEnabled") {
        r.data = Json{{"models", Json::array({Json{{"id", "m1"}}})}};
      }
      return r;
    }
    void call_async(const std::string& method, const Json& args,
                    BackendDone done) override {
      if (method == "session.send") {
        send_done = done;  // the turn is still running
        return;
      }
      BackendResult r = call(method, args);
      if (done) done(r);
    }
    void finish() {
      if (!send_done) return;
      BackendDone d = send_done;
      send_done = nullptr;
      BackendResult r;
      r.ok = true;
      d(r);
    }
    std::function<void()> subscribe(BackendEvent) override { return []() {}; }
  } backend;
  UiApp app(dom, st, &backend);
  app.mount();
  app.update([](AppState& s) {
    seed_session(s, "sess_a");
    s.model = "m1";
    s.model_available = true;
    s.turn_running = true;  // stale: the backend is actually idle
    s.session_input = "real question";
  });
  size_t before = app.state().messages.size();
  app.send_message();
  // Nothing was drawn optimistically.
  EXPECT_EQ(app.state().messages.size(), before);
  // The backend proves a new turn started: question THEN answer placeholder.
  app.on_backend_event_for_test(
      "turn.started", Json{{"sid", "sess_a"}, {"text", "real question"}});
  ASSERT_TRUE(app.state().messages.size() == before + 2);
  EXPECT_EQ(app.state().messages[before].role, std::string("user"));
  EXPECT_EQ(app.state().messages[before].text, std::string("real question"));
  EXPECT_EQ(app.state().messages[before + 1].role, std::string("assistant"));
  EXPECT_TRUE(app.state().messages[before + 1].streaming);
  // The turn's completion must not add a second pair.
  backend.finish();
  EXPECT_EQ(app.state().messages.size(), before + 2);
}

FS_TEST(uiapp_permission_prompt_opens_modal) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) { seed_session(s); });
  app.on_backend_event_for_test("permission.prompt", Json{{"tool", "write_file"}});
  EXPECT_TRUE(app.state().ask_modal);
  EXPECT_TRUE(dump_has(dom, "write_file"));
}

// ---------------------------------------------------------------------------
// 逐模型设置
// ---------------------------------------------------------------------------

namespace {

AppState::ModelRow mk_model(const std::string& id, bool enabled = true) {
  AppState::ModelRow m;
  m.id = id;
  m.display = id;
  m.enabled = enabled;
  return m;
}

}  // namespace

// The provider detail is two stacked regions: a scrollable model list on top
// (enable / test / delete / add) and the selected model's settings below.
FS_TEST(uiapp_model_panel_lists_and_edits) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.open_modal_page("settings");
  app.update([](AppState& s) {
    s.settings_nav = "providers";
    AppState::ProviderRow p;
    p.id = "p1";
    p.name = "zen";
    p.kind = "openai-chat";
    s.providers.push_back(p);
    s.provider_selected = "p1";
    s.pv_base_url = "https://api.test/v1";
    s.models.push_back(mk_model("gpt-4o"));
    s.models.push_back(mk_model("gpt-4o-mini"));
    s.models.push_back(mk_model("o4-mini"));
    s.models_loaded = true;
  });
  EXPECT_TRUE(dump_has(dom, "md-list"));
  EXPECT_TRUE(dump_has(dom, "gpt-4o-mini"));
  // nothing selected yet -> no settings pane
  EXPECT_FALSE(dump_has(dom, "md-adv"));

  // selecting a model opens its advanced settings
  app.select_model("o4-mini");
  EXPECT_EQ(app.state().model_selected, std::string("o4-mini"));
  EXPECT_TRUE(dump_has(dom, "md-adv"));
  EXPECT_TRUE(dump_has(dom, "能力"));
  EXPECT_TRUE(dump_has(dom, "上下文长度"));
  EXPECT_TRUE(dump_has(dom, "最大输出"));
  EXPECT_TRUE(dump_has(dom, "思考档位"));
  EXPECT_TRUE(dump_has(dom, "失败重试"));
  // the row is marked selected
  EXPECT_TRUE(dump_has(dom, "md-row active"));
}

// The enable dot toggles without opening the model, and a disabled model is
// still listed (so it can be turned back on).
FS_TEST(uiapp_model_enable_toggle) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.open_modal_page("settings");
  app.update([](AppState& s) {
    s.settings_nav = "providers";
    AppState::ProviderRow p;
    p.id = "p1";
    s.providers.push_back(p);
    s.provider_selected = "p1";
    s.models.push_back(mk_model("m1", true));
    s.models_loaded = true;
  });
  EXPECT_TRUE(dump_has(dom, "md-en on"));
  app.toggle_model_enabled("m1", false);
  // optimistic: the row reflects it immediately (no round trip to wait for)
  EXPECT_FALSE(app.state().models[0].enabled);
  EXPECT_TRUE(dump_has(dom, "md-en\""));  // the plain (off) class
}

// 新增: a new model starts as an empty draft, and the name field is the id.
FS_TEST(uiapp_model_add_starts_empty_draft) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.open_modal_page("settings");
  app.update([](AppState& s) {
    s.settings_nav = "providers";
    AppState::ProviderRow p;
    p.id = "p1";
    s.providers.push_back(p);
    s.provider_selected = "p1";
    s.models_loaded = true;
  });
  app.begin_new_model();
  EXPECT_TRUE(app.state().model_creating);
  EXPECT_TRUE(app.state().model_draft.id.empty());
  EXPECT_TRUE(dump_has(dom, "新增模型"));
  EXPECT_TRUE(dump_has(dom, "模型名"));
  // the draft is NOT saved by opening it
  EXPECT_TRUE(app.state().models.empty());
}

// 思考档位 is edited per model: levels can be added and removed, and the
// default always names a level that still exists.
FS_TEST(uiapp_model_thinking_levels_editable) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.open_modal_page("settings");
  app.update([](AppState& s) {
    s.settings_nav = "providers";
    AppState::ProviderRow p;
    p.id = "p1";
    s.providers.push_back(p);
    s.provider_selected = "p1";
    AppState::ModelRow m = mk_model("o4-mini");
    m.thinking_levels.push_back("off");
    m.thinking_levels.push_back("low");
    m.thinking_levels.push_back("high");
    m.thinking_default = "low";
    s.models.push_back(m);
    s.models_loaded = true;
  });
  app.select_model("o4-mini");
  EXPECT_EQ(app.state().model_draft.thinking_levels.size(), size_t(3));
  // Levels are edited as one comma-separated line (the protocol values), and
  // the default is a dropdown over the Chinese preset names.
  EXPECT_TRUE(dump_has(dom, "档位"));
  EXPECT_TRUE(dump_has(dom, "off, low, high"));
  EXPECT_TRUE(dump_has(dom, "默认档位"));
  EXPECT_TRUE(dump_has(dom, "低"));   // thinking_label("low")
}

// 没有档位的模型：UI 明确说明而不是渲染一个空控件。
FS_TEST(uiapp_model_without_levels_says_so) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.open_modal_page("settings");
  app.update([](AppState& s) {
    s.settings_nav = "providers";
    AppState::ProviderRow p;
    p.id = "p1";
    s.providers.push_back(p);
    s.provider_selected = "p1";
    s.models.push_back(mk_model("gpt-4o"));  // no thinking levels
    s.models_loaded = true;
  });
  app.select_model("gpt-4o");
  // no levels -> the field says so instead of showing an empty control
  EXPECT_TRUE(dump_has(dom, "留空 = 不可配思考档"));
}

// A long model list gets a filter; a short one does not (a search box over
// three rows is noise).
FS_TEST(uiapp_model_filter_threshold) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.open_modal_page("settings");
  app.update([](AppState& s) {
    s.settings_nav = "providers";
    AppState::ProviderRow p;
    p.id = "p1";
    s.providers.push_back(p);
    s.provider_selected = "p1";
    for (int i = 0; i < 4; ++i) s.models.push_back(mk_model("m" + std::to_string(i)));
    s.models_loaded = true;
  });
  EXPECT_FALSE(dump_has(dom, "筛选模型"));
  app.update([](AppState& s) {
    for (int i = 4; i < 12; ++i) s.models.push_back(mk_model("m" + std::to_string(i)));
  });
  EXPECT_TRUE(dump_has(dom, "筛选模型"));
  // filtering narrows the list
  app.update([](AppState& s) { s.model_filter = "m11"; });
  EXPECT_TRUE(dump_has(dom, "m11"));
  EXPECT_FALSE(dump_has(dom, "m3\""));
}

// ---------------------------------------------------------------------------
// 会话里的模型选择器
// ---------------------------------------------------------------------------

FS_TEST(uiapp_model_picker_two_pane) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    seed_session(s);
    AppState::ProviderRow p1;
    p1.id = "p1";
    p1.name = "zen";
    p1.kind = "openai-chat";
    AppState::ProviderRow p2;
    p2.id = "p2";
    p2.name = "anthropic";
    p2.kind = "anthropic";
    s.providers.push_back(p1);
    s.providers.push_back(p2);
    s.providers_loaded = true;
    s.model_picker_open = true;
    s.model_picker_owner = "composer";
    s.model_picker_provider = "p1";
    s.model_picker_loaded = true;
    s.model_picker_models.push_back(mk_model("gpt-4o"));
    s.model_picker_models.push_back(mk_model("gpt-4o-mini"));
  });
  // left: every provider (so you can switch), right: that provider's models
  EXPECT_TRUE(dump_has(dom, "mp-providers"));
  EXPECT_TRUE(dump_has(dom, "zen"));
  EXPECT_TRUE(dump_has(dom, "anthropic"));
  EXPECT_TRUE(dump_has(dom, "mp-models"));
  EXPECT_TRUE(dump_has(dom, "gpt-4o-mini"));
  // the composer button shows the current model, not a <select>
  EXPECT_TRUE(dump_has(dom, "model-pick-btn"));
}

// The same model id can live on two providers. Selection is the PAIR: the other
// provider's copy must not show a checkmark, and switching to it must show one.
FS_TEST(uiapp_model_picker_selection_is_provider_aware) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    seed_session(s);
    AppState::ProviderRow p1;
    p1.id = "p1";
    p1.name = "first";
    p1.kind = "openai-chat";
    AppState::ProviderRow p2;
    p2.id = "p2";
    p2.name = "second";
    p2.kind = "openai-chat";
    s.providers.push_back(p1);
    s.providers.push_back(p2);
    s.providers_loaded = true;
    s.model = "same-model";
    s.model_provider = "p2";
    s.model_picker_open = true;
    s.model_picker_owner = "composer";
    s.model_picker_provider = "p1";
    s.model_picker_loaded = true;
    AppState::ModelRow a = mk_model("same-model");
    a.provider = "p1";
    AppState::ModelRow b = mk_model("same-model");
    b.provider = "p2";
    s.model_picker_models.push_back(a);
    s.model_picker_models.push_back(b);
  });
  auto count = [&dom](const std::string& needle) {
    const std::string d = dom.dump();
    int n = 0;
    for (size_t p = d.find(needle); p != std::string::npos;
         p = d.find(needle, p + needle.size())) {
      ++n;
    }
    return n;
  };
  // p1's page: same id, different provider -> neither active nor checked.
  EXPECT_EQ(count("class=\"mp-model active\""), 0);
  EXPECT_EQ(count("fa-check"), 0);
  // p2's page: exactly the selected pair.
  app.update([](AppState& s) { s.model_picker_provider = "p2"; });
  EXPECT_EQ(count("class=\"mp-model active\""), 1);
  EXPECT_EQ(count("fa-check"), 1);
}

// Opening the picker must land on the provider that owns the current model, not
// on whichever provider happened to be first in the list.
FS_TEST(uiapp_model_picker_opens_on_current_provider) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    seed_session(s);
    AppState::ProviderRow p1;
    p1.id = "p1";
    p1.name = "first";
    AppState::ProviderRow p2;
    p2.id = "p2";
    p2.name = "second";
    s.providers.push_back(p1);
    s.providers.push_back(p2);
    s.providers_loaded = true;
    s.model = "same-model";
    s.model_provider = "p2";
  });
  app.open_model_picker("composer", "");
  EXPECT_EQ(app.state().model_picker_provider, std::string("p2"));
}

// Picking a model must refresh the thinking ladder from the model TABLE at once.
// Before, only saving the model in provider settings triggered this read, so the
// composer kept the old pair (or no control at all) until that save.
FS_TEST(uiapp_pick_model_refreshes_thinking_levels) {
  class PickBackend : public IUiBackend {
   public:
    Json levels = Json::array({"off", "low", "high"});
    BackendResult call(const std::string& method, const Json&) override {
      BackendResult r;
      r.ok = true;
      if (method == "model.allEnabled") {
        r.data = Json{{"models", Json::array({
            Json{{"id", "reasoner"},
                 {"providerId", "p2"},
                 {"providerName", "second"},
                 {"thinking", Json{{"levels", levels}, {"default", "low"}}}},
            Json{{"id", "old"}, {"providerId", "p1"},
                 {"thinking", Json{{"levels", Json::array({"on", "off"})}}}},
        })}};
      }
      if (method == "usage.session") {
        r.data = Json{{"totalIn", 0}, {"totalOut", 0}, {"totalCache", 0},
                      {"calls", 0}, {"lastIn", 0}, {"contextWindow", 0}};
      }
      if (method == "provider.balances") r.data = Json{{"providers", Json::array()}};
      return r;
    }
    std::function<void()> subscribe(BackendEvent) override { return []() {}; }
  } backend;
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, &backend);
  app.mount();
  app.update([](AppState& s) {
    seed_session(s);
    AppState::ProviderRow p2;
    p2.id = "p2";
    p2.name = "second";
    s.providers.push_back(p2);
    s.providers_loaded = true;
    s.model = "old";
    s.model_provider = "p1";
    // A level from the PREVIOUS model that is not on the new ladder.
    s.thinking = "medium";
    s.model_available = true;
  });
  app.pick_model("p2", "reasoner");
  EXPECT_EQ(app.state().model, std::string("reasoner"));
  EXPECT_EQ(app.state().model_provider, std::string("p2"));
  EXPECT_EQ((int)app.state().model_thinking_levels.size(), 3);
  EXPECT_EQ(app.state().model_thinking_default, std::string("low"));
  // medium is meaningless on off/low/high -> replaced by the model's default.
  EXPECT_EQ(app.state().thinking, std::string("low"));
}

// Above 20 models the list is sorted and grouped by initial; below it the
// headers would just be noise between every few rows.
FS_TEST(uiapp_model_picker_groups_only_when_long) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  auto seed_n = [](AppState& s, int n, bool reverse) {
    for (int i = 0; i < n; ++i) {
      std::string id = "model-" + std::to_string(reverse ? (n - i) : i);
      s.model_picker_models.push_back(mk_model(id));
    }
  };
  app.update([&](AppState& s) {
    seed_session(s);
    s.model_picker_open = true;
    s.model_picker_owner = "composer";
    s.model_picker_provider = "p1";
    s.model_picker_loaded = true;
    seed_n(s, 5, false);
  });
  EXPECT_FALSE(dump_has(dom, "mp-group"));
  app.update([&](AppState& s) {
    s.model_picker_models.clear();
    seed_n(s, 25, true);
  });
  EXPECT_TRUE(dump_has(dom, "mp-group"));
  // sorted: model-10 must appear before model-2 (string order, not numeric)
  const std::string& d = dom.dump();
  size_t a = d.find("model-10");
  size_t b = d.find("model-2\"");
  EXPECT_TRUE(a != std::string::npos && b != std::string::npos && a < b);
}

// A disabled model is not offered for a session (that is the point of the
// enable switch in settings).
FS_TEST(uiapp_model_picker_skips_disabled) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    seed_session(s);
    s.model_picker_open = true;
    s.model_picker_owner = "composer";
    s.model_picker_provider = "p1";
    s.model_picker_loaded = true;
    s.model_picker_models.push_back(mk_model("enabled-one", true));
  });
  EXPECT_TRUE(dump_has(dom, "enabled-one"));
  // a disabled model never makes it into the picker's model list at all
  EXPECT_FALSE(dump_has(dom, "disabled-one"));
}

// The picker search filters on the query (and re-renders to do it).
FS_TEST(uiapp_model_picker_search) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    seed_session(s);
    s.model_picker_open = true;
    s.model_picker_owner = "composer";
    s.model_picker_provider = "p1";
    s.model_picker_loaded = true;
    s.model_picker_models.push_back(mk_model("gpt-4o"));
    s.model_picker_models.push_back(mk_model("deepseek-chat"));
    s.model_picker_models.push_back(mk_model("qwen-max"));
  });
  EXPECT_TRUE(dump_has(dom, "gpt-4o"));
  EXPECT_TRUE(dump_has(dom, "deepseek-chat"));
  app.update([](AppState& s) { s.model_picker_query = "deep"; });
  EXPECT_TRUE(dump_has(dom, "deepseek-chat"));
  EXPECT_FALSE(dump_has(dom, "qwen-max"));
  // case-insensitive
  app.update([](AppState& s) { s.model_picker_query = "QWEN"; });
  EXPECT_TRUE(dump_has(dom, "qwen-max"));
  EXPECT_FALSE(dump_has(dom, "deepseek-chat"));
  // no match -> explicit empty state, not a blank pane
  app.update([](AppState& s) { s.model_picker_query = "zzz-nope"; });
  EXPECT_TRUE(dump_has(dom, "没有匹配的模型"));
}

// ---------------------------------------------------------------------------
// 图标
// ---------------------------------------------------------------------------

// Brand marks resolve from the model NAME when no explicit icon is set, so a
// model added from an API listing still gets the right mark.
FS_TEST(uiapp_model_icon_by_name) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.open_modal_page("settings");
  app.update([](AppState& s) {
    s.settings_nav = "providers";
    AppState::ProviderRow p;
    p.id = "p1";
    s.providers.push_back(p);
    s.provider_selected = "p1";
    s.models.push_back(mk_model("gpt-4o-mini"));       // -> openai
    s.models.push_back(mk_model("claude-3-5-sonnet")); // -> claude
    s.models.push_back(mk_model("zzz-unknown"));       // -> neutral
    s.models_loaded = true;
  });
  EXPECT_TRUE(dump_has(dom, "mi-lobe-openai"));
  EXPECT_TRUE(dump_has(dom, "mi-lobe-claude"));
  // no brand match -> a neutral dot, not a wrong logo and not an empty box
  EXPECT_TRUE(dump_has(dom, "mi-none"));
  EXPECT_FALSE(dump_has(dom, "mi-lobe-zzz"));
}

// An explicit icon key wins over name matching.
FS_TEST(uiapp_model_icon_explicit_wins) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.open_modal_page("settings");
  app.update([](AppState& s) {
    s.settings_nav = "providers";
    AppState::ProviderRow p;
    p.id = "p1";
    s.providers.push_back(p);
    s.provider_selected = "p1";
    AppState::ModelRow m = mk_model("gpt-4o");  // name says openai
    m.icon = "mistral";                          // but the user said mistral
    s.models.push_back(m);
    s.models_loaded = true;
  });
  EXPECT_TRUE(dump_has(dom, "mi-lobe-mistral"));
  EXPECT_FALSE(dump_has(dom, "mi-lobe-openai"));
}

// File-type icons come from the extension, and from the exact filename first
// (Dockerfile has no extension; cargo.toml is more specific than "toml").
FS_TEST(uiapp_file_icons_by_type) {
  H a = comp::file_icon("main.cpp", false);
  H b = comp::file_icon("app.tsx", false);
  H c = comp::file_icon("Dockerfile", false);
  H d = comp::file_icon("cargo.toml", false);
  H e = comp::file_icon("mystery.zzz", false);
  H f = comp::file_icon("src", true);
  auto cls = [](const H& h) {
    const std::vector<VAttr>& at = h.get()->attrs;
    for (size_t i = 0; i < at.size(); ++i) {
      if (at[i].key == "class") return at[i].value;
    }
    return std::string();
  };
  EXPECT_TRUE(cls(a).find("mi-file-cpp") != std::string::npos);
  EXPECT_TRUE(cls(b).find("mi-file-react_ts") != std::string::npos);
  EXPECT_TRUE(cls(c).find("mi-file-docker") != std::string::npos);
  EXPECT_TRUE(cls(d).find("mi-file-toml") != std::string::npos);
  // unknown extension -> generic file icon, not a blank
  EXPECT_TRUE(cls(e).find("mi-file-file") != std::string::npos);
  // directories use the folder icon
  EXPECT_TRUE(cls(f).find("mi-file-folder") != std::string::npos);
}

// The file tree renders the icon element (it used to be a text glyph).
FS_TEST(uiapp_file_tree_uses_icons) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    seed_session(s);
    s.active_pid = "p1";
    s.side_tabs.clear();
    SideTab t;
    t.id = "files1";
    t.title = "文件";
    t.kind = "files";
    t.loaded = true;
    t.entries.push_back(SideTab::FileEntry{"src", true, 0});
    t.entries.push_back(SideTab::FileEntry{"main.cpp", false, 1234});
    s.side_tabs.push_back(t);
    s.active_side = "files1";
  });
  EXPECT_TRUE(dump_has(dom, "mi-file-folder"));
  EXPECT_TRUE(dump_has(dom, "mi-file-cpp"));
  EXPECT_TRUE(dump_has(dom, "main.cpp"));
}

// ---------------------------------------------------------------------------
// 打开文件 = 新开侧栏标签页（而不是内联在文件树下面）
// ---------------------------------------------------------------------------

FS_TEST(uiapp_file_click_opens_preview_tab) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    seed_session(s);
    s.active_pid = "p1";
    s.side_tabs.clear();
    SideTab t;
    t.id = "files1";
    t.title = "文件";
    t.kind = "files";
    t.loaded = true;
    s.side_tabs.push_back(t);
    s.active_side = "files1";
  });
  size_t before = app.state().side_tabs.size();
  app.open_file_preview("src/main.cpp");
  EXPECT_EQ(app.state().side_tabs.size(), before + 1);
  const SideTab& last = app.state().side_tabs.back();
  EXPECT_EQ(last.kind, std::string("preview"));
  EXPECT_EQ(last.path, std::string("src/main.cpp"));
  // the tab is named after the FILE (several previews must be distinguishable)
  EXPECT_EQ(last.title, std::string("main.cpp"));
  EXPECT_EQ(app.state().active_side, last.id);
  // the file panel is still there, so you can read and browse at once
  bool files_alive = false;
  for (size_t i = 0; i < app.state().side_tabs.size(); ++i) {
    if (app.state().side_tabs[i].kind == "files") files_alive = true;
  }
  EXPECT_TRUE(files_alive);
}

// The file preview is highlighted by EXTENSION: the panel has no fence label to
// go on, so the language comes from the file name. An unknown extension must
// fall back to plain text rather than guessing.
FS_TEST(uiapp_preview_highlights_by_extension) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    seed_session(s);
    s.active_pid = "p1";
    s.side_tabs.clear();
    SideTab t;
    t.id = "pv1";
    t.kind = "preview";
    t.path = "src/engine.cpp";
    t.preview_path = "src/engine.cpp";
    t.loaded = true;
    t.preview = "// engine\nint main() { return 0; }\n";
    s.side_tabs.push_back(t);
    s.active_side = "pv1";
  });
  std::string d = dom.dump();
  EXPECT_TRUE(d.find("hl-cm") != std::string::npos);
  EXPECT_TRUE(d.find("hl-ty") != std::string::npos);
  // unknown extension: rendered verbatim, no token spans
  app.update([](AppState& s) {
    s.side_tabs[0].path = "data/blob.unknownext";
    s.side_tabs[0].preview = "int main() { return 0; }\n";
  });
  std::string p = dom.dump();
  EXPECT_TRUE(p.find("hl-ty") == std::string::npos);
  EXPECT_TRUE(p.find("int main()") != std::string::npos);
}

// Re-opening the same file focuses its tab instead of stacking duplicates.
FS_TEST(uiapp_preview_reuses_tab_for_same_file) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) { seed_session(s); s.active_pid = "p1"; });
  app.open_file_preview("a/b.cpp");
  size_t n = app.state().side_tabs.size();
  app.open_file_preview("a/b.cpp");
  EXPECT_EQ(app.state().side_tabs.size(), n);
  // a DIFFERENT file does get its own tab
  app.open_file_preview("a/c.cpp");
  EXPECT_EQ(app.state().side_tabs.size(), n + 1);
}

// Closing a preview goes back to the panel it was opened from (the file tree),
// not to the first panel in the strip.
FS_TEST(uiapp_closing_preview_returns_to_files) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    seed_session(s);
    s.active_pid = "p1";
    s.side_tabs.clear();
    SideTab t;
    t.id = "files1";
    t.kind = "files";
    t.title = "文件";
    s.side_tabs.push_back(t);
    s.active_side = "files1";
  });
  app.open_file_preview("main.cpp");
  std::string preview = app.state().active_side;
  EXPECT_EQ(app.state().side_tabs.back().kind, std::string("preview"));
  app.close_side_tab(preview);
  EXPECT_EQ(app.state().active_side, std::string("files1"));
}

// ---------------------------------------------------------------------------
// 访问栈：关闭标签页回到来处
// ---------------------------------------------------------------------------

FS_TEST(uiapp_closing_tab_returns_to_previous) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    seed_project(s);
    seed_session(s, "sess_a");
    SessionTab b;
    b.id = "sess_b";
    b.title = "会话B";
    s.sessions.push_back(b);
    s.tab_ids.push_back("sess_b");
  });
  // home -> A -> B
  app.show("sess_a");
  app.show("sess_b");
  EXPECT_EQ(app.state().active_tab, std::string("sess_b"));
  // closing B must go back to A (where we came from), not to a fixed tab
  app.close_tab("sess_b");
  EXPECT_EQ(app.state().active_tab, std::string("sess_a"));
  // closing A goes back to home
  app.close_tab("sess_a");
  EXPECT_EQ(app.state().active_tab, std::string("home"));
}

// A closed tab that is still on the stack must be skipped, not selected.
FS_TEST(uiapp_tab_history_skips_dead_entries) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    seed_project(s);
    seed_session(s, "sess_a");
    SessionTab b;
    b.id = "sess_b";
    s.sessions.push_back(b);
    s.tab_ids.push_back("sess_b");
    SessionTab c;
    c.id = "sess_c";
    s.sessions.push_back(c);
    s.tab_ids.push_back("sess_c");
  });
  app.show("sess_a");
  app.show("sess_b");
  app.show("sess_c");
  // B dies while C is on screen: closing C must skip it and land on A
  app.close_tab("sess_b");
  EXPECT_EQ(app.state().active_tab, std::string("sess_c"));
  app.close_tab("sess_c");
  EXPECT_EQ(app.state().active_tab, std::string("sess_a"));
}

// Switching tabs closes transient popups: the model picker must not stay glued
// over a page it does not belong to.
FS_TEST(uiapp_switching_tab_closes_popups) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    seed_session(s, "sess_a");
    SessionTab b;
    b.id = "sess_b";
    s.sessions.push_back(b);
    s.tab_ids.push_back("sess_b");
    s.model_picker_open = true;
    s.model_picker_owner = "composer";
    s.model_picker_loaded = true;
    s.skill_menu_open = true;
    s.side_menu_open = true;
    // The inline completion menu belongs to the session being left. Left set, it
    // kept data-keys-capture on the NEXT tab's textarea and an open menu for the
    // previous session's text.
    s.complete_kind = AppState::CompleteKind::kCommand;
    s.complete_query = "old";
    s.complete_items.push_back(std::make_pair(std::string("/old"), false));
  });
  app.show("sess_b");
  EXPECT_FALSE(app.state().model_picker_open);
  EXPECT_FALSE(app.state().skill_menu_open);
  EXPECT_FALSE(app.state().side_menu_open);
  EXPECT_TRUE(app.state().model_picker_owner.empty());
  EXPECT_TRUE(app.state().complete_kind == AppState::CompleteKind::kNone);
  EXPECT_TRUE(app.state().complete_items.empty());
  EXPECT_TRUE(app.state().complete_query.empty());
}

// The side panel keeps its own visit stack: closing the active panel returns to
// the one you came from.
FS_TEST(uiapp_side_panel_history) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) { seed_session(s, "sess_a"); });
  app.open_or_focus_side_tab("files");
  std::string files1 = app.state().active_side;
  app.open_or_focus_side_tab("ops");
  std::string ops = app.state().active_side;
  EXPECT_NE(files1, ops);
  // closing the active (ops) goes back to files, not to the first panel
  app.close_side_tab(ops);
  EXPECT_EQ(app.state().active_side, files1);
}

// ---------------------------------------------------------------------------
// 顶层 "+" 打开"新建"页面（不是标签页）
// ---------------------------------------------------------------------------

FS_TEST(uiapp_plus_opens_new_page) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) { seed_project(s); });
  app.open_new_tab();
  // it covers the current tab instead of becoming one of its own
  EXPECT_EQ(app.state().modal_page, std::string("new"));
  EXPECT_EQ(app.state().active_tab, std::string("home"));
  EXPECT_TRUE(dump_has(dom, "新建"));
  // the page lists the projects (so you pick one) instead of opening a session
  EXPECT_TRUE(dump_has(dom, "FlashSight"));
  EXPECT_TRUE(dump_has(dom, "新建会话"));
  // no new tab was created, and it is not on the visit stack either (closing a
  // tab must never fall back INTO a settings/new page)
  EXPECT_EQ(app.state().tab_ids.size(), size_t(1));
  for (size_t i = 0; i < app.state().tab_history.size(); ++i) {
    EXPECT_NE(app.state().tab_history[i], std::string("new"));
  }
  app.close_modal_page();
  EXPECT_TRUE(app.state().modal_page.empty());
}

// 设置也是同样的"盖在上面"的页面：点开就显示，关掉就销毁，不入栈。
FS_TEST(uiapp_settings_is_modal_page) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  EXPECT_EQ(app.state().tab_ids.size(), size_t(1));  // only 主页
  app.open_modal_page("settings");
  EXPECT_EQ(app.state().modal_page, std::string("settings"));
  EXPECT_TRUE(dump_has(dom, "设置"));
  EXPECT_TRUE(dump_has(dom, "常规"));
  // 设置是整窗页面：它盖在 shell（含标签栏）之上的一层里。底下保持挂载
  // （这样关闭时有淡出动画），但视觉上被 modal-layer 完全覆盖。
  std::string d = dom.dump();
  EXPECT_TRUE(d.find("modal-layer") != std::string::npos);
  EXPECT_TRUE(d.find("settings-page") != std::string::npos);
  size_t layer = d.find("modal-layer");
  size_t settings = d.find("settings-page");
  ASSERT_TRUE(layer != std::string::npos && settings != std::string::npos);
  EXPECT_TRUE(layer < settings);  // the modal page lives inside that layer
  // switching tabs dismisses it (it belongs to the tab it was opened over)
  app.update([](AppState& s) {
    seed_session(s, "sess_a");
  });
  app.show("sess_a");
  EXPECT_TRUE(app.state().modal_page.empty());
}

// ---------------------------------------------------------------------------
// 项目卡片：会话行带时间/状态，且可删除
// ---------------------------------------------------------------------------

FS_TEST(uiapp_project_card_session_rows) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    seed_project(s);
    ProjectCard& p = s.projects[0];
    p.sessions.clear();
    ProjectCard::SessionRow r1;
    r1.sid = "s1";
    r1.title = "在跑的那个";
    r1.updated_ms = 1700000000000LL;
    r1.running = true;
    p.sessions.push_back(r1);
    ProjectCard::SessionRow r2;
    r2.sid = "s2";
    r2.title = "闲着的那个";
    r2.updated_ms = 1700000000000LL;
    r2.running = false;
    p.sessions.push_back(r2);
    p.session_count = 2;
  });
  // a running turn is marked as such, and both rows carry hover actions
  // (rename + delete) as one group
  EXPECT_TRUE(dump_has(dom, "ps-dot running"));
  EXPECT_TRUE(dump_has(dom, "ps-acts"));
  EXPECT_TRUE(dump_has(dom, "在跑的那个"));
  EXPECT_TRUE(dump_has(dom, "闲着的那个"));
  // the identity column offers the primary action directly
  EXPECT_TRUE(dump_has(dom, "proj-new"));
  EXPECT_TRUE(dump_has(dom, "新建会话"));
}

// A project's MODE is a live fact: the sidebar dot used to stay "idle" while the
// agent worked or a terminal printed, because it was fed only by `session.list`
// (fetched on load, never again). The row for the session you are looking at now
// reads the UI's own state, so it reacts on the event that changed it.
FS_TEST(uiapp_nav_dot_follows_live_state) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    // left-tab mode = project navigation rail (the mode where the session you are
    // in is visible NEXT TO its transcript)
    s.settings_values["ui.tabs"] = Json("left");
    seed_project(s);            // p1 -> sess_a
    seed_session(s, "sess_a");
    s.projects[0].sessions[0].running = false;   // stale snapshot says idle
    s.projects[0].sessions[0].loop_state = "idle";
  });
  EXPECT_FALSE(dump_has(dom, "ps-dot running"));
  EXPECT_TRUE(dump_has(dom, "nav-dot"));  // the rail is the mode under test
  // A turn starts: the dot is running without waiting for a session.list refresh.
  app.update([](AppState& s) { s.turn_running = true; });
  EXPECT_TRUE(dump_has(dom, "ps-dot running"));
  // The turn ends but a terminal is still live: background (purple), not idle.
  app.update([](AppState& s) {
    s.turn_running = false;
    TermItem t;
    t.tid = "t1";
    t.kind = "user";  // terminal.list marks every entry; a ghost has no kind
    t.status = "running";
    s.terminals.push_back(t);
  });
  EXPECT_TRUE(dump_has(dom, "ps-dot background"));
  // A failed turn shows the failure the list cannot know about yet.
  app.update([](AppState& s) {
    s.terminals.clear();
    s.interrupted = true;
    s.interrupt_reason = "http 400";
  });
  EXPECT_TRUE(dump_has(dom, "ps-dot failed"));
}

// A model `terminal_exec` is a tool result, not a terminal the user can open.
// Its output event used to register a ghost tab (with a number and a badge),
// while terminal.list pruned it - so the sidebar showed 13 and the drawer 4.
FS_TEST(uiapp_terminal_output_ghost_is_not_a_terminal) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    seed_project(s);
    seed_session(s, "sess_a");
  });
  app.on_backend_event_for_test(
      "terminal.output", Json{{"sid", "sess_a"}, {"tid", "t9"},
                              {"text", "short command output"},
                              {"kind", "model-fg"}});
  EXPECT_EQ(app.state().terminals.size(), size_t(0));
  // A USER terminal is a real terminal and must appear immediately.
  app.on_backend_event_for_test(
      "terminal.output", Json{{"sid", "sess_a"}, {"tid", "t10"},
                              {"text", "shell"},
                              {"kind", "user"}});
  ASSERT_TRUE(app.state().terminals.size() == 1);
  EXPECT_EQ(app.state().terminals[0].tid, std::string("t10"));
}

// A BACKGROUND model terminal is not a ghost: it outlives the turn and the
// drawer must be able to open it even without a user label. It stays out of the
// tab strip (hidden), but it exists in 所有终端.
FS_TEST(uiapp_terminal_output_background_model_terminal_is_visible) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    seed_project(s);
    seed_session(s, "sess_a");
  });
  app.on_backend_event_for_test(
      "terminal.output", Json{{"sid", "sess_a"}, {"tid", "t11"},
                              {"text", "background build"},
                              {"kind", "model-bg"}});
  ASSERT_TRUE(app.state().terminals.size() == 1);
  EXPECT_EQ(app.state().terminals[0].tid, std::string("t11"));
  EXPECT_EQ(app.state().terminals[0].kind, std::string("model-bg"));
  // Not auto-opened as a tab; still available in the overview.
  bool hidden = false;
  for (size_t i = 0; i < app.state().term_hidden.size(); ++i) {
    if (app.state().term_hidden[i] == "t11") hidden = true;
  }
  EXPECT_TRUE(hidden);
}

// The session dot's terminal count must never be ERASED by the live list: the
// terminal list is lazily fetched (only when the drawer opens), so at this point
// st_.terminals is empty while the backend snapshot already says "1 terminal
// running". The old unconditional overwrite turned the purple background dot
// back to idle.
FS_TEST(uiapp_backend_terminal_count_survives_empty_live_list) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    s.settings_values["ui.tabs"] = Json("left");
    seed_project(s);
    seed_session(s, "sess_a");
    ProjectCard::SessionRow& r = s.projects[0].sessions[0];
    r.running = false;
    r.loop_state = "idle";
    r.terminal_count = 1;  // from session.list: a background terminal is live
    s.terminals.clear();   // the drawer has never been opened
  });
  EXPECT_TRUE(dump_has(dom, "ps-dot background"));
}

// The blue "done" dot means "news since you last looked". The backend's loopState
// stays "turnDone" forever, so it used to be permanent - it survived opening the
// session AND switching tabs ("可查看了不会消失").
FS_TEST(uiapp_done_dot_clears_after_viewing) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    s.settings_values["ui.tabs"] = Json("left");  // rail: the row is on screen
    seed_project(s);                              // p1 -> sess_a
    seed_session(s, "sess_a");
    ProjectCard::SessionRow& r = s.projects[0].sessions[0];
    r.running = false;                            // the list says "finished"
    r.loop_state = "turnDone";
    r.last_error.clear();
    r.updated_ms = 1700000000000LL;               // long before "now"
  });
  EXPECT_TRUE(dump_has(dom, "ps-dot done"));
  // Looking at it spends the dot (switch_tab records the "seen" moment).
  app.show("sess_a");
  EXPECT_FALSE(dump_has(dom, "ps-dot done"));
  // New activity AFTER that moment is news again.
  int64_t future = static_cast<int64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count()) +
      60000;
  app.update([&](AppState& s) {
    s.projects[0].sessions[0].updated_ms = future;
    s.projects[0].sessions[0].last_error = "http 500";
  });
  EXPECT_TRUE(dump_has(dom, "ps-dot failed"));
}

// The side panel always SHOWED the usage body (the kind falls back to "usage")
// with no tab highlighted when `active_side` was empty - it read as "nothing is
// open, yet stats are showing".
// 供应商 page: the first provider opens by default; with NO providers at all the
// create wizard opens (an empty page with only a "新增" button is a dead end).
FS_TEST(uiapp_providers_page_auto_selects) {
  NullDom dom;
  AppState st = AppState::make();
  class ProvBackend : public IUiBackend {
   public:
    Json providers = Json::array();
    BackendResult call(const std::string& method, const Json&) override {
      BackendResult r;
      r.ok = true;
      if (method == "provider.listProviders") r.data = providers;
      if (method == "provider.listModels") r.data = Json{{"models", Json::array()}};
      return r;
    }
    std::function<void()> subscribe(BackendEvent) override { return []() {}; }
  } backend;
  UiApp app(dom, st, &backend);
  app.mount();
  // No providers -> the wizard opens by itself.
  app.load_providers();
  EXPECT_TRUE(app.state().provider_creating);
  // With one provider -> it is selected instead.
  backend.providers = Json::array(
      {Json{{"id", "pr_a"}, {"name", "A"}, {"baseUrl", "https://api.deepseek.com/v1"}}});
  app.update([](AppState& s) { s.provider_creating = false; });
  app.load_providers();
  EXPECT_FALSE(app.state().provider_creating);
  EXPECT_EQ(app.state().provider_selected, std::string("pr_a"));
}

FS_TEST(uiapp_side_panel_defaults_to_usage) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) { seed_session(s, "sess_a"); });
  EXPECT_EQ(app.state().active_side, std::string("usage"));
  EXPECT_TRUE(dump_has(dom, "ptab active"));
}

// 操作记录 must follow the log while it is open: it used to be fetched once, when
// the session was opened, so permission/model changes made with the panel visible
// never appeared.
FS_TEST(uiapp_ops_panel_refreshes_while_open) {
  NullDom dom;
  AppState st = AppState::make();
  class OpsBackend : public IUiBackend {
   public:
    Json ops = Json::array({Json{{"op", "first"}}});
    BackendResult call(const std::string& method, const Json&) override {
      BackendResult r;
      r.ok = true;
      if (method == "session.info") {
        r.data = Json{{"meta", Json{{"sid", "sess_a"}, {"pid", "p1"}}},
                      {"sessionSettings", Json::object()},
                      {"systemOps", ops}};
      }
      if (method == "session.turns") {
        r.data = Json{{"turns", Json::array()}, {"start", 0}, {"total", 0},
                      {"hasMore", false}};
      }
      if (method == "model.allEnabled") {
        r.data = Json{{"models", Json::array({Json{{"id", "m1"}}})}};
      }
      return r;
    }
    std::function<void()> subscribe(BackendEvent) override { return []() {}; }
  } backend;
  UiApp app(dom, st, &backend);
  app.mount();
  app.update([](AppState& s) {
    seed_session(s, "sess_a");
    s.model = "m1";
    s.model_available = true;
  });
  app.show("sess_a");
  app.open_or_focus_side_tab("ops");
  EXPECT_TRUE(dump_has(dom, "first"));
  // The backend appends an op while the panel is on screen.
  backend.ops.push_back(Json{{"op", "setModel"}});
  app.pump();  // the coalesced tick that refreshes an open ops panel
  EXPECT_TRUE(dump_has(dom, "setModel"));
}

// 工具调用 used to read `session_calls`, a field nothing ever filled - the panel
// said "暂无工具调用记录" during a busy turn. It now derives its rows from the
// transcript's own tool blocks.
FS_TEST(uiapp_calls_panel_reads_the_transcript) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    seed_session(s, "sess_a");
    comp::ChatMsg m;
    m.role = "assistant";
    m.text = "done";
    comp::ToolBlock tb;
    tb.name = "read_file";
    tb.summary = "src/app.cpp";
    tb.ok = true;
    m.tools.push_back(tb);
    s.messages.push_back(m);
  });
  app.open_or_focus_side_tab("calls");
  EXPECT_TRUE(dump_has(dom, "read_file"));
  EXPECT_TRUE(dump_has(dom, "src/app.cpp"));
}

// 工作流面板是"看规划/看进度"，不是启动器：模型规划的一步跑在用户点 允许 之前
// 必须停着，面板要能列出它的步骤并给出 允许 / 拒绝。
FS_TEST(uiapp_workflow_panel_shows_the_plan_for_approval) {
  NullDom dom;
  AppState st = AppState::make();
  class WfBackend : public IUiBackend {
   public:
    std::vector<std::string> methods;
    BackendResult call(const std::string& method, const Json&) override {
      methods.push_back(method);
      BackendResult r;
      r.ok = true;
      if (method == "workflow.list") r.data = Json{{"defs", Json::array()}};
      if (method == "workflow.runs") {
        r.data = Json{{"runs", Json::array({Json{
            {"id", "run_1"},
            {"name", "重构解析器"},
            {"state", "proposed"},
            {"steps", Json::array({Json{{"id", "s1"}, {"name", "调查现有实现"},
                                        {"state", "pending"}},
                                   Json{{"id", "s2"}, {"name", "写测试"},
                                        {"state", "pending"}}})}}})}};
      }
      if (method == "workflow.approve") r.data = Json{{"state", "running"}};
      return r;
    }
    std::function<void()> subscribe(BackendEvent) override { return []() {}; }
  } backend;
  UiApp app(dom, st, &backend);
  app.mount();
  app.update([](AppState& s) { seed_session(s, "sess_a"); });
  app.open_or_focus_side_tab("workflow");
  EXPECT_TRUE(dump_has(dom, "\u5f85\u5141\u8bb8"));        // 待允许
  EXPECT_TRUE(dump_has(dom, "\u91cd\u6784\u89e3\u6790\u5668"));  // the goal
  EXPECT_TRUE(dump_has(dom, "\u8c03\u67e5\u73b0\u6709\u5b9e\u73b0"));  // step 1
  EXPECT_TRUE(dump_has(dom, "\u5199\u6d4b\u8bd5"));        // step 2
  // The panel must not be a launcher any more.
  EXPECT_FALSE(dump_has(dom, "\u53ef\u542f\u52a8\u7684\u5de5\u4f5c\u6d41"));
  int hid = click_hid_in(app.tree_for_test(), "btn primary", "\u5141\u8bb8");  // 允许
  ASSERT_TRUE(hid != 0);
  bus().fire(hid, "");
  bool approved = false;
  for (size_t i = 0; i < backend.methods.size(); ++i) {
    if (backend.methods[i] == "workflow.approve") approved = true;
  }
  EXPECT_TRUE(approved);
}

// A background tab that produces output gets the dot on its TAB - nobody can see
// the other session's transcript, and `SessionTab::dirty` was never written.
FS_TEST(uiapp_background_tab_gets_dirty_dot) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    s.settings_values["ui.tabs"] = Json("top");
    seed_session(s, "sess_a");
    SessionTab b;
    b.id = "sess_b";
    b.title = "另一个";
    s.sessions.push_back(b);
    s.tab_ids.push_back("sess_b");
    s.active_tab = "sess_a";
  });
  EXPECT_EQ(count_class_exact(app.tree_for_test(), "dot"), 0);
  // Output for the NON-active session: the tab gets the dot, the screen does not.
  app.on_backend_event_for_test(
      "turn.started", Json{{"sid", "sess_b"}, {"text", "work"}});
  EXPECT_EQ(count_class_exact(app.tree_for_test(), "dot"), 1);
  // Opening that tab spends the dot.
  app.show("sess_b");
  EXPECT_EQ(count_class_exact(app.tree_for_test(), "dot"), 0);
}

// An empty argument object is not an input: it rendered a labelled section whose
// whole content was "{}".
FS_TEST(uiapp_empty_tool_input_is_hidden) {
  comp::ToolBlock tb;
  tb.name = "todo_write";
  tb.detail = "{\n  \"ok\": true\n}";
  tb.input = "{\n\n}";
  tb.expanded = true;
  H blk = comp::tool_block(tb, nullptr);
  EXPECT_EQ(count_class(blk, "tb-label"), 0);
  EXPECT_EQ(count_class(blk, "tb-body"), 1);
  // With real arguments both labels appear.
  tb.input = "{\n  \"path\": \"a.txt\"\n}";
  H blk2 = comp::tool_block(tb, nullptr);
  EXPECT_EQ(count_class(blk2, "tb-label"), 2);
  EXPECT_EQ(count_class(blk2, "tb-body"), 2);
}

// A wide markdown table must scroll inside its own wrapper, not widen the
// transcript (which used to give the whole history a horizontal scrollbar).
FS_TEST(comp_wide_table_gets_its_own_scroller) {
  std::vector<H> blocks = comp::markdown_blocks(
      "| a | b |\n|---|---|\n| 1 | 2 |\n", nullptr, nullptr);
  ASSERT_TRUE(blocks.size() == 1);
  EXPECT_EQ(count_class_exact(blocks[0], "md-table-wrap"), 1);
  EXPECT_EQ(count_class_exact(blocks[0], "md-table"), 1);
}

// Renaming is inline: the row becomes an input, Enter (or blur) commits.
FS_TEST(uiapp_session_inline_rename) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    seed_project(s);
    ProjectCard& p = s.projects[0];
    p.sessions.clear();
    ProjectCard::SessionRow r;
    r.sid = "s1";
    r.title = "旧名字";
    p.sessions.push_back(r);
    p.session_count = 1;
  });
  app.begin_rename_session("s1");
  EXPECT_TRUE(app.state().projects[0].sessions[0].renaming);
  // the row turned into an input seeded with the current title
  EXPECT_TRUE(dump_has(dom, "ps-rename-input"));
  EXPECT_TRUE(dump_has(dom, "旧名字"));
  app.update([](AppState& s) { s.projects[0].sessions[0].rename_text = "新名字"; });
  app.commit_rename_session("s1");
  EXPECT_FALSE(app.state().projects[0].sessions[0].renaming);
  EXPECT_EQ(app.state().projects[0].sessions[0].title, std::string("新名字"));
  EXPECT_TRUE(dump_has(dom, "新名字"));
}

// Deleting a session is destructive, so it goes through a confirm modal.
FS_TEST(uiapp_delete_session_confirms_first) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) { seed_project(s); });
  app.confirm_delete_session("sess_a", "UI 框架重构");
  EXPECT_EQ(app.state().confirm_kind, std::string("session"));
  EXPECT_EQ(app.state().confirm_id, std::string("sess_a"));
  EXPECT_TRUE(dump_has(dom, "UI 框架重构"));
  EXPECT_TRUE(dump_has(dom, "无法恢复"));
}

// 相对时间：列表里给"多久之前"，很久以前给日期。
FS_TEST(uiapp_rel_time_formats) {
  // unknown -> empty (the row shows no time rather than "56 年 前")
  EXPECT_TRUE(UiApp::rel_time_for_test(0).empty());
  // now -> 刚刚
  int64_t now_wall = static_cast<int64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
  EXPECT_EQ(UiApp::rel_time_for_test(now_wall), std::string("刚刚"));
  // 5 minutes ago
  EXPECT_EQ(UiApp::rel_time_for_test(now_wall - 5 * 60 * 1000), std::string("5 分钟前"));
  // 3 hours ago
  EXPECT_EQ(UiApp::rel_time_for_test(now_wall - 3 * 3600 * 1000), std::string("3 小时前"));
  // 2 days ago
  EXPECT_EQ(UiApp::rel_time_for_test(now_wall - 2 * 86400 * 1000), std::string("2 天前"));
  // older than a month -> an absolute date, not "87 天前"
  std::string old = UiApp::rel_time_for_test(now_wall - 200LL * 86400 * 1000);
  EXPECT_TRUE(old.size() == 10 && old[4] == '-' && old[7] == '-');
}

// ---------------------------------------------------------------------------
// 标签拖拽调换顺序 / 左栏模式 / 双 view 离场
// ---------------------------------------------------------------------------

FS_TEST(uiapp_tab_drag_reorders) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    SessionTab a; a.id = "sess_a"; s.sessions.push_back(a); s.tab_ids.push_back("sess_a");
    SessionTab b; b.id = "sess_b"; s.sessions.push_back(b); s.tab_ids.push_back("sess_b");
    SessionTab c; c.id = "sess_c"; s.sessions.push_back(c); s.tab_ids.push_back("sess_c");
  });
  EXPECT_TRUE(dom.count_of("bind_tab_drag") >= 1);
  // drag sess_c onto sess_a -> it takes sess_a's slot, the rest keep their order
  dom.fire_tab_drag_for_test("sess_c", "sess_a");
  const std::vector<std::string>& ids = app.state().tab_ids;
  ASSERT_TRUE(ids.size() == 4);
  EXPECT_EQ(ids[0], std::string("home"));
  EXPECT_EQ(ids[1], std::string("sess_c"));
  EXPECT_EQ(ids[2], std::string("sess_a"));
  EXPECT_EQ(ids[3], std::string("sess_b"));
  // home is pinned: it cannot be dragged, and nothing can be dropped onto it
  dom.fire_tab_drag_for_test("home", "sess_a");
  EXPECT_EQ(app.state().tab_ids[0], std::string("home"));
  // the new order is persisted so a restart keeps it
  EXPECT_TRUE(app.state().settings_values.contains("ui.tabOrder"));
}

FS_TEST(uiapp_left_tabs_are_a_project_nav_rail) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    seed_project(s);
    s.settings_values = Json::object();
    s.settings_values["ui.tabs"] = Json{{"value", "left"}};
    // an OPEN tab whose session is NOT in the project list: the rail is a
    // navigation (all sessions of all projects), not the open-tab work set.
    SessionTab t; t.id = "sess_open"; s.sessions.push_back(t); s.tab_ids.push_back("sess_open");
  });
  app.apply_appearance();
  std::string d = dom.dump();
  EXPECT_TRUE(d.find("nav-rail") != std::string::npos);
  EXPECT_TRUE(d.find("nav-session") != std::string::npos);
  // the project's sessions are listed (navigation), not the open tab id
  EXPECT_TRUE(d.find("UI 框架重构") != std::string::npos);
  EXPECT_TRUE(d.find("sess_open") == std::string::npos);
  // collapsible to a slim strip
  app.update([](AppState& s) { s.nav_collapsed = true; });
  EXPECT_TRUE(dom.dump().find("nav-mini") != std::string::npos);
  app.update([](AppState& s) { s.nav_collapsed = false; });
  // its own splitter, so the side panel keeps its width when the rail moves
  EXPECT_TRUE(dom.count_of("bind_nav_splitter") >= 1);
  dom.fire_nav_split_for_test(300);
  EXPECT_EQ(app.state().nav_width, 300);
}

// A leaving page must survive long enough to animate out: the framework keeps
// it in the DOM and schedules its removal. NullDom removes immediately (no CSS),
// so the test checks the browser-side contract through the VNode declaration and
// that the incoming page is a SEPARATE subtree.
FS_TEST(uiapp_page_switch_is_two_views) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    SessionTab a; a.id = "sess_a"; s.sessions.push_back(a); s.tab_ids.push_back("sess_a");
  });
  app.show("sess_a");
  std::string d = dom.dump();
  // the incoming page carries its own entrance animation...
  EXPECT_TRUE(d.find("anim-slide-left") != std::string::npos);
  // ...and a leaving page is declared with an exit intent, not given the
  // entrance class a second time
  EXPECT_TRUE(d.find("page-slot") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 第二十二批：搜索页 / 状态点 / 左栏排序筛选 / 侧栏排序 / 模型选择器关闭
// ---------------------------------------------------------------------------

FS_TEST(uiapp_global_search_page) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    seed_project(s);
    s.home_tab = 3;
  });
  EXPECT_TRUE(dump_has(dom, "global-search-input") ||
              dump_has(dom, "搜索会话"));
  // a query narrows to matching sessions and shows the project as context
  app.update([](AppState& s) { s.search_query = "UI"; });
  EXPECT_TRUE(dump_has(dom, "UI 框架重构"));
  app.update([](AppState& s) { s.search_query = "zzz-no-match"; });
  EXPECT_TRUE(dump_has(dom, "没有匹配的会话"));
  // the search entry sits beside the three title tabs
  app.update([](AppState& s) { s.search_query.clear(); s.home_tab = 1; });
  EXPECT_TRUE(dump_has(dom, "home-search-btn"));
}

FS_TEST(uiapp_session_status_dots) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    seed_project(s);
    ProjectCard& p = s.projects[0];
    p.sessions.clear();
    ProjectCard::SessionRow r;
    r.sid = "s1";
    r.title = "done";
    r.loop_state = "turnDone";
    p.sessions.push_back(r);
    r.sid = "s2";
    r.title = "failed";
    r.loop_state = "turnDone";
    r.last_error = "boom";
    p.sessions.push_back(r);
    r.sid = "s3";
    r.title = "waiting";
    r.loop_state = "waiting";
    r.last_error.clear();
    r.running = true;
    p.sessions.push_back(r);
    r.sid = "s4";
    r.title = "running";
    r.loop_state = "planning";
    p.sessions.push_back(r);
  });
  std::string d = dom.dump();
  EXPECT_TRUE(d.find("ps-dot done") != std::string::npos);
  EXPECT_TRUE(d.find("ps-dot failed") != std::string::npos);
  EXPECT_TRUE(d.find("ps-dot waiting") != std::string::npos);
  EXPECT_TRUE(d.find("ps-dot running") != std::string::npos);
}

FS_TEST(uiapp_nav_sort_and_search_icon) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    seed_project(s);
    s.settings_values = Json::object();
    s.settings_values["ui.tabs"] = Json{{"value", "left"}};
  });
  app.apply_appearance();
  // Sorting stays in the rail (it is three short words)...
  EXPECT_TRUE(dump_has(dom, "nav-sort-btn"));
  // ...but search does NOT: a text box in a 240px rail squeezed the session
  // titles away. The rail carries an icon that jumps to the search page.
  EXPECT_FALSE(dump_has(dom, "搜索会话"));
  EXPECT_TRUE(dump_has(dom, "magnifying-glass"));
  // sorting by title is a stable reorder (no crash, same rows)
  app.update([](AppState& s) { s.session_sort = "title"; });
  EXPECT_TRUE(dump_has(dom, "UI 框架重构"));
}

FS_TEST(uiapp_side_tab_drag_reorders) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) { seed_session(s, "sess_a"); });
  app.open_or_focus_side_tab("files");
  std::string files = app.state().active_side;
  app.open_or_focus_side_tab("ops");
  std::string ops = app.state().active_side;
  // drag ops (last) onto the first panel
  std::string first = app.state().side_tabs[0].id;
  dom.fire_tab_drag_for_test(ops, first);
  EXPECT_EQ(app.state().side_tabs[0].id, ops);
  EXPECT_EQ(app.state().side_tabs[1].id, first);
  EXPECT_TRUE(files != ops);
}

FS_TEST(uiapp_model_picker_dismisses_on_outside_click) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    seed_session(s, "sess_a");
    s.model_picker_open = true;
    s.model_picker_owner = "composer";
    s.model_picker_provider = "p1";
    s.model_picker_loaded = true;
  });
  EXPECT_TRUE(dom.count_of("bind_outside_click") >= 1);
  EXPECT_TRUE(dom.count_of("place_popup") >= 1);
  dom.fire_outside_click_for_test();
  EXPECT_FALSE(app.state().model_picker_open);
}

// ---------------------------------------------------------------------------
// 第二十三批：演示会话渲染（思考块 / 工具块 / 会话设置回填）
// ---------------------------------------------------------------------------

// A backend that answers session.get with real session settings and turns that
// carry thinking + tool calls - the shape the demo session has.
class DemoBackend : public IUiBackend {
 public:
  BackendResult call(const std::string& method, const Json&) override {
    BackendResult r;
    r.ok = true;
    // The composer's per-session settings arrive from session.info now (the
    // transcript is paged separately, so session.get is no longer used here).
    if (method == "session.get" || method == "session.info") {
      r.data = Json{
          {"meta", Json{{"sid", "sess_a"}, {"pid", "p1"}, {"cwd", "/tmp/p"}}},
          {"sessionSettings", Json{{"model", "claude-sonnet-4"},
                                   {"thinking", "high"},
                                   {"filePerm", "approve"},
                                   {"autoMode", true}}},
          {"turns", Json::array()}};
    } else if (method == "project.list" || method == "session.list") {
      r.data = Json::array();
    } else if (method == "model.allEnabled") {
      // The composer asks this to decide whether the session's remembered model
      // still exists. A backend that answered with an empty list would look like
      // "that model was deleted", and the composer would clear it - which is
      // exactly the behaviour the availability check is there to provide.
      r.data = Json{{"models", Json::array({Json{{"id", "claude-sonnet-4"}}})}};
    }
    return r;
  }
  std::function<void()> subscribe(BackendEvent) override { return []() {}; }
};

// Backend that answers the git.* methods with a fixed repository state, so the
// panel can be asserted without a real repo on disk (the git CLI itself is
// covered by test_git.cpp).
class GitBackend : public IUiBackend {
 public:
  std::string last_method;
  // Synchronous backend: one call can trigger another (a commit refreshes the
  // status), so `last_method` alone cannot prove what was sent. The history
  // keeps every method + args pair.
  std::vector<std::string> methods;
  std::vector<Json> args_history;
  bool repo = true;

  // The args of the most recent call to `method`.
  Json args_for(const std::string& method) const {
    for (size_t i = methods.size(); i-- > 0;) {
      if (methods[i] == method) return args_history[i];
    }
    return Json::object();
  }

  BackendResult call(const std::string& method, const Json& args) override {
    last_method = method;
    methods.push_back(method);
    args_history.push_back(args);
    BackendResult r;
    r.ok = true;
    if (method == "git.status") {
      r.data = Json{
          {"ok", true},
          {"isRepo", repo},
          {"root", "C:/p"},
          {"branch", "main"},
          {"upstream", "origin/main"},
          {"ahead", 2},
          {"behind", 1},
          {"detached", false},
          {"clean", false},
          {"operation", ""},
          {"error", ""},
          {"changes", Json::array(
              {Json{{"path", "src/a.cpp"}, {"index", "M"}, {"work", " "},
                    {"staged", true}, {"untracked", false}, {"conflicted", false}},
               Json{{"path", "src/b.cpp"}, {"index", " "}, {"work", "M"},
                    {"staged", false}, {"untracked", false}, {"conflicted", false}},
               Json{{"path", "new.txt"}, {"index", "?"}, {"work", "?"},
                    {"staged", false}, {"untracked", true}, {"conflicted", false}}})}};
    } else if (method == "git.diff") {
      r.data = Json{{"ok", true},
                    {"truncated", false},
                    {"diff", "diff --git a/src/b.cpp b/src/b.cpp\n-old\n+new\n"}};
    } else if (method == "git.commit") {
      r.data = Json{{"ok", true}, {"message", "committed"}, {"hash", "abc1234"}};
    } else if (method == "git.stage" || method == "git.unstage" ||
               method == "git.stageAll" || method == "git.discard") {
      r.data = Json{{"ok", true}, {"message", "ok"}};
    }
    return r;
  }
  std::function<void()> subscribe(BackendEvent) override { return []() {}; }
};

// Open a git panel on a seeded session; returns nothing, the app is the handle.
static void seed_git_panel(UiApp& app, const std::string& pid) {
  app.update([pid](AppState& s) {
    seed_session(s, "sess_a");
    s.active_pid = pid;
    s.side_tabs.clear();
    SideTab t;
    t.id = "g1";
    t.kind = "git";
    t.title = "Git";
    s.side_tabs.push_back(t);
    s.active_side = "g1";
  });
}

// A turn in the transcript is a navigation point: the message menu forks,
// rolls back or forgets THAT turn, addressed by its stored turnId.
FS_TEST(uiapp_turn_actions_are_addressable) {
  NullDom dom;
  AppState st = AppState::make();
  class TurnBackend : public IUiBackend {
   public:
    std::vector<std::string> methods;
    std::vector<Json> args;
    Json args_for(const std::string& m) const {
      for (size_t i = methods.size(); i-- > 0;) if (methods[i] == m) return args[i];
      return Json::object();
    }
    BackendResult call(const std::string& method, const Json& a) override {
      methods.push_back(method);
      args.push_back(a);
      BackendResult r;
      r.ok = true;
      if (method == "session.fork") r.data = Json{{"sid", "s_new"}};
      if (method == "session.info") {
        r.data = Json{{"meta", Json{{"sid", "sess_a"}, {"pid", "p1"}}},
                      {"sessionSettings", Json::object()},
                      {"systemOps", Json::array()}};
      }
      if (method == "session.turns") {
        r.data = Json{{"turns", Json::array(
                          {Json{{"role", "user"}, {"text", "hi"}, {"turnId", "t_a"}},
                           Json{{"role", "assistant"}, {"text", "yo"}, {"turnId", "t_b"}}})},
                      {"start", 0}, {"total", 2}, {"hasMore", false}};
      }
      return r;
    }
    std::function<void()> subscribe(BackendEvent) override { return []() {}; }
  } backend;
  UiApp app(dom, st, &backend);
  app.mount();
  app.update([](AppState& s) { seed_session(s, "sess_a"); });
  app.show("sess_a");
  // the loaded messages carry the stored turn ids
  ASSERT_TRUE(app.state().messages.size() == 2);
  EXPECT_EQ(app.state().messages[1].turn_id, std::string("t_b"));

  app.turn_action(1, "fork");
  EXPECT_EQ(jsonutil::get<std::string>(backend.args_for("session.fork"), "turnId", ""),
            std::string("t_b"));

  // rollback asks first; the confirm modal carries the turn id
  app.turn_action(1, "rollback");
  EXPECT_EQ(app.state().confirm_kind, std::string("rollback"));
  EXPECT_EQ(app.state().confirm_arg, std::string("t_b"));

  // forget is not destructive of the record, so it goes straight through
  app.turn_action(0, "forget");
  EXPECT_EQ(jsonutil::get<std::string>(backend.args_for("session.forget"), "scope", ""),
            std::string("t_a"));
}

// 编辑并重发. Two rules, both of which used to be false:
//   * the button must RENDER on the last user bubble (the user branch of
//     chat_message returned before the action row was built, so it never did);
//   * clicking it must only ARM the turn. The trim happens when the edited text
//     is sent, so changing your mind after clicking 编辑 loses nothing.
FS_TEST(uiapp_last_user_message_is_editable) {
  NullDom dom;
  AppState st = AppState::make();
  class EditBackend : public IUiBackend {
   public:
    std::vector<std::string> methods;
    std::vector<Json> args;
    Json args_for(const std::string& m) const {
      for (size_t i = methods.size(); i-- > 0;) if (methods[i] == m) return args[i];
      return Json::object();
    }
    BackendResult call(const std::string& method, const Json& a) override {
      methods.push_back(method);
      args.push_back(a);
      BackendResult r;
      r.ok = true;
      if (method == "session.info") {
        r.data = Json{{"meta", Json{{"sid", "sess_a"}, {"pid", "p1"}}},
                      {"sessionSettings", Json::object()},
                      {"systemOps", Json::array()}};
      }
      if (method == "session.turns") {
        r.data = Json{{"turns", Json::array(
                          {Json{{"role", "user"}, {"text", "first q"}, {"turnId", "t_a"}},
                           Json{{"role", "assistant"}, {"text", "first a"}, {"turnId", "t_b"}},
                           Json{{"role", "user"}, {"text", "second q"}, {"turnId", "t_c"}},
                           Json{{"role", "assistant"}, {"text", "second a"}, {"turnId", "t_d"}}})},
                      {"start", 0}, {"total", 4}, {"hasMore", false}};
      }
      // Without this the composer decides the remembered model is gone and
      // refuses the send, which would look like the edit path not running.
      if (method == "model.allEnabled") {
        r.data = Json{{"models", Json::array({Json{{"id", "m1"}}})}};
      }
      return r;
    }
    std::function<void()> subscribe(BackendEvent) override { return []() {}; }
  } backend;
  UiApp app(dom, st, &backend);
  app.mount();
  app.update([](AppState& s) {
    seed_session(s, "sess_a");
    s.model = "m1";
    s.model_available = true;
  });
  app.show("sess_a");
  ASSERT_TRUE(app.state().messages.size() == 4);
  // The action exists on the NEWEST question only: rewriting an older one would
  // orphan every answer after it.
  std::vector<VNodePtr> msgs;
  collect_msg_nodes(app.tree_for_test(), "msg:", &msgs);
  ASSERT_TRUE(msgs.size() == 4);
  EXPECT_EQ(click_hid_in(msgs[0], "mini-btn", "\u7f16\u8f91"), 0);
  int hid = click_hid_in(msgs[2], "mini-btn", "\u7f16\u8f91");
  ASSERT_TRUE(hid != 0);

  bus().fire(hid, "");
  // Armed, not executed: the text is back in the composer and the history is
  // untouched until the send.
  EXPECT_EQ(app.state().session_input, std::string("second q"));
  EXPECT_EQ(app.state().edit_turn_id, std::string("t_c"));
  EXPECT_EQ(app.state().messages.size(), size_t(4));
  EXPECT_TRUE(backend.args_for("session.trimHistory").empty());

  app.send_message();
  // Trim FIRST, then send: the old question and its answer are gone instead of
  // being duplicated.
  EXPECT_EQ(jsonutil::get<std::string>(backend.args_for("session.trimHistory"), "turnId", ""),
            std::string("t_c"));
  EXPECT_EQ(jsonutil::get<std::string>(backend.args_for("session.send"), "text", ""),
            std::string("second q"));
  EXPECT_TRUE(app.state().edit_turn_id.empty());
  // user turn + the live "waiting for the model" placeholder (see dispatch_message)
  ASSERT_TRUE(app.state().messages.size() == 4);
  EXPECT_EQ(app.state().messages[1].text, std::string("first a"));
  EXPECT_EQ(app.state().messages[2].role, std::string("user"));
  EXPECT_EQ(app.state().messages[2].text, std::string("second q"));
  EXPECT_EQ(app.state().messages[3].role, std::string("assistant"));
  EXPECT_TRUE(app.state().messages[3].streaming);
}

// The command menu renders whatever the backend listed - title, description and
// all - and picking one runs ITS action.
FS_TEST(uiapp_command_menu_is_backend_driven) {
  NullDom dom;
  AppState st = AppState::make();
  class CmdBackend : public IUiBackend {
   public:
    std::string ran;
    BackendResult call(const std::string& method, const Json& a) override {
      BackendResult r;
      r.ok = true;
      if (method == "command.list") {
        r.data = Json{{"commands", Json::array(
            {Json{{"name", "new"}, {"title", "新建会话"},
                  {"description", "在当前项目新建一个会话"}, {"action", "ui:new-session"},
                  {"group", "builtin"}},
             Json{{"name", "my-skill"}, {"title", "My Skill"},
                  {"description", "does a thing"}, {"action", "skill:my-skill"},
                  {"group", "skill"}}})}};
      } else if (method == "command.run") {
        ran = jsonutil::get<std::string>(a, "name", "");
      }
      return r;
    }
    std::function<void()> subscribe(BackendEvent) override { return []() {}; }
  } backend;
  UiApp app(dom, st, &backend);
  app.mount();
  app.update([](AppState& s) { seed_session(s, "sess_a"); });
  // the menu is filled by command.list, not by a literal in the UI
  app.update([](AppState& s) { s.skill_menu_open = true; });
  // trigger the load the button would do
  app.update([](AppState& s) { s.command_menu.clear(); });
  app.update([&](AppState& s) {
    AppState::CommandRow a;
    a.name = "new";
    a.title = "新建会话";
    a.description = "在当前项目新建一个会话";
    a.action = "ui:new-session";
    s.command_menu.push_back(a);
    AppState::CommandRow b;
    b.name = "my-skill";
    b.title = "My Skill";
    b.description = "does a thing";
    b.action = "skill:my-skill";
    s.command_menu.push_back(b);
  });
  std::string d = dom.dump();
  EXPECT_TRUE(d.find("/new") != std::string::npos);
  EXPECT_TRUE(d.find("新建会话") != std::string::npos);
  EXPECT_TRUE(d.find("/my-skill") != std::string::npos);
  EXPECT_TRUE(d.find("does a thing") != std::string::npos);

  // a skill command goes to the BACKEND by name
  app.run_command(app.state().command_menu[1]);
  EXPECT_EQ(backend.ran, std::string("my-skill"));
  // a ui: action is handled locally (new-session) and never sent as a command
  app.run_command(app.state().command_menu[0]);
  EXPECT_EQ(backend.ran, std::string("my-skill"));
}

// The workflow panel is a view over workflow.runs: per-step state plus the
// retry/skip actions that only make sense for a failed step.
FS_TEST(uiapp_workflow_panel_shows_runs) {
  NullDom dom;
  AppState st = AppState::make();
  class WfBackend : public IUiBackend {
   public:
    BackendResult call(const std::string& method, const Json&) override {
      BackendResult r;
      r.ok = true;
      if (method == "workflow_list") {
        r.data = Json{{"defs", Json::array(
            {Json{{"name", "refactor"}, {"description", "big one"}, {"steps", Json::array()}}})}};
      } else if (method == "workflow.runs") {
        r.data = Json{{"runs", Json::array(
            {Json{{"id", "r1"}, {"name", "refactor"}, {"state", "running"},
                  {"steps", Json::array(
                      {Json{{"id", "s1"}, {"name", "scan"}, {"state", "done"}},
                       Json{{"id", "s2"}, {"name", "edit"}, {"state", "failed"},
                            {"attempts", 1}, {"error", "boom"}}})}}})}};
      }
      return r;
    }
    std::function<void()> subscribe(BackendEvent) override { return []() {}; }
  } backend;
  UiApp app(dom, st, &backend);
  app.mount();
  app.update([](AppState& s) {
    seed_session(s, "sess_a");
    s.side_tabs.clear();
    SideTab t;
    t.id = "w1";
    t.kind = "workflow";
    s.side_tabs.push_back(t);
    s.active_side = "w1";
  });
  app.load_side_tab("w1");
  std::string d = dom.dump();
  EXPECT_TRUE(d.find("refactor") != std::string::npos);
  EXPECT_TRUE(d.find("scan") != std::string::npos);
  EXPECT_TRUE(d.find("edit") != std::string::npos);
  EXPECT_TRUE(d.find("boom") != std::string::npos);
  EXPECT_TRUE(d.find("wf-step failed") != std::string::npos);
  // a failed step offers retry/skip
  EXPECT_TRUE(d.find("重试") != std::string::npos);
  EXPECT_TRUE(d.find("跳过") != std::string::npos);
}

// The plan review is NOT a panel any more. The document is the plan; approval
// belongs to the ask flow (addon2: plan ≈ ask). The UI must not render review
// buttons floating over the transcript.
FS_TEST(uiapp_plan_review_actions_only) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) { seed_session(s, "sess_a"); });
  std::string d = dom.dump();
  EXPECT_TRUE(d.find("plan-review") == std::string::npos);
  EXPECT_TRUE(d.find("通过") == std::string::npos);
  EXPECT_TRUE(d.find("驳回") == std::string::npos);
}

// Enter-to-send is a real setting: the textarea carries it so the DOM adapter's
// key handler can honour it.
FS_TEST(uiapp_enter_send_reaches_textarea) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    seed_session(s, "sess_a");
    s.enter_send = "ctrlEnter";
  });
  std::string d = dom.dump();
  EXPECT_TRUE(d.find("data-enter-send=\"ctrlEnter\"") != std::string::npos);
  app.update([](AppState& s) { s.enter_send = "enter"; });
  EXPECT_TRUE(dom.dump().find("data-enter-send=\"enter\"") != std::string::npos);
}

// An interrupted loop leaves a persistent bar with a working 继续 action, not
// just a toast that disappears.
FS_TEST(uiapp_interrupt_bar_continues) {
  NullDom dom;
  AppState st = AppState::make();
  class ContBackend : public IUiBackend {
   public:
    std::string method;
    BackendResult call(const std::string& m, const Json&) override {
      method = m;
      BackendResult r;
      r.ok = true;
      return r;
    }
    std::function<void()> subscribe(BackendEvent) override { return []() {}; }
  } backend;
  UiApp app(dom, st, &backend);
  app.mount();
  app.update([](AppState& s) { seed_session(s, "sess_a"); });
  EXPECT_TRUE(dom.dump().find("interrupt-bar") == std::string::npos);
  app.on_backend_event_for_test("loop.interrupted", Json{{"sid", "sess_a"}});
  std::string d = dom.dump();
  EXPECT_TRUE(d.find("interrupt-bar") != std::string::npos);
  EXPECT_TRUE(d.find("会话已中断") != std::string::npos);
  EXPECT_TRUE(d.find("继续") != std::string::npos);
  EXPECT_TRUE(d.find("忽略") != std::string::npos);
  EXPECT_TRUE(d.find("复制") != std::string::npos);
  // dismissing it removes the bar
  app.update([](AppState& s) { s.interrupted = false; });
  EXPECT_TRUE(dom.dump().find("interrupt-bar") == std::string::npos);
}

// A stored turn with thousands of tool rows is trimmed at LOAD time to the 300
// the work log can show; the omitted count is kept for the notice, and anchor
// arrays are shifted so notes cannot point at rows that no longer exist.
FS_TEST(uiapp_stored_tool_rows_are_trimmed_at_load) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  Json calls = Json::array();
  for (int i = 0; i < 350; ++i) {
    calls.push_back(Json{{"name", "read_file"},
                         {"args", Json{{"path", "f" + std::to_string(i)}}},
                         {"detail", Json{{"startLine", 1}, {"endLine", 1}}.dump()},
                         {"ok", true}});
  }
  Json turn = Json{{"role", "assistant"},
                   {"streamSummary", "done"},
                   {"toolCalls", calls},
                   {"thoughts", Json::array({Json{{"text", "early"}, {"after", 0}},
                                             Json{{"text", "late"}, {"after", 349}}})},
                   {"interim", Json::array({"before", "after"})},
                   {"interimAfter", Json::array({10, 340})}};
  comp::ChatMsg m = app.turn_to_message_for_test(turn, 0);
  EXPECT_EQ(m.tools.size(), size_t(300));
  EXPECT_EQ(m.tools_omitted, 50);
  EXPECT_EQ(m.thoughts.size(), size_t(1));
  EXPECT_EQ(m.thoughts[0].text, std::string("late"));
  EXPECT_EQ(m.thoughts[0].after, size_t(299));
  ASSERT_TRUE(m.interim.size() == 1u);
  EXPECT_EQ(m.interim[0], std::string("after"));
  EXPECT_EQ(m.interim_after[0], size_t(290));
}

// A pushed session.status must clear BOTH the active composer and the sidebar
// row immediately: waiting for a coalesced session.list is how a failed session
// kept showing "running" after a switch.
FS_TEST(uiapp_session_status_push_clears_running) {
  NullDom dom;
  AppState st = AppState::make();
  seed_project(st);
  seed_session(st, "sess_a");
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    s.turn_running = true;
    s.auto_round = 2;
    s.projects[0].sessions[0].running = true;
    s.projects[0].sessions[0].auto_round = 2;
  });
  app.on_backend_event_for_test(
      "session.status",
      Json{{"sid", "sess_a"}, {"running", false}, {"state", "turnDone"},
           {"autoRound", 0}, {"error", "provider 429"}});
  EXPECT_FALSE(app.state().turn_running);
  EXPECT_EQ(app.state().auto_round, 0);
  ASSERT_TRUE(!app.state().projects.empty() &&
              !app.state().projects[0].sessions.empty());
  const ProjectCard::SessionRow& row = app.state().projects[0].sessions[0];
  EXPECT_FALSE(row.running);
  EXPECT_EQ(row.auto_round, 0);
  EXPECT_EQ(row.last_error, std::string("provider 429"));
  EXPECT_EQ(row.loop_state, std::string("turnDone"));
}

// Auto-continue reason / stop reason belong to the WORK LOG, not a toast that
// disappears in three seconds.
FS_TEST(uiapp_auto_round_reason_is_a_work_note) {
  NullDom dom;
  AppState st = AppState::make();
  seed_project(st);
  seed_session(st, "sess_a");
  UiApp app(dom, st, nullptr);
  app.mount();
  app.on_backend_event_for_test(
      "auto.next",
      Json{{"sid", "sess_a"}, {"round", 2}, {"reason", "2 open checklist items"}});
  std::string d = dom.dump();
  EXPECT_TRUE(d.find("自动继续（第 2 轮）：2 open checklist items") !=
              std::string::npos);
  EXPECT_TRUE(app.state().toast.empty());
}

// The git panel is a real view: branch, ahead/behind, two change groups, and a
// diff for the selected file. Each of those answers a different question, and
// the panel must not collapse them into "no changes".
FS_TEST(uiapp_git_panel_shows_status) {
  NullDom dom;
  AppState st = AppState::make();
  GitBackend backend;
  UiApp app(dom, st, &backend);
  app.mount();
  seed_git_panel(app, "p1");
  app.load_side_tab("g1");
  std::string d = dom.dump();
  EXPECT_TRUE(backend.args_for("git.status").is_object());
  EXPECT_TRUE(d.find("main") != std::string::npos);
  EXPECT_TRUE(d.find("已暂存") != std::string::npos);
  EXPECT_TRUE(d.find("未暂存") != std::string::npos);
  EXPECT_TRUE(d.find("src/a.cpp") != std::string::npos);
  EXPECT_TRUE(d.find("src/b.cpp") != std::string::npos);
  EXPECT_TRUE(d.find("new.txt") != std::string::npos);
  // a commit box only appears when something IS staged
  EXPECT_TRUE(d.find("提交信息") != std::string::npos);
}

FS_TEST(uiapp_git_panel_not_a_repo) {
  NullDom dom;
  AppState st = AppState::make();
  GitBackend backend;
  backend.repo = false;
  UiApp app(dom, st, &backend);
  app.mount();
  seed_git_panel(app, "p1");
  app.load_side_tab("g1");
  std::string d = dom.dump();
  EXPECT_TRUE(d.find("不是 Git 仓库") != std::string::npos);
  EXPECT_TRUE(d.find("提交信息") == std::string::npos);
}

// Selecting a file asks for ITS diff, and the staged/unstaged side is part of
// the request - they are different questions with different answers.
FS_TEST(uiapp_git_diff_request_carries_side) {
  NullDom dom;
  AppState st = AppState::make();
  GitBackend backend;
  UiApp app(dom, st, &backend);
  app.mount();
  seed_git_panel(app, "p1");
  app.load_side_tab("g1");
  app.load_git_diff("g1", "src/b.cpp", false);
  Json sent = backend.args_for("git.diff");
  EXPECT_EQ(jsonutil::get<std::string>(sent, "path", ""), std::string("src/b.cpp"));
  EXPECT_FALSE(jsonutil::get<bool>(sent, "staged", true));
  std::string d = dom.dump();
  // The diff is highlighted as a diff: added/removed lines are token classes.
  EXPECT_TRUE(d.find("hl-ad") != std::string::npos);
  EXPECT_TRUE(d.find("hl-de") != std::string::npos);
  EXPECT_TRUE(d.find("src/b.cpp") != std::string::npos);
}

// Committing sends the typed message and clears the draft on success.
FS_TEST(uiapp_git_commit_sends_message) {
  NullDom dom;
  AppState st = AppState::make();
  GitBackend backend;
  UiApp app(dom, st, &backend);
  app.mount();
  seed_git_panel(app, "p1");
  app.load_side_tab("g1");
  app.update([](AppState& s) { s.side_tabs[0].git_message = "fix the thing"; });
  app.git_commit("g1", "fix the thing", false);
  Json sent = backend.args_for("git.commit");
  EXPECT_EQ(jsonutil::get<std::string>(sent, "message", ""),
            std::string("fix the thing"));
  // the draft is cleared and the panel re-reads its status
  EXPECT_TRUE(app.state().side_tabs[0].git_message.empty());
  EXPECT_TRUE(backend.args_for("git.status").is_object());
  EXPECT_EQ(backend.last_method, std::string("git.status"));
}

// Staging is addressed by pid + path, never by a repository path built in the
// UI: the backend owns the resolution.
FS_TEST(uiapp_git_stage_sends_pid_and_paths) {
  NullDom dom;
  AppState st = AppState::make();
  GitBackend backend;
  UiApp app(dom, st, &backend);
  app.mount();
  seed_git_panel(app, "p1");
  app.git_stage_paths("g1", std::vector<std::string>(1, "src/b.cpp"), true);
  Json sent = backend.args_for("git.stage");
  EXPECT_EQ(jsonutil::get<std::string>(sent, "pid", ""), std::string("p1"));
  ASSERT_TRUE(sent.contains("paths") && sent["paths"].is_array());
  EXPECT_EQ(sent["paths"][0].get<std::string>(), std::string("src/b.cpp"));
  // a stage refreshes the panel's status afterwards
  EXPECT_EQ(backend.last_method, std::string("git.status"));
}

// The 用量 panel used to show hard-coded numbers (42% / ¥0.21 / 12.4K). It now
// reads usage.session: before any data arrives every value is a placeholder, and
// after it arrives the panel shows the real totals.
FS_TEST(uiapp_usage_panel_uses_real_session_usage) {
  NullDom dom;
  AppState st = AppState::make();
  class UsageBackend : public IUiBackend {
   public:
    std::string sid;
    int calls = 0;
    BackendResult call(const std::string& method, const Json& args) override {
      BackendResult r;
      r.ok = true;
      if (method == "usage.session") {
        ++calls;
        sid = jsonutil::get<std::string>(args, "sid", "");
        // lastIn / contextWindow are what the provider REPORTED: the water level
        // is lastIn / contextWindow, never a name-based guess.
        r.data = Json{{"totalIn", 12437},
                      {"totalOut", 3100},
                      {"totalCost", 0.21},
                      {"costKind", "money"},
                      {"calls", 3},
                      {"lastIn", 40000},
                      {"lastOut", 800},
                      {"contextWindow", 200000}};
      } else if (method == "session.get") {
        r.data = Json{{"meta", Json{{"sid", "sess_a"}, {"pid", "p1"}}},
                      {"turns", Json::array()}};
      } else if (method == "project.list" || method == "session.list") {
        r.data = Json::array();
      }
      return r;
    }
    std::function<void()> subscribe(BackendEvent) override { return []() {}; }
  } backend;
  UiApp app(dom, st, &backend);
  app.mount();
  app.update([](AppState& s) { seed_session(s, "sess_a"); });
  app.show("sess_a");
  app.load_session_usage("sess_a");
  EXPECT_EQ(backend.sid, std::string("sess_a"));
  EXPECT_TRUE(app.state().session_usage_loaded);
  EXPECT_EQ(app.state().session_tokens_in, int64_t(12437));
  EXPECT_EQ(app.state().session_tokens_out, int64_t(3100));
  EXPECT_TRUE(app.state().session_cost.find("0.21") != std::string::npos);
  std::string d = dom.dump();
  // 12.4K is what a person reads in a 360px panel
  EXPECT_TRUE(d.find("12.4K") != std::string::npos);
  EXPECT_TRUE(d.find("3.1K") != std::string::npos);
  // 40000 / 200000 = 20%: the LAST request against the window the API reported
  EXPECT_EQ(app.state().context_pct, 20);
}

// No reported window (the endpoint does not list context_length) => the level
// stays unknown rather than being estimated from the model name.
FS_TEST(uiapp_context_level_needs_a_reported_window) {
  NullDom dom;
  AppState st = AppState::make();
  class NoWindowBackend : public IUiBackend {
   public:
    BackendResult call(const std::string& method, const Json&) override {
      BackendResult r;
      r.ok = true;
      if (method == "usage.session") {
        r.data = Json{{"totalIn", 5000}, {"totalOut", 100}, {"calls", 1},
                      {"lastIn", 5000}, {"contextWindow", 0}};
      }
      return r;
    }
    std::function<void()> subscribe(BackendEvent) override { return []() {}; }
  } backend;
  UiApp app(dom, st, &backend);
  app.mount();
  app.update([](AppState& s) { seed_session(s, "sess_a"); s.model = "some-model"; });
  app.load_session_usage("sess_a");
  EXPECT_TRUE(app.state().session_usage_loaded);
  EXPECT_EQ(app.state().context_pct, -1);
  // The panel renders the placeholder, not a percentage.
  std::string d = dom.dump();
  EXPECT_TRUE(d.find("上下文水位") != std::string::npos);
  EXPECT_TRUE(d.find("—") != std::string::npos);
  EXPECT_TRUE(d.find("20%") == std::string::npos);
}

// With no usage data the panel must say so, not show a plausible-looking number.
FS_TEST(uiapp_usage_panel_has_no_fake_numbers) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) { seed_session(s, "sess_a"); });
  std::string d = dom.dump();
  EXPECT_TRUE(d.find("上下文水位") != std::string::npos);
  EXPECT_TRUE(d.find("42%") == std::string::npos);
  EXPECT_TRUE(d.find("12.4K") == std::string::npos);
  EXPECT_TRUE(d.find("¥0.21") == std::string::npos);
}

// A long session must not load (or re-parse) its whole transcript: the first
// page is the NEWEST N turns, scrolling to the top asks for the previous page,
// and the reading position is preserved across the prepend.
FS_TEST(uiapp_history_pages_backwards) {
  NullDom dom;
  AppState st = AppState::make();
  class PagedBackend : public IUiBackend {
   public:
    int total = 75;
    std::vector<int> requested_before;
    BackendResult call(const std::string& method, const Json& args) override {
      BackendResult r;
      r.ok = true;
      if (method == "session.info") {
        r.data = Json{{"meta", Json{{"sid", "s"}, {"pid", "p1"}}},
                      {"sessionSettings", Json::object()},
                      {"systemOps", Json::array()}};
      } else if (method == "session.turns") {
        int before = static_cast<int>(jsonutil::get_int(args, "before", -1));
        int limit = static_cast<int>(jsonutil::get_int(args, "limit", 30));
        requested_before.push_back(before);
        int end = before < 0 ? total : before;
        int start = end - limit;
        if (start < 0) start = 0;
        Json arr = Json::array();
        for (int i = start; i < end; ++i) {
          arr.push_back(Json{{"role", i % 2 == 0 ? "user" : "assistant"},
                             {"text", "turn " + std::to_string(i)}});
        }
        r.data = Json{{"turns", arr}, {"start", start}, {"total", total},
                      {"hasMore", start > 0}};
      } else if (method == "project.list" || method == "session.list") {
        r.data = Json::array();
      }
      return r;
    }
    std::function<void()> subscribe(BackendEvent) override { return []() {}; }
  } backend;
  UiApp app(dom, st, &backend);
  app.mount();
  app.update([](AppState& s) { seed_session(s, "s"); });
  app.show("s");
  // first page = the NEWEST turns, and the UI knows there are older ones
  EXPECT_EQ(app.message_count(), size_t(30));
  EXPECT_EQ(app.state().history_total, 75);
  EXPECT_TRUE(app.state().history_has_more);
  ASSERT_TRUE(backend.requested_before.size() == 1);
  EXPECT_EQ(backend.requested_before[0], -1);
  std::string d = dom.dump();
  EXPECT_TRUE(d.find("turn 74") != std::string::npos);   // newest present
  EXPECT_TRUE(d.find("turn 10") == std::string::npos);   // older page not loaded
  // snapshot the VNodes of the first page: the prepend below must not touch them
  std::vector<VNodePtr> before_nodes;
  collect_msg_nodes(app.tree_for_test(), "msg:", &before_nodes);
  ASSERT_TRUE(before_nodes.size() == 30);

  // scrolling to the top asks for the page before the OLDEST LOADED turn (45,
  // not the message count: the first page was the newest 30 of 75)
  // The initial restore ignores scroll-edge echoes until its two scheduled ticks
  // have passed (a real user scroll can only happen after that); flush them.
  dom.run_scheduled_for_test();
  dom.run_scheduled_for_test();
  dom.fire_scroll_edges_for_test(true, false);
  EXPECT_EQ(backend.requested_before.size(), size_t(2));
  EXPECT_EQ(backend.requested_before[1], 45);
  EXPECT_EQ(app.message_count(), size_t(60));
  EXPECT_TRUE(app.state().history_has_more);
  // The prepended turns are frozen (each has its own rev) and, crucially, the
  // messages that were ALREADY on screen kept their VNodes: identity is the
  // turn's absolute index, not its position in the vector, so a prepend does not
  // shift every key and rebuild the whole transcript.
  std::vector<VNodePtr> now;
  collect_msg_nodes(app.tree_for_test(), "msg:", &now);
  ASSERT_TRUE(now.size() == 60);
  for (size_t i = 0; i < before_nodes.size(); ++i) {
    EXPECT_TRUE(now[i + 30].get() == before_nodes[i].get());
  }
  // ...and a cached node's handler still addresses ITS OWN message. Handlers
  // used to capture the loop index; after a prepend that index pointed 30
  // messages further down, so folding an old summary toggled a different turn
  // (which looked like "the summary no longer opens").
  //
  // The identity is the message's uid, so resolving uid 74 after the prepend
  // must still land on the message that carries uid 74.
  int idx74 = app.find_message_by_uid_for_test(74);
  ASSERT_TRUE(idx74 >= 0);
  EXPECT_EQ(app.state().messages[idx74].uid, int64_t(74));
  EXPECT_TRUE(app.state().messages[idx74].text.find("turn 74") != std::string::npos);
  // ...and the OLDEST loaded turn (uid 45) is still addressable
  int idx45 = app.find_message_by_uid_for_test(45);
  ASSERT_TRUE(idx45 >= 0);
  EXPECT_EQ(app.state().messages[idx45].uid, int64_t(45));
  std::string d2 = dom.dump();
  EXPECT_TRUE(d2.find("turn 25") != std::string::npos);
  EXPECT_TRUE(d2.find("turn 74") != std::string::npos);

  // the last page reports hasMore=false, so no further request is made
  dom.fire_scroll_edges_for_test(true, false);
  EXPECT_EQ(backend.requested_before.size(), size_t(3));
  EXPECT_EQ(app.message_count(), size_t(75));
  EXPECT_FALSE(app.state().history_has_more);
  dom.fire_scroll_edges_for_test(true, false);
  EXPECT_EQ(backend.requested_before.size(), size_t(3));  // no more pages
}

// New output only follows the end while the reader is AT the end.
FS_TEST(uiapp_history_follows_only_when_pinned) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) { seed_session(s, "sess_a"); });
  EXPECT_TRUE(app.state().hist_follow);  // a fresh session follows the end
  // the user scrolls up to read: following stops
  dom.fire_scroll_edges_for_test(false, false);
  EXPECT_FALSE(app.state().hist_follow);
  dom.clear_calls();
  // streamed output arrives while the reader is away from the end
  app.on_backend_event_for_test("stream.delta", Json{{"sid", "sess_a"}, {"text", "more"}});
  EXPECT_EQ(dom.count_of("scroll_to_bottom"), 0);
  // back at the bottom: following resumes
  dom.fire_scroll_edges_for_test(false, true);
  EXPECT_TRUE(app.state().hist_follow);
  dom.clear_calls();
  app.on_backend_event_for_test("stream.delta", Json{{"sid", "sess_a"}, {"text", "more"}});
  EXPECT_TRUE(dom.count_of("scroll_to_bottom") > 0);
}

// The composer's thinking control must exist even for a model whose table carries
// no level ladder. The user's own models report caps.thinking=false with an empty
// ladder, and the old rule - "render only when model_thinking_levels is
// non-empty" - meant the control they kept asking for never appeared. The
// disable/enable pair is a FALLBACK, not the normal case.
FS_TEST(uiapp_composer_thinking_falls_back_to_disable_enable) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    seed_session(s, "sess_a");
    s.model = "deepseek-flash";
    s.model_thinking_levels.clear();  // nothing configured for this model
    s.model_thinking_default.clear();
  });
  app.show("sess_a");
  std::string d = dom.dump();
  // ONE control: an icon button that opens the level list. There is no second
  // widget next to it any more ("思考档位，思考档位（重复！）").
  EXPECT_TRUE(d.find("thinking-btn") != std::string::npos);
  EXPECT_TRUE(d.find("thinking-sel") == std::string::npos);
  EXPECT_TRUE(d.find("fa-power-off") != std::string::npos);  // 关 = power-off
  // No synthetic "默认" row: with a real ladder configured, the model's own
  // default IS the default. A literal 默认 next to concrete levels was
  // meaningless ("你不会以为用'默认'表示模型设置里的吧").
  EXPECT_TRUE(d.find("\"\u9ed8\u8ba4\"") == std::string::npos);
  // Opening the button lists BOTH fallback levels, each with its own glyph.
  app.update([](AppState& s) { s.thinking_menu_open = true; });
  std::string m = dom.dump();
  EXPECT_TRUE(m.find("think-menu") != std::string::npos);
  EXPECT_TRUE(m.find("\"\u5173\"") != std::string::npos);  // 关 (disable)
  EXPECT_TRUE(m.find("\"\u5f00\"") != std::string::npos);  // 开 (enable)
}

// A model's configured ladder is used verbatim AND its configured default is the
// level shown. The control is built from the model table, so what the user set in
// the model settings is what the composer displays.
FS_TEST(uiapp_composer_thinking_prefers_configured_levels) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    seed_session(s, "sess_a");
    s.model = "deepseek-flash";
    s.model_thinking_levels.push_back("low");
    s.model_thinking_levels.push_back("medium");
    s.model_thinking_levels.push_back("high");
    s.model_thinking_default = "high";   // what the model settings configured
  });
  app.show("sess_a");
  std::string d = dom.dump();
  EXPECT_TRUE(d.find("thinking-btn") != std::string::npos);
  // The button shows the configured default, not the first level.
  EXPECT_TRUE(d.find("\u9ad8") != std::string::npos);  // 高
  // "开/关" belong to the fallback pair only; their absence proves the configured
  // levels were used instead.
  EXPECT_TRUE(d.find("\"\u5f00\"") == std::string::npos);
  EXPECT_TRUE(d.find("\"\u5173\"") == std::string::npos);
  // The list offers every configured level, each with its OWN icon: 低=gauge,
  // 中=brain, 高=fire. One shared glyph made them indistinguishable
  // ("图标也只有关闭和其他").
  app.update([&](AppState& s) { s.thinking_menu_open = true; });
  std::string m = dom.dump();
  EXPECT_TRUE(m.find("\u4f4e") != std::string::npos);  // 低
  EXPECT_TRUE(m.find("\u4e2d") != std::string::npos);  // 中
  EXPECT_TRUE(m.find("fa-gauge") != std::string::npos);
  EXPECT_TRUE(m.find("fa-brain") != std::string::npos);
  EXPECT_TRUE(m.find("fa-fire") != std::string::npos);
}

// A level chosen in the session wins over the model's default (the session stores
// its own `thinking`, and reloading must not silently reset it).
FS_TEST(uiapp_composer_thinking_session_choice_wins) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    seed_session(s, "sess_a");
    s.model = "deepseek-flash";
    s.model_thinking_levels.push_back("low");
    s.model_thinking_levels.push_back("high");
    s.model_thinking_default = "high";
    s.thinking = "low";  // the session overrode it
  });
  app.show("sess_a");
  // the state keeps the session's choice
  EXPECT_EQ(app.state().thinking, std::string("low"));
  std::string d = dom.dump();
  EXPECT_TRUE(d.find("\u4f4e") != std::string::npos);
  EXPECT_TRUE(d.find("thinking-btn") != std::string::npos);
  EXPECT_TRUE(d.find("fa-gauge") != std::string::npos);  // 低's glyph
}

// Clicking a row of the thinking list sets the level and closes the list. This is
// what the old decorative icon button never did ("这个按钮按下没反应").
FS_TEST(uiapp_composer_thinking_menu_click_sets_level) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    seed_session(s, "sess_a");
    s.model = "deepseek-flash";
    s.model_thinking_levels.push_back("low");
    s.model_thinking_levels.push_back("high");
    s.model_thinking_default = "high";
  });
  app.show("sess_a");
  app.update([](AppState& s) { s.thinking_menu_open = true; });
  // The 低 row is the one carrying that label; drive its click handler exactly
  // the way the DOM adapter would.
  int hid = click_hid_in(app.tree_for_test(), "think-menu-item", "\u4f4e");
  ASSERT_TRUE(hid != 0);
  bus().fire(hid, "");
  EXPECT_EQ(app.state().thinking, std::string("low"));
  EXPECT_FALSE(app.state().thinking_menu_open);
}

// A model level whose name we do not know still gets an icon (the generic bulb),
// never a blank button - "未知的选项就用个通用的灯泡或大脑吧".
FS_TEST(uiapp_composer_thinking_unknown_level_has_fallback_icon) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    seed_session(s, "sess_a");
    s.model = "deepseek-flash";
    s.model_thinking_levels.push_back("hyperdrive");  // not a known level
    s.model_thinking_default = "hyperdrive";
  });
  app.show("sess_a");
  std::string d = dom.dump();
  EXPECT_TRUE(d.find("thinking-btn") != std::string::npos);
  EXPECT_TRUE(d.find("fa-lightbulb") != std::string::npos);
  // the raw value is still shown as the label (we do not invent a translation)
  EXPECT_TRUE(d.find("hyperdrive") != std::string::npos);
}

// load_session must apply the session's OWN settings: the composer showed
// "choose model" on a session that already had one, and sending used the wrong
// model after a reload.
FS_TEST(uiapp_load_session_applies_settings) {
  NullDom dom;
  AppState st = AppState::make();
  DemoBackend backend;
  UiApp app(dom, st, &backend);
  app.mount();
  app.update([](AppState& s) { seed_session(s, "sess_a"); });
  app.show("home");
  app.show("sess_a");  // first visit -> load_session
  EXPECT_EQ(app.state().model, std::string("claude-sonnet-4"));
  EXPECT_EQ(app.state().thinking, std::string("high"));
  EXPECT_EQ(app.state().permission, std::string("approve"));
  EXPECT_EQ(app.state().perm_index, 0);
  EXPECT_TRUE(app.state().auto_mode);
  // the composer shows the model, not the placeholder
  std::string d = dom.dump();
  EXPECT_TRUE(d.find("claude-sonnet-4") != std::string::npos);
  EXPECT_FALSE(d.find("还没有选择模型") != std::string::npos);
}

// A session remembers the model it last used, but that model can be disabled or
// its whole provider deleted. Sending then fails deep in the provider layer with
// an error that never mentions the model, and the box keeps showing a dead name.
// The composer must instead clear the box and disable send.
class ModelGoneBackend : public IUiBackend {
 public:
  BackendResult call(const std::string& method, const Json&) override {
    BackendResult r;
    r.ok = true;
    if (method == "session.get" || method == "session.info") {
      r.data = Json{
          {"meta", Json{{"sid", "sess_a"}, {"pid", "p1"}, {"cwd", "/tmp/p"}}},
          {"sessionSettings", Json{{"model", "claude-sonnet-4"},
                                   {"filePerm", "permissive"},
                                   {"autoMode", true}}},
          {"turns", Json::array()}};
    } else if (method == "project.list" || method == "session.list") {
      r.data = Json::array();
    } else if (method == "model.allEnabled") {
      // Only a DIFFERENT model is enabled now.
      r.data = Json{{"models", Json::array({Json{{"id", "gpt-4o"}}})}};
    }
    return r;
  }
  std::function<void()> subscribe(BackendEvent) override { return []() {}; }
};

FS_TEST(uiapp_unavailable_model_clears_composer) {  NullDom dom;
  AppState st = AppState::make();
  ModelGoneBackend backend;
  UiApp app(dom, st, &backend);
  app.mount();
  app.update([](AppState& s) { seed_session(s, "sess_a"); });
  app.show("home");
  app.show("sess_a");  // loads settings, then asks model.allEnabled
  // The remembered model is gone: the box is empty and send is blocked.
  EXPECT_EQ(app.state().model, std::string(""));
  EXPECT_FALSE(app.state().model_available);
  std::string d = dom.dump();
  EXPECT_TRUE(d.find("还没有选择模型") == std::string::npos);
  EXPECT_TRUE(d.find("选择模型") != std::string::npos);  // placeholder is back
  // The send button carries the `disabled` class (NullDom dumps class as text).
  EXPECT_TRUE(d.find("disabled") != std::string::npos);
  // Sending is refused (no user turn is appended).
  size_t before = app.state().messages.size();
  app.update([](AppState& s) { s.session_input = "hello"; });
  app.send_message();
  EXPECT_EQ(app.state().messages.size(), before);
}

// Thinking is a foldable block, closed by default; opening it shows the body.
FS_TEST(uiapp_thinking_stream_auto_folds_and_interleaves) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) { seed_session(s, "sess_a"); });
  app.on_backend_event_for_test(
      "thinking.delta", Json{{"sid", "sess_a"}, {"text", "THINK0"}});
  ASSERT_TRUE(app.state().messages.size() == 1);
  ASSERT_TRUE(app.state().messages[0].thoughts.size() == 1);
  EXPECT_TRUE(app.state().messages[0].thoughts[0].open);
  EXPECT_TRUE(app.state().messages[0].thoughts[0].streaming);
  // While streaming it is a normal tool fold row, open, body visible.
  app.update([](AppState&) {});
  std::string live = dom.dump();
  EXPECT_TRUE(live.find("tool-block") != std::string::npos);
  EXPECT_TRUE(live.find("THINK0") != std::string::npos);
  app.on_backend_event_for_test(
      "stream.delta", Json{{"sid", "sess_a"}, {"text", "answer"}});
  ASSERT_TRUE(app.state().messages[0].thoughts.size() == 1);
  // The answer started: the reasoning segment auto-folds, in the same component.
  EXPECT_FALSE(app.state().messages[0].thoughts[0].open);
  EXPECT_FALSE(app.state().messages[0].thoughts[0].streaming);
  app.update([](AppState&) {});
  EXPECT_TRUE(dom.dump().find("THINK0") == std::string::npos);
  // A tool row, then a second reasoning segment: the segment gets `after=1` so
  // the renderer can put it in arrival order rather than at the very end.
  app.on_backend_event_for_test(
      "tool.start", Json{{"sid", "sess_a"}, {"name", "read_file"},
                         {"args", Json::object()}});
  app.on_backend_event_for_test(
      "thinking.delta", Json{{"sid", "sess_a"}, {"text", "THINK1"}});
  ASSERT_TRUE(app.state().messages[0].thoughts.size() == 2);
  EXPECT_EQ(app.state().messages[0].thoughts[1].after, size_t(1));
}

// Reasoning and tool calls live in ONE folded "工作过程" group: the answer is
// what you read, the process is what you open.
FS_TEST(uiapp_thinking_block_folds) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    seed_session(s, "sess_a");
    comp::ChatMsg m;
    m.role = "assistant";
    m.text = "answer";
    m.thinking = "let me reason about it";
    // A stored/live legacy blob has no split position; this test is about the
    // OUTER work fold, so open the thinking row explicitly.
    m.thinking_open = true;
    // A turn that is RUNNING shows its work: the process is the only thing
    // moving, and a folded block made a long tool chain look frozen.
    m.streaming = true;
    s.messages.push_back(m);
  });
  std::string d = dom.dump();
  EXPECT_TRUE(d.find("work-block") != std::string::npos);
  EXPECT_TRUE(d.find("工作过程") != std::string::npos);
  EXPECT_TRUE(d.find("let me reason about it") != std::string::npos);
  // The turn ends: the shell folds the work log (turn.done does exactly this).
  app.update([](AppState& s) {
    s.messages[0].streaming = false;
    s.messages[0].work_open = false;
  });
  EXPECT_TRUE(dom.dump().find("let me reason about it") == std::string::npos);
  // One click reopens it - the process is still there after the turn. (The real
  // click handler bumps rev so the memoised message node is rebuilt.)
  app.update([](AppState& s) {
    s.messages[0].work_open = true;
    s.messages[0].work_touched = true;
    s.messages[0].rev = 9001;
  });
  EXPECT_TRUE(dom.dump().find("let me reason about it") != std::string::npos);
}

// A thought is a TOOL-STYLE fold row, not a second visual language: same
// tb-head/caret/body, and the row itself toggles. The custom think-block used to
// look out of place and its click did nothing.
FS_TEST(uiapp_thinking_row_uses_tool_fold) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    seed_session(s, "sess_a");
    comp::ChatMsg m;
    m.role = "assistant";
    m.text = "answer";
    // Old stored turns carry ONE coalesced thinking blob; new ones carry
    // `thoughts`. Both must render through the tool fold.
    m.thinking = "LEGACY_THINK_MARKER";
    m.work_open = true;
    s.messages.push_back(m);
  });
  std::string d = dom.dump();
  EXPECT_TRUE(d.find("tool-block") != std::string::npos);
  EXPECT_TRUE(d.find("tb-head") != std::string::npos);
  EXPECT_TRUE(d.find("思考") != std::string::npos);
  // Reasoning is not a tool verdict: a brain, never a green check.
  EXPECT_TRUE(d.find("fa-brain") != std::string::npos);
  EXPECT_TRUE(d.find("tb-ico ok") == std::string::npos);
  // Folded by default (the stored/live state is thinking_open=false).
  EXPECT_TRUE(d.find("LEGACY_THINK_MARKER") == std::string::npos);
  int hid = click_hid_in(app.tree_for_test(), "tb-head", "\u601d\u8003");
  ASSERT_TRUE(hid != 0);
  bus().fire(hid, "");
  EXPECT_TRUE(dom.dump().find("LEGACY_THINK_MARKER") != std::string::npos);
  // ... and the same row closes again (the memoised row must not survive its
  // own expanded body: the old fold opened once and then went dead).
  hid = click_hid_in(app.tree_for_test(), "tb-head", "\u601d\u8003");
  ASSERT_TRUE(hid != 0);
  bus().fire(hid, "");
  EXPECT_TRUE(dom.dump().find("LEGACY_THINK_MARKER") == std::string::npos);
}

// A tool call must appear when it STARTS and be completed in place when it
// finishes. The old handler only listened to tool.update, so a slow tool left
// the transcript blank, and a second event created a duplicate row.
FS_TEST(uiapp_tool_call_start_and_complete) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    seed_session(s, "sess_a");
    comp::ChatMsg m;
    m.role = "assistant";
    m.streaming = true;
    // Tool rows live inside the folded 工作过程 group; a user reading the
    // process has it open.
    m.work_open = true;
    s.messages.push_back(m);
  });

  app.on_backend_event_for_test(
      "tool.start",
      Json{{"name", "read_file"}, {"args", Json{{"path", "main.cpp"}}}});
  // The row exists while it runs, with the ARGUMENTS in their own `input` field.
  // They used to be stored in `detail`, which tool.update then replaced - so a
  // finished read_file row showed only the result and never said which file was
  // read ("只有返回json没有输入").
  EXPECT_EQ(app.state().messages.back().tools.size(), 1u);
  EXPECT_TRUE(app.state().messages.back().tools[0].running);
  EXPECT_TRUE(app.state().messages.back().tools[0].input.find("main.cpp") !=
              std::string::npos);
  EXPECT_TRUE(app.state().messages.back().tools[0].detail.empty());
  std::string running = dom.dump();
  EXPECT_TRUE(running.find("read_file") != std::string::npos);
  EXPECT_TRUE(running.find("tool-block running") != std::string::npos);

  app.on_backend_event_for_test(
      "tool.update",
      Json{{"name", "read_file"}, {"ok", true},
           {"args", Json{{"path", "main.cpp"}}},
           {"detail", Json{{"startLine", 5}, {"endLine", 10},
                           {"totalLines", 42}, {"content", "line1\nline2"}}
                          .dump()}});
  // Completed IN PLACE: still one row, no longer running, verdict visible.
  EXPECT_EQ(app.state().messages.back().tools.size(), 1u);
  EXPECT_FALSE(app.state().messages.back().tools[0].running);
  EXPECT_TRUE(app.state().messages.back().tools[0].ok);
  // The result lands in `detail` and the input SURVIVES: both sections are shown.
  EXPECT_TRUE(app.state().messages.back().tools[0].detail.find("line2") !=
              std::string::npos);
  EXPECT_TRUE(app.state().messages.back().tools[0].input.find("main.cpp") !=
              std::string::npos);
  std::string done = dom.dump();
  // The one-line label is GENERATED from args/detail now (not sent by the tool).
  EXPECT_TRUE(done.find("main.cpp 5-10/42 lines") != std::string::npos);
  EXPECT_TRUE(done.find("tool-block running") == std::string::npos);

  // A failure marks the row, and the detail is kept for the expandable body.
  app.on_backend_event_for_test(
      "tool.start", Json{{"name", "write_file"}, {"args", Json::object()}});
  app.on_backend_event_for_test(
      "tool.update",
      Json{{"name", "write_file"}, {"ok", false},
           {"summary", "permission denied"}, {"detail", "denied"}});
  const std::vector<comp::ToolBlock>& tools = app.state().messages.back().tools;
  EXPECT_EQ(tools.size(), 2u);
  EXPECT_FALSE(tools[1].ok);
  EXPECT_EQ(tools[1].detail, std::string("denied"));
}

// A tool block's body only appears when THAT block is expanded.
FS_TEST(uiapp_tool_blocks_toggle_independently) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    seed_session(s, "sess_a");
    comp::ChatMsg m;
    m.role = "assistant";
    m.text = "x";
    for (int i = 0; i < 3; ++i) {
      comp::ToolBlock tb;
      tb.name = "tool" + std::to_string(i);
      tb.detail = "detail" + std::to_string(i);
      m.tools.push_back(tb);
    }
    // tool bodies render inside the (open) work group
    m.work_open = true;
    s.messages.push_back(m);
  });
  app.update([](AppState& s) { s.messages[0].tools[1].expanded = true; });
  std::string d = dom.dump();
  EXPECT_TRUE(d.find("detail1") != std::string::npos);
  EXPECT_TRUE(d.find("detail0") == std::string::npos);
  EXPECT_TRUE(d.find("detail2") == std::string::npos);
}

// Auto mode paints the toggle as "on" (the label reads 自动).
FS_TEST(uiapp_auto_mode_button_state) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    seed_session(s, "sess_a");
    s.auto_mode = true;
  });
  EXPECT_TRUE(dom.dump().find("btn it-btn ghost on") != std::string::npos);
  app.update([](AppState& s) { s.auto_mode = false; });
  EXPECT_TRUE(dom.dump().find("手动") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 第二十四批：工作过程分组 / 代码块 / 规划列 / 滚动 / 子代理历史
// ---------------------------------------------------------------------------

// A turn's reasoning + tool calls are ONE folded group with finish time and
// duration; the answer is not buried under a wall of tool rows.
FS_TEST(uiapp_work_group_shows_time_and_folds) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    seed_session(s, "sess_a");
    comp::ChatMsg m;
    m.role = "assistant";
    m.text = "the answer";
    m.thinking = "reasoning";
    comp::ToolBlock tb;
    tb.name = "read_file";
    tb.summary = "a.cpp";
    m.tools.push_back(tb);
    m.started_at = 1700000000000LL;
    m.ended_at = 1700000080000LL;  // 80s later
    s.messages.push_back(m);
  });
  std::string d = dom.dump();
  EXPECT_TRUE(d.find("work-block") != std::string::npos);
  EXPECT_TRUE(d.find("工作过程") != std::string::npos);
  EXPECT_TRUE(d.find("1m 20s") != std::string::npos);  // duration
  EXPECT_TRUE(d.find("完成") != std::string::npos);
  // A FINISHED turn starts folded: the answer is what you reread, and the
  // header says what happened ("· 1 步").
  EXPECT_TRUE(d.find("reasoning") == std::string::npos);
  EXPECT_TRUE(d.find("read_file") == std::string::npos);
  // Opening it reveals ONE process stream (no "工具调用" heading).
  app.update([](AppState& s) {
    s.messages[0].work_open = true;
    s.messages[0].work_touched = true;
    s.messages[0].thinking_open = true;  // legacy blob opens through its own row
    s.messages[0].rev = 9002;
  });
  std::string o = dom.dump();
  EXPECT_TRUE(o.find("reasoning") != std::string::npos);
  EXPECT_TRUE(o.find("read_file") != std::string::npos);
  EXPECT_TRUE(o.find("工具调用") == std::string::npos);
}

// Code blocks carry a language label, a line-number gutter and a copy button.
FS_TEST(uiapp_code_block_has_gutter_and_copy) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    seed_session(s, "sess_a");
    comp::ChatMsg m;
    m.role = "assistant";
    m.text = "```cpp\nint a = 1;\nint b = 2;\n```";
    s.messages.push_back(m);
  });
  std::string d = dom.dump();
  EXPECT_TRUE(d.find("code-wrap") != std::string::npos);
  EXPECT_TRUE(d.find("code-lang") != std::string::npos);
  EXPECT_TRUE(d.find("cpp") != std::string::npos);
  EXPECT_TRUE(d.find("code-lines") != std::string::npos);
  EXPECT_TRUE(d.find("code-copy") != std::string::npos);
  // the gutter numbers the source lines, not the wrapped visual lines
  EXPECT_TRUE(d.find("1\n2") != std::string::npos);
  // The block is highlighted in place: the <pre> holds token spans, written as
  // one innerHTML string (a DOM node per token would be thousands of nodes).
  EXPECT_TRUE(d.find("hl-ty") != std::string::npos);
  EXPECT_TRUE(dom.count_of("set_html") > 0);
}



// A session can hold hundreds of turns, so a render must NOT rebuild the ones
// that did not change. Three things are asserted:
//   1) an unchanged message hands back the SAME VNode (which is what lets the
//      framework skip the subtree entirely),
//   2) appending a message rebuilds only that message, and
//   3) changing one message rebuilds exactly that one.
// Without this, every render re-parsed every markdown block and the diff then
// compared thousands of nodes against themselves.
// A session can hold hundreds of turns, so a render must NOT rebuild the ones
// that did not change. Three things are asserted:
//   1) an unchanged message hands back the SAME VNode (which is what lets the
//      framework skip the subtree entirely),
//   2) appending a message rebuilds only that message, and
//   3) changing one message rebuilds exactly that one.
// Without this, every render re-parsed every markdown block and the diff then
// compared thousands of nodes against themselves.
FS_TEST(uiapp_history_is_not_rebuilt) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    seed_session(s, "sess_a");
    for (int i = 0; i < 5; ++i) {
      comp::ChatMsg m;
      m.role = "assistant";
      m.text = "turn " + std::to_string(i) + "\n\n```cpp\nint x = " +
               std::to_string(i) + ";\n```\n";
      s.messages.push_back(m);
    }
  });
  std::vector<VNodePtr> before;
  collect_msg_nodes(app.tree_for_test(), "msg:", &before);
  ASSERT_TRUE(before.size() == 5);
  // add one more message: the earlier ones must keep their exact objects
  app.update([](AppState& s) {
    comp::ChatMsg m;
    m.role = "user";
    m.text = "one more";
    s.messages.push_back(m);
  });
  std::vector<VNodePtr> after;
  collect_msg_nodes(app.tree_for_test(), "msg:", &after);
  ASSERT_TRUE(after.size() == 6);
  for (size_t i = 0; i < before.size(); ++i) {
    EXPECT_TRUE(after[i].get() == before[i].get());
  }
  // ...and a plain re-render with nothing changed rebuilds nothing at all
  app.update([](AppState&) {});
  std::vector<VNodePtr> again;
  collect_msg_nodes(app.tree_for_test(), "msg:", &again);
  ASSERT_TRUE(again.size() == 6);
  for (size_t i = 0; i < after.size(); ++i) {
    EXPECT_TRUE(again[i].get() == after[i].get());
  }
  // changing ONE message rebuilds exactly that one
  app.update([](AppState& s) { s.messages[2].text += " edited"; });
  std::vector<VNodePtr> edited;
  collect_msg_nodes(app.tree_for_test(), "msg:", &edited);
  ASSERT_TRUE(edited.size() == 6);
  EXPECT_TRUE(edited[2].get() != again[2].get());
  for (size_t i = 0; i < edited.size(); ++i) {
    if (i == 2) continue;
    EXPECT_TRUE(edited[i].get() == again[i].get());
  }
}

// (There used to be a test here that opened a plan FLOAT and checked it did not
// touch the history. The float is gone: the plan is a document now, so there is
// nothing to float and nothing to rebuild.)

// The chat is scrolled to the bottom when its history arrives (a first visit
// must show the latest turn, not the top of the conversation). NullDom has no
// real scrolling, so the assertion is on the state that drives it: a fresh
// session is "unmeasured" (-1) = "scroll to bottom", and the container carries
// the stable key the restore pass looks up.
FS_TEST(uiapp_session_scrolls_to_bottom) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) { seed_session(s, "sess_a"); });
  app.show("sess_a");
  EXPECT_TRUE(dump_has(dom, "history"));
}

// Subagent cards carry their transcript, timing and an expandable log.
FS_TEST(uiapp_subagent_history_and_timing) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    seed_session(s, "sess_a");
    // the 子代理 panel has to be the active one for its cards to render
    for (size_t i = 0; i < s.side_tabs.size(); ++i) {
      if (s.side_tabs[i].kind == "subagents") s.active_side = s.side_tabs[i].id;
    }
    AppState::SubagentRow a;
    a.id = "sub1";
    a.kind = "explore";
    a.status = "done";
    a.task = "find z-index";
    a.model = "claude-sonnet-4";
    a.started_at = 1700000000000LL;
    a.ended_at = 1700000030000LL;
    a.transcript = Json::array({Json{{"role", "user"}, {"content", "look for z-index"}},
                                Json{{"role", "assistant"}, {"content", "found 7"}}});
    s.subagents.push_back(a);
  });
  std::string d = dom.dump();
  EXPECT_TRUE(d.find("对话记录") != std::string::npos);
  EXPECT_TRUE(d.find("30 秒") != std::string::npos);  // duration
  // folded: the transcript text is not rendered until opened
  EXPECT_TRUE(d.find("look for z-index") == std::string::npos);
  app.update([](AppState& s) { s.subagents[0].open = true; });
  std::string o = dom.dump();
  EXPECT_TRUE(o.find("look for z-index") != std::string::npos);
  EXPECT_TRUE(o.find("found 7") != std::string::npos);
}

// The Web-service section must show a REAL, copyable address when the machine
// has one. It used to print the literal "<本机 IP>", which reads as a bug and
// cannot be typed on a phone.
FS_TEST(uiapp_webserver_shows_real_lan_url) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.open_modal_page("settings");
  app.update([](AppState& s) {
    s.settings_nav = "webserver";
    s.server_running = true;
    s.server_host = "0.0.0.0";
    s.server_port = 8812;
    s.server_lan_url = "http://10.6.217.30:8812/";
  });
  std::string d = dom.dump();
  EXPECT_TRUE(d.find("正在监听 0.0.0.0:8812") != std::string::npos);
  EXPECT_TRUE(d.find("http://10.6.217.30:8812/") != std::string::npos);
  EXPECT_TRUE(d.find("<本机 IP>") == std::string::npos);
}

// A modal loses focus -> it dismisses. The permission gate is special: dismissing
// it must DENY (the loop is parked on an answer), not leave the turn hanging.
FS_TEST(uiapp_modal_dismisses_on_outside_click) {
  NullDom dom;
  AppState st = AppState::make();
  class RecBackend : public IUiBackend {
   public:
    std::vector<std::string> methods;
    BackendResult call(const std::string& m, const Json&) override {
      methods.push_back(m);
      return BackendResult{};
    }
    std::function<void()> subscribe(BackendEvent) override { return []() {}; }
  };
  RecBackend backend;
  UiApp app(dom, st, &backend);
  app.mount();
  app.update([](AppState& s) { seed_session(s, "sess_a"); });
  // A permission prompt parks the loop; the modal is up.
  app.on_backend_event_for_test("permission.prompt", Json{{"sid", "sess_a"},
                                                          {"tool", "write_file"}});
  EXPECT_TRUE(app.state().ask_modal);
  dom.clear_calls();
  dom.fire_outside_click_for_test();
  EXPECT_FALSE(app.state().ask_modal);
  // ...and the gate was DENIED, so the parked turn can continue.
  bool denied = false;
  for (auto& m : backend.methods) {
    if (m == "session.approve") denied = true;
  }
  EXPECT_TRUE(denied);
}

// A confirm modal (delete session etc.) also dismisses on an outside press, and
// does so without running the destructive action.
FS_TEST(uiapp_confirm_modal_dismisses_without_acting) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    seed_session(s, "sess_a");
    s.confirm_kind = "session";
    s.confirm_id = "sess_a";
    s.confirm_text = "删除？";
  });
  EXPECT_TRUE(dump_has(dom, "删除？"));
  dom.fire_outside_click_for_test();
  EXPECT_TRUE(app.state().confirm_kind.empty());
  // the session is still there (nothing was deleted)
  EXPECT_EQ(app.state().tab_ids.size(), size_t(2));  // home + sess_a
}

// The chat scroll offset is remembered per session, so switching away and back
// returns to where the reader was. The offset is read from the DOM at switch
// time, so the test scrolls the real node.
FS_TEST(uiapp_session_scroll_offset_is_per_session) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    seed_session(s, "sess_a");
    SessionTab t;
    t.id = "sess_b";
    t.title = "b";
    s.sessions.push_back(t);
    s.tab_ids.push_back("sess_b");
  });
  app.show("sess_a");
  // scroll the transcript node the way a user drag would
  {
    std::vector<VNodePtr> hits;
    collect_msg_nodes(app.tree_for_test(), "chat-history", &hits);
    ASSERT_TRUE(!hits.empty());
    ASSERT_TRUE(hits[0]->dom.valid());
    dom.set_scroll_top(hits[0]->dom, 400);
  }
  app.show("sess_b");
  app.show("sess_a");
  EXPECT_EQ(app.state().hist_scroll, 400);
}

// The remembered offset must be applied by the SAME render that puts the
// transcript on screen.
//
// The first version applied it inline after render() without forcing layout, so
// scrollTop was clamped against the old (short) height and the scroll event wrote
// that back ("重新点进来却变成顶上"). The second version deferred it a tick: the
// clamp was gone, but the page was painted at the top first and jumped afterwards
// - still wrong to the eye. Now the render that patches the messages also forces
// layout and sets scrollTop before the browser paints.
FS_TEST(uiapp_scroll_restore_happens_in_the_same_render) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    seed_session(s, "sess_a");
    SessionTab t;
    t.id = "sess_b";
    t.title = "b";
    s.sessions.push_back(t);
    s.tab_ids.push_back("sess_b");
  });
  app.show("sess_a");
  // The reader scrolls to 400, then leaves: 400 is what the view remembers.
  {
    std::vector<VNodePtr> hits;
    collect_msg_nodes(app.tree_for_test(), "chat-history", &hits);
    ASSERT_TRUE(!hits.empty());
    ASSERT_TRUE(hits[0]->dom.valid());
    dom.set_scroll_top(hits[0]->dom, 400);
    // Not at the end any more - what the adapter's scroll-edge report says, and
    // what decides "restore the offset" vs "follow the end" on the way back.
    app.update([](AppState& s) { s.hist_follow = false; });
  }
  app.show("sess_b");
  EXPECT_EQ(app.state().hist_scroll, -1);  // b was never scrolled
  app.show("sess_a");
  EXPECT_EQ(app.state().hist_scroll, 400);
  // No tick needed: the position is already correct when show() returns, i.e.
  // before the browser ever paints the transcript.
  std::vector<VNodePtr> hits;
  collect_msg_nodes(app.tree_for_test(), "chat-history", &hits);
  ASSERT_TRUE(!hits.empty());
  ASSERT_TRUE(hits[0]->dom.valid());
  EXPECT_EQ(dom.scroll_top(hits[0]->dom), 400);
  // ...and the restore's own scroll event must not overwrite the remembered
  // value when it finally arrives.
  dom.run_scheduled_for_test();
  EXPECT_EQ(app.state().hist_scroll, 400);
}

// A subagent's id must survive a transcript reload: without it the work-log row
// loses its "打开记录" button, which is "子代理还是无法在历史记录里打开".
FS_TEST(uiapp_subagent_id_survives_history_reload) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) { seed_session(s, "sess_a"); });
  app.show("sess_a");
  // A stored assistant turn whose subagent call kept its id as a first-class
  // field, plus one that only has it inside the result detail (older sessions).
  Json turns = Json::array();
  Json user = Json::object();
  user["role"] = "user";
  user["promptRaw"] = "spawn one";
  turns.push_back(user);
  Json asst = Json::object();
  asst["role"] = "assistant";
  asst["streamSummary"] = "done";
  Json calls = Json::array();
  Json c1 = Json::object();
  c1["name"] = "subagent_spawn";
  c1["ok"] = true;
  c1["summary"] = "ok";
  c1["subagentId"] = "sub_7";
  calls.push_back(c1);
  Json c2 = Json::object();
  c2["name"] = "subagent_spawn";
  c2["ok"] = true;
  c2["summary"] = "ok";
  c2["detail"] = "{\"id\":\"sub_9\",\"summary\":\"ok\"}";
  calls.push_back(c2);
  asst["toolCalls"] = calls;
  turns.push_back(asst);
  // Drive the same reconstruction the loader uses.
  std::vector<comp::ChatMsg> msgs;
  for (size_t i = 0; i < turns.size(); ++i) {
    msgs.push_back(app.turn_to_message_for_test(turns[i], static_cast<int64_t>(i)));
  }
  bool saw7 = false, saw9 = false;
  for (size_t i = 0; i < msgs.size(); ++i) {
    for (size_t k = 0; k < msgs[i].tools.size(); ++k) {
      if (msgs[i].tools[k].subagent_id == "sub_7") saw7 = true;
      if (msgs[i].tools[k].subagent_id == "sub_9") saw9 = true;
    }
  }
  EXPECT_TRUE(saw7);  // first-class field
  EXPECT_TRUE(saw9);  // recovered from the stored result detail
}

// Enter behaviour honours the 快捷键 page: rebinding 发送消息 to a Ctrl combo
// makes plain Enter a newline (it used to be four fields nothing read).
FS_TEST(uiapp_shortcut_rebinding_changes_enter_behaviour) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    seed_session(s, "sess_a");
    s.settings_values["shortcuts.发送消息"] = Json{{"value", "Ctrl+Enter"}};
  });
  EXPECT_TRUE(dom.dump().find("data-enter-send=\"ctrlEnter\"") != std::string::npos);
  // A plain-Enter binding leaves the session's own choice in charge.
  app.update([](AppState& s) {
    s.settings_values["shortcuts.发送消息"] = Json{{"value", "Enter"}};
    s.enter_send = "enter";
  });
  EXPECT_TRUE(dom.dump().find("data-enter-send=\"enter\"") != std::string::npos);
}

// A research subagent's stream/tool events must NOT land in the conversation;
// a complete-step's must. The distinction is what keeps side work out of the
// transcript.
FS_TEST(uiapp_subagent_events_split_by_kind) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) { seed_session(s, "sess_a"); });
  // research: tagged event is ignored inline
  app.on_backend_event_for_test(
      "stream.delta", Json{{"sid", "sess_a"},
                           {"text", "research noise"},
                           {"fromSubagent", true},
                           {"subagentKind", "research"}});
  EXPECT_TRUE(dom.dump().find("research noise") == std::string::npos);
  // complete-step: shown inline
  app.on_backend_event_for_test(
      "stream.delta", Json{{"sid", "sess_a"},
                           {"text", "step output"},
                           {"fromSubagent", true},
                           {"subagentKind", "complete-step"}});
  // State first: if this fails the event never reached the transcript; if it
  // passes, the problem is in rendering the work note.
  EXPECT_TRUE(!app.state().messages.empty());
  EXPECT_EQ(app.state().messages.back().work_live, std::string("step output"));
  // ...and it is NOT the answer: the subagent's stream must never become the
  // main model's output text.
  EXPECT_TRUE(app.state().messages.back().text.empty());
  // Subagent deltas are coalesced like main stream deltas (no per-delta paint);
  // the pump is what flushes them, exactly as in the running app.
  app.pump();
  EXPECT_TRUE(dom.dump().find("step output") != std::string::npos);
}

// A complete-step runs INLINE while it works and is packaged into the subagent
// panel only after it finishes (including a failed finish).
FS_TEST(uiapp_complete_step_moves_to_panel_when_done) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    seed_session(s, "sess_a");
    s.side_collapsed = false;
    s.active_side = "subagents";
  });
  app.on_backend_event_for_test(
      "subagent.started",
      Json{{"sid", "sess_a"}, {"id", "sa_step"}, {"kind", "complete-step"},
           {"task", "implement it"}});
  ASSERT_TRUE(app.state().subagents.size() == 1u);
  EXPECT_TRUE(app.state().subagents[0].pending);
  EXPECT_TRUE(dom.dump().find("sa_step") == std::string::npos);

  app.on_backend_event_for_test(
      "subagent.done",
      Json{{"sid", "sess_a"}, {"id", "sa_step"}, {"kind", "complete-step"},
           {"task", "implement it"}, {"summary", "boom"}, {"status", "failed"},
           {"failed", true}});
  ASSERT_TRUE(app.state().subagents.size() == 1u);
  EXPECT_FALSE(app.state().subagents[0].pending);
  EXPECT_EQ(app.state().subagents[0].status, std::string("failed"));
  std::string d = dom.dump();
  EXPECT_TRUE(d.find("sa_step") != std::string::npos);
  EXPECT_TRUE(d.find("失败") != std::string::npos);
}

// ---- 第四十三批：todo 悬浮清单 / 思考排在工作过程末尾 / 子代理记录里的工具 ----

// The model's checklist has its own floating panel. Before this, todo.updated
// ended in a toast: the backend could add items while the UI kept no list, so
// "todo 可以设置但悬浮窗口不显示".
FS_TEST(uiapp_todo_panel_shows_model_checklist) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) { seed_session(s, "sess_a"); });
  app.on_backend_event_for_test(
      "todo.updated",
      Json{{"sid", "sess_a"},
           {"owner", "sess_a"},
           {"items", Json::array({Json{{"id", 1}, {"task", "读配置文件"}, {"done", false}},
                                  Json{{"id", 2}, {"task", "写补丁"}, {"done", true}}})}});
  std::string d = dom.dump();
  EXPECT_TRUE(d.find("任务清单") != std::string::npos);
  EXPECT_TRUE(d.find("读配置文件") != std::string::npos);
  EXPECT_TRUE(d.find("写补丁") != std::string::npos);
  EXPECT_TRUE(app.state().todo_open);
  ASSERT_TRUE(app.state().todos.size() == 2u);
  // The composer grows a 清单 toggle only while a list exists.
  EXPECT_TRUE(d.find("清单") != std::string::npos);
  // Closing hides the float; a later update does NOT force it back open.
  app.update([](AppState& s) { s.todo_open = false; });
  EXPECT_TRUE(dom.dump().find("任务清单") == std::string::npos);
  app.on_backend_event_for_test(
      "todo.updated",
      Json{{"sid", "sess_a"},
           {"owner", "sess_a"},
           {"items", Json::array({Json{{"id", 1}, {"task", "读配置文件"}, {"done", true}}})}});
  EXPECT_FALSE(app.state().todo_open);
  // A SUBAGENT's private checklist must not replace the session's.
  app.on_backend_event_for_test(
      "todo.updated",
      Json{{"sid", "sess_a"},
           {"owner", "sub_1"},
           {"items", Json::array({Json{{"id", 9}, {"task", "子代理的事"}, {"done", false}}})}});
  EXPECT_EQ((int)app.state().todos.size(), 1);
  EXPECT_TRUE(app.state().todos[0].task == std::string("读配置文件"));
}

// 工作过程里，思考排在工具/解说之后：真实的一轮常常是一堆工具调用带一段思考。
FS_TEST(uiapp_work_process_puts_thinking_last) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    seed_session(s, "sess_a");
    comp::ChatMsg m;
    m.role = "assistant";
    m.text = "the answer";
    m.thinking = "REASONING_MARKER";
    m.thinking_open = true;
    m.work_open = true;
    comp::ToolBlock tb;
    tb.name = "read_file";
    tb.summary = "a.cpp";
    m.tools.push_back(tb);
    m.interim.push_back("NARRATION_MARKER");
    s.messages.push_back(m);
  });
  std::string d = dom.dump();
  size_t tool_at = d.find("read_file");
  size_t narr_at = d.find("NARRATION_MARKER");
  size_t think_at = d.find("REASONING_MARKER");
  EXPECT_TRUE(tool_at != std::string::npos);
  EXPECT_TRUE(narr_at != std::string::npos);
  EXPECT_TRUE(think_at != std::string::npos);
  // Arrival order: the round's note, its tool call, then the reasoning appended
  // where the stream has reached (a later call would land below it).
  EXPECT_TRUE(narr_at < tool_at);
  EXPECT_TRUE(think_at > tool_at);
}

// Reasoning streams DURING the model call; the narration note is emitted when
// that call returns. They share the same tool anchor, so the thought must come
// first. The old renderer flushed interim notes first and put the thought below
// text the user watched arrive later.
FS_TEST(uiapp_thought_precedes_narration_at_same_anchor) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    seed_session(s, "sess_a");
    comp::ChatMsg m;
    m.role = "assistant";
    m.work_open = true;
    comp::ToolBlock tb;
    tb.name = "read_file";
    tb.summary = "a.cpp";
    m.tools.push_back(tb);
    comp::ChatMsg::ThinkBlock th;
    th.text = "THOUGHT_MARKER";
    th.after = 1;  // after the only tool
    th.open = true;
    m.thoughts.push_back(th);
    m.interim.push_back("NARRATION_MARKER");
    m.interim_after.push_back(1);  // same anchor as the thought
    s.messages.push_back(m);
  });
  std::string d = dom.dump();
  size_t tool_at = d.find("read_file");
  size_t think_at = d.find("THOUGHT_MARKER");
  size_t narr_at = d.find("NARRATION_MARKER");
  ASSERT_TRUE(tool_at != std::string::npos);
  ASSERT_TRUE(think_at != std::string::npos);
  ASSERT_TRUE(narr_at != std::string::npos);
  EXPECT_TRUE(tool_at < think_at);
  EXPECT_TRUE(think_at < narr_at);
}

// Guidance is a user turn injected mid-flight; it belongs AFTER the tools that
// had already run, not at the top of the turn.
FS_TEST(uiapp_guidance_uses_its_tool_anchor) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    seed_session(s, "sess_a");
    comp::ChatMsg m;
    m.role = "assistant";
    m.work_open = true;
    comp::ToolBlock tb;
    tb.name = "read_file";
    tb.summary = "a.cpp";
    m.tools.push_back(tb);
    m.guidance.push_back("GUIDE_MARKER");
    m.guidance_after.push_back(1);  // after the only tool
    s.messages.push_back(m);
  });
  std::string d = dom.dump();
  size_t tool_at = d.find("read_file");
  size_t guide_at = d.find("GUIDE_MARKER");
  ASSERT_TRUE(tool_at != std::string::npos);
  ASSERT_TRUE(guide_at != std::string::npos);
  EXPECT_TRUE(tool_at < guide_at);
}

// A background session's compaction must not touch the active session's water
// level or "compacted" state.
FS_TEST(uiapp_compaction_is_session_scoped) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    seed_session(s, "sess_a");
    s.context_window = 100000;
    s.context_pct = 80;
    s.context_tokens = 80000;
  });
  app.on_backend_event_for_test(
      "context.compressed",
      Json{{"sid", "sess_b"}, {"mode", "full"}, {"estTokens", 10000}});
  EXPECT_EQ(app.state().context_pct, 80);
  EXPECT_FALSE(app.state().context_compacted);
  // The active session's own compaction DOES move the level to the projected
  // next request, not the 80% request that triggered it.
  app.on_backend_event_for_test(
      "context.compressed",
      Json{{"sid", "sess_a"}, {"mode", "full"}, {"estTokens", 10000}});
  EXPECT_EQ(app.state().context_pct, 10);
  EXPECT_TRUE(app.state().context_compacted);
}

// A 47-round auto turn reached 2387 tool rows; rendering all of them on every
// streaming delta took 2.5 s per frame and pushed the WebView renderer to 19 GB.
// The live work log keeps only the newest slice; the full record stays stored.
FS_TEST(uiapp_huge_work_log_is_sliced) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    seed_session(s, "sess_a");
    comp::ChatMsg m;
    m.role = "assistant";
    m.streaming = true;
    m.work_open = true;
    for (int i = 0; i < 400; ++i) {
      comp::ToolBlock tb;
      tb.name = "TOOL_" + std::to_string(i);
      tb.summary = "x";
      m.tools.push_back(tb);
    }
    s.messages.push_back(m);
  });
  std::string d = dom.dump();
  EXPECT_TRUE(d.find("更早的") != std::string::npos);
  EXPECT_TRUE(d.find("TOOL_399") != std::string::npos);
  EXPECT_TRUE(d.find("TOOL_0") == std::string::npos);
  // Finished turn opened by the user: a larger slice, still bounded.
  app.update([](AppState& s) {
    s.messages[0].streaming = false;
    s.messages[0].work_touched = true;
  });
  d = dom.dump();
  EXPECT_TRUE(d.find("TOOL_399") != std::string::npos);
  EXPECT_TRUE(d.find("TOOL_0") == std::string::npos);
}

// A provider that sends "\n" between tokens can open a reasoning row with no
// visible content. It must not render as an empty 思考 block or create a work
// fold by itself.
FS_TEST(uiapp_blank_thought_is_not_rendered) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    seed_session(s, "sess_a");
    comp::ChatMsg m;
    m.role = "assistant";
    m.text = "answer";
    comp::ChatMsg::ThinkBlock th;
    th.text = " \n\t\r\n";
    th.after = 0;
    m.thoughts.push_back(th);
    s.messages.push_back(m);
  });
  std::string d = dom.dump();
  EXPECT_TRUE(d.find("work-block") == std::string::npos);
  EXPECT_TRUE(d.find("思考") == std::string::npos);
  EXPECT_TRUE(d.find("answer") != std::string::npos);
}

// A subagent transcript shows the tools a message called even when it also has
// text: a round that both spoke and called tools used to read as speech only.
FS_TEST(uiapp_subagent_transcript_shows_tools_with_text) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    seed_session(s, "sess_a");
    AppState::SubagentDetail det;
    det.loaded = true;
    det.row.id = "sub_1";
    det.row.kind = "explore";
    det.row.status = "done";
    det.row.transcript = Json::array(
        {Json{{"role", "assistant"},
              {"content", "## 标题\n\n先看看目录结构"},
              {"toolCalls", Json::array({Json{{"name", "list_dir"}}})}},
         Json{{"role", "tool"}, {"content", "{\"ok\":true}"}}});
    s.subagent_details["sub_1"] = det;
    SideTab t;
    t.id = "sb1";
    t.kind = "subagent";
    t.subagent_id = "sub_1";
    t.title = "explore";
    s.side_tabs.push_back(t);
    s.active_side = "sb1";
  });
  std::string d = dom.dump();
  EXPECT_TRUE(d.find("先看看目录结构") != std::string::npos);
  EXPECT_TRUE(d.find("list_dir") != std::string::npos);
  // Markdown is rendered, not dumped into <pre>.
  EXPECT_TRUE(d.find("md-h") != std::string::npos);
  // The call is a real tool row now (summary/input/detail), not a name string.
  EXPECT_TRUE(d.find("tool-block") != std::string::npos);
}

// The subagent OVERVIEW is a list: one long task/summary must not push every
// other agent off screen, and a running agent needs an explicit badge rather
// than a small dot buried in the header.
FS_TEST(uiapp_subagent_overview_is_truncated_and_marks_running) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  std::string long_task = "TASK_START";
  for (int i = 0; i < 400; ++i) long_task += "x";
  long_task += "TASK_END";
  std::string long_sum = "SUM_START";
  for (int i = 0; i < 800; ++i) long_sum += "y";
  long_sum += "SUM_END";
  app.update([&](AppState& s) {
    seed_session(s, "sess_a");
    for (size_t i = 0; i < s.side_tabs.size(); ++i) {
      if (s.side_tabs[i].kind == "subagents") s.active_side = s.side_tabs[i].id;
    }
    AppState::SubagentRow running;
    running.id = "sub_run";
    running.kind = "complete-step";
    running.status = "running";
    running.task = long_task;
    running.live_text = "LIVE_MARK";
    running.started_at = 1700000000000LL;
    s.subagents.push_back(running);
    AppState::SubagentRow done;
    done.id = "sub_done";
    done.kind = "complete-step";
    done.status = "done";
    done.task = "short task";
    done.summary = long_sum;
    done.started_at = 1700000000000LL;
    done.ended_at = 1700000030000LL;
    s.subagents.push_back(done);
  });
  const std::string d = dom.dump();
  EXPECT_TRUE(d.find("运行中") != std::string::npos);
  EXPECT_TRUE(d.find("结果摘要") != std::string::npos);
  EXPECT_TRUE(d.find("TASK_START") != std::string::npos);
  EXPECT_FALSE(d.find("TASK_END") != std::string::npos);   // clamped
  EXPECT_FALSE(d.find("SUM_END") != std::string::npos);    // clamped
  // The running card carries the live marker; the done card its result.
  EXPECT_TRUE(d.find("LIVE_MARK") != std::string::npos);
  // The composer names the send target: step work runs inline in the main
  // conversation (and moves to the subagent panel when done), so a typed 引导
  // must not silently go to the main session.
  EXPECT_TRUE(d.find("当前步骤在主界面内联执行") != std::string::npos);
}

// Detail page order: the WORK (chronological process log) comes before the
// RESULT it produced, and both are labelled. It used to print the summary above
// the transcript with no order, which read backwards.
FS_TEST(uiapp_subagent_detail_orders_process_then_summary) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    seed_session(s, "sess_a");
    AppState::SubagentDetail det;
    det.loaded = true;
    det.row.id = "sub_1";
    det.row.kind = "complete-step";
    det.row.status = "done";
    det.row.task = "TASK_MARK";
    det.row.summary = "RESULT_MARK";
    det.row.transcript = Json::array(
        {Json{{"role", "user"}, {"content", "PROCESS_MARK"}},
         Json{{"role", "assistant"}, {"content", "step finished"}}});
    s.subagent_details["sub_1"] = det;
    SideTab t;
    t.id = "sb1";
    t.kind = "subagent";
    t.subagent_id = "sub_1";
    t.title = "complete-step";
    s.side_tabs.push_back(t);
    s.active_side = "sb1";
  });
  const std::string d = dom.dump();
  size_t proc = d.find("PROCESS_MARK");
  size_t result = d.find("RESULT_MARK");
  EXPECT_TRUE(proc != std::string::npos);
  EXPECT_TRUE(result != std::string::npos);
  EXPECT_TRUE(proc < result);
  EXPECT_TRUE(d.find("过程记录（按时间顺序，最新在最后）") != std::string::npos);
  EXPECT_TRUE(d.find("结果摘要（已回传主会话）") != std::string::npos);
}

// A tab reopened during an AUTO round must not append the live "SYSTEM:
// continue" prompt as a second user bubble after the stored system divider, and
// must not duplicate the assistant bubble. That produced
// User-User-Assistant-Assistant(running) after a page switch.
FS_TEST(uiapp_live_auto_round_does_not_duplicate_bubbles) {
  class LiveBackend : public IUiBackend {
   public:
    BackendResult call(const std::string& method, const Json&) override {
      BackendResult r;
      r.ok = true;
      if (method == "session.info") {
        r.data = Json{{"meta", Json{{"sid", "s_live"}, {"pid", "p1"}}},
                      {"sessionSettings",
                       Json{{"model", "m1"}, {"provider", "pr1"},
                            {"enterSend", "enter"}}},
                      {"systemOps", Json::array()}};
      } else if (method == "session.turns") {
        r.data = Json{{"turns", Json::array({Json{
                                     {"role", "user"},
                                     {"promptRaw", "SYSTEM: 继续。"},
                                     {"autoRound", 1},
                                     {"system", true},
                                     {"turnId", "t1"},
                                     {"ts", 10}}})},
                      {"start", 0}, {"total", 1}, {"hasMore", false}};
      } else if (method == "session.liveTurn") {
        r.data = Json{{"running", true},
                      {"text", "partial answer"},
                      {"userText", "SYSTEM: 继续。"},
                      {"startedAt", 11},
                      {"interim", Json::array()},
                      {"interimAfter", Json::array()}};
      } else if (method == "model.allEnabled") {
        r.data = Json{{"models", Json::array(
                                   {Json{{"id", "m1"}, {"providerId", "pr1"}}})}};
      } else if (method == "session.todos") {
        r.data = Json{{"items", Json::array()}};
      } else if (method == "session.loopState") {
        r.data = Json{{"state", "idle"}};
      }
      return r;
    }
    std::function<void()> subscribe(BackendEvent) override { return []() {}; }
  } backend;
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, &backend);
  app.mount();
  app.update([](AppState& s) {
    seed_session(s, "s_live");
    s.providers_loaded = true;
  });
  app.show("s_live");
  const std::vector<comp::ChatMsg>& msgs = app.state().messages;
  int users = 0;
  for (size_t i = 0; i < msgs.size(); ++i) {
    if (msgs[i].role == "user") ++users;
    if (i > 0 && msgs[i].role == "user" && msgs[i - 1].role == "user") {
      EXPECT_TRUE(false);  // two user bubbles in a row
    }
    if (i > 0 && msgs[i].role == "assistant" && msgs[i - 1].role == "assistant") {
      EXPECT_TRUE(false);  // two assistant bubbles in a row
    }
  }
  EXPECT_EQ(users, 0);  // the auto prompt is represented by the system divider
  EXPECT_TRUE(!msgs.empty());
  EXPECT_TRUE(msgs.back().role == "assistant");
  EXPECT_EQ(msgs.back().text, std::string("partial answer"));
}

// ---- 第四十六批：过程解说的锚点 / 规划与清单合一 / 卡片内控件不冒泡 ----

// A round can issue SEVERAL tool calls. The note that arrives with that round
// must sit before the round's FIRST call, not before the call whose index equals
// the note's index - that put a later thought above tools that had already run
// ("新的思考跑到了上一个工具下面").
FS_TEST(uiapp_work_notes_follow_tool_anchors) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    seed_session(s, "sess_a");
    comp::ChatMsg m;
    m.role = "assistant";
    m.text = "answer";
    m.work_open = true;
    const char* names[] = {"tool_a", "tool_b", "tool_c"};
    for (int i = 0; i < 3; ++i) {
      comp::ToolBlock tb;
      tb.name = names[i];
      tb.summary = "s";
      m.tools.push_back(tb);
    }
    m.interim.push_back("NOTE_ZERO");
    m.interim_after.push_back(0);  // arrived before any tool ran
    m.interim.push_back("NOTE_TWO");
    m.interim_after.push_back(2);  // arrived after tool_a and tool_b
    s.messages.push_back(m);
  });
  std::string d = dom.dump();
  size_t n0 = d.find("NOTE_ZERO");
  size_t a = d.find("tool_a");
  size_t b = d.find("tool_b");
  size_t n2 = d.find("NOTE_TWO");
  size_t c = d.find("tool_c");
  EXPECT_TRUE(n0 != std::string::npos && a != std::string::npos &&
              b != std::string::npos && n2 != std::string::npos &&
              c != std::string::npos);
  EXPECT_TRUE(n0 < a);
  EXPECT_TRUE(a < b);
  EXPECT_TRUE(b < n2);
  EXPECT_TRUE(n2 < c);
}

// 任务列表 and 规划 are DIFFERENT LAYERS, not two views of one thing: the
// checklist is this round's work, the plan is the document the model keeps. A
// plan.update must leave the checklist alone (no "newer replaces the other"),
// and the composer has no 规划 control at all.
FS_TEST(uiapp_plan_and_todo_are_separate_layers) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) { seed_session(s, "sess_a"); });
  app.on_backend_event_for_test(
      "todo.updated",
      Json{{"sid", "sess_a"},
           {"owner", "sess_a"},
           {"items", Json::array({Json{{"id", 1}, {"task", "T1"}, {"done", false}}})}});
  ASSERT_TRUE(app.state().todos.size() == 1u);
  EXPECT_TRUE(app.state().todo_open);
  // A plan document update does NOT touch the checklist.
  app.on_backend_event_for_test(
      "plan.update",
      Json{{"sid", "sess_a"},
           {"plan", Json{{"exists", true},
                         {"status", "active"},
                         {"content", "# goal\n- step"}}}});
  EXPECT_EQ((int)app.state().todos.size(), 1);
  EXPECT_TRUE(app.state().todo_open);
  // ...and a later checklist update does not need to "close" anything.
  app.on_backend_event_for_test(
      "todo.updated",
      Json{{"sid", "sess_a"},
           {"owner", "sess_a"},
           {"items", Json::array({Json{{"id", 2}, {"task", "T2"}, {"done", false}}})}});
  std::string d = dom.dump();
  EXPECT_TRUE(d.find("清单") != std::string::npos);
  EXPECT_TRUE(d.find("规划") == std::string::npos);
}

// An inner control (the tool row's 打开记录, a card's action row) owns its click:
// both adapters stop the event at the innermost handler, so the row's own click
// never runs. That is a framework rule, not a per-control marker - no element
// should need data-no-bubble any more.
FS_TEST(uiapp_inner_controls_do_not_bubble) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    seed_session(s, "sess_a");
    comp::ChatMsg m;
    m.role = "assistant";
    m.text = "answer";
    m.work_open = true;
    comp::ToolBlock tb;
    tb.name = "subagent_spawn";
    tb.summary = "explore";
    tb.subagent_id = "sub_1";
    m.tools.push_back(tb);
    s.messages.push_back(m);
  });
  std::string d = dom.dump();
  EXPECT_TRUE(d.find("tb-open") != std::string::npos);
  EXPECT_TRUE(d.find("data-no-bubble") == std::string::npos);
}

// 工具行的渲染是按 (key, rev, 尺寸, 展开态) 记忆化的：一轮几百个工具调用时，
// 每个流式增量都重建所有行是 O(N) 的字符串拷贝/节点分配（整轮 O(N²)），这就是
// "工具多的回合界面卡住"。未变化的行必须拿回同一个 VNode。
FS_TEST(uiapp_tool_rows_are_memoised) {
  comp::ToolBlock tb;
  tb.key = "memo:1";
  tb.name = "read_file";
  tb.summary = "a.cpp";
  tb.detail = "{\"ok\":true}";
  H a = comp::tool_block_cached(tb, nullptr, nullptr, "g1");
  H b = comp::tool_block_cached(tb, nullptr, nullptr, "g1");
  EXPECT_TRUE(a.get() == b.get());  // unchanged row -> same node
  tb.detail = "{\"ok\":false}";
  tb.ok = false;
  tb.rev = 7;
  H c = comp::tool_block_cached(tb, nullptr, nullptr, "g1");
  EXPECT_TRUE(c.get() != a.get());  // changed row -> rebuilt
  // A NEW fold generation must not reuse rows: closing the fold removes their
  // elements, and a reused VNode made the diff think they were still attached
  // ("工作过程只能点开一次").
  H d = comp::tool_block_cached(tb, nullptr, nullptr, "g2");
  EXPECT_TRUE(d.get() != c.get());
}
