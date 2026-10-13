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

// last path segment (both separators; the backend may be on another OS)
std::string folder_basename(const std::string& path) {
  size_t end = path.size();
  while (end > 0 && (path[end - 1] == '\\' || path[end - 1] == '/')) --end;
  size_t cut = end;
  while (cut > 0 && path[cut - 1] != '\\' && path[cut - 1] != '/') --cut;
  return path.substr(cut, end - cut);
}

}  // namespace

// ===========================================================================
// AppState
// ===========================================================================
AppState AppState::make() {
  AppState s;
  s.tab_ids.push_back("home");
  s.tab_ids.push_back("settings");
  // side panel starts with the panels that have real data today; others are
  // added on demand via the "+" button
  s.side_tabs.push_back(SideTab{"usage", "用量", "usage"});
  s.side_tabs.push_back(SideTab{"subagents", "子代理", "subagents"});
  s.side_tabs.push_back(SideTab{"terminal", "终端", "terminal"});
  s.side_seq = 3;
  // project/session/terminal/subagent lists are loaded from the backend
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
  if (unsubscribe_events_) unsubscribe_events_();
  bus().retain(std::vector<int>());
}

void UiApp::mount() {
  root_ = dom_.root();
  if (backend_) {
    unsubscribe_events_ = backend_->subscribe(
        [this](const std::string& e, const Json& p) { on_backend_event(e, p); });
    load_projects();
  }
  render();
}

// ---------------------------------------------------------------------------
// Backend plumbing
// ---------------------------------------------------------------------------
void UiApp::call_async(const std::string& method, const Json& args,
                       std::function<void(const BackendResult&)> done) {
  if (!backend_) {
    if (done) {
      BackendResult r;
      r.ok = false;
      r.code = "no-backend";
      r.message = "no backend bound";
      done(r);
    }
    return;
  }
  backend_->call_async(method, args, [this, done](const BackendResult& r) {
    if (!r.ok && !r.message.empty()) {
      // surface backend failures instead of failing silently
      st_.toast = r.code.empty() ? r.message : (r.code + ": " + r.message);
    }
    if (done) done(r);
    render();
  });
}

void UiApp::pump() {
  if (backend_) backend_->poll();
}

std::string UiApp::active_sid() const {
  if (st_.active_tab == "home" || st_.active_tab == "settings") return "";
  return st_.active_tab;
}

void UiApp::load_projects() {
  call_async("project.list", Json::object(), [this](const BackendResult& r) {
    if (!r.ok || !r.data.is_array()) return;
    std::vector<ProjectCard> cards;
    for (size_t i = 0; i < r.data.size(); ++i) {
      const Json& p = r.data[i];
      ProjectCard c;
      c.pid = jsonutil::get<std::string>(p, "pid", "");
      c.name = jsonutil::get<std::string>(p, "name", "");
      c.path = jsonutil::get<std::string>(p, "path", "");
      c.session_count = static_cast<int>(jsonutil::get_int(p, "sessionCount", 0));
      if (c.pid.empty() && c.path.empty()) continue;
      if (c.name.empty()) c.name = folder_basename(c.path);
      if (c.name.empty()) c.name = c.pid;
      cards.push_back(c);
    }
    st_.projects.swap(cards);
    render();
  });
}

void UiApp::load_terminals(const std::string& sid) {
  if (sid.empty()) return;
  call_async("terminal.list", Json{{"sid", sid}}, [this](const BackendResult& r) {
    if (!r.ok || !r.data.is_array()) return;
    std::vector<TermItem> terms;
    for (size_t i = 0; i < r.data.size(); ++i) {
      const Json& t = r.data[i];
      TermItem it;
      it.tid = jsonutil::get<std::string>(t, "tid", "");
      it.shell = jsonutil::get<std::string>(t, "shell", "");
      it.status = jsonutil::get<std::string>(t, "status", "running");
      terms.push_back(it);
    }
    st_.terminals.swap(terms);
    if (!st_.terminals.empty() && st_.active_term.empty()) {
      st_.active_term = st_.terminals[0].tid;
    }
    render();
  });
}

void UiApp::open_session(const std::string& sid, const std::string& title) {
  if (sid.empty()) return;
  // add the tab if missing
  update([&](AppState& s) {
    bool found = false;
    for (size_t t = 0; t < s.tab_ids.size(); ++t) {
      if (s.tab_ids[t] == sid) found = true;
    }
    if (!found) {
      s.tab_ids.push_back(sid);
      SessionTab st;
      st.id = sid;
      st.title = title.empty() ? ("会话" + sid) : title;
      s.sessions.push_back(st);
    }
    s.active_tab = sid;
    s.messages.clear();
  });
  // load history
  call_async("session.get", Json{{"sid", sid}}, [this](const BackendResult& r) {
    if (!r.ok) return;
    const Json& turns = r.data.contains("turns") ? r.data["turns"] : Json::array();
    std::vector<comp::ChatMsg> msgs;
    for (size_t i = 0; i < turns.size(); ++i) {
      const Json& t = turns[i];
      std::string role = jsonutil::get<std::string>(t, "role", "");
      if (role != "user" && role != "assistant") continue;
      comp::ChatMsg m;
      m.role = role;
      m.text = jsonutil::get<std::string>(t, "text",
                 jsonutil::get<std::string>(t, "streamSummary", ""));
      if (role == "assistant" && t.contains("keyPoints") && t["keyPoints"].is_array()) {
        for (size_t k = 0; k < t["keyPoints"].size(); ++k) {
          if (!m.summary.empty()) m.summary += "\n";
          if (t["keyPoints"][k].is_string()) m.summary += t["keyPoints"][k].get<std::string>();
        }
      }
      msgs.push_back(m);
    }
    update([&](AppState& s) { s.messages = msgs; });
    load_terminals(st_.active_tab);
  });
}

void UiApp::load_sessions(size_t project_index) {
  if (project_index >= st_.projects.size()) return;
  std::string pid = st_.projects[project_index].pid;
  if (pid.empty()) return;
  call_async("session.list", Json{{"pid", pid}}, [this, project_index](const BackendResult& r) {
    if (!r.ok || !r.data.is_array()) return;
    std::vector<std::pair<std::string, std::string> > rows;
    for (size_t i = 0; i < r.data.size(); ++i) {
      const Json& s = r.data[i];
      std::string sid = jsonutil::get<std::string>(s, "sid", "");
      std::string title = jsonutil::get<std::string>(s, "title", "");
      if (title.empty()) title = "会话 " + sid;  // untitled: still readable
      if (!sid.empty()) rows.push_back(std::make_pair(sid, title));
    }
    update([&](AppState& st) {
      if (project_index < st.projects.size()) {
        st.projects[project_index].sessions = rows;
        st.projects[project_index].session_count = static_cast<int>(rows.size());
      }
    });
  });
}

void UiApp::load_usage() {
  call_async("usage.query", Json{{"filter", Json::object()}}, [this](const BackendResult& r) {
    if (!r.ok) return;
    update([&](AppState& s) {
      s.usage = r.data;
      s.usage_loaded = true;
    });
  });
}

