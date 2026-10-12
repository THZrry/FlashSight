// ui_app.cpp - User layer implementation: the FlashSight UI itself.
//
// Layout follows the user's reference screenshots and feedback list:
//   * top tab strip with closable tabs and a "+" button
//   * home: centered title buttons (探索/项目/用量统计) - color+underline only
//   * 项目: cards; left = project info, right = expandable session list
//   * session: article-style chat (user = bubble, assistant = plain article,
//     system = centered dim line), foldable flow blocks, unfolded summary,
//     free-form side-panel tabs, terminal selector
//   * settings: roomy two-column page
#include "ui_app.hpp"

#include <cstdio>

namespace fs {
namespace ui {

using comp::TabSpec;

namespace {

std::string itos(int v) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%d", v);
  return std::string(buf);
}

const char* kSideTitles[][2] = {
    {"usage", "用量"},   {"files", "文件"},     {"ops", "操作"},
    {"git", "Git"},      {"subagents", "子代理"}, {"calls", "调用"},
    {"terminal", "终端"},
};

std::string side_title(const std::string& kind) {
  for (size_t i = 0; i < sizeof(kSideTitles) / sizeof(kSideTitles[0]); ++i) {
    if (kind == kSideTitles[i][0]) return kSideTitles[i][1];
  }
  return kind;
}

}  // namespace

// ===========================================================================
// AppState
// ===========================================================================
AppState AppState::make() {
  AppState s;
  s.tab_ids.push_back("home");
  s.tab_ids.push_back("settings");
  s.side_tabs.push_back(SideTab{"usage", "用量", "usage"});
  s.side_tabs.push_back(SideTab{"subagents", "子代理", "subagents"});
  s.side_tabs.push_back(SideTab{"terminal", "终端", "terminal"});
  s.side_seq = 3;

  // demo content so the prototype shows real layout (backend wiring later)
  ProjectCard p1;
  p1.pid = "p1";
  p1.name = "FlashSight";
  p1.path = "C:\\aaa\\FlashSight";
  p1.updated = "2 分钟前";
  p1.session_count = 3;
  p1.sessions.push_back(std::make_pair(std::string("s1"), std::string("UI 框架重构")));
  p1.sessions.push_back(std::make_pair(std::string("s2"), std::string("reproc 接线")));
  p1.sessions.push_back(std::make_pair(std::string("s3"), std::string("构建流程改造")));
  s.projects.push_back(p1);
  ProjectCard p2;
  p2.pid = "p2";
  p2.name = "demo-web";
  p2.path = "C:\\work\\demo-web";
  p2.updated = "昨天";
  p2.session_count = 1;
  p2.sessions.push_back(std::make_pair(std::string("s4"), std::string("首页改版")));
  s.projects.push_back(p2);

  s.plan_steps.push_back("分析需求与现有实现");
  s.plan_steps.push_back("重写 DOM Adapter（val.h）");
  s.plan_steps.push_back("实现 VNode diff + 组件库");
  s.plan_steps.push_back("重写页面与事件接线");

  s.terminals.push_back(TermItem{"1", "pwsh", "running"});
  s.terminals.push_back(TermItem{"2", "cmd", "exited"});
  s.active_term = "1";

  s.subagents.push_back(AppState::SubagentRow{"a1", "explore", "running", "扫描 src/ui 依赖"});
  s.subagents.push_back(AppState::SubagentRow{"a2", "research", "done", "调研 reproc 用法"});
  return s;
}

// ===========================================================================
// UiApp
// ===========================================================================
UiApp::UiApp(IDom& dom, AppState state, IUiBackend* backend)
    : dom_(dom), st_(std::move(state)), backend_(backend) {}

UiApp::~UiApp() {
  // drop all handlers this app registered (ids are monotonic; a new app starts
  // fresh above the old high-water mark)
  bus().retain(std::vector<int>());
}

void UiApp::mount() {
  root_ = dom_.root();
  render();
}

void UiApp::update(const std::function<void(AppState&)>& f) {
  f(st_);
  render();
}

void UiApp::show(const std::string& tab_id) {
  st_.active_tab = tab_id;
  render();
}

void UiApp::toast(const std::string& text) {
  st_.toast = text;
  render();
}

void UiApp::render() {
  VNodePtr neu = build_shell();
  patch(dom_, tree_, neu, root_);
  tree_ = neu;
  last_rendered_tab_ = st_.active_tab;
  // drop handlers from the previous generation (ids are monotonic, so no
  // collision with the still-bound listeners of the current tree)
  std::vector<int> live;
  collect_handler_ids(tree_, &live);
  bus().retain(live);
}

// ===========================================================================
// Shell: top tabs + active page + overlays
// ===========================================================================
VNodePtr UiApp::build_shell() {
  H win = el("div", "window");

  // ---- top tab strip ----
  std::vector<TabSpec> tabs;
  for (size_t i = 0; i < st_.tab_ids.size(); ++i) {
    const std::string& id = st_.tab_ids[i];
    TabSpec t;
    t.key = id;
    if (id == "home") {
      t.title = "主页";
    } else if (id == "settings") {
      t.title = "设置";
    } else {
      t.title = id;
      for (size_t k = 0; k < st_.sessions.size(); ++k) {
        if (st_.sessions[k].id == id) {
          t.title = st_.sessions[k].title;
          t.dirty = st_.sessions[k].dirty;
        }
      }
    }
    t.closable = (id != "home");
    tabs.push_back(t);
  }
  // subagent badge on the strip? keep it on the 子代理 side tab instead.
  H strip = comp::tab_strip(
      tabs, st_.active_tab,
      [this](const std::string& k) { show(k); },
      [this](const std::string& k) { close_tab(k); },
      [this](const std::string&) { open_new_session(); });
  win.add(strip);

  // ---- page ----
  // keyed by tab so switching pages replaces the subtree instead of patching
  // unrelated content into it
  VNodePtr page;
  if (st_.active_tab == "home") {
    page = build_home();
  } else if (st_.active_tab == "settings") {
    page = build_settings();
  } else {
    page = build_session();
  }
  page->key = "page:" + st_.active_tab;
  win.add(page);
  win.add(build_overlays());
  return win.get();
}
// ===========================================================================
// Home
// ===========================================================================
VNodePtr UiApp::build_home() {
  H wrap = el("div", "page home-page");
  // centered title buttons: 探索 / 项目 / 用量统计
  std::vector<std::string> items;
  items.push_back("探索");
  items.push_back("项目");
  items.push_back("用量统计");
  H head = el("div", "home-head");
  head.add(comp::title_tabs(items, st_.home_tab, [this](const std::string& v) {
    update([&](AppState& s) { s.home_tab = std::atoi(v.c_str()); });
  }));
  wrap.add(head);

  H body = el("div", "home-body");
  switch (st_.home_tab) {
    case 0: {
      VNodePtr p = build_home_explore();
      p->key = "home:explore";
      body.add(p);
      break;
    }
    case 2: {
      VNodePtr p = build_home_usage();
      p->key = "home:usage";
      body.add(p);
      break;
    }
    default: {
      VNodePtr p = build_home_projects();
      p->key = "home:projects";
      body.add(p);
      break;
    }
  }
  wrap.add(body);

  // footer actions
  H foot = el("div", "home-foot");
  foot.add(comp::button("新建项目", [this](const std::string&) {
    if (backend_) backend_->call("project.create", Json::object());
    toast("新建项目（待接后端）");
  }, "primary"));
  foot.add(comp::button("打开文件夹", [this](const std::string&) {
    if (backend_) backend_->call("project.open", Json::object());
    toast("打开文件夹（待接后端）");
  }));
  H grow = el("span", "grow");
  foot.add(grow);
  foot.add(comp::button("设置", [this](const std::string&) { show("settings"); }, "ghost"));
  wrap.add(foot);
  return wrap.get();
}

VNodePtr UiApp::build_home_explore() {
  H wrap = el("div", "explore");
  H box = el("div", "explore-box");
  box.add(el("div", "explore-title").text("探索"));
  box.add(el("div", "explore-sub").text("搜索项目、会话与文档（原型）"));
  box.add(comp::search_box(st_.explore_query, "输入关键词…", [this](const std::string& v) {
    update([&](AppState& s) { s.explore_query = v; });
  }));
  wrap.add(box);
  return wrap.get();
}

VNodePtr UiApp::build_home_projects() {
  H wrap = el("div", "projects");
  if (st_.projects.empty()) {
    wrap.add(comp::empty_state("还没有项目。新建或打开一个文件夹开始。", "新建项目",
                               [this](const std::string&) { toast("新建项目（待接后端）"); }));
    return wrap.get();
  }
  for (size_t i = 0; i < st_.projects.size(); ++i) {
    ProjectCard& p = st_.projects[i];
    H card = el("div", "proj-card");
    // left: project info
    H left = el("div", "proj-left");
    left.add(el("div", "proj-name").text(p.name));
    left.add(el("div", "proj-path").text(p.path));
    H meta = el("div", "proj-meta");
    meta.add(el("span", "dim").text(p.updated));
    meta.add(el("span", "dot-sep").text("·"));
    meta.add(el("span", "dim").text(itos(p.session_count) + " 个会话"));
    left.add(meta);
    H actions = el("div", "proj-actions");
    actions.add(comp::button("打开", [this, i](const std::string&) {
      update([&](AppState& s) {
        if (i < s.projects.size()) {
          s.projects[i].expanded = !s.projects[i].expanded;
        }
      });
    }, "primary"));
    actions.add(comp::button("移出列表", [this, i](const std::string&) {
      update([&](AppState& s) {
        if (i < s.projects.size()) s.projects.erase(s.projects.begin() + i);
      });
    }, "ghost"));
    left.add(actions);
    card.add(left);

    // right: expandable session list
    H right = el("div", p.expanded ? "proj-right open" : "proj-right");
    H rhead = el("div", "proj-right-head");
    rhead.on(EventKind::kClick, [this, i](const std::string&) {
      update([&](AppState& s) {
        if (i < s.projects.size()) s.projects[i].expanded = !s.projects[i].expanded;
      });
    });
    rhead.add(el("span", "prh-title").text("会话"));
    rhead.add(el("span", "prh-count").text(itos(p.session_count)));
    rhead.add(el("span", "tb-caret").text(p.expanded ? "\u25be" : "\u25b8"));
    right.add(rhead);
    if (p.expanded) {
      H list = el("div", "proj-sessions");
      for (size_t k = 0; k < p.sessions.size(); ++k) {
        H row = el("div", "proj-session");
        std::string sid = p.sessions[k].first;
        row.on(EventKind::kClick, [this, sid](const std::string&) {
          // open as a tab (create if missing)
          update([&](AppState& s) {
            bool found = false;
            for (size_t t = 0; t < s.tab_ids.size(); ++t) {
              if (s.tab_ids[t] == sid) found = true;
            }
            if (!found) {
              s.tab_ids.push_back(sid);
              SessionTab st;
              st.id = sid;
              st.title = "会话" + sid;
              s.sessions.push_back(st);
            }
            s.active_tab = sid;
          });
        });
        row.add(el("span", "ps-title").text(p.sessions[k].second));
        row.add(el("span", "dim").text("›"));
        list.add(row);
      }
      H addrow = el("div", "proj-session add");
      addrow.on(EventKind::kClick, [this](const std::string&) { open_new_session(); });
      addrow.add(el("span", "ps-title").text("+ 新建会话"));
      list.add(addrow);
      right.add(list);
    }
    card.add(right);
    wrap.add(card);
  }
  return wrap.get();
}

VNodePtr UiApp::build_home_usage() {
  H wrap = el("div", "usage-page");
  // left: scope list
  H left = el("div", "usage-scopes");
  const char* scopes[3] = {"总计", "opencode-zen", "自定义"};
  for (int i = 0; i < 3; ++i) {
    H it = el("div", i == 0 ? "scope sel" : "scope");
    it.add(el("span").text(scopes[i]));
    it.add(el("span", "dim").text("›"));
    left.add(it);
  }
  wrap.add(left);

  H panel = el("div", "usage-panel");
  H row1 = el("div", "chart-row");
  H c1 = el("div", "chart");
  c1.add(el("div", "chart-title").text("按时间（12h / 1天 / 7天 / 30天）"));
  H bars = el("div", "bars");
  int heights[12] = {30, 55, 40, 70, 62, 85, 48, 66, 74, 52, 80, 58};
  for (int i = 0; i < 12; ++i) {
    H b = el("div", "bar");
    b.style("height", itos(heights[i]) + "%");
    bars.add(b);
  }
  c1.add(bars);
  row1.add(c1);
  H c2 = el("div", "chart");
  c2.add(el("div", "chart-title").text("按模型"));
  H pie = el("div", "pie");
  pie.add(el("div", "pie-slice s1"));
  pie.add(el("div", "pie-slice s2"));
  pie.add(el("div", "pie-slice s3"));
  c2.add(pie);
  row1.add(c2);
  panel.add(row1);

  H tbl = el("table", "stat-table");
  H thead = el("tr");
  const char* hd[5] = {"模型", "输入", "输出", "缓存", "成本"};
  for (int i = 0; i < 5; ++i) thead.add(el("th").text(hd[i]));
  tbl.add(thead);
  const char* rows[3][5] = {
      {"gpt-4o-mini", "1.2M", "340K", "80K", "¥12.40"},
      {"claude-sonnet", "820K", "210K", "45K", "¥31.02"},
      {"deepseek-v4", "2.1M", "660K", "120K", "¥6.88"},
  };
  for (int r = 0; r < 3; ++r) {
    H tr = el("tr");
    for (int c = 0; c < 5; ++c) tr.add(el("td").text(rows[r][c]));
    tbl.add(tr);
  }
  panel.add(tbl);
  wrap.add(panel);
  return wrap.get();
}

// ===========================================================================
// Session page
// ===========================================================================
VNodePtr UiApp::build_session() {
  H wrap = el("div", "page session-page");

  // ---- toolbar ----
  H bar = el("div", "session-bar");
  H model_sel = el("div", "bar-group");
  model_sel.add(el("span", "bar-label").text("模型"));
  std::vector<std::string> models;
  models.push_back("gpt-4o-mini");
  models.push_back("claude-sonnet");
  models.push_back("deepseek-v4");
  model_sel.add(comp::select(models, st_.model_index, [this](const std::string& v) {
    update([&](AppState& s) {
      s.model_index = std::atoi(v.c_str());
      s.model = s.model_index == 1 ? "claude-sonnet"
                : s.model_index == 2 ? "deepseek-v4" : "gpt-4o-mini";
    });
  }));
  bar.add(model_sel);

  H perm_sel = el("div", "bar-group");
  perm_sel.add(el("span", "bar-label").text("权限"));
  std::vector<std::string> perms;
  perms.push_back("approve");
  perms.push_back("permissive");
  perms.push_back("full");
  perm_sel.add(comp::select(perms, st_.perm_index, [this](const std::string& v) {
    update([&](AppState& s) {
      s.perm_index = std::atoi(v.c_str());
      s.permission = s.perm_index == 0 ? "approve"
                     : s.perm_index == 2 ? "full" : "permissive";
    });
  }));
  bar.add(perm_sel);

  bar.add(comp::toggle(st_.auto_mode, "自动模式", [this](const std::string&) {
    update([&](AppState& s) { s.auto_mode = !s.auto_mode; });
  }));
  bar.add(comp::toggle(st_.suspended, "挂起", [this](const std::string&) {
    update([&](AppState& s) { s.suspended = !s.suspended; });
  }));

  H grow = el("span", "grow");
  bar.add(grow);
  bar.add(comp::button(st_.plan_open ? "收起规划" : "规划", [this](const std::string&) {
    update([&](AppState& s) { s.plan_open = !s.plan_open; });
  }, "ghost"));
  wrap.add(bar);

  // ---- plan panel (collapsible) ----
  if (st_.plan_open) {
    H plan = el("div", "plan-panel");
    plan.add(el("div", "plan-title").text("任务规划"));
    for (size_t i = 0; i < st_.plan_steps.size(); ++i) {
      H step = el("div", "plan-step");
      step.add(el("span", "plan-no").text(itos(static_cast<int>(i + 1))));
      step.add(el("span", "plan-text").text(st_.plan_steps[i]));
      plan.add(step);
    }
    wrap.add(plan);
  }

  // ---- main: history + side panel ----
  H mid = el("div", "session-mid");
  H hist = el("div", "history");
  if (st_.messages.empty()) {
    hist.add(comp::empty_state("开始新的对话。输入消息后 Enter 发送。", "", nullptr));
  }
  for (size_t i = 0; i < st_.messages.size(); ++i) {
    comp::ChatMsg m = st_.messages[i];
    hist.add(comp::chat_message(m, [this](const std::string&) { toast("已复制"); },
                                [this, i](const std::string&) {
      update([&](AppState& s) {
        if (i < s.messages.size() && !s.messages[i].tools.empty()) {
          // toggle the first block (prototype: single block toggling)
          s.messages[i].tools[0].expanded = !s.messages[i].tools[0].expanded;
        }
      });
    }));
  }
  mid.add(hist);
  mid.add(build_side_panel());
  wrap.add(mid);

  // ---- composer ----
  H comp_box = el("div", "composer");
  H ta = comp::textarea(st_.session_input, "输入消息…（Enter 发送，Shift+Enter 换行）",
                        [this](const std::string& v) {
    st_.session_input = v;  // no re-render while typing (avoid caret jumps)
  },
                        [this](const std::string&) { send_message(); });
  comp_box.add(ta);
  H tools = el("div", "composer-tools");
  tools.add(comp::button("+ 文件", [this](const std::string&) { toast("加入文件（待接后端）"); }, "ghost"));
  tools.add(comp::button("/ 命令", [this](const std::string&) { toast("skill 命令（待接后端）"); }, "ghost"));
  tools.add(comp::button("子代理", [this](const std::string&) {
    update([&](AppState& s) {
      s.subagents.push_back(AppState::SubagentRow{
          "a" + itos(static_cast<int>(s.subagents.size() + 1)), "explore", "running",
          "新子代理任务"});
      s.active_side = "subagents";
    });
  }, "ghost"));
  H g2 = el("span", "grow");
  tools.add(g2);
  tools.add(el("span", "composer-hint").text("Enter 发送 · Shift+Enter 换行"));
  tools.add(comp::button("发送", [this](const std::string&) { send_message(); }, "primary"));
  comp_box.add(tools);
  wrap.add(comp_box);

  // ---- terminal drawer ----
  wrap.add(build_terminal_drawer());
  return wrap.get();
}

// Side panel: free-form tabs (add/close), each renders its own content.
VNodePtr UiApp::build_side_panel() {
  H side = el("div", "side-panel");
  std::vector<TabSpec> tabs;
  for (size_t i = 0; i < st_.side_tabs.size(); ++i) {
    TabSpec t;
    t.key = st_.side_tabs[i].id;
    t.title = st_.side_tabs[i].title;
    t.closable = true;
    if (st_.side_tabs[i].kind == "subagents") {
      t.badge = static_cast<int>(st_.subagents.size());
    }
    tabs.push_back(t);
  }
  side.add(comp::panel_tabs(tabs, st_.active_side,
                            [this](const std::string& k) {
    update([&](AppState& s) { s.active_side = k; });
  },
                            [this](const std::string& k) { close_side_tab(k); },
                            [this](const std::string&) { open_side_tab(""); }));

  H body = el("div", "side-body");
  std::string kind = "usage";
  for (size_t i = 0; i < st_.side_tabs.size(); ++i) {
    if (st_.side_tabs[i].id == st_.active_side) kind = st_.side_tabs[i].kind;
  }

  if (kind == "usage") {
    body.add(comp::key_value("上下文水位", itos(st_.context_pct) + "%"));
    body.add(comp::progress(st_.context_pct));
    body.add(comp::key_value("本会话成本", st_.session_cost));
    body.add(comp::key_value("本轮 tokens", "12.4K / 3.1K"));
  } else if (kind == "subagents") {
    if (st_.subagents.empty()) {
      body.add(comp::empty_state("没有子代理在运行。", "", nullptr));
    }
    for (size_t i = 0; i < st_.subagents.size(); ++i) {
      const AppState::SubagentRow& a = st_.subagents[i];
      H card = el("div", "subagent-card");
      H head = el("div", "sa-head");
      head.add(el("span", a.status == "running" ? "sa-status running" : "sa-status done")
                   .text(a.status == "running" ? "●" : "✓"));
      head.add(el("span", "sa-kind").text(a.kind));
      head.add(el("span", "sa-id dim").text(a.id));
      card.add(head);
      card.add(el("div", "sa-task").text(a.task));
      H acts = el("div", "sa-actions");
      acts.add(comp::button("引导", [this, i](const std::string&) {
        toast("子代理引导（待接后端）");
        (void)i;
      }, "ghost"));
      acts.add(comp::button("取消", [this, i](const std::string&) {
        update([&](AppState& s) {
          if (i < s.subagents.size()) s.subagents.erase(s.subagents.begin() + i);
        });
      }, "ghost"));
      card.add(acts);
      body.add(card);
    }
  } else if (kind == "terminal") {
    for (size_t i = 0; i < st_.terminals.size(); ++i) {
      const TermItem& t = st_.terminals[i];
      H row = el("div", t.tid == st_.active_term ? "term-row sel" : "term-row");
      std::string tid = t.tid;
      row.on(EventKind::kClick, [this, tid](const std::string&) {
        update([&](AppState& s) {
          s.active_term = tid;
          s.terminal_open = true;
        });
      });
      row.add(el("span", t.status == "running" ? "term-dot running" : "term-dot").text("●"));
      row.add(el("span", "term-name").text("#" + t.tid + " " + t.shell));
      row.add(el("span", "dim").text(t.status == "running" ? "运行中" : "已退出"));
      body.add(row);
    }
    H acts = el("div", "side-actions");
    acts.add(comp::button("新建终端", [this](const std::string&) {
      update([&](AppState& s) {
        int n = static_cast<int>(s.terminals.size()) + 1;
        s.terminals.push_back(TermItem{itos(n), "pwsh", "running"});
        s.active_term = itos(n);
        s.terminal_open = true;
      });
    }, "primary"));
    body.add(acts);
  } else if (kind == "files") {
    body.add(comp::empty_state("文件树（待接后端 project.files）", "", nullptr));
  } else if (kind == "git") {
    body.add(comp::empty_state("Git 状态（待接后端）", "", nullptr));
  } else if (kind == "ops") {
    body.add(comp::empty_state("操作记录（待接后端）", "", nullptr));
  } else if (kind == "calls") {
    body.add(comp::empty_state("工具调用记录（待接后端）", "", nullptr));
  } else {
    body.add(comp::empty_state("面板内容", "", nullptr));
  }
  side.add(body);
  return side.get();
}

VNodePtr UiApp::build_terminal_drawer() {
  H drawer = el("div", st_.terminal_open ? "terminal-drawer open" : "terminal-drawer");
  H head = el("div", "td-head");
  // terminal selector
  H sel = el("div", "term-selector");
  for (size_t i = 0; i < st_.terminals.size(); ++i) {
    const TermItem& t = st_.terminals[i];
    H chip = el("span", t.tid == st_.active_term ? "term-chip active" : "term-chip");
    std::string tid = t.tid;
    chip.on(EventKind::kClick, [this, tid](const std::string&) {
      update([&](AppState& s) { s.active_term = tid; });
    });
    chip.text("#" + t.tid + " " + t.shell);
    sel.add(chip);
  }
  head.add(sel);
  H g = el("span", "grow");
  head.add(g);
  head.add(comp::button("停止", [this](const std::string&) { toast("停止终端（待接后端）"); }, "ghost"));
  head.add(comp::icon_button("\u2715", "收起", [this](const std::string&) {
    update([&](AppState& s) { s.terminal_open = false; });
  }));
  drawer.add(head);
  H out = el("pre", "td-out");
  out.text("PS C:\\aaa\\FlashSight> xmake -r\n[ 21%]: compiling.release src/ui/ui_app.cpp\n[100%]: build ok, spent 7.687s\nPS C:\\aaa\\FlashSight> ");
  drawer.add(out);
  return drawer.get();
}

// ===========================================================================
// Settings
// ===========================================================================
VNodePtr UiApp::build_settings() {
  H wrap = el("div", "page settings-page");
  H nav = el("div", "settings-nav");
  const char* groups[4] = {"通用", "模型", "权限", "其他"};
  const char* items[4][5] = {
      {"常规", "外观", "浏览器控制", "电脑控制", ""},
      {"供应商", "模型偏好", "MCP 与 Skill", "子代理", "长期记忆"},
      {"网络", "终端", "文件", "环境", ""},
      {"快捷键", "关于", "", "", ""},
  };
  int flat = 0;
  for (int g = 0; g < 4; ++g) {
    nav.add(el("div", "snav-grp").text(groups[g]));
    for (int i = 0; i < 5; ++i) {
      if (items[g][i][0] == '\0') break;
      int idx = flat++;
      H n = el("div", idx == st_.settings_nav ? "snav-item active" : "snav-item");
      n.on(EventKind::kClick, [this, idx](const std::string&) {
        update([&](AppState& s) { s.settings_nav = idx; });
      });
      n.add(el("span").text(items[g][i]));
      nav.add(n);
    }
  }
  wrap.add(nav);

  H body = el("div", "settings-body");
  if (st_.settings_nav == 5) {
    // 供应商
    body.add(comp::section_title("供应商"));
    std::vector<std::string> provs;
    provs.push_back("opencode-zen（套餐）");
    provs.push_back("[OI] 兼容");
    provs.push_back("Anthropic");
    provs.push_back("自定义");
    body.add(comp::field("供应商", comp::select(provs, st_.provider_index,
                                                [this](const std::string& v) {
      update([&](AppState& s) { s.provider_index = std::atoi(v.c_str()); });
    })));
    body.add(comp::field("Endpoint", comp::text_input(st_.provider_endpoint, "",
                                                      [this](const std::string& v) {
      st_.provider_endpoint = v;
    })));
    body.add(comp::field("API Key", comp::text_input(st_.provider_key, "sk-…",
                                                     [this](const std::string& v) {
      st_.provider_key = v;
    })));
    body.add(comp::field("自定义 Header", comp::text_input(st_.provider_header, "",
                                                           [this](const std::string& v) {
      st_.provider_header = v;
    }), "opencode go 需要 x-opencode-session（自动生成）"));
    H acts = el("div", "settings-actions");
    acts.add(comp::button("测试连接", [this](const std::string&) {
      if (backend_) backend_->call("provider.testConnection", Json::object());
      toast("测试连接（待接后端）");
    }));
    acts.add(comp::button("一键获取模型列表", [this](const std::string&) {
      if (backend_) backend_->call("provider.listModels", Json::object());
      toast("获取模型列表（待接后端）");
    }, "primary"));
    body.add(acts);
  } else if (st_.settings_nav == 1) {
    body.add(comp::section_title("外观"));
    body.add(comp::field("主题", comp::select(std::vector<std::string>{"跟随系统", "浅色", "深色"}, 0,
                                              [](const std::string&) {})));
    body.add(comp::field("字号", comp::select(std::vector<std::string>{"小", "标准", "大"}, 1,
                                              [](const std::string&) {})));
    body.add(comp::field("代码高亮", comp::toggle(true, "", [](const std::string&) {})));
  } else if (st_.settings_nav == 7) {
    body.add(comp::section_title("子代理"));
    const char* agents[3] = {"explore", "research", "review"};
    for (int i = 0; i < 3; ++i) {
      H row = el("div", "settings-row");
      row.add(el("span", "sr-name").text(agents[i]));
      row.add(comp::toggle(true, "", [](const std::string&) {}));
      row.add(comp::select(std::vector<std::string>{"继承会话", "gpt-4o-mini", "claude-sonnet"}, 0,
                           [](const std::string&) {}));
      body.add(row);
    }
  } else if (st_.settings_nav == 10) {
    body.add(comp::section_title("终端"));
    body.add(comp::field("默认 shell", comp::select(
        std::vector<std::string>{"自动探测", "pwsh", "powershell", "cmd", "bash"}, 0,
        [](const std::string&) {})));
    body.add(comp::field("编码", comp::select(std::vector<std::string>{"系统默认", "UTF-8", "GBK"}, 0,
                                              [](const std::string&) {})));
  } else {
    body.add(comp::section_title("常规"));
    body.add(comp::field("思考语言", comp::select(std::vector<std::string>{"自动", "中文", "English"},
                                                  st_.thinking_lang == "zh" ? 1 : st_.thinking_lang == "en" ? 2 : 0,
                                                  [this](const std::string& v) {
      update([&](AppState& s) {
        int i = std::atoi(v.c_str());
        s.thinking_lang = i == 1 ? "zh" : i == 2 ? "en" : "auto";
      });
    }), "切换工具描述与思考输出的语言"));
    body.add(comp::field("历史轮数", comp::text_input("20", "", nullptr)));
    body.add(comp::field("token 预算", comp::text_input("8000", "", nullptr)));
  }
  wrap.add(body);
  return wrap.get();
}

// ===========================================================================
// Overlays
// ===========================================================================
VNodePtr UiApp::build_overlays() {
  H layer = el("div", "overlay-layer");
  if (st_.ask_modal) {
    std::vector<H> body;
    body.push_back(el("div").text(st_.ask_text.empty() ? "模型请求确认。" : st_.ask_text));
    layer.add(comp::modal("需要确认", body, "允许",
                          [this](const std::string&) {
    update([&](AppState& s) { s.ask_modal = false; });
  },
                          [this](const std::string&) {
    update([&](AppState& s) { s.ask_modal = false; });
  }));
  }
  if (!st_.toast.empty()) {
    H t = el("div", "toast");
    t.text(st_.toast);
    layer.add(t);
  }
  return layer.get();
}

// ===========================================================================
// Actions
// ===========================================================================
void UiApp::send_message() {
  if (st_.session_input.empty()) return;
  std::string text = st_.session_input;
  update([&](AppState& s) {
    comp::ChatMsg m;
    m.role = "user";
    m.text = text;
    m.time = "刚刚";
    s.messages.push_back(m);
    // demo assistant reply with a flow block + summary
    comp::ChatMsg a;
    a.role = "assistant";
    a.text = "收到。这是原型回复：\n\n- 事件已接通（点击/输入/回车）\n- 组件由 VNode diff 增量更新\n\n```cpp\npatch(dom, old_tree, new_tree, root);\n```\n";
    a.time = "刚刚";
    comp::ToolBlock tb;
    tb.name = "file.read";
    tb.summary = "src/ui/ui_app.cpp · 420 行";
    tb.detail = "// flow block body (collapsed by default)\nint main() { return 0; }";
    a.tools.push_back(tb);
    a.summary = "事件接线完成；下一步接后端。";
    s.messages.push_back(a);
    s.session_input.clear();
  });
}

void UiApp::open_new_session() {
  update([&](AppState& s) {
    s.session_seq++;
    std::string id = "s" + itos(s.session_seq);
    SessionTab t;
    t.id = id;
    t.title = "会话" + itos(s.session_seq);
    s.sessions.push_back(t);
    s.tab_ids.push_back(id);
    s.active_tab = id;
    s.messages.clear();
    s.session_input.clear();
  });
}

void UiApp::close_tab(const std::string& id) {
  if (id == "home") return;
  update([&](AppState& s) {
    for (size_t i = 0; i < s.tab_ids.size(); ++i) {
      if (s.tab_ids[i] == id) {
        s.tab_ids.erase(s.tab_ids.begin() + i);
        break;
      }
    }
    for (size_t i = 0; i < s.sessions.size(); ++i) {
      if (s.sessions[i].id == id) {
        s.sessions.erase(s.sessions.begin() + i);
        break;
      }
    }
    if (s.active_tab == id) s.active_tab = "home";
  });
}

void UiApp::open_side_tab(const std::string& kind) {
  update([&](AppState& s) {
    s.side_seq++;
    SideTab t;
    t.id = "sb" + itos(s.side_seq);
    t.kind = kind.empty() ? "files" : kind;
    t.title = side_title(t.kind);
    s.side_tabs.push_back(t);
    s.active_side = t.id;
  });
}

void UiApp::close_side_tab(const std::string& id) {
  update([&](AppState& s) {
    for (size_t i = 0; i < s.side_tabs.size(); ++i) {
      if (s.side_tabs[i].id == id) {
        s.side_tabs.erase(s.side_tabs.begin() + i);
        break;
      }
    }
    if (s.active_side == id && !s.side_tabs.empty()) {
      s.active_side = s.side_tabs[0].id;
    }
  });
}

}  // namespace ui
}  // namespace fs
