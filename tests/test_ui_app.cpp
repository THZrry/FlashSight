// test_ui_app.cpp - UiApp integration tests on NullDom: render, tabs, chat,
// side panel, settings. Verifies the whole state->tree->patch loop.
//
// The app is backend-driven now, so these tests seed AppState directly (the
// same shape a backend response produces) instead of relying on demo data.
#include "test_framework.hpp"

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

// seed one project with one session, mirroring a backend project.list reply
void seed_project(AppState& s) {
  ProjectCard p;
  p.pid = "p1";
  p.name = "FlashSight";
  p.path = "C:\\aaa\\FlashSight";
  p.session_count = 1;
  p.sessions.push_back(std::make_pair(std::string("sess_a"), std::string("UI 框架重构")));
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
  app.update([](AppState& s) { s.home_tab = 0; });  // 探索
  EXPECT_TRUE(dump_has(dom, "输入关键词"));
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
  app.update([](AppState& s) { s.active_side = "terminal"; });
  EXPECT_TRUE(dump_has(dom, "刷新"));
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
  // choosing a kind adds/focuses that tab (no backend -> tab is created)
  app.open_or_focus_side_tab("git");
  EXPECT_EQ(app.state().side_tabs.size(), before + 1);
  EXPECT_EQ(app.state().side_tabs.back().kind, std::string("git"));
  // focusing the same kind again reuses the tab instead of duplicating it
  app.open_or_focus_side_tab("git");
  EXPECT_EQ(app.state().side_tabs.size(), before + 1);
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
  EXPECT_EQ(app.state().side_width, 320);
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
  EXPECT_EQ(app.state().side_width, 320);  // B keeps its own default
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
  EXPECT_TRUE(app.state().session_input.find("@notes.md") != std::string::npos);

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

FS_TEST(uiapp_settings_page) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.show("settings");
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
    s.pv_send_session = true;  // x-opencode-session toggle lives in 高级
    s.pv_advanced = true;
  });
  EXPECT_TRUE(dump_has(dom, "Endpoint"));
  EXPECT_TRUE(dump_has(dom, "发送会话亲和头"));
  // every nav entry must render its own body (nav key == body branch)
  const char* sec_keys[] = {"general", "appearance", "providers", "session_defaults",
                            "subagents", "skills", "memory", "network", "terminal",
                            "files", "environment", "browser", "computer",
                            "shortcuts", "about"};
  const char* sec_text[] = {"思考语言", "主题", "Endpoint", "会话默认",
                            "并行上限", "外部工具包", "长期记忆", "网络总开关", "默认 shell",
                            "自动 git init", "CLI 工具路径", "浏览器控制", "电脑控制",
                            "发送消息", "C++ VNode 框架"};
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
  app.show("settings");
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
  app.show("settings");
  app.update([](AppState& s) {
    s.settings_nav = "skills";
    s.mcp_tools.push_back(AppState::McpRow{"my-tools", "C:/tools/skills", true});
    s.mcp_tools.push_back(AppState::McpRow{"other", "C:/other", false});
  });
  EXPECT_TRUE(dump_has(dom, "外部工具包"));
  EXPECT_TRUE(dump_has(dom, "my-tools"));
  EXPECT_TRUE(dump_has(dom, "自动探测"));
  EXPECT_TRUE(dump_has(dom, "移除"));
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