void UiApp::load_providers() {
  call_async("provider.listProviders", Json::object(), [this](const BackendResult& r) {
    std::vector<AppState::ProviderRow> rows;
    const Json& arr = r.ok ? (r.data.contains("providers") ? r.data["providers"] : r.data)
                           : Json::array();
    if (arr.is_array()) {
      for (size_t i = 0; i < arr.size(); ++i) {
        const Json& p = arr[i];
        AppState::ProviderRow row;
        row.id = jsonutil::get<std::string>(p, "id", "");
        row.kind = jsonutil::get<std::string>(p, "kind", "");
        row.endpoint = jsonutil::get<std::string>(p, "endpoint", "");
        row.enabled = jsonutil::get<bool>(p, "enabled", true);
        rows.push_back(row);
      }
    }
    update([&](AppState& s) {
      s.providers = rows;
      // set even on failure: the load is render-driven, a retry loop would hang
      s.providers_loaded = true;
      if (!rows.empty() && s.provider_endpoint.empty()) s.provider_endpoint = rows[0].endpoint;
    });
  });
}

void UiApp::load_shells() {
  call_async("settings.detectShells", Json::object(), [this](const BackendResult& r) {
    std::vector<std::string> names;
    names.push_back("自动探测");
    const Json& arr = r.ok ? (r.data.contains("shells") ? r.data["shells"] : r.data)
                           : Json::array();
    if (arr.is_array()) {
      for (size_t i = 0; i < arr.size(); ++i) {
        std::string n = jsonutil::get<std::string>(arr[i], "name", "");
        if (!n.empty()) names.push_back(n);
      }
    }
    // always non-empty afterwards -> the render-driven guard stops retrying
    update([&](AppState& s) { s.shells = names; });
  });
}

void UiApp::load_subagents(const std::string& sid) {
  if (sid.empty()) return;
  call_async("subagent.list", Json{{"sid", sid}}, [this](const BackendResult& r) {
    if (!r.ok || !r.data.is_array()) return;
    std::vector<AppState::SubagentRow> rows;
    for (size_t i = 0; i < r.data.size(); ++i) {
      const Json& a = r.data[i];
      AppState::SubagentRow row;
      row.id = jsonutil::get<std::string>(a, "id", "");
      row.kind = jsonutil::get<std::string>(a, "kind", "");
      row.status = jsonutil::get<std::string>(a, "status", "");
      row.task = jsonutil::get<std::string>(a, "task", "");
      rows.push_back(row);
    }
    update([&](AppState& s) { s.subagents = rows; });
  });
}

void UiApp::append_stream_delta(const std::string& text) {
  if (st_.messages.empty() || st_.messages.back().role != "assistant") {
    comp::ChatMsg m;
    m.role = "assistant";
    m.streaming = true;
    st_.messages.push_back(m);
  }
  st_.messages.back().text += text;
  st_.messages.back().streaming = true;
}

void UiApp::on_backend_event(const std::string& event, const Json& payload) {
  ++event_count_;
  if (event == "stream.delta") {
    append_stream_delta(jsonutil::get<std::string>(payload, "text", ""));
  } else if (event == "turn.done") {
    if (!st_.messages.empty() && st_.messages.back().role == "assistant") {
      st_.messages.back().streaming = false;
    }
  } else if (event == "tool.update") {
    std::string name = jsonutil::get<std::string>(payload, "name", "");
    bool ok = jsonutil::get<bool>(payload, "ok", true);
    if (!st_.messages.empty() && st_.messages.back().role == "assistant") {
      comp::ToolBlock tb;
      tb.name = name;
      tb.summary = ok ? "ok" : "failed";
      tb.ok = ok;
      st_.messages.back().tools.push_back(tb);
    }
  } else if (event == "permission.prompt") {
    st_.ask_modal = true;
    st_.ask_text = "允许执行 " + jsonutil::get<std::string>(payload, "tool", "") + "？";
  } else if (event == "subagent.done") {
    std::string id = jsonutil::get<std::string>(payload, "id", "");
    for (size_t i = 0; i < st_.subagents.size(); ++i) {
      if (st_.subagents[i].id == id) st_.subagents[i].status = "done";
    }
  } else if (event == "subagent.steered") {
    st_.toast = "子代理已收到引导";
  } else if (event == "todo.updated") {
    st_.toast = "任务清单已更新";
  } else if (event == "terminal.exit") {
    std::string tid = jsonutil::get<std::string>(payload, "tid", "");
    for (size_t i = 0; i < st_.terminals.size(); ++i) {
      if (st_.terminals[i].tid == tid) st_.terminals[i].status = "exited";
    }
  } else if (event == "loop.interrupted") {
    st_.toast = "已中断（可继续）";
  }
  render();
}

void UiApp::show(const std::string& tab_id) {
  st_.active_tab = tab_id;
  render();
  // opening a session tab pulls its history + terminals
  std::string sid = active_sid();
  if (!sid.empty() && st_.messages.empty()) {
    open_session(sid, "");
  }
}

void UiApp::update(const std::function<void(AppState&)>& f) {
  f(st_);
  render();
}

void UiApp::toast(const std::string& text) {
  st_.toast = text;
  render();
}

void UiApp::render() {
  // With synchronous backends (NativeBackend), a loader triggered from inside
  // a build can complete and call update()->render() while we are still
  // building. Defer that nested render instead of patching mid-build.
  if (render_in_progress_) {
    render_pending_ = true;
    return;
  }
  render_in_progress_ = true;
  do {
    render_pending_ = false;
    ++render_count_;
    VNodePtr neu = build_shell();
    patch(dom_, tree_, neu, root_);
    tree_ = neu;
    last_rendered_tab_ = st_.active_tab;
    // drop handlers from the previous generation (ids are monotonic, so no
    // collision with the still-bound listeners of the current tree)
    std::vector<int> live;
    collect_handler_ids(tree_, &live);
    bus().retain(live);
    // (re)wire the side-panel drag divider; bind_splitter is idempotent, so
    // this works whether the node survived a patch or was just created
    VNodePtr bar = find_by_key(tree_, "side-splitter");
    VNodePtr side = find_by_key(tree_, "side-panel");
    if (bar && side && bar->dom.valid() && side->dom.valid()) {
      dom_.bind_splitter(bar->dom, side->dom);
    }
    // (re)wire the window-wide drop zone (folder drag & drop)
    VNodePtr dz = find_by_key(tree_, "drop-zone");
    if (dz && dz->dom.valid()) {
      dom_.bind_drop_zone(dz->dom, [this](const std::string& name) {
        handle_dropped_folder(name);
      });
    }
  } while (render_pending_);
  render_in_progress_ = false;
}

