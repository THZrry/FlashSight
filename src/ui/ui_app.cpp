// ui_app.cpp - UiApp 实现（顶部标签页模式；class 名与 web/index.html 一致）
#include "ui_app.hpp"

namespace fs {
namespace ui {

UiApp::UiApp(IDom& dom, AppState state, IUiBackend* backend)
    : dom_(dom), state_(std::move(state)), backend_(backend) {}

const char* UiApp::page_name(Page p) {
  switch (p) {
    case Page::kHome: return "主页";
    case Page::kSession: return "会话1";
    case Page::kSettings: return "设置";
  }
  return "";
}

const char* UiApp::home_tab_name(HomeTab t) {
  switch (t) {
    case HomeTab::kExplore: return "探索";
    case HomeTab::kProject: return "项目";
    case HomeTab::kUsage: return "用量统计";
  }
  return "";
}

void UiApp::mount() {
  Node root = dom_.root();
  Node win = dom_.create("div", "window", "");
  dom_.append(root, win);
  build_tabstrip(win);
  content_ = dom_.create("div", "page", "");
  dom_.append(win, content_);
  bindings_.push_back(state_.page->subscribe([this](const Page&) { rebuild_content(); }));
  bindings_.push_back(
      state_.home_tab->subscribe([this](const HomeTab&) { rebuild_content(); }));
  rebuild_content();
}

void UiApp::show(Page p) { state_.page->set(p); }

void UiApp::rebuild_content() {
  drop_widgets();
  dom_.clear(content_);
  switch (state_.page->get()) {
    case Page::kHome: build_home(content_); break;
    case Page::kSession: build_session(content_); break;
    case Page::kSettings: build_settings(content_); break;
  }
}

// 顶部 tab 条（唯一导航模式）
void UiApp::build_tabstrip(Node parent) {
  Node bar = dom_.create("div", "tabs", "");
  const Page pages[2] = {Page::kHome, Page::kSession};
  for (int i = 0; i < 2; ++i) {
    bool active = state_.page->get() == pages[i];
    Node t = dom_.create("a", active ? "tab active" : "tab", page_name(pages[i]));
    Page p = pages[i];
    dom_.on(t, EventKind::kClick, [this, p](const std::string&) { show(p); });
    dom_.append(bar, t);
  }
  Node spacer = dom_.create("span", "spacer", "");
  dom_.append(bar, spacer);
  Node add = dom_.create("a", "tab add", "+");
  dom_.append(bar, add);
  dom_.append(parent, bar);
}

// ---------- 主页 ----------
void UiApp::build_home(Node parent) {
  Node inner = dom_.create("div", "page-inner", "");
  // 子 tab（探索/项目/用量统计）
  Node sub = dom_.create("div", "subtabs", "");
  const HomeTab tabs[3] = {HomeTab::kExplore, HomeTab::kProject, HomeTab::kUsage};
  for (int i = 0; i < 3; ++i) {
    bool active = state_.home_tab->get() == tabs[i];
    Node t = dom_.create("a", active ? "tab active" : "tab", home_tab_name(tabs[i]));
    HomeTab h = tabs[i];
    dom_.on(t, EventKind::kClick, [this, h](const std::string&) { state_.home_tab->set(h); });
    dom_.append(sub, t);
  }
  dom_.append(inner, sub);
  switch (state_.home_tab->get()) {
    case HomeTab::kProject: build_home_project(inner); break;
    case HomeTab::kUsage: build_home_usage(inner); break;
    case HomeTab::kExplore: {
      Node box = dom_.create("div", "big-card dim", "探索（暂未开放）");
      dom_.append(inner, box);
      break;
    }
  }
  // 底栏药丸
  Node foot = dom_.create("div", "footbar", "");
  own<Button>(dom_, "⚙ 设置", [this](const std::string&) { show(Page::kSettings); })
      ->mount(foot);
  IUiBackend* be = backend_;
  own<Button>(dom_, "新建项目", [be](const std::string&) {
    if (be) be->call("project.create", Json::object());
  })->mount(foot);
  own<Button>(dom_, "打开文件夹", [be](const std::string&) {
    if (be) be->call("project.open", Json::object());
  })->mount(foot);
  dom_.append(inner, foot);
  dom_.append(parent, inner);
}

void UiApp::build_home_project(Node parent) {
  Node home = dom_.create("div", "home", "");
  Node list = dom_.create("div", "list-col", "");
  // v1 静态项目行；P4 接 project.list
  const char* names[2] = {"项目1", "demo-web"};
  for (int i = 0; i < 2; ++i) {
    Node it = dom_.create("div", "item", "");
    Node n = dom_.create("span", "", names[i]);
    dom_.append(it, n);
    Node arrow = dom_.create("span", "dim", "›");
    dom_.append(it, arrow);
    dom_.append(list, it);
  }
  dom_.append(home, list);
  Node card = dom_.create("div", "big-card dim", "（选中项目的最近会话与概览）");
  dom_.append(home, card);
  dom_.append(parent, home);
}

void UiApp::build_home_usage(Node parent) {
  Node usage = dom_.create("div", "usage", "");
  Node list = dom_.create("div", "list-col", "");
  const char* rows[3] = {"总计", "供应商1", "供应商2"};
  for (int i = 0; i < 3; ++i) {
    Node it = dom_.create("div", "item", "");
    Node n = dom_.create("span", "", rows[i]);
    dom_.append(it, n);
    Node arrow = dom_.create("span", "dim", "›");
    dom_.append(it, arrow);
    dom_.append(list, it);
  }
  dom_.append(usage, list);
  Node panel = dom_.create("div", "panel", "");
  Node b1 = dom_.create("div", "block tall", "按模型与时间的柱状统计图（12h/1天/7天/30天）");
  dom_.append(panel, b1);
  Node row = dom_.create("div", "row", "");
  Node b2 = dom_.create("div", "block grow", "模型扇形统计图");
  Node b3 = dom_.create("div", "block grow", "token 分布表格");
  dom_.append(row, b2);
  dom_.append(row, b3);
  dom_.append(panel, row);
  Node b4 = dom_.create("div", "block tall", "成本分析表格（钱/额度/无法统计）");
  dom_.append(panel, b4);
  dom_.append(usage, panel);
  dom_.append(parent, usage);
}

// ---------- 会话 ----------
void UiApp::build_session(Node parent) {
  Node inner = dom_.create("div", "page-inner", "");
  Node sess = dom_.create("div", "session", "");

  // 规划药丸
  own<Button>(dom_, "规划", [this](const std::string&) {
    state_.plan_open->set(!state_.plan_open->get());
  })->mount(sess);

  // 中部：历史 + 侧栏
  Node mid = dom_.create("div", "mid", "");
  Node hist = dom_.create("div", "history", "");
  // 消息列表（v1 静态 3 条；P4 虚拟化）
  auto msg_count = Signal<int>::make(3);
  own<ListView>(dom_, msg_count, 10, [](IDom& d, Node p, int i) {
    Node m = d.create("div", "msg", "");
    Node role = d.create("div", "role", i == 0 ? "你" : "gpt-4o");
    d.append(m, role);
    Node b = d.create("div", "bubble", i == 2 ? "流式输出中…" : "历史消息内容");
    d.append(m, b);
    d.append(p, m);
  })->mount(hist);
  dom_.append(mid, hist);

  Node side = dom_.create("div", "sidebar", "");
  std::vector<std::string> side_titles = {"用量", "文件", "操作", "Git", "子代理", "调用"};
  std::vector<Tabs::PageFn> side_pages(6);
  side_pages[0] = [](IDom& d, Node p) {
    Node t = d.create("div", "dim", "上下文水位 42% · 本会话 ¥0.21");
    d.append(p, t);
  };
  own<Tabs>(dom_, side_titles, state_.side_tab, side_pages)->mount(side);
  dom_.append(mid, side);
  dom_.append(sess, mid);

  // 输入区（composer）
  Node comp = dom_.create("div", "composer", "");
  own<TextArea>(dom_, state_.session_input, "输入消息…（Enter 发送，Shift+Enter 换行）")
      ->mount(comp);
  Node tools = dom_.create("div", "tools", "");
  own<Button>(dom_, "+ 加入文件", [](const std::string&) {})->mount(tools);
  own<Button>(dom_, "/ skill命令", [](const std::string&) {})->mount(tools);
  Node grow = dom_.create("span", "grow", "");
  dom_.append(tools, grow);
  auto susp = state_.suspended;
  auto susp_label = Signal<std::string>::make("挂起（暂停）");
  {
    auto s = susp;
    auto l = susp_label;
    state_.suspended->subscribe([s, l](const bool&) {
      l->set(s->get() ? "恢复" : "挂起（暂停）");
    });
  }
  own<Button>(dom_, susp_label, [susp](const std::string&) { susp->set(!susp->get()); })
      ->mount(tools);
  IUiBackend* be = backend_;
  auto sinput = state_.session_input;
  own<Button>(dom_, "发送/追加", [be, sinput](const std::string&) {
    if (be && !sinput->get().empty()) {
      Json args = Json::object();
      args["text"] = sinput->get();
      be->call("session.send", args);
      sinput->set("");
    }
  })->mount(tools);
  dom_.append(comp, tools);
  dom_.append(sess, comp);

  // ask 弹窗
  own<Modal>(dom_, state_.ask_modal, "需要确认",
             [](IDom& d, Node p) {
               Node t = d.create("div", "", "模型请求确认，请选择。");
               d.append(p, t);
             },
             [](const std::string&) {})
      ->mount(sess);

  // 终端抽屉（默认隐藏）
  Node term = dom_.create("div", "terminal", "");
  Node th = dom_.create("div", "th", "#1 pwsh · 手动停止");
  dom_.append(term, th);
  bindings_.push_back(bind_visible(dom_, term, state_.terminal_visible));
  dom_.append(sess, term);

  dom_.append(inner, sess);
  dom_.append(parent, inner);
}

// ---------- 设置 ----------
void UiApp::build_settings(Node parent) {
  Node wrap = dom_.create("div", "settings", "");
  Node nav = dom_.create("div", "nav", "");
  const char* groups[4] = {"通用", "模型", "权限", "其他"};
  const char* items[4][5] = {
      {"常规", "外观", "浏览器控制", "电脑控制", ""},
      {"供应商", "模型偏好", "MCP 与 Skill", "子代理", "长期记忆"},
      {"网络", "终端", "文件", "环境", ""},
      {"快捷键", "关于", "", "", ""},
  };
  for (int g = 0; g < 4; ++g) {
    Node grp = dom_.create("div", "grp", groups[g]);
    dom_.append(nav, grp);
    for (int i = 0; i < 5; ++i) {
      if (items[g][i][0] == '\0') break;
      Node n = dom_.create("div", "n", items[g][i]);
      dom_.append(nav, n);
    }
  }
  dom_.append(wrap, nav);

  Node body = dom_.create("div", "body", "");
  Node h = dom_.create("h2", "", "供应商设置");
  dom_.set_attr(h, "style", "margin:0 0 14px");
  dom_.append(body, h);
  Node f1 = dom_.create("div", "field", "");
  Node l1 = dom_.create("label", "", "供应商");
  dom_.append(f1, l1);
  std::vector<std::string> provs = {"opencode-zen（套餐）", "[OI] 兼容", "Anthropic", "自定义"};
  own<Select>(dom_, provs, state_.provider_index)->mount(f1);
  dom_.append(body, f1);
  Node f2 = dom_.create("div", "field", "");
  Node l2 = dom_.create("label", "", "Endpoint");
  dom_.append(f2, l2);
  auto ep = Signal<std::string>::make("https://opencode.ai/zen/v1");
  own<TextInput>(dom_, ep, "")->mount(f2);
  dom_.append(body, f2);
  Node f3 = dom_.create("div", "field", "");
  Node l3 = dom_.create("label", "", "API Key");
  dom_.append(f3, l3);
  auto key = Signal<std::string>::make("****");
  own<TextInput>(dom_, key, "")->mount(f3);
  dom_.append(body, f3);
  Node f4 = dom_.create("div", "field", "");
  Node l4 = dom_.create("label", "", "自定义 Header");
  dom_.append(f4, l4);
  auto hdr = Signal<std::string>::make("");
  own<TextInput>(dom_, hdr, "x-opencode-session: auto")->mount(f4);
  dom_.append(body, f4);
  IUiBackend* be = backend_;
  Node f5 = dom_.create("div", "field", "");
  Node l5 = dom_.create("label", "", "");
  dom_.append(f5, l5);
  own<Button>(dom_, "测试连接", [be](const std::string&) {
    if (be) be->call("provider.testConnection", Json::object());
  })->mount(f5);
  own<Button>(dom_, "一键获取模型列表", [be](const std::string&) {
    if (be) be->call("provider.listModels", Json::object());
  })->mount(f5);
  dom_.append(body, f5);
  dom_.append(wrap, body);
  dom_.append(parent, wrap);
}

}  // namespace ui
}  // namespace fs