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

FS_TEST(uiapp_project_expand_sessions) {
  NullDom dom;
  AppState st = AppState::make();
  seed_project(st);
  UiApp app(dom, st, nullptr);
  app.mount();
  // right half is collapsed first
  EXPECT_FALSE(dump_has(dom, "UI 框架重构"));
  app.update([](AppState& s) { s.projects[0].expanded = true; });
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

FS_TEST(uiapp_drop_zone_bound) {
  NullDom dom;
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
    s.terminals.push_back(TermItem{"1", "pwsh", "running"});
    s.terminals.push_back(TermItem{"2", "cmd", "exited"});
    s.active_term = "1";
    s.terminal_open = true;
  });
  EXPECT_TRUE(dump_has(dom, "#1 pwsh"));
  EXPECT_TRUE(dump_has(dom, "#2 cmd"));
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
    r.kind = "plan";
    r.endpoint = "https://opencode.ai/zen/v1";
    s.providers.push_back(r);
    s.provider_selected = "p1";
    s.pv_kind = "plan";
    s.pv_base_url = "https://opencode.ai/zen/v1";
  });
  EXPECT_TRUE(dump_has(dom, "Endpoint"));
  EXPECT_TRUE(dump_has(dom, "x-opencode-session"));
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
  // templates drive the new-provider wizard
  app.update([](AppState& s) {
    AppState::ProviderTemplate t;
    t.id = "anthropic";
    t.name = "Anthropic Messages";
    t.kind = "anthropic";
    t.base_url = "https://api.anthropic.com/v1";
    t.description = "x-api-key + anthropic-version";
    s.provider_templates.push_back(t);
    s.provider_creating = true;
  });
  EXPECT_TRUE(dump_has(dom, "新建供应商"));
  EXPECT_TRUE(dump_has(dom, "Anthropic Messages"));
  // choosing a template fills the draft form (kind + endpoint)
  app.update([](AppState& s) {
    s.pv_name = "Anthropic Messages";
    s.pv_kind = "anthropic";
    s.pv_base_url = "https://api.anthropic.com/v1";
  });
  EXPECT_TRUE(dump_has(dom, "填写连接"));
  EXPECT_TRUE(dump_has(dom, "https://api.anthropic.com/v1"));
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
