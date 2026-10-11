// ui_app.cpp - UiApp 实现（Widget 库装配；class 名与 web 原型一致）
#include "ui_app.hpp"

namespace fs {
namespace ui {

UiApp::UiApp(IDom& dom, AppState state, IUiBackend* backend)
    : dom_(dom), state_(std::move(state)), backend_(backend) {}

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
  bindings_.push_back(state_.page->subscribe([this](const Page&) { rebuild_content(); }));
  rebuild_content();
}

void UiApp::show(Page p) { state_.page->set(p); }

void UiApp::rebuild_content() {
  drop_widgets();
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
  bindings_.push_back(bind_text(dom_, name, state_.title));
  dom_.append(parent, bar);
}

void UiApp::build_tabs(Node parent) {
  std::vector<std::string> titles = {"主页", "会话", "用量", "设置"};
  auto cur = Signal<int>::make(0);
  // tab 与 page 双向：点击切页
  std::vector<Tabs::PageFn> pages(4);
  Tabs* tabs = own<Tabs>(dom_, titles, cur, pages);
  // tab 点击后同步 AppState（Tabs 内部只切自己的 current）
  auto page = state_.page;
  cur->subscribe([this, page](const int& idx) {
    const Page ps[4] = {Page::kHome, Page::kSession, Page::kUsage, Page::kSettings};
    if (idx >= 0 && idx < 4) page->set(ps[idx]);
  });
  tabs->mount(parent);
  (void)pages;
}

void UiApp::build_home(Node parent) {
  Node side = dom_.create("div", "sidebar", "");
  own<Text>(dom_, "项目", "")->mount(side);
  // 项目列表（v1 静态两行；P4 接后端 project.list）
  Node list = dom_.create("ul", "proj-list", "");
  dom_.append(side, list);
  Node item = dom_.create("li", "", "FlashSight");
  dom_.append(list, item);
  dom_.append(parent, side);
  Node main = dom_.create("div", "main", "");
  own<Text>(dom_, "项目概览", "")->mount(main);
  own<Text>(dom_, "点击左侧项目进入最近会话。", "dim")->mount(main);
  // 搜索框（addon1 侧栏搜索）
  own<SearchBox>(dom_, [](const std::string&) {}, "搜索项目/会话…")->mount(main);
  dom_.append(parent, main);
}

void UiApp::build_session(Node parent) {
  Node wrap = dom_.create("div", "", "");
  dom_.set_attr(wrap, "style", "display:flex;flex-direction:column;flex:1;min-width:0");

  // ---- 顶栏：模型/权限 Select + 自动挡 Toggle + 挂起 Button（label 绑定）+ 刷新 ----
  Node top = dom_.create("div", "session-topbar", "");
  std::vector<std::string> models = {"gpt-4o", "claude-sonnet-4", "gpt-4o-mini"};
  own<Select>(dom_, models, state_.model_index)->mount(top);
  std::vector<std::string> perms = {"项目放行", "每次询问", "完整"};
  own<Select>(dom_, perms, state_.perm_index)->mount(top);
  own<Toggle>(dom_, state_.auto_mode)->mount(top);
  // 挂起按钮：label 随 suspended 切换
  auto suspend_label = Signal<std::string>::make("挂起");
  {
    auto s = state_.suspended;
    auto l = suspend_label;
    state_.suspended->subscribe([s, l](const bool&) {
      l->set(s->get() ? "恢复" : "挂起");
    });
  }
  auto susp = state_.suspended;
  own<Button>(dom_, suspend_label, [susp](const std::string&) { susp->set(!susp->get()); })
      ->mount(top);
  IUiBackend* be = backend_;
  own<Button>(dom_, "刷新配置", [be](const std::string&) {
    if (be) be->call("session.refreshConfig", Json::object());
  })->mount(top);
  dom_.append(wrap, top);

  // ---- 历史区：规划 Accordion + 消息 ListView + 右侧 Tabs ----
  Node body = dom_.create("div", "session-body", "");
  auto plan_open = Signal<bool>::make(true);
  own<Accordion>(dom_, "规划", plan_open, [](IDom& d, Node p) {
    Node t = d.create("div", "", "◉ 实现登录鉴权\n◉ 接入 OAuth\n○ 写单测");
    d.append(p, t);
  })->mount(body);
  // 消息列表（v1 静态 3 行占位；P4 接虚拟化历史）
  auto msg_count = Signal<int>::make(3);
  own<ListView>(dom_, msg_count, 10, [](IDom& d, Node p, int i) {
    Node m = d.create("div", "msg", "");
    Node b = d.create("div", "bubble", i == 2 ? "流式输出中…" : "历史消息");
    d.append(m, b);
    d.append(p, m);
  })->mount(body);
  // 右侧栏 Tabs
  std::vector<std::string> side_titles = {"用量", "文件", "操作", "Git", "子代理", "调用"};
  std::vector<Tabs::PageFn> side_pages(6);
  side_pages[0] = [](IDom& d, Node p) {
    Node t = d.create("div", "side-pane", "上下文水位 42%");
    d.append(p, t);
  };
  own<Tabs>(dom_, side_titles, state_.side_tab, side_pages)->mount(body);
  dom_.append(wrap, body);

  // ---- 输入区 ----
  Node input = dom_.create("div", "input-area", "");
  own<TextArea>(dom_, state_.session_input, "输入消息…（Enter 发送，Shift+Enter 换行）")
      ->mount(input);
  IUiBackend* be2 = backend_;
  auto sinput = state_.session_input;
  own<Button>(dom_, "发送/追加", [be2, sinput](const std::string&) {
    if (be2 && !sinput->get().empty()) {
      Json args = Json::object();
      args["text"] = sinput->get();
      be2->call("session.send", args);
      sinput->set("");
    }
  }, true)->mount(input);
  dom_.append(wrap, input);

  // ---- ask 弹窗 ----
  own<Modal>(dom_, state_.ask_modal, "需要确认", [](IDom& d, Node p) {
    Node t = d.create("div", "", "模型请求确认，请选择。");
    d.append(p, t);
  }, [](const std::string&) {})->mount(wrap);

  // ---- 终端区（绑定显隐） ----
  Node term = dom_.create("div", "terminal-zone", "#1 shell · 手动停止");
  dom_.append(wrap, term);
  bindings_.push_back(bind_visible(dom_, term, state_.terminal_visible));

  dom_.append(parent, wrap);
}

void UiApp::build_usage(Node parent) {
  Node main = dom_.create("div", "main", "");
  own<Text>(dom_, "用量统计", "")->mount(main);
  // 上下文水位进度条
  own<Progress>(dom_, state_.context_pct)->mount(main);
  Node c1 = dom_.create("div", "chart", "按模型与时间的柱状图");
  dom_.append(main, c1);
  Node c2 = dom_.create("div", "chart", "模型占比扇形图");
  dom_.append(main, c2);
  dom_.append(parent, main);
}

void UiApp::build_settings(Node parent) {
  Node wrap = dom_.create("div", "settings", "");
  // 左导航 Tabs（纵向用普通按钮列；v1 简化）
  Node nav = dom_.create("nav", "", "");
  const char* groups[4] = {"通用", "模型", "权限", "其他"};
  for (int i = 0; i < 4; ++i) {
    Node g = dom_.create("div", "group", groups[i]);
    dom_.append(nav, g);
  }
  dom_.append(wrap, nav);
  Node content = dom_.create("div", "content", "");
  own<Text>(dom_, "供应商设置", "")->mount(content);
  std::vector<std::string> provs = {"opencode-zen（套餐）", "OpenAI 兼容", "Anthropic", "自定义"};
  auto prov_idx = Signal<int>::make(0);
  own<Select>(dom_, provs, prov_idx)->mount(content);
  auto key = Signal<std::string>::make("****");
  own<TextInput>(dom_, key, "API Key")->mount(content);
  IUiBackend* be = backend_;
  own<Button>(dom_, "测试连接", [be](const std::string&) {
    if (be) be->call("provider.testConnection", Json::object());
  })->mount(content);
  own<Text>(dom_, "模型偏好", "")->mount(content);
  auto sub = Signal<bool>::make(false);
  own<Checkbox>(dom_, "子代理继承会话模型", sub)->mount(content);
  own<Text>(dom_, "插件入口灰色禁用。", "dim")->mount(content);
  dom_.append(wrap, content);
  dom_.append(parent, wrap);
}

}  // namespace ui
}  // namespace fs