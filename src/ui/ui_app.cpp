// ui_app.cpp - UiApp 实现（四页骨架；class 名与 web 原型保持一致，方便对照）
#include "ui_app.hpp"

namespace fs {
namespace ui {

UiApp::UiApp(IDom& dom, AppState state) : dom_(dom), state_(std::move(state)) {}

const char* UiApp::page_name(Page p) {
  switch (p) {
    case Page::kHome: return "主页";
    case Page::kSession: return "会话";
    case Page::kUsage: return "用量";
    case Page::kSettings: return "设置";
  }
  return "";
}

void UiApp::mount() {
  Node root = dom_.root();
  build_titlebar(root);
  build_tabs(root);
  content_ = dom_.create("div", "page", "");
  dom_.append(root, content_);
  // 切页订阅
  auto self_page = state_.page;
  bindings_.push_back(state_.page->subscribe([this](const Page&) { rebuild_content(); }));
  (void)self_page;
  rebuild_content();
}

void UiApp::show(Page p) { state_.page->set(p); }

void UiApp::rebuild_content() {
  dom_.clear(content_);
  switch (state_.page->get()) {
    case Page::kHome: build_home(content_); break;
    case Page::kSession: build_session(content_); break;
    case Page::kUsage: build_usage(content_); break;
    case Page::kSettings: build_settings(content_); break;
  }
}

void UiApp::build_titlebar(Node parent) {
  Node bar = dom_.create("div", "titlebar", "");
  Node name = dom_.create("span", "app-name", "");
  dom_.append(bar, name);
  // 标题绑定
  bindings_.push_back(bind_text(dom_, name, state_.title));
  dom_.append(parent, bar);
}

void UiApp::build_tabs(Node parent) {
  Node strip = dom_.create("div", "tabstrip", "");
  const Page pages[4] = {Page::kHome, Page::kSession, Page::kUsage, Page::kSettings};
  for (int i = 0; i < 4; ++i) {
    Node tab = dom_.create("a", "tab", page_name(pages[i]));
    Page p = pages[i];
    dom_.on(tab, EventKind::kClick, [this, p](const std::string&) { show(p); });
    dom_.append(strip, tab);
  }
  dom_.append(parent, strip);
}

void UiApp::build_home(Node parent) {
  Node side = dom_.create("div", "sidebar", "");
  Node h = dom_.create("h3", "", "项目");
  dom_.append(side, h);
  Node list = dom_.create("ul", "proj-list", "");
  Node item = dom_.create("li", "", "FlashSight");
  dom_.append(list, item);
  dom_.append(side, list);
  dom_.append(parent, side);
  Node main = dom_.create("div", "main", "");
  Node h2 = dom_.create("h2", "", "项目概览");
  dom_.append(main, h2);
  Node tip = dom_.create("p", "dim", "点击左侧项目进入最近会话。");
  dom_.append(main, tip);
  dom_.append(parent, main);
}

void UiApp::build_session(Node parent) {
  Node wrap = dom_.create("div", "", "");
  dom_.set_attr(wrap, "style", "display:flex;flex-direction:column;flex:1;min-width:0");
  // 顶栏
  Node top = dom_.create("div", "session-topbar", "");
  Node send = dom_.create("button", "btn primary", "发送/追加");
  dom_.on(send, EventKind::kClick, [this](const std::string&) {
    state_.session_input->set("");
  });
  dom_.append(top, send);
  Node suspend = dom_.create("button", "btn", "挂起");
  dom_.append(top, suspend);
  dom_.append(wrap, top);
  // 历史区
  Node body = dom_.create("div", "session-body", "");
  Node plan = dom_.create("div", "plan-rail", "");
  Node ph = dom_.create("h3", "", "规划");
  dom_.append(plan, ph);
  dom_.append(body, plan);
  Node hist = dom_.create("div", "history", "");
  Node msg = dom_.create("div", "msg", "");
  Node bubble = dom_.create("div", "bubble dim", "会话历史（虚拟化一次 10 条）");
  dom_.append(msg, bubble);
  dom_.append(hist, msg);
  dom_.append(body, hist);
  // 右侧栏
  Node rail = dom_.create("div", "side-rail", "");
  Node pane = dom_.create("div", "side-pane", "用量 / 文件 / Git / 子代理");
  dom_.append(rail, pane);
  dom_.append(body, rail);
  dom_.append(wrap, body);
  // 输入区
  Node input = dom_.create("div", "input-area", "");
  Node area = dom_.create("textarea", "", "");
  dom_.on(area, EventKind::kInput, [this](const std::string& v) {
    state_.session_input->set(v);
  });
  dom_.append(input, area);
  dom_.append(wrap, input);
  // 终端区（绑定显隐）
  Node term = dom_.create("div", "terminal-zone", "#1 shell · 手动停止");
  bindings_.push_back(bind_visible(dom_, term, state_.terminal_visible));
  dom_.append(wrap, term);
  dom_.append(parent, wrap);
}

void UiApp::build_usage(Node parent) {
  Node main = dom_.create("div", "main", "");
  Node h2 = dom_.create("h2", "", "用量统计");
  dom_.append(main, h2);
  Node c1 = dom_.create("div", "chart", "按模型与时间的柱状图");
  dom_.append(main, c1);
  Node c2 = dom_.create("div", "chart", "模型占比扇形图");
  dom_.append(main, c2);
  dom_.append(parent, main);
}

void UiApp::build_settings(Node parent) {
  Node wrap = dom_.create("div", "settings", "");
  Node nav = dom_.create("nav", "", "");
  const char* groups[4] = {"通用", "模型", "权限", "其他"};
  for (int i = 0; i < 4; ++i) {
    Node g = dom_.create("div", "group", groups[i]);
    dom_.append(nav, g);
  }
  dom_.append(wrap, nav);
  Node content = dom_.create("div", "content", "");
  Node h2 = dom_.create("h2", "", "设置");
  dom_.append(content, h2);
  Node tip = dom_.create("p", "dim", "双栏布局；插件入口灰色禁用。");
  dom_.append(content, tip);
  dom_.append(wrap, content);
  dom_.append(parent, wrap);
}

}  // namespace ui
}  // namespace fs