// ===========================================================================
// Shell: top tabs + active page + overlays
// ===========================================================================
VNodePtr UiApp::build_shell() {
  H win = el("div", "window");
  win.key("drop-zone");  // drag & drop a folder anywhere on the window

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
    int idx = std::atoi(v.c_str());
    update([&](AppState& s) { s.home_tab = idx; });
    if (idx == 2 && !st_.usage_loaded) load_usage();
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

  // footer actions; both flows start from the native folder picker (the
  // browser cannot hand out OS paths, so the backend opens the dialog)
  H foot = el("div", "home-foot");
  foot.add(comp::button("打开文件夹", [this](const std::string&) {
    pick_and_open_folder();
  }, "primary"));
  foot.add(comp::button("新建项目", [this](const std::string&) {
    call_async("project.pickFolder", Json::object(), [this](const BackendResult& r) {
      if (!r.ok) return;
      std::string path = jsonutil::get<std::string>(r.data, "path", "");
      if (path.empty()) return;  // cancelled
      call_async("project.create", Json{{"path", path}}, [this](const BackendResult&) {
        load_projects();
      });
    });
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
    wrap.add(comp::empty_state(
        "还没有项目。点「打开文件夹」选择目录，或直接把文件夹拖进窗口。",
        "打开文件夹", [this](const std::string&) { pick_and_open_folder(); }));
    return wrap.get();
  }
  for (size_t i = 0; i < st_.projects.size(); ++i) {
    ProjectCard& p = st_.projects[i];
    H card = el("div", "proj-card");
    // top row: project identity + actions
    H top = el("div", "proj-top");
    H left = el("div", "proj-left");
    left.add(el("div", "proj-name").text(p.name));
    left.add(el("div", "proj-path").text(p.path));
    top.add(left);
    H actions = el("div", "proj-actions");
    std::string pid = p.pid;
    if (!pid.empty()) {
      // open the folder in the OS file manager (not "expand the card")
      actions.add(comp::button("在资源管理器中打开", [this, pid](const std::string&) {
        call_async("project.reveal", Json{{"pid", pid}}, [this](const BackendResult& r) {
          if (!r.ok) toast("无法打开文件管理器");
        });
      }, "ghost"));
      actions.add(comp::button("移出列表", [this, pid](const std::string&) {
        call_async("project.remove", Json{{"pid", pid}}, [this](const BackendResult&) {
          load_projects();
        });
      }, "ghost"));
    }
    top.add(actions);
    card.add(top);

    // sessions area: the dominant half, always visible; clicking it expands
    H right = el("div", p.expanded ? "proj-right open" : "proj-right");
    H rhead = el("div", "proj-right-head");
    rhead.on(EventKind::kClick, [this, i](const std::string&) {
      update([&](AppState& s) {
        if (i < s.projects.size()) s.projects[i].expanded = !s.projects[i].expanded;
      });
      if (i < st_.projects.size() && st_.projects[i].expanded) load_sessions(i);
    });
    rhead.add(el("span", "prh-title").text("会话"));
    rhead.add(el("span", "prh-count").text(itos(p.expanded ? static_cast<int>(p.sessions.size())
                                                           : p.session_count)));
    rhead.add(el("span", "grow"));
    rhead.add(el("span", "prh-toggle").text(p.expanded ? "收起 \u25be" : "展开 \u25b8"));
    right.add(rhead);
    if (p.expanded) {
      H list = el("div", "proj-sessions");
      for (size_t k = 0; k < p.sessions.size(); ++k) {
        H row = el("div", "proj-session");
        std::string sid = p.sessions[k].first;
        std::string title = p.sessions[k].second;
        row.on(EventKind::kClick, [this, sid, title](const std::string&) {
          open_session(sid, title);
        });
        row.add(el("span", "ps-title").text(p.sessions[k].second));
        row.add(el("span", "dim").text("›"));
        list.add(row);
      }
      if (p.sessions.empty()) {
        H empty = el("div", "proj-session dim-none");
        empty.add(el("span", "ps-title").text("暂无会话"));
        list.add(empty);
      }
      H addrow = el("div", "proj-session add");
      std::string pid2 = p.pid;
      addrow.on(EventKind::kClick, [this, pid2](const std::string&) {
        new_session_in_project(pid2);
      });
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

  // header: totals from usage.query (falls back to placeholders when empty)
  H head = el("div", "usage-head");
  std::string calls = "0";
  std::string cost = "—";
  std::string in_tok = "0";
  std::string out_tok = "0";
  if (st_.usage_loaded) {
    calls = std::to_string(jsonutil::get_int(st_.usage, "calls", 0));
    in_tok = std::to_string(jsonutil::get_int(st_.usage, "totalIn", 0));
    out_tok = std::to_string(jsonutil::get_int(st_.usage, "totalOut", 0));
    std::string kind = jsonutil::get<std::string>(st_.usage, "costKind", "");
    double c = jsonutil::get_number(st_.usage, "totalCost", 0.0);
    char buf[64];
    std::snprintf(buf, sizeof(buf), kind == "money" ? "%.4f" : "%.0f", c);
    cost = std::string(buf) + (kind == "quota" ? " (额度)" : "");
  }
  head.add(comp::key_value("调用次数", calls));
  head.add(comp::key_value("输入 tokens", in_tok));
  head.add(comp::key_value("输出 tokens", out_tok));
  head.add(comp::key_value("总成本", cost));
  panel.add(head);

  H row1 = el("div", "chart-row");
  H c1 = el("div", "chart");
  c1.add(el("div", "chart-title").text("按时间（12h / 1天 / 7天 / 30天）"));
  H bars = el("div", "bars");
  int heights[12] = {30, 55, 40, 70, 62, 85, 48, 66, 74, 52, 80, 58};
  if (st_.usage_loaded && st_.usage.contains("byDay")) {
    const Json& byDay = st_.usage["byDay"];
    std::vector<int> hs;
    for (Json::const_iterator it = byDay.begin(); it != byDay.end(); ++it) {
      int64_t n = jsonutil::get_int(it.value(), "in", 0) +
                  jsonutil::get_int(it.value(), "out", 0);
      hs.push_back(static_cast<int>(n > 1000 ? 100 : (n / 10)));
    }
    for (size_t i = 0; i < hs.size() && i < 12; ++i) heights[i] = hs[i] > 4 ? hs[i] : 4;
  }
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

  // table: real byProviderModel rows, else a placeholder
  H tbl = el("table", "stat-table");
  H thead = el("tr");
  const char* hd[5] = {"模型", "输入", "输出", "缓存", "成本"};
  for (int i = 0; i < 5; ++i) thead.add(el("th").text(hd[i]));
  tbl.add(thead);
  bool any = false;
  if (st_.usage_loaded && st_.usage.contains("byProviderModel") &&
      st_.usage["byProviderModel"].is_object()) {
    const Json& pm = st_.usage["byProviderModel"];
    for (Json::const_iterator it = pm.begin(); it != pm.end(); ++it) {
      const Json& e = it.value();
      H tr = el("tr");
      std::string model = jsonutil::get<std::string>(e, "model", "");
      std::string prov = jsonutil::get<std::string>(e, "provider", "");
      tr.add(el("td").text(prov.empty() ? model : (prov + " / " + model)));
      tr.add(el("td").text(std::to_string(jsonutil::get_int(e, "in", 0))));
      tr.add(el("td").text(std::to_string(jsonutil::get_int(e, "out", 0))));
      tr.add(el("td").text(std::to_string(jsonutil::get_int(e, "cache", 0))));
      tr.add(el("td").text(std::to_string(jsonutil::get_int(e, "cost", 0))));
      tbl.add(tr);
      any = true;
    }
  }
  if (!any) {
    H tr = el("tr");
    for (int c = 0; c < 5; ++c) tr.add(el("td").text(c == 0 ? "（暂无用量记录）" : ""));
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

  // There is no separate switch/toolbar strip: everything lives in or under
  // the composer (per user feedback).

  // ---- plan panel (collapsible, toggled from the composer bar) ----
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
  // drag divider between history and the side panel
  H split = el("div", "side-splitter");
  split.key("side-splitter");
  mid.add(split);
  mid.add(build_side_panel());
  wrap.add(mid);

  // ---- composer ----
  H comp_box = el("div", "composer");
  H ta = comp::textarea(st_.session_input, "输入消息…（Enter 发送，Shift+Enter 换行）",
                        [this](const std::string& v) {
    st_.session_input = v;  // no re-render while typing (avoid caret jumps)
  },
                        [this](const std::string&) { send_message(); });
  ta.key("session-input");  // send_message() clears it directly after sending
  comp_box.add(ta);
  H tools = el("div", "composer-tools");
  // left: session functions, compact buttons
  tools.add(comp::button("+ 文件", [this](const std::string&) { toast("加入文件（待接后端）"); }, "ghost"));
  tools.add(comp::button("/ 命令", [this](const std::string&) { toast("skill 命令（待接后端）"); }, "ghost"));
  tools.add(comp::button("子代理", [this](const std::string&) {
    if (active_sid().empty()) {
      toast("请先打开一个会话");
      return;
    }
    open_or_focus_side_tab("subagents");
  }, "ghost"));
  tools.add(comp::button("规划", [this](const std::string&) {
    bool open = !st_.plan_open;
    std::string sid = active_sid();
    update([&](AppState& s) { s.plan_open = open; });
    if (open && !sid.empty()) {
      call_async("plan.get", Json{{"sid", sid}}, [this](const BackendResult& r) {
        if (!r.ok) return;
        std::vector<std::string> steps;
        const Json& tree = r.data.contains("plan") ? r.data["plan"] : r.data;
        if (tree.is_array()) {
          for (size_t i = 0; i < tree.size(); ++i) {
            std::string t = jsonutil::get<std::string>(tree[i], "title", "");
            if (t.empty() && tree[i].is_string()) t = tree[i].get<std::string>();
            if (!t.empty()) steps.push_back(t);
          }
        }
        update([&](AppState& s) { s.plan_steps = steps; });
      });
    }
  }, "ghost"));
  // middle: session settings (selects styled compactly)
  H grow = el("span", "grow");
  tools.add(grow);
  H model_sel = el("div", "bar-group");
  std::vector<std::string> models;
  models.push_back("gpt-4o-mini");
  models.push_back("claude-sonnet");
  models.push_back("deepseek-v4");
  model_sel.add(comp::select(models, st_.model_index, [this](const std::string& v) {
    int idx = std::atoi(v.c_str());
    std::string model = idx == 1 ? "" : idx == 2 ? "deepseek-v4" : "gpt-4o-mini";
    update([&](AppState& s) {
      s.model_index = idx;
      s.model = model;
    });
    std::string sid = active_sid();
    if (!sid.empty()) call_async("session.setModel", Json{{"sid", sid}, {"model", model}});
  }));
  tools.add(model_sel);
  H perm_sel = el("div", "bar-group");
  std::vector<std::string> perms;
  perms.push_back("approve");
  perms.push_back("permissive");
  perms.push_back("full");
  perm_sel.add(comp::select(perms, st_.perm_index, [this](const std::string& v) {
    int idx = std::atoi(v.c_str());
    std::string perm = idx == 0 ? "approve" : idx == 2 ? "full" : "permissive";
    std::string sid = active_sid();
    update([&](AppState& s) {
      s.perm_index = idx;
      s.permission = perm;
    });
    if (!sid.empty()) {
      call_async("session.setPermission", Json{{"sid", sid}, {"permission", perm}},
                 [this, sid, perm](const BackendResult& r) {
        if (r.ok) return;
        // low->high needs an explicit token confirmation; surface it
        if (r.code == "confirm-required") {
          call_async("session.requestPermissionToken",
                     Json{{"sid", sid}, {"newPermission", perm}},
                     [this, sid, perm](const BackendResult& t) {
            std::string token = jsonutil::get<std::string>(t.data, "token", "");
            if (token.empty()) return;
            call_async("session.setPermissionConfirm",
                       Json{{"sid", sid}, {"permission", perm}, {"token", token}});
          });
        }
      });
    }
  }));
  tools.add(perm_sel);
  // 挂起 is a plain button and sits right next to 发送
  tools.add(comp::button(st_.suspended ? "恢复" : "挂起", [this](const std::string&) {
    bool next = !st_.suspended;
    std::string sid = active_sid();
    update([&](AppState& s) { s.suspended = next; });
    if (!sid.empty()) {
      call_async(next ? "session.suspend" : "session.resume", Json{{"sid", sid}});
    }
  }, "ghost"));
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
  side.key("side-panel");  // find_by_key: splitter drag target
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
    // refresh the panel's data when it becomes active
    std::string kind = "";
    for (size_t i = 0; i < st_.side_tabs.size(); ++i) {
      if (st_.side_tabs[i].id == k) kind = st_.side_tabs[i].kind;
    }
    if (kind == "subagents") load_subagents(active_sid());
    if (kind == "terminal") load_terminals(active_sid());
  },
                            [this](const std::string& k) { close_side_tab(k); },
                            [this](const std::string&) {
    // "+" opens a kind picker; it must never silently default to one page
    update([&](AppState& s) { s.side_menu_open = !s.side_menu_open; });
  }));
  if (st_.side_menu_open) {
    H pop = el("div", "side-menu");
    // kind order roughly by frequency; key -> title from side_title()
    const char* kinds[] = {"files", "ops", "calls", "git", "subagents", "terminal", "usage"};
    for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); ++i) {
      std::string kind = kinds[i];
      H it = el("div", "side-menu-item");
      it.on(EventKind::kClick, [this, kind](const std::string&) {
        update([&](AppState& s) { s.side_menu_open = false; });
        open_or_focus_side_tab(kind);
      });
      it.add(el("span").text(side_title(kind)));
      pop.add(it);
    }
    side.add(pop);
  }

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
        if (i >= st_.subagents.size()) return;
        std::string id = st_.subagents[i].id;
        call_async("subagent.steer", Json{{"id", id}, {"text", "继续"}},
                   [this](const BackendResult&) { toast("已发送引导"); });
      }, "ghost"));
      acts.add(comp::button("取消", [this, i](const std::string&) {
        if (i >= st_.subagents.size()) return;
        std::string id = st_.subagents[i].id;
        call_async("subagent.cancel", Json{{"id", id}}, [this, i](const BackendResult&) {
          update([&](AppState& s) {
            if (i < s.subagents.size()) s.subagents.erase(s.subagents.begin() + i);
          });
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
    acts.add(comp::button("刷新", [this](const std::string&) {
      load_terminals(active_sid());
    }, "primary"));
    body.add(acts);
    body.add(el("div", "hint").text(
        "交互式用户终端需要 PTY（ConPTY/POSIX pty），reproc 不提供；"
        "模型通过 terminal.spawn/exec 起的终端会出现在此列表。"));
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
  head.add(comp::button("轮询", [this](const std::string&) {
    std::string tid = st_.active_term;
    if (tid.empty()) return;
    call_async("terminal.show", Json{{"tid", tid}}, [this](const BackendResult& r) {
      if (!r.ok) return;
      update([&](AppState& s) {
        s.terminal_output = jsonutil::get<std::string>(r.data, "output", "");
      });
    });
  }, "ghost"));
  head.add(comp::button("停止", [this](const std::string&) {
    std::string tid = st_.active_term;
    if (tid.empty()) return;
    call_async("terminal.kill", Json{{"tid", tid}}, [this, tid](const BackendResult&) {
      update([&](AppState& s) {
        for (size_t i = 0; i < s.terminals.size(); ++i) {
          if (s.terminals[i].tid == tid) s.terminals[i].status = "exited";
        }
      });
    });
  }, "ghost"));
  head.add(comp::icon_button("\u2715", "收起", [this](const std::string&) {
    update([&](AppState& s) { s.terminal_open = false; });
  }));
  drawer.add(head);
  H out = el("pre", "td-out");
  out.text(st_.terminal_output.empty() ? "（无输出；点“轮询”获取）" : st_.terminal_output);
  drawer.add(out);
  return drawer.get();
}

// ===========================================================================
// Settings
// ===========================================================================
//
// Navigation is keyed by section (string), so the highlighted item and the
// rendered body can never drift apart. Sections map to real backend settings
// keys (settings_store.hpp) with loaders for the dynamic ones.

namespace {
struct SettingsItem {
  const char* group;
  const char* key;
  const char* title;
};
const SettingsItem kSettingsNav[] = {
    {"通用", "general", "常规"},
    {"通用", "appearance", "外观"},
    {"模型", "providers", "供应商"},
    {"模型", "subagents", "子代理"},
    {"模型", "skills", "MCP 与 Skill"},
    {"模型", "memory", "长期记忆"},
    {"权限", "network", "网络"},
    {"权限", "terminal", "终端"},
    {"权限", "files", "文件"},
    {"权限", "environment", "环境"},
    {"其他", "shortcuts", "快捷键"},
    {"其他", "about", "关于"},
};
const size_t kSettingsNavCount =
    sizeof(kSettingsNav) / sizeof(kSettingsNav[0]);

// Read a boolean setting from the cached section values (default when absent).
bool sv_bool(const Json& vals, const std::string& key, bool def) {
  if (!vals.is_object() || !vals.contains(key)) return def;
  const Json& v = vals[key];
  if (v.is_boolean()) return v.get<bool>();
  if (v.is_object() && v.contains("value") && v["value"].is_boolean()) {
    return v["value"].get<bool>();
  }
  return def;
}

int sv_int(const Json& vals, const std::string& key, int def) {
  if (!vals.is_object() || !vals.contains(key)) return def;
  const Json& v = vals[key];
  if (v.is_number()) return static_cast<int>(v.get<int64_t>());
  if (v.is_object() && v.contains("value") && v["value"].is_number()) {
    return static_cast<int>(v["value"].get<int64_t>());
  }
  return def;
}

std::string sv_str(const Json& vals, const std::string& key,
                   const std::string& def) {
  if (!vals.is_object() || !vals.contains(key)) return def;
  const Json& v = vals[key];
  if (v.is_string()) return v.get<std::string>();
  if (v.is_object() && v.contains("value") && v["value"].is_string()) {
    return v["value"].get<std::string>();
  }
  return def;
}

}  // namespace

void UiApp::set_setting(const std::string& key, const Json& value) {
  update([&](AppState& s) {
    if (!s.settings_values.is_object()) s.settings_values = Json::object();
    Json entry = Json::object();
    entry["value"] = value;
    entry["source"] = "global";
    s.settings_values[key] = entry;
  });
  call_async("settings.set", Json{{"key", key}, {"value", value}});
}

void UiApp::apply_settings(const Json& values) {
  // also usable for the settings.dump shape: {"global": {...}}
  if (values.is_object() && values.contains("global") &&
      values["global"].is_object()) {
    st_.settings_values = values["global"];
  } else if (values.is_object()) {
    st_.settings_values = values;
  }
  // thinking language is host-side too
  std::string lang = sv_str(st_.settings_values, "model.thinkingLang", "");
  if (!lang.empty()) st_.thinking_lang = lang;
  st_.history_recent =
      std::to_string(sv_int(st_.settings_values, "history.recentTurns", 20));
  st_.history_budget =
      std::to_string(sv_int(st_.settings_values, "history.tokenBudget", 8000));
  // long memory / env / cli maps
  if (st_.settings_values.contains("model.longMemory") &&
      st_.settings_values["model.longMemory"].is_object()) {
    st_.long_memory.clear();
    const Json& m = st_.settings_values["model.longMemory"];
    for (Json::const_iterator it = m.begin(); it != m.end(); ++it) {
      if (it.value().is_string()) {
        st_.long_memory.push_back(
            std::make_pair(it.key(), it.value().get<std::string>()));
      }
    }
  }
  st_.env_vars.clear();
  if (st_.settings_values.contains("environment.vars") &&
      st_.settings_values["environment.vars"].is_object()) {
    const Json& m = st_.settings_values["environment.vars"];
    for (Json::const_iterator it = m.begin(); it != m.end(); ++it) {
      std::string v = it.value().is_string()
                          ? it.value().get<std::string>()
                          : (it.value().is_object()
                                 ? jsonutil::get<std::string>(it.value(), "value", "")
                                 : "");
      st_.env_vars.push_back(std::make_pair(it.key(), v));
    }
  }
  st_.cli_tools.clear();
  if (st_.settings_values.contains("environment.cliTools") &&
      st_.settings_values["environment.cliTools"].is_object()) {
    const Json& m = st_.settings_values["environment.cliTools"];
    for (Json::const_iterator it = m.begin(); it != m.end(); ++it) {
      std::string v = it.value().is_string()
                          ? it.value().get<std::string>()
                          : (it.value().is_object()
                                 ? jsonutil::get<std::string>(it.value(), "path", "")
                                 : "");
      st_.cli_tools.push_back(std::make_pair(it.key(), v));
    }
  }
}

void UiApp::load_settings_section(const std::string& section) {
  if (!backend_) return;
  // pull all settings once (settings.dump gives global + project). This may be
  // called from a click handler or from build_settings (via render), so it
  // must not itself render: mutate flags directly, the async completion
  // re-renders when data lands.
  if (!st_.settings_dump_loaded && !st_.settings_dump_inflight) {
    st_.settings_dump_inflight = true;
    call_async("settings.dump", Json::object(), [this](const BackendResult& r) {
      st_.settings_dump_inflight = false;
      if (r.ok) apply_settings(r.data);
      // set even on failure: the load is render-driven, a retry loop would hang
      st_.settings_dump_loaded = true;
    });
  }
  if (section == "providers" && !st_.providers_loaded) {
    load_providers();
  }
  if (section == "subagents" && !st_.presets_loaded) {
    load_preset_agents();
  }
  if (section == "skills" && !st_.skills_loaded) {
    load_skills();
  }
  if (section == "terminal" && st_.shells.empty()) {
    load_shells();
  }
}

void UiApp::load_preset_agents() {
  call_async("subagent.presets", Json::object(), [this](const BackendResult& r) {
    std::vector<AppState::PresetRow> rows;
    if (r.ok && r.data.is_array()) {
      for (size_t i = 0; i < r.data.size(); ++i) {
        const Json& a = r.data[i];
        AppState::PresetRow row;
        row.name = jsonutil::get<std::string>(a, "name", "");
        row.description = jsonutil::get<std::string>(a, "description", "");
        row.enabled = jsonutil::get<bool>(a, "enabled", true);
        row.read_only = jsonutil::get<bool>(a, "readOnly", false);
        rows.push_back(row);
      }
    }
    update([&](AppState& s) {
      s.preset_agents = rows;
      // set even on failure: the load is render-driven, a retry loop would hang
      s.presets_loaded = true;
    });
  });
}

void UiApp::load_skills() {
  call_async("skill.list", Json::object(), [this](const BackendResult& r) {
    std::vector<std::string> names;
    if (r.ok) {
      const Json& arr = r.data.contains("skills") ? r.data["skills"] : r.data;
      if (arr.is_array()) {
        for (size_t i = 0; i < arr.size(); ++i) {
          std::string n = arr[i].is_string() ? arr[i].get<std::string>()
                                             : jsonutil::get<std::string>(arr[i], "name", "");
          if (!n.empty()) names.push_back(n);
        }
      }
    }
    update([&](AppState& s) {
      s.skills = names;
      s.skills_loaded = true;  // render-driven load: stop retrying either way
    });
  });
}

VNodePtr UiApp::build_settings() {
  H wrap = el("div", "page settings-page");
  H nav = el("div", "settings-nav");
  std::string last_group;
  for (size_t i = 0; i < kSettingsNavCount; ++i) {
    const SettingsItem& it = kSettingsNav[i];
    if (last_group != it.group) {
      nav.add(el("div", "snav-grp").text(it.group));
      last_group = it.group;
    }
    std::string key = it.key;
    H n = el("div", key == st_.settings_nav ? "snav-item active" : "snav-item");
    n.on(EventKind::kClick, [this, key](const std::string&) {
      update([&](AppState& s) { s.settings_nav = key; });
      if (backend_) load_settings_section(key);
    });
    n.add(el("span").text(it.title));
    nav.add(n);
  }
  wrap.add(nav);

  H body = el("div", "settings-body");
  const std::string& sec = st_.settings_nav;
  if (backend_) load_settings_section(sec);

  if (sec == "providers") {
    // --- 供应商 ---
    body.add(comp::section_title("供应商"));
    for (size_t i = 0; i < st_.providers.size(); ++i) {
      const AppState::ProviderRow& p = st_.providers[i];
      H row = el("div", "settings-row");
      row.add(el("span", "sr-name").text(p.kind.empty() ? p.id : p.kind));
      row.add(el("span", "dim grow").text(p.endpoint));
      row.add(comp::badge(p.enabled ? "启用" : "停用"));
      std::string id = p.id;
      row.add(comp::button("删除", [this, id](const std::string&) {
        call_async("provider.removeProvider", Json{{"providerId", id}},
                   [this](const BackendResult&) { load_providers(); });
      }, "ghost"));
      body.add(row);
    }
    std::vector<std::string> provs;
    provs.push_back("opencode-zen（套餐）");
    provs.push_back("[OI] 兼容");
    provs.push_back("Anthropic");
    provs.push_back("自定义");
    body.add(comp::field("类型", comp::select(provs, st_.provider_index,
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
    acts.add(comp::button("保存供应商", [this](const std::string&) {
      Json conn = Json::object();
      const char* kinds[4] = {"plan", "openai-compatible", "anthropic", "custom"};
      int ki = st_.provider_index;
      conn["kind"] = kinds[ki < 0 || ki > 3 ? 0 : ki];
      conn["endpoint"] = st_.provider_endpoint;
      if (!st_.provider_key.empty()) conn["apiKey"] = st_.provider_key;
      conn["enabled"] = true;
      call_async("provider.addProvider", Json{{"conn", conn}},
                 [this](const BackendResult&) { load_providers(); });
    }, "primary"));
    acts.add(comp::button("测试连接", [this](const std::string&) {
      std::string pid = st_.providers.empty() ? "" : st_.providers[0].id;
      if (pid.empty()) {
        toast("请先保存一个供应商");
        return;
      }
      call_async("provider.testConnection", Json{{"providerId", pid}},
                 [this](const BackendResult& r) {
        if (r.ok) {
          toast("连接成功（" +
                std::to_string(jsonutil::get_int(r.data, "latencyMs", 0)) + "ms）");
        } else {
          toast("连接失败：" + r.message);
        }
      });
    }));
    acts.add(comp::button("一键获取模型列表", [this](const std::string&) {
      std::string pid = st_.providers.empty() ? "" : st_.providers[0].id;
      if (pid.empty()) {
        toast("请先保存一个供应商");
        return;
      }
      call_async("provider.listModels", Json{{"providerId", pid}},
                 [this](const BackendResult& r) {
        if (r.ok) {
          size_t n = r.data.contains("models") && r.data["models"].is_array()
                         ? r.data["models"].size() : 0;
          toast("获取到 " + std::to_string(n) + " 个模型（级别：" +
                jsonutil::get<std::string>(r.data, "level", "") + "）");
        } else {
          toast("获取失败：" + r.message);
        }
      });
    }, "primary"));
    body.add(acts);
  } else if (sec == "subagents") {
    // --- 子代理 ---
    body.add(comp::section_title("子代理"));
    body.add(comp::hint("预置代理（agent-loop.md 6）；开关与模型写入 subagents.<name>.* 设置键。"));
    for (size_t i = 0; i < st_.preset_agents.size(); ++i) {
      const AppState::PresetRow& a = st_.preset_agents[i];
      H row = el("div", "settings-row");
      row.add(el("span", "sr-name").text(a.name));
      row.add(el("span", "dim grow").text(a.description));
      if (a.read_only) row.add(comp::badge("只读"));
      bool enabled = a.enabled;
      std::string name = a.name;
      row.add(comp::toggle(enabled, "", [this, name, enabled](const std::string&) {
        Json v = !enabled;
        call_async("settings.set",
                   Json{{"key", "subagents." + name + ".enabled"}, {"value", v}},
                   [this, name, v](const BackendResult& r) {
          if (!r.ok) return;
          update([&](AppState& s) {
            for (size_t k = 0; k < s.preset_agents.size(); ++k) {
              if (s.preset_agents[k].name == name) s.preset_agents[k].enabled = v;
            }
          });
        });
      }));
      row.add(comp::select(std::vector<std::string>{"继承会话", "gpt-4o-mini",
                                                    "claude-sonnet"},
                           0, [this, name](const std::string& v) {
        int idx = std::atoi(v.c_str());
        std::string model = idx == 1 ? "gpt-4o-mini"
                            : idx == 2 ? "claude-sonnet" : "";
        set_setting("subagents." + name + ".model", model);
      }));
      body.add(row);
    }
    body.add(comp::field("并行上限", comp::text_input(
        std::to_string(sv_int(st_.settings_values, "subagents.maxParallel", 4)),
        "", [this](const std::string& v) {
      int n = std::atoi(v.c_str());
      set_setting("subagents.maxParallel", n > 0 ? n : 4);
    }), "subagents.maxParallel（默认 4）"));
    body.add(comp::field("嵌套深度", comp::text_input(
        std::to_string(sv_int(st_.settings_values, "subagents.maxDepth", 3)),
        "", [this](const std::string& v) {
      int n = std::atoi(v.c_str());
      set_setting("subagents.maxDepth", n > 0 ? n : 3);
    }), "subagents.maxDepth（默认 3，addon1.md）"));
  } else if (sec == "skills") {
    // --- MCP 与 Skill ---
    body.add(comp::section_title("MCP 与 Skill"));
    body.add(comp::hint("内置 Skill 由模型按需加载（skill.list / skill.load）；"
                        "browser-control 等经由浏览器加载口。"));
    if (st_.skills.empty()) {
      body.add(comp::empty_state("暂无已注册 skill。", "", nullptr));
    }
    for (size_t i = 0; i < st_.skills.size(); ++i) {
      H row = el("div", "settings-row");
      row.add(el("span", "sr-name").text(st_.skills[i]));
      std::string name = st_.skills[i];
      row.add(comp::button("查看", [this, name](const std::string&) {
        call_async("skill.load", Json{{"name", name}},
                   [this, name](const BackendResult& r) {
          toast(r.ok ? ("已加载 skill: " + name) : ("加载失败：" + r.message));
        });
      }, "ghost"));
      body.add(row);
    }
    body.add(comp::hint("MCP：连接、工具发现与调用尚未接线（mcp.* 为 P3 后置项）。"));
  } else if (sec == "memory") {
    // --- 长期记忆 ---
    body.add(comp::section_title("长期记忆"));
    body.add(comp::hint("键值对存于 model.longMemory，随设置持久化，模型可读；"
                        "会话内持久要点见 session.summarize（turn.keyPoints）。"));
    for (size_t i = 0; i < st_.long_memory.size(); ++i) {
      H row = el("div", "settings-row");
      row.add(el("span", "sr-name").text(st_.long_memory[i].first));
      row.add(el("span", "dim grow").text(st_.long_memory[i].second));
      body.add(row);
    }
    if (st_.long_memory.empty()) {
      body.add(comp::empty_state("暂无长期记忆条目。", "", nullptr));
    }
  } else if (sec == "appearance") {
    body.add(comp::section_title("外观"));
    body.add(comp::field("主题", comp::select(
        std::vector<std::string>{"跟随系统", "浅色", "深色"}, 0,
        [this](const std::string& v) {
      int idx = std::atoi(v.c_str());
      const char* themes[3] = {"default", "light", "dark"};
      set_setting("ui.theme", themes[idx < 0 || idx > 2 ? 0 : idx]);
    }), "网页端深浅色经 prefers-color-scheme 自动；桌面端接 WhaleUI 主题。"));
    body.add(comp::field("界面语言", comp::select(
        std::vector<std::string>{"跟随系统", "中文", "English"}, 0,
        [this](const std::string& v) {
      int idx = std::atoi(v.c_str());
      const char* langs[3] = {"auto", "zh", "en"};
      set_setting("ui.language", langs[idx < 0 || idx > 2 ? 0 : idx]);
    })));
    body.add(comp::field("代码高亮", comp::toggle(
        sv_bool(st_.settings_values, "ui.codeHighlight", true), "",
        [this](const std::string&) {
      bool cur = sv_bool(st_.settings_values, "ui.codeHighlight", true);
      set_setting("ui.codeHighlight", !cur);
    })));
  } else if (sec == "network") {
    body.add(comp::section_title("网络"));
    body.add(comp::field("网络总开关", comp::toggle(
        sv_bool(st_.settings_values, "permissions.network.enabled", true),
        "允许模型发起网络请求（fetch/search）", [this](const std::string&) {
      bool cur = sv_bool(st_.settings_values, "permissions.network.enabled", true);
      set_setting("permissions.network.enabled", !cur);
    })));
    body.add(comp::field("校验证书", comp::toggle(
        sv_bool(st_.settings_values, "permissions.network.verifyCert", true), "",
        [this](const std::string&) {
      bool cur = sv_bool(st_.settings_values, "permissions.network.verifyCert", true);
      set_setting("permissions.network.verifyCert", !cur);
    })));
    body.add(comp::hint("注意：httplib 不读系统代理环境变量；需要代理时在应用层设置。"));
  } else if (sec == "terminal") {
    body.add(comp::section_title("终端"));
    std::vector<std::string> shell_opts = st_.shells;
    if (shell_opts.empty()) shell_opts.push_back("自动探测");
    body.add(comp::field("默认 shell", comp::select(shell_opts, st_.shell_index,
                                                    [this, shell_opts](const std::string& v) {
      int idx = std::atoi(v.c_str());
      update([&](AppState& s) { s.shell_index = idx; });
      std::string name = (idx <= 0 || idx >= static_cast<int>(shell_opts.size()))
                             ? "" : shell_opts[idx];
      call_async("settings.setShell", Json{{"name", name}},
                 [this](const BackendResult& r) {
        toast(r.ok ? "已设置 shell" : ("设置失败：" + r.message));
      });
    }), "bash > zsh > pwsh7+ > PowerShell > cmd（cmd 兜底）"));
    body.add(comp::field("编码", comp::select(
        std::vector<std::string>{"系统默认", "UTF-8", "GBK"}, 0,
        [this](const std::string& v) {
      int idx = std::atoi(v.c_str());
      const char* enc[3] = {"", "UTF-8", "GBK"};
      set_setting("terminal.encoding", enc[idx < 0 || idx > 2 ? 0 : idx]);
    }), "终端 spawn 时使用"));
  } else if (sec == "files") {
    body.add(comp::section_title("文件"));
    body.add(comp::field("自动 git init", comp::toggle(
        sv_bool(st_.settings_values, "permissions.files.autoGitInit", false),
        "非 git 项目打开时自动 git init（默认关，permissions.md §3）",
        [this](const std::string&) {
      bool cur = sv_bool(st_.settings_values, "permissions.files.autoGitInit", false);
      set_setting("permissions.files.autoGitInit", !cur);
    })));
    body.add(comp::hint("黑/白名单编辑在 P4 提供；优先级：黑名单 > 白名单 > 三档默认。"));
  } else if (sec == "environment") {
    body.add(comp::section_title("环境"));
    body.add(comp::hint("全局环境变量 + 项目 project.json 覆盖（可 ignoreGlobal）；"
                        "终端 spawn 时合并注入。"));
    for (size_t i = 0; i < st_.env_vars.size(); ++i) {
      H row = el("div", "settings-row");
      row.add(el("span", "sr-name").text(st_.env_vars[i].first));
      row.add(el("span", "dim grow").text(st_.env_vars[i].second));
      body.add(row);
    }
    if (st_.env_vars.empty()) {
      body.add(comp::empty_state("暂无环境变量。", "", nullptr));
    }
    body.add(comp::section_title("CLI 工具路径"));
    body.add(comp::hint("key-value 表；运行时把 key 告诉模型，模型按 key 调用。"));
    for (size_t i = 0; i < st_.cli_tools.size(); ++i) {
      H row = el("div", "settings-row");
      row.add(el("span", "sr-name").text(st_.cli_tools[i].first));
      row.add(el("span", "dim grow").text(st_.cli_tools[i].second));
      body.add(row);
    }
    if (st_.cli_tools.empty()) {
      body.add(comp::empty_state("暂无 CLI 工具条目。", "", nullptr));
    }
  } else if (sec == "shortcuts") {
    body.add(comp::section_title("快捷键"));
    body.add(comp::hint("v1 仅做可配表（shortcuts 设置键）；键位绑定待桌面端接入。"));
    const char* keys[][2] = {
        {"发送消息", "Enter"},    {"换行", "Shift+Enter"},
        {"关闭标签页", "Ctrl+W"}, {"新建标签页", "Ctrl+T"},
    };
    for (int i = 0; i < 4; ++i) {
      H row = el("div", "settings-row");
      row.add(el("span", "sr-name").text(keys[i][0]));
      row.add(el("span", "dim grow").text(keys[i][1]));
      body.add(row);
    }
  } else if (sec == "about") {
    body.add(comp::section_title("关于"));
    body.add(comp::key_value("FlashSight", st_.about_version));
    body.add(comp::key_value("构建", "C++14 · xmake · wasm(emcc) · 内嵌本地服务"));
    body.add(comp::key_value("UI", "C++ VNode 框架（DOM Adapter / diff / Composers / user）"));
    body.add(comp::hint("WhaleUI 就绪后：桌面版改走 NativeBackend 进程内直调 FsApi，不再起 HTTP。"));
  } else {
    // --- 常规 ---
    body.add(comp::section_title("常规"));
    body.add(comp::field("思考语言", comp::select(
        std::vector<std::string>{"自动", "中文", "English"},
        st_.thinking_lang == "zh" ? 1 : st_.thinking_lang == "en" ? 2 : 0,
        [this](const std::string& v) {
      int i = std::atoi(v.c_str());
      std::string lang = i == 1 ? "zh" : i == 2 ? "en" : "auto";
      update([&](AppState& s) { s.thinking_lang = lang; });
      call_async("thinking.setLang", Json{{"lang", lang}},
                 [this](const BackendResult& r) {
        toast(r.ok ? "已切换思考语言" : ("失败：" + r.message));
      });
    }), "切换工具描述与思考输出的语言（model.thinkingLang）"));
    body.add(comp::field("历史轮数", comp::text_input(st_.history_recent, "",
                                                      [this](const std::string& v) {
      st_.history_recent = v;
    }), "压缩折叠前保留的最近轮数（history.recentTurns）"));
    body.add(comp::field("token 预算", comp::text_input(st_.history_budget, "",
                                                        [this](const std::string& v) {
      st_.history_budget = v;
    }), "压缩折叠的 token 预算（history.tokenBudget）"));
    body.add(comp::field("防过拟合", comp::toggle(
        sv_bool(st_.settings_values, "session.overfitEnabled", false), "",
        [this](const std::string&) {
      bool cur = sv_bool(st_.settings_values, "session.overfitEnabled", false);
      set_setting("session.overfitEnabled", !cur);
    }), "重复输出/工具循环检测（默认关）"));
    body.add(comp::field("用量保留天数", comp::text_input(
        std::to_string(sv_int(st_.settings_values, "usage.retainDays", 90)), "",
        [this](const std::string& v) {
      int n = std::atoi(v.c_str());
      set_setting("usage.retainDays", n > 0 ? n : 90);
    }), "usage.retainDays（默认 90）"));
    H save = el("div", "settings-actions");
    save.add(comp::button("保存", [this](const std::string&) {
      Json v1 = st_.history_recent.empty() ? Json(20)
                                           : Json(std::stoi(st_.history_recent));
      Json v2 = st_.history_budget.empty() ? Json(8000)
                                           : Json(std::stoi(st_.history_budget));
      call_async("settings.set", Json{{"key", "history.recentTurns"}, {"value", v1}});
      call_async("settings.set", Json{{"key", "history.tokenBudget"}, {"value", v2}},
                 [this](const BackendResult& r) { toast(r.ok ? "已保存" : "保存失败"); });
    }, "primary"));
    body.add(save);
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
    int ask_id = st_.ask_id;
    layer.add(comp::modal("需要确认", body, "允许",
                          [this, ask_id](const std::string&) {
    std::string sid = active_sid();
    update([&](AppState& s) { s.ask_modal = false; });
    if (sid.empty()) return;
    if (ask_id > 0) {
      // ask tool: record the choice
      call_async("tool.askAnswer",
                 Json{{"sid", sid}, {"askId", ask_id}, {"selection", "allow"}});
    } else {
      // permission gate: approve and resume
      call_async("session.approve", Json{{"sid", sid}, {"yes", true}});
    }
  },
                          [this, ask_id](const std::string&) {
    std::string sid = active_sid();
    update([&](AppState& s) { s.ask_modal = false; });
    if (sid.empty()) return;
    if (ask_id > 0) {
      call_async("tool.askAnswer",
                 Json{{"sid", sid}, {"askId", ask_id}, {"selection", "deny"}});
    } else {
      call_async("session.approve", Json{{"sid", sid}, {"yes", false}});
    }
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
  std::string sid = active_sid();
  // echo the user's message immediately (optimistic; the assistant text streams in)
  update([&](AppState& s) {
    comp::ChatMsg m;
    m.role = "user";
    m.text = text;
    m.time = "刚刚";
    s.messages.push_back(m);
    s.session_input.clear();
  });
  // The DOM textarea still holds the typed text: no re-render happens while
  // typing, so both the old and new trees say value="". Write the cleared
  // value through directly.
  VNodePtr ta = find_by_key(tree_, "session-input");
  if (ta && ta->dom.valid()) dom_.set_attr(ta->dom, "value", "");
  if (sid.empty() || !backend_) {
    // no session/backend: keep a local demo reply so the prototype stays usable
    update([&](AppState& s) {
      comp::ChatMsg a;
      a.role = "assistant";
      a.text = "（未绑定会话后端）这是原型回复。\n\n- 事件已接通\n- 组件由 VNode diff 增量更新\n";
      a.time = "刚刚";
      s.messages.push_back(a);
    });
    return;
  }
  call_async("session.send", Json{{"sid", sid}, {"text", text}});
}

void UiApp::open_new_session() {
  // a real session needs a project; without one, fall back to a local tab
  if (!backend_ || st_.projects.empty()) {
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
    return;
  }
  new_session_in_project(st_.projects[0].pid);
}

void UiApp::new_session_in_project(const std::string& pid) {
  if (!backend_) return;
  std::string cwd;
  for (size_t i = 0; i < st_.projects.size(); ++i) {
    if (st_.projects[i].pid == pid) {
      cwd = st_.projects[i].path;
      break;
    }
  }
  call_async("session.new", Json{{"pid", pid}, {"cwd", cwd}},
             [this](const BackendResult& r) {
    if (!r.ok) return;
    std::string sid = jsonutil::get<std::string>(r.data, "sid", "");
    if (sid.empty()) sid = jsonutil::get<std::string>(r.data, "id", "");
    if (sid.empty()) return;
    update([&](AppState& s) {
      SessionTab t;
      t.id = sid;
      t.title = "会话" + itos(static_cast<int>(s.sessions.size() + 1));
      s.sessions.push_back(t);
      s.tab_ids.push_back(sid);
      s.active_tab = sid;
      s.messages.clear();
      s.session_input.clear();
    });
    load_terminals(sid);
  });
}

void UiApp::pick_and_open_folder() {
  // the OS dialog runs in the native process; the browser gets the path back
  call_async("project.pickFolder", Json::object(), [this](const BackendResult& r) {
    if (!r.ok) {
      toast("打开文件夹选择框失败");
      return;
    }
    std::string path = jsonutil::get<std::string>(r.data, "path", "");
    if (path.empty()) return;  // user cancelled
    call_async("project.open", Json{{"path", path}}, [this](const BackendResult& r2) {
      if (r2.ok) toast("已打开项目");
      load_projects();
    });
  });
}

void UiApp::handle_dropped_folder(const std::string& name) {
  if (name.empty()) return;
  call_async("project.resolveDrop", Json{{"name", name}}, [this, name](const BackendResult& r) {
    if (!r.ok) return;
    if (jsonutil::get<bool>(r.data, "notFound", false)) {
      toast("未找到文件夹「" + name + "」，请用「打开文件夹」手动选择");
      return;
    }
    std::string path = jsonutil::get<std::string>(r.data, "path", "");
    if (path.empty()) return;
    call_async("project.open", Json{{"path", path}}, [this](const BackendResult& r2) {
      if (r2.ok) toast("已打开拖入的项目");
      load_projects();
    });
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
  if (kind.empty()) return;  // never silently default to a tab kind
  update([&](AppState& s) {
    s.side_seq++;
    SideTab t;
    t.id = "sb" + itos(s.side_seq);
    t.kind = kind;
    t.title = side_title(t.kind);
    s.side_tabs.push_back(t);
    s.active_side = t.id;
  });
}

void UiApp::open_or_focus_side_tab(const std::string& kind) {
  if (kind.empty()) return;
  // an existing tab of the same kind is activated instead of duplicated
  for (size_t i = 0; i < st_.side_tabs.size(); ++i) {
    if (st_.side_tabs[i].kind == kind) {
      std::string key = st_.side_tabs[i].id;
      update([&](AppState& s) { s.active_side = key; });
      if (kind == "subagents") load_subagents(active_sid());
      if (kind == "terminal") load_terminals(active_sid());
      return;
    }
  }
  open_side_tab(kind);
  if (kind == "subagents") load_subagents(active_sid());
  if (kind == "terminal") load_terminals(active_sid());
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
