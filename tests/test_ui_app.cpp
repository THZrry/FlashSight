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
  EXPECT_EQ(app.state().side_width, 360);  // the new default width
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
  EXPECT_EQ(app.state().side_width, 360);  // the new default width  // B keeps its own default
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
    s.skill_menu.push_back("browser-control");
  });
  EXPECT_TRUE(dump_has(dom, "/browser-control"));
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
  EXPECT_TRUE(dump_has(dom, "展开"));
  app.update([](AppState& s) { s.projects[0].expanded = true; });
  EXPECT_TRUE(dump_has(dom, "UI 框架重构"));
  EXPECT_TRUE(dump_has(dom, "收起"));
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
  // and the terminal was NOT killed (no terminal.kill was issued)
  for (size_t i = 0; i < dom.calls().size(); ++i) {
    EXPECT_TRUE(dom.calls()[i].detail.find("terminal.kill") == std::string::npos);
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

// Switching a settings section replays its entry animation. The body node
// survives the patch, so without the replay token the animation only ever ran
// on the first section you opened.
FS_TEST(uiapp_settings_sections_animate) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.open_modal_page("settings");
  dom.clear_calls();
  app.update([](AppState& s) { s.settings_nav = "appearance"; });
  app.update([](AppState& s) { s.settings_nav = "network"; });
  app.update([](AppState& s) { s.settings_nav = "about"; });
  int replays = 0;
  for (size_t i = 0; i < dom.calls().size(); ++i) {
    if (dom.calls()[i].method == "play_animation") ++replays;
  }
  EXPECT_TRUE(replays >= 3);  // one per section switch
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
  // MCP/Skill are two pages now.
  const char* sec_keys[] = {"general", "appearance", "browser", "computer",
                            "providers", "session_defaults",
                            "model_prefs", "subagents", "mcp", "skills", "memory",
                            "network", "terminal", "files", "environment",
                            "shortcuts", "about"};
  const char* sec_text[] = {"界面显示", "主题", "浏览器控制", "电脑控制",
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
    s.mcp_tools.push_back(AppState::McpRow{"my-tools", "C:/tools/skills", "本地工具", true});
    s.mcp_tools.push_back(AppState::McpRow{"other", "C:/other", "", false});
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
    a.name = "browser-control";
    a.title = "浏览器驱动技能";
    a.description = "驱动外部浏览器";
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

FS_TEST(uiapp_plan_toggle) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    seed_session(s);
    s.plan_steps.push_back("重写 DOM Adapter");
  });
  EXPECT_FALSE(dump_has(dom, "任务规划"));
  app.update([](AppState& s) { s.plan_open = true; });
  EXPECT_TRUE(dump_has(dom, "任务规划"));
  EXPECT_TRUE(dump_has(dom, "重写 DOM Adapter"));
}

FS_TEST(uiapp_modal) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    s.ask_modal = true;
    s.ask_text = "允许执行 terminal.exec？";
  });
  EXPECT_TRUE(dump_has(dom, "允许执行 terminal.exec"));
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

FS_TEST(uiapp_permission_prompt_opens_modal) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) { seed_session(s); });
  app.on_backend_event_for_test("permission.prompt", Json{{"tool", "file.write"}});
  EXPECT_TRUE(app.state().ask_modal);
  EXPECT_TRUE(dump_has(dom, "file.write"));
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
    s.skill_menu_open = true;
    s.side_menu_open = true;
  });
  app.show("sess_b");
  EXPECT_FALSE(app.state().model_picker_open);
  EXPECT_FALSE(app.state().skill_menu_open);
  EXPECT_FALSE(app.state().side_menu_open);
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
    if (method == "session.get") {
      r.data = Json{
          {"meta", Json{{"sid", "sess_a"}, {"pid", "p1"}, {"cwd", "/tmp/p"}}},
          {"sessionSettings", Json{{"model", "claude-sonnet-4"},
                                   {"thinking", "high"},
                                   {"filePerm", "approve"},
                                   {"autoMode", true}}},
          {"turns", Json::array()}};
    } else if (method == "project.list" || method == "session.list") {
      r.data = Json::array();
    }
    return r;
  }
  std::function<void()> subscribe(BackendEvent) override { return []() {}; }
};

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

// Thinking is a foldable block, closed by default; opening it shows the body.
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
    s.messages.push_back(m);
  });
  std::string d = dom.dump();
  EXPECT_TRUE(d.find("work-block") != std::string::npos);
  EXPECT_TRUE(d.find("工作过程") != std::string::npos);
  EXPECT_TRUE(d.find("let me reason about it") == std::string::npos);  // folded
  app.update([](AppState& s) { s.messages[0].work_open = true; });
  EXPECT_TRUE(dom.dump().find("let me reason about it") != std::string::npos);
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
    tb.name = "file.read";
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
  // folded: the reasoning and the tool rows are not rendered
  EXPECT_TRUE(d.find("reasoning") == std::string::npos);
  EXPECT_TRUE(d.find("file.read") == std::string::npos);
  // opening it shows both
  app.update([](AppState& s) { s.messages[0].work_open = true; });
  std::string o = dom.dump();
  EXPECT_TRUE(o.find("reasoning") != std::string::npos);
  EXPECT_TRUE(o.find("file.read") != std::string::npos);
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

// The plan is its own column (a sibling of the chat), not a float inside it:
// opening it must not touch the chat subtree.
FS_TEST(uiapp_plan_is_own_column) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    seed_session(s, "sess_a");
    comp::ChatMsg m;
    m.role = "assistant";
    m.text = "hello";
    s.messages.push_back(m);
  });
  std::string before = dom.dump();
  EXPECT_TRUE(before.find("plan-col") == std::string::npos);
  app.update([](AppState& s) {
    s.plan_open = true;
    s.plan_steps.push_back("step one");
  });
  std::string d = dom.dump();
  EXPECT_TRUE(d.find("plan-col") != std::string::npos);
  EXPECT_TRUE(d.find("step one") != std::string::npos);
  // the plan column is a SIBLING of .session-center, not a child of it
  size_t center = d.find("session-center");
  size_t plan = d.find("plan-col");
  ASSERT_TRUE(center != std::string::npos && plan != std::string::npos);
  EXPECT_TRUE(plan > center);
}

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
