// test_ui_app.cpp - UiApp integration tests on NullDom: render, tabs, chat,
// side panel, settings. Verifies the whole state->tree->patch loop.
#include "test_framework.hpp"

#include "ui/ui_app.hpp"
#include "ui/ui_backend.hpp"
#include "ui/ui_components.hpp"
#include "ui/ui_dom.hpp"
#include "ui/ui_vdom.hpp"

using namespace fs::ui;

namespace {

// find the first node in the dump containing `needle`
bool dump_has(NullDom& dom, const std::string& needle) {
  return dom.dump().find(needle) != std::string::npos;
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
  // project card content
  EXPECT_TRUE(dump_has(dom, "FlashSight"));
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
  app.update([](AppState& s) {
    SessionTab t;
    t.id = "s1";
    t.title = "会话1";
    s.sessions.push_back(t);
    s.tab_ids.push_back("s1");
    s.active_tab = "s1";
  });
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
  app.update([](AppState& s) {
    SessionTab t;
    t.id = "s1";
    t.title = "会话1";
    s.sessions.push_back(t);
    s.tab_ids.push_back("s1");
    s.active_tab = "s1";
  });
  app.update([](AppState& s) {
    // emulate close_tab logic
    for (size_t i = 0; i < s.tab_ids.size(); ++i) {
      if (s.tab_ids[i] == "s1") {
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
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    SessionTab t;
    t.id = "s1";
    t.title = "会话1";
    s.sessions.push_back(t);
    s.tab_ids.push_back("s1");
    s.active_tab = "s1";
    s.session_input = "hello world";
  });
  app.send_message();
  EXPECT_TRUE(dump_has(dom, "hello world"));
  EXPECT_TRUE(dump_has(dom, "file.read"));       // tool block
  EXPECT_TRUE(dump_has(dom, "本轮要点"));         // summary label
  EXPECT_TRUE(app.state().session_input.empty());
}

FS_TEST(uiapp_side_panel_switch) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    SessionTab t;
    t.id = "s1";
    t.title = "会话1";
    s.sessions.push_back(t);
    s.tab_ids.push_back("s1");
    s.active_tab = "s1";
  });
  // default side tab = usage
  EXPECT_TRUE(dump_has(dom, "上下文水位"));
  app.update([](AppState& s) { s.active_side = "subagents"; });
  EXPECT_TRUE(dump_has(dom, "explore"));
  app.update([](AppState& s) { s.active_side = "terminal"; });
  EXPECT_TRUE(dump_has(dom, "新建终端"));
}

FS_TEST(uiapp_terminal_selector) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    SessionTab t;
    t.id = "s1";
    t.title = "会话1";
    s.sessions.push_back(t);
    s.tab_ids.push_back("s1");
    s.active_tab = "s1";
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
  app.update([](AppState& s) { s.settings_nav = 5; });  // 供应商
  EXPECT_TRUE(dump_has(dom, "Endpoint"));
  EXPECT_TRUE(dump_has(dom, "x-opencode-session"));
}

FS_TEST(uiapp_plan_toggle) {
  NullDom dom;
  AppState st = AppState::make();
  UiApp app(dom, st, nullptr);
  app.mount();
  app.update([](AppState& s) {
    SessionTab t;
    t.id = "s1";
    t.title = "会话1";
    s.sessions.push_back(t);
    s.tab_ids.push_back("s1");
    s.active_tab = "s1";
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
