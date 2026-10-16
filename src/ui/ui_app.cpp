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

#include <chrono>
#include <cstdio>

#include "fs_i18n.hpp"
#include "term/fs_vt.hpp"

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#endif

namespace fs {
namespace ui {

using comp::TabSpec;

namespace {

// i18n: 源码写中文，tr() 按当前语言查表（中文时原样返回）
std::string tr(const std::string& zh) { return i18n::tr(zh); }

std::string itos(int v) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%d", v);
  return std::string(buf);
}

// what a terminal tab shows: the user's name when given, else the shell
std::string term_display_name(const TermItem& t) {
  if (!t.label.empty()) return t.label;
  return t.shell.empty() ? std::string("shell") : t.shell;
}

// 0xRRGGBB -> "#rrggbb" for an inline style
std::string color_css(int32_t rgb) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "#%06x", static_cast<unsigned>(rgb) & 0xFFFFFF);
  return std::string(buf);
}

// A terminal grid (terminal.grid reply) as plain text: one line per row, the
// runs concatenated. This is what the overview shows, so escape sequences are
// already resolved by the engine and never reach the screen.
std::string term_grid_text(const Json& grid) {
  if (!grid.is_object()) return std::string();
  const Json& lines = grid["lines"];
  if (!lines.is_array()) return std::string();
  std::string out;
  for (size_t y = 0; y < lines.size(); ++y) {
    std::string line;
    const Json& runs = lines[y];
    if (runs.is_array()) {
      for (size_t k = 0; k < runs.size(); ++k) {
        line += jsonutil::get<std::string>(runs[k], "t", "");
      }
    }
    while (!line.empty() && line[line.size() - 1] == ' ') line.erase(line.size() - 1);
    out += line;
    out += "\n";
  }
  while (!out.empty() && out[out.size() - 1] == '\n') out.erase(out.size() - 1);
  return out;
}

// The last `n` non-empty lines of a grid: what the overview card shows. A whole
// screen per terminal made the list unusable as soon as there were a few.
std::string term_grid_tail(const Json& grid, int n) {
  std::string all = term_grid_text(grid);
  if (all.empty()) return all;
  std::vector<std::string> lines;
  size_t i = 0;
  while (i <= all.size()) {
    size_t e = all.find('\n', i);
    if (e == std::string::npos) e = all.size();
    std::string line = all.substr(i, e - i);
    if (!line.empty()) lines.push_back(line);
    if (e == all.size()) break;
    i = e + 1;
  }
  if (static_cast<int>(lines.size()) > n) {
    lines.erase(lines.begin(), lines.end() - n);
  }
  std::string out;
  for (size_t k = 0; k < lines.size(); ++k) {
    if (!out.empty()) out += "\n";
    out += lines[k];
  }
  return out;
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

// append a class to an already-built VNode (used for mount animations on
// subtrees returned by the page builders)
void add_class(const VNodePtr& v, const std::string& cls) {
  if (!v || cls.empty()) return;
  for (size_t i = 0; i < v->attrs.size(); ++i) {
    if (v->attrs[i].key == "class") {
      if (!v->attrs[i].value.empty()) v->attrs[i].value += " ";
      v->attrs[i].value += cls;
      return;
    }
  }
  VAttr a;
  a.key = "class";
  a.value = cls;
  v->attrs.push_back(a);
}

// Apply an animation INTENT to an already-built subtree (see the anim registry
// in ui_vdom.hpp). Used by the page builders, which return a finished tree.
void add_anim(const VNodePtr& v, const std::string& intent) {
  add_class(v, anim::css_class(intent));
}

// wall clock in ms (toast deadlines). On wasm, std::chrono::steady_clock can
// be non-monotonic/constant depending on the libc++ ABI, so use
// emscripten_get_now() (performance.now()) there; std::chrono on native.
int64_t now_ms() {
#if defined(__EMSCRIPTEN__)
  return static_cast<int64_t>(emscripten_get_now());
#else
  return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now().time_since_epoch())
                                  .count());
#endif
}

const int64_t kToastMs = 3200;  // how long a toast stays on screen

// a session that has never been opened starts from these defaults
SessionView default_session_view() {
  SessionView v;
  v.side_tabs.push_back(SideTab{"usage", "用量", "usage"});
  v.side_tabs.push_back(SideTab{"subagents", "子代理", "subagents"});
  v.side_tabs.push_back(SideTab{"terminal", "终端", "terminal"});
  v.side_seq = 3;
  return v;
}

// split a textarea's lines into a JSON string array (blanks/whitespace dropped)
Json split_lines(const std::string& text) {
  Json arr = Json::array();
  size_t i = 0;
  while (i <= text.size()) {
    size_t e = text.find('\n', i);
    if (e == std::string::npos) e = text.size();
    std::string line = text.substr(i, e - i);
    size_t a = 0, b = line.size();
    while (a < b && (line[a] == ' ' || line[a] == '\t' || line[a] == '\r')) ++a;
    while (b > a && (line[b - 1] == ' ' || line[b - 1] == '\t' || line[b - 1] == '\r')) --b;
    if (b > a) arr.push_back(line.substr(a, b - a));
    if (e == text.size()) break;
    i = e + 1;
  }
  return arr;
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
      st_.toast_deadline_ms = now_ms() + kToastMs;
    }
    if (done) done(r);
    render();
  });
}

void UiApp::pump() {
  if (backend_) backend_->poll();
  // Drive the backend's non-blocking terminal poller. On desktop FsApi runs it
  // on a thread already, so only the browser needs this tick; any output still
  // arrives as a terminal.output event.
  if (backend_ && !backend_->polls_server_side()) {
    backend_->call_async("terminal.tick", Json::object(), nullptr);
  }
  // toasts carry a deadline: clear them here so they cannot linger forever
  // (pump runs on the browser's 250ms timer; the native build calls it too)
  if (!st_.toast.empty() && now_ms() >= st_.toast_deadline_ms) {
    st_.toast.clear();
    render();
  }
}

// Terminal refresh, driven by its own (faster) timer: a terminal must keep up
// with typing, so it cannot wait for the 250ms event pump. Only one grid fetch
// is in flight at a time.
//
// The overview needs every terminal's screen (each card shows its last few
// lines), so there it walks the list round-robin - one terminal per tick keeps
// the request rate bounded no matter how many terminals are open.
void UiApp::tick_terminal() {
  if (!st_.terminal_open) return;
  if (term_grid_inflight_) return;
  if (st_.term_tab != "all") {
    for (size_t i = 0; i < st_.terminals.size(); ++i) {
      if (st_.terminals[i].tid == st_.term_tab) {
        if (st_.terminals[i].status == "running") load_terminal_grid(st_.term_tab);
        return;
      }
    }
    return;
  }
  // overview: round-robin over the running terminals
  if (st_.terminals.empty()) return;
  size_t n = st_.terminals.size();
  for (size_t k = 0; k < n; ++k) {
    size_t i = (term_grid_rr_ + k) % n;
    if (st_.terminals[i].status != "running") continue;
    term_grid_rr_ = (i + 1) % n;
    load_terminal_grid(st_.terminals[i].tid);
    return;
  }
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
    // The session list is part of the card in the wide layout (it is shown
    // without any interaction), so fetch it for every card up front rather than
    // waiting for a click that the wide layout never needs.
    for (size_t i = 0; i < st_.projects.size(); ++i) load_sessions(i);
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
      it.label = jsonutil::get<std::string>(t, "label", "");
      it.status = jsonutil::get<std::string>(t, "status", "running");
      // a terminal whose tab the user closed stays hidden (the process is still
      // running; only 停止 ends it)
      bool hidden = false;
      for (size_t k = 0; k < st_.term_hidden.size(); ++k) {
        if (st_.term_hidden[k] == it.tid) { hidden = true; break; }
      }
      if (hidden) continue;
      terms.push_back(it);
    }
    st_.terminals.swap(terms);
    if (!st_.terminals.empty() && st_.active_term.empty()) {
      st_.active_term = st_.terminals[0].tid;
    }
    render();
  });
}

void UiApp::save_session_view(const std::string& sid) {
  if (sid.empty()) return;
  SessionView& v = views_.get_or_create(sid);
  v.messages = st_.messages;
  v.session_input = st_.session_input;
  v.model = st_.model;
  v.model_index = st_.model_index;
  v.permission = st_.permission;
  v.perm_index = st_.perm_index;
  v.auto_mode = st_.auto_mode;
  v.suspended = st_.suspended;
  v.context_pct = st_.context_pct;
  v.session_cost = st_.session_cost;
  v.side_tabs = st_.side_tabs;
  v.active_side = st_.active_side;
  v.side_seq = st_.side_seq;
  v.side_menu_open = st_.side_menu_open;
  v.side_width = st_.side_width;
  v.active_pid = st_.active_pid;
  v.files_path = st_.files_path;
  v.file_entries = st_.file_entries;
  v.files_loaded = st_.files_loaded;
  v.file_preview_path = st_.file_preview_path;
  v.file_preview = st_.file_preview;
  v.session_ops = st_.session_ops;
  v.session_calls = st_.session_calls;
  v.skill_menu_open = st_.skill_menu_open;
  v.skill_menu = st_.skill_menu;
  v.terminals = st_.terminals;
  v.active_term = st_.active_term;
  v.terminal_open = st_.terminal_open;
  v.term_height = st_.term_height;
  v.term_outputs = st_.term_outputs;
  v.term_tab = st_.term_tab;
  v.term_grids = st_.term_grids;
  v.term_renaming = st_.term_renaming;
  v.term_rename_text = st_.term_rename_text;
  v.term_cols = st_.term_cols;
  v.term_rows = st_.term_rows;
  v.term_app_keys = st_.term_app_keys;
  v.subagents = st_.subagents;
  v.plan_open = st_.plan_open;
  v.plan_steps = st_.plan_steps;
}

void UiApp::load_session_view(const std::string& sid) {
  SessionView v = default_session_view();
  views_.restore(sid, &v);  // keeps the defaults when the key is new
  st_.messages = v.messages;
  st_.session_input = v.session_input;
  st_.model = v.model;
  st_.model_index = v.model_index;
  st_.permission = v.permission;
  st_.perm_index = v.perm_index;
  st_.auto_mode = v.auto_mode;
  st_.suspended = v.suspended;
  st_.context_pct = v.context_pct;
  st_.session_cost = v.session_cost;
  st_.side_tabs = v.side_tabs;
  st_.active_side = v.active_side;
  st_.side_seq = v.side_seq;
  st_.side_menu_open = false;  // transient popovers never survive a switch
  st_.side_width = v.side_width;
  st_.active_pid = v.active_pid;
  st_.files_path = v.files_path;
  st_.file_entries = v.file_entries;
  st_.files_loaded = v.files_loaded;
  st_.file_preview_path = v.file_preview_path;
  st_.file_preview = v.file_preview;
  st_.session_ops = v.session_ops;
  st_.session_calls = v.session_calls;
  st_.skill_menu_open = false;
  st_.skill_menu = v.skill_menu;
  st_.terminals = v.terminals;
  st_.active_term = v.active_term;
  st_.terminal_open = v.terminal_open;
  st_.term_height = v.term_height;
  st_.term_outputs = v.term_outputs;
  st_.term_tab = v.term_tab;
  st_.term_grids = v.term_grids;
  st_.term_renaming = v.term_renaming;
  st_.term_rename_text = v.term_rename_text;
  st_.term_cols = v.term_cols;
  st_.term_rows = v.term_rows;
  st_.term_app_keys = v.term_app_keys;
  st_.subagents = v.subagents;
  st_.plan_open = v.plan_open;
  st_.plan_steps = v.plan_steps;
}

void UiApp::switch_tab(const std::string& id) {
  // The flat session fields belong to the tab we are leaving. Prefer the
  // tracked key, but fall back to active_tab: if something set active_tab
  // directly, the fields still have to be stashed under the right session or
  // its history would be lost on the way back.
  std::string leaving = last_session_key_;
  if (leaving.empty() && st_.active_tab != "home" && st_.active_tab != "settings") {
    leaving = st_.active_tab;
  }
  if (id == st_.active_tab && last_session_key_ == leaving && leaving == id) return;
  if (!leaving.empty()) save_session_view(leaving);
  st_.active_tab = id;
  std::string sid = (id == "home" || id == "settings") ? "" : id;
  if (sid.empty()) {
    last_session_key_.clear();
    render();
    return;
  }
  bool had_view = views_.has(sid);
  load_session_view(sid);
  last_session_key_ = sid;
  render();
  if (!had_view) load_session(sid);  // first visit: pull history + panels
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
  });
  switch_tab(sid);  // restores an existing view, or fetches on first visit
}

// fetch history + system ops + tool calls for a session into its view
void UiApp::load_session(const std::string& sid) {
  if (sid.empty()) return;
  call_async("session.get", Json{{"sid", sid}}, [this, sid](const BackendResult& r) {
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
    std::string pid = r.data.contains("meta")
                          ? jsonutil::get<std::string>(r.data["meta"], "pid", "")
                          : "";
    std::vector<AppState::OpRow> ops;
    if (r.data.contains("systemOps") && r.data["systemOps"].is_array()) {
      const Json& so = r.data["systemOps"];
      for (size_t i = 0; i < so.size(); ++i) {
        AppState::OpRow o;
        o.op = jsonutil::get<std::string>(so[i], "op", "");
        o.detail = so[i].dump();
        ops.push_back(o);
      }
    }
    std::vector<AppState::CallRow> calls;
    for (size_t i = 0; i < turns.size(); ++i) {
      if (!turns[i].contains("toolCalls") || !turns[i]["toolCalls"].is_array()) continue;
      const Json& tc = turns[i]["toolCalls"];
      for (size_t k = 0; k < tc.size(); ++k) {
        AppState::CallRow c;
        c.name = jsonutil::get<std::string>(tc[k], "name", "");
        c.args = tc[k].contains("args") ? tc[k]["args"].dump() : "";
        calls.push_back(c);
      }
    }
    // write into the session's view; mirror to the flat fields only when the
    // user is still looking at it (a slow reply must not clobber another tab)
    SessionView& v = views_.get_or_create(sid);
    v.loaded = true;
    v.messages = msgs;
    v.session_ops = ops;
    v.session_calls = calls;
    if (!pid.empty()) v.active_pid = pid;
    if (st_.active_tab == sid) {
      st_.messages = msgs;
      st_.session_ops = ops;
      st_.session_calls = calls;
      if (!pid.empty()) st_.active_pid = pid;
      render();
    }
    load_terminals(sid);
    if (!pid.empty()) load_files(pid, "");
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
        st.projects[project_index].sessions_loaded = true;
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
        row.name = jsonutil::get<std::string>(p, "name", "");
        row.kind = jsonutil::get<std::string>(p, "kind", "");
        row.endpoint = jsonutil::get<std::string>(p, "baseUrl", "");
        row.enabled = jsonutil::get<bool>(p, "enabled", true);
        row.has_key = jsonutil::get<bool>(p, "hasKey", false);
        rows.push_back(row);
      }
    }
    update([&](AppState& s) {
      s.providers = rows;
      // set even on failure: the load is render-driven, a retry loop would hang
      s.providers_loaded = true;
    });
  });
}

void UiApp::load_provider_templates() {
  call_async("provider.templates", Json::object(), [this](const BackendResult& r) {
    std::vector<AppState::ProviderTemplate> rows;
    if (r.ok && r.data.is_array()) {
      for (size_t i = 0; i < r.data.size(); ++i) {
        const Json& t = r.data[i];
        AppState::ProviderTemplate row;
        row.id = jsonutil::get<std::string>(t, "id", "");
        row.name = jsonutil::get<std::string>(t, "name", "");
        row.kind = jsonutil::get<std::string>(t, "kind", "");
        row.base_url = jsonutil::get<std::string>(t, "baseUrl", "");
        row.description = jsonutil::get<std::string>(t, "description", "");
        if (t.contains("headers") && t["headers"].is_object()) {
          const Json& h = t["headers"];
          for (Json::const_iterator it = h.begin(); it != h.end(); ++it) {
            if (it.value().is_string()) {
              row.headers.push_back(
                  std::make_pair(it.key(), it.value().get<std::string>()));
            }
          }
        }
        rows.push_back(row);
      }
    }
    update([&](AppState& s) {
      s.provider_templates = rows;
      s.provider_templates_loaded = true;
    });
  });
}

void UiApp::load_mcp_tools() {
  call_async("mcp.list", Json::object(), [this](const BackendResult& r) {
    std::vector<AppState::McpRow> rows;
    const Json& arr = r.ok && r.data.contains("tools") ? r.data["tools"] : Json::array();
    if (arr.is_array()) {
      for (size_t i = 0; i < arr.size(); ++i) {
        const Json& t = arr[i];
        AppState::McpRow row;
        row.name = jsonutil::get<std::string>(t, "name", "");
        row.dir = jsonutil::get<std::string>(t, "dir", "");
        row.enabled = jsonutil::get<bool>(t, "enabled", true);
        rows.push_back(row);
      }
    }
    update([&](AppState& s) {
      s.mcp_tools = rows;
      s.mcp_loaded = true;  // render-driven load: stop retrying either way
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

void UiApp::load_files(const std::string& pid, const std::string& rel) {
  if (pid.empty()) return;
  call_async("project.files", Json{{"pid", pid}, {"path", rel}},
             [this, rel](const BackendResult& r) {
    std::vector<AppState::FileEntry> rows;
    const Json& arr = r.ok && r.data.contains("entries") ? r.data["entries"] : Json::array();
    if (arr.is_array()) {
      for (size_t i = 0; i < arr.size(); ++i) {
        AppState::FileEntry e;
        e.name = jsonutil::get<std::string>(arr[i], "name", "");
        e.is_dir = jsonutil::get<bool>(arr[i], "isDir", false);
        e.size = jsonutil::get_int(arr[i], "size", 0);
        if (!e.name.empty()) rows.push_back(e);
      }
    }
    update([&](AppState& s) {
      s.file_entries = rows;
      s.files_path = rel;
      s.files_loaded = true;
      s.file_preview_path.clear();
      s.file_preview.clear();
    });
  });
}

void UiApp::preview_file(const std::string& rel) {
  if (st_.active_pid.empty() || rel.empty()) return;
  std::string pid = st_.active_pid;
  call_async("project.readFile", Json{{"pid", pid}, {"path", rel}},
             [this, rel](const BackendResult& r) {
    update([&](AppState& s) {
      s.file_preview_path = rel;
      s.file_preview = r.ok ? jsonutil::get<std::string>(r.data, "content", "")
                            : ("（无法预览：" + r.message + "）");
    });
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

// Backend events are per-session (payload.sid). They must land in that
// session's view, not blindly on whatever tab happens to be active - a
// background session can stream while the user is elsewhere.
void UiApp::apply_session_event(const std::string& sid, const std::string& event,
                                const Json& payload) {
  bool is_active = sid.empty() || sid == st_.active_tab;
  // only touch views_ for a background session; a fresh one gets the default
  // panels seeded so opening it later does not look empty
  SessionView* vp = nullptr;
  if (!is_active) vp = &views_.get_or_create(sid);
  if (event == "stream.delta") {
    std::string text = jsonutil::get<std::string>(payload, "text", "");
    std::vector<comp::ChatMsg>& m = is_active ? st_.messages : vp->messages;
    if (m.empty() || m.back().role != "assistant") {
      comp::ChatMsg a;
      a.role = "assistant";
      a.streaming = true;
      m.push_back(a);
    }
    m.back().text += text;
    m.back().streaming = true;
  } else if (event == "turn.done") {
    std::vector<comp::ChatMsg>& m = is_active ? st_.messages : vp->messages;
    if (!m.empty() && m.back().role == "assistant") m.back().streaming = false;
  } else if (event == "tool.update") {
    std::string name = jsonutil::get<std::string>(payload, "name", "");
    bool ok = jsonutil::get<bool>(payload, "ok", true);
    std::vector<comp::ChatMsg>& m = is_active ? st_.messages : vp->messages;
    if (!m.empty() && m.back().role == "assistant") {
      comp::ToolBlock tb;
      tb.name = name;
      tb.summary = ok ? "ok" : "failed";
      tb.ok = ok;
      m.back().tools.push_back(tb);
    }
  } else if (event == "subagent.done") {
    std::string id = jsonutil::get<std::string>(payload, "id", "");
    std::vector<AppState::SubagentRow>& subs = is_active ? st_.subagents : vp->subagents;
    for (size_t i = 0; i < subs.size(); ++i) {
      if (subs[i].id == id) subs[i].status = "done";
    }
  } else if (event == "terminal.exit") {
    std::string tid = jsonutil::get<std::string>(payload, "tid", "");
    std::vector<TermItem>& terms = is_active ? st_.terminals : vp->terminals;
    for (size_t i = 0; i < terms.size(); ++i) {
      if (terms[i].tid == tid) terms[i].status = "exited";
    }
    // The screen itself lives in the backend (that is where the VT parser is),
    // and pump() re-reads the visible terminal's grid, so nothing to do here -
    // the accumulated text above is only for the overview and for copy.
  } else if (event == "terminal.output") {
    // pushed by the backend's non-blocking poller (no manual refresh).
    // Output accumulates per terminal, so "所有终端" can show every stream
    // and switching tabs never loses what another one printed.
    std::string tid = jsonutil::get<std::string>(payload, "tid", "");
    std::string text = jsonutil::get<std::string>(payload, "text", "");
    std::map<std::string, std::string>& outs =
        is_active ? st_.term_outputs : vp->term_outputs;
    outs[tid] += text;
    if (outs[tid].size() > (1u << 20)) outs[tid].erase(0, outs[tid].size() - (1u << 20));
    // A terminal the model started shows up here first: register it so the tab
    // strip lists it without waiting for a manual refresh. A terminal the user
    // closed the tab for is not resurrected by its own output.
    bool hidden = false;
    for (size_t i = 0; i < st_.term_hidden.size(); ++i) {
      if (st_.term_hidden[i] == tid) { hidden = true; break; }
    }
    std::vector<TermItem>& terms = is_active ? st_.terminals : vp->terminals;
    bool known = hidden;
    for (size_t i = 0; i < terms.size(); ++i) {
      if (terms[i].tid == tid) { known = true; break; }
    }
    if (!known && !tid.empty()) {
      TermItem it;
      it.tid = tid;
      it.shell = "shell";
      it.status = "running";
      terms.push_back(it);
    }
  }
}

void UiApp::on_backend_event(const std::string& event, const Json& payload) {
  ++event_count_;
  std::string sid = jsonutil::get<std::string>(payload, "sid", "");
  // session-scoped events go to their own view (may be a background tab)
  if (!sid.empty() && sid != st_.active_tab) {
    apply_session_event(sid, event, payload);
    render();
    return;
  }
  if (event == "stream.delta" || event == "turn.done" || event == "tool.update" ||
      event == "subagent.done" || event == "terminal.exit" ||
      event == "terminal.output") {
    apply_session_event(sid, event, payload);
  } else if (event == "permission.prompt") {
    st_.ask_modal = true;
    st_.ask_text = "允许执行 " + jsonutil::get<std::string>(payload, "tool", "") + "？";
  } else if (event == "subagent.steered") {
    st_.toast = "子代理已收到引导";
    st_.toast_deadline_ms = now_ms() + kToastMs;
  } else if (event == "todo.updated") {
    st_.toast = "任务清单已更新";
    st_.toast_deadline_ms = now_ms() + kToastMs;
  } else if (event == "loop.interrupted") {
    st_.toast = "已中断（可继续）";
    st_.toast_deadline_ms = now_ms() + kToastMs;
  }
  render();
}

void UiApp::show(const std::string& tab_id) {
  switch_tab(tab_id);
}

void UiApp::update(const std::function<void(AppState&)>& f) {
  f(st_);
  render();
}

void UiApp::toast(const std::string& text) {
  st_.toast = text;
  st_.toast_deadline_ms = now_ms() + kToastMs;
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
      // persist the dragged width into state so it survives tab switches and
      // node re-creation (the inline width alone is lost with the node)
      dom_.bind_splitter(bar->dom, side->dom, [this](int w) {
        if (w > 0) {
          st_.side_width = w;
          if (!last_session_key_.empty()) {
            views_.get_or_create(last_session_key_).side_width = w;
          }
        }
      });
    }
    // (re)wire the bottom panel's top-edge drag handle (terminal height)
    VNodePtr grip = find_by_key(tree_, "term-splitter");
    VNodePtr panel = find_by_key(tree_, "term-panel");
    if (grip && panel && grip->dom.valid() && panel->dom.valid()) {
      dom_.bind_vsplitter(grip->dom, panel->dom, [this](int h) {
        if (h > 0) {
          st_.term_height = h;
          if (!last_session_key_.empty()) {
            views_.get_or_create(last_session_key_).term_height = h;
          }
        }
      });
    }
    // (re)wire the window-wide drop zone (folder / file / url / text)
    VNodePtr dz = find_by_key(tree_, "drop-zone");
    if (dz && dz->dom.valid()) {
      dom_.bind_drop_zone(dz->dom,
                          [this](const std::string& kind, const std::string& value) {
        handle_drop(kind, value);
      });
    }
    // document-level keys (Escape dismisses transient overlays)
    dom_.bind_global_key([this](const std::string& key) {
      if (key != "Escape") return;
      if (st_.skill_menu_open || st_.side_menu_open) {
        update([&](AppState& s) {
          s.skill_menu_open = false;
          s.side_menu_open = false;
        });
      }
    });
    // the terminal surface takes raw keys (Ctrl+C is a byte, not a letter, and
    // every browser shortcut must be suppressed while it has focus)
    VNodePtr term = find_by_key(tree_, "term-screen");
    if (term && term->dom.valid()) {
      dom_.bind_key_input(term->dom, [this](const KeyPress& k) {
        send_terminal_key(k);
      });
      // How many character cells fit? Only the DOM knows its own pixel size, so
      // the adapter measures the mono font and reports cols/rows. The child
      // process needs this (a full-screen program lays itself out by it).
      dom_.bind_cell_size(term->dom, [this](int cols, int rows) {
        resize_terminal(cols, rows);
      });
    }
    // rename field: focus it so the user can type immediately
    VNodePtr ren = find_by_key(tree_, "term-rename-input");
    if (ren && ren->dom.valid()) dom_.focus(ren->dom);
    // the 文件 button opens the OS file chooser (only the browser can do that),
    // and the adapter hands back the saved paths
    VNodePtr up = find_by_key(tree_, "upload-btn");
    if (up && up->dom.valid()) {
      dom_.bind_file_picker(up->dom, st_.active_pid, [this](const std::string& reply) {
        handle_upload_reply(reply);
      });
    }
    // Focus the terminal surface only when asked (on spawn / tab switch / click).
    // Focusing on every render would fight with the composer: pump() re-renders
    // ~4x/sec, so anything the user focuses elsewhere would be stolen back.
    if (term_focus_pending_) {
      term_focus_pending_ = false;
      if (term && term->dom.valid()) dom_.focus(term->dom);
    }
  } while (render_pending_);
  render_in_progress_ = false;
}

// ===========================================================================
// Shell: top tabs + active page + overlays
// ===========================================================================

// Which animation each PAGE appears with. Every page must be listed here: the
// framework looks the intent up by page id, so a page cannot quietly end up
// without an animation - adding a page means adding its row (and the test
// below asserts the table covers every page the app can show).
namespace {
struct PageAnim {
  const char* page;   // page id ("home", "settings", "session")
  const char* anim;   // anim registry intent
};
const PageAnim kPageAnims[] = {
    {"home", anim::kView},
    {"settings", anim::kView},
    // a session page also fades/slides in; it is the most common switch, so it
    // gets the same treatment rather than being special-cased
    {"session", anim::kView},
};
const size_t kPageAnimCount = sizeof(kPageAnims) / sizeof(kPageAnims[0]);

const char* page_anim_for(const std::string& page) {
  for (size_t i = 0; i < kPageAnimCount; ++i) {
    if (page == kPageAnims[i].page) return kPageAnims[i].anim;
  }
  return nullptr;
}
}  // namespace

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
  std::string page_id;
  VNodePtr page;
  if (st_.active_tab == "home") {
    page_id = "home";
    page = build_home();
  } else if (st_.active_tab == "settings") {
    page_id = "settings";
    page = build_settings();
  } else {
    page_id = "session";
    page = build_session();
  }
  page->key = "page:" + st_.active_tab;
  // The framework applies the animation, looked up from the page's declaration
  // above - so a page switch animates because it is DECLARED, not because a
  // builder remembered to add a class. The key change makes the diff recreate
  // the node, which is what fires the animation.
  add_anim(page, page_anim_for(page_id) ? page_anim_for(page_id) : anim::kView);
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
    H meta = el("div", "proj-meta");
    meta.add(el("span", "dim").text(itos(p.session_count) + " 个会话"));
    if (!p.updated.empty()) {
      meta.add(el("span", "dot-sep").text("·"));
      meta.add(el("span", "dim").text(p.updated));
    }
    left.add(meta);
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

    // Sessions area: the dominant half. The list is ALWAYS built - CSS decides
    // how it is shown (wide: directly, with its own height and scroll; narrow:
    // stacked below and collapsed until the header is clicked). Building it
    // conditionally would mean the wide layout could not show it at all.
    H right = el("div", p.expanded ? "proj-right open" : "proj-right");
    H rhead = el("div", "proj-right-head");
    rhead.on(EventKind::kClick, [this, i](const std::string&) {
      // Wide layout ignores this (CSS shows the list and hides the toggle); in
      // the narrow layout it reveals/collapses the stacked list.
      update([&](AppState& s) {
        if (i < s.projects.size()) s.projects[i].expanded = !s.projects[i].expanded;
      });
    });
    rhead.add(el("span", "prh-title").text(tr("会话")));
    rhead.add(el("span", "prh-count").text(itos(p.expanded ? static_cast<int>(p.sessions.size())
                                                           : p.session_count)));
    rhead.add(el("span", "grow"));
    rhead.add(el("span", "prh-toggle").text(p.expanded ? tr("收起 \u25be")
                                                       : tr("展开 \u25b8")));
    right.add(rhead);
    H list = el("div", "proj-sessions");
    for (size_t k = 0; k < p.sessions.size(); ++k) {
      H row = el("div", "proj-session");
      std::string sid = p.sessions[k].first;
      std::string title = p.sessions[k].second;
      row.on(EventKind::kClick, [this, sid, title](const std::string&) {
        open_session(sid, title);
      });
      row.add(el("span", "ps-title").text(p.sessions[k].second));
      row.add(el("span", "dim").text("\u203a"));
      list.add(row);
    }
    if (p.sessions.empty()) {
      H empty = el("div", "proj-session dim-none");
      empty.add(el("span", "ps-title").text(tr("暂无会话")));
      list.add(empty);
    }
    H addrow = el("div", "proj-session add");
    std::string pid2 = p.pid;
    addrow.on(EventKind::kClick, [this, pid2](const std::string&) {
      new_session_in_project(pid2);
    });
    addrow.add(el("span", "ps-title").text(tr("+ 新建会话")));
    list.add(addrow);
    right.add(list);
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

  // Layout:
  //   top row : [ plan float ] [ history + composer ] [splitter] [ side panel ]
  //   bottom  : terminal bar (collapsible; hidden by default)
  // The plan panel floats over the history instead of pushing it down, and the
  // side panel collapses from the handle on the divider.
  H top = el("div", "session-top");
  H mid = el("div", "session-mid");

  // ---- center column: history (centered column) + composer ----
  H center = el("div", "session-center");
  H hist = el("div", "history");
  H hist_inner = el("div", "history-inner");
  if (st_.messages.empty()) {
    hist_inner.add(comp::empty_state("开始新的对话。输入消息后 Enter 发送。", "", nullptr));
  }
  for (size_t i = 0; i < st_.messages.size(); ++i) {
    comp::ChatMsg m = st_.messages[i];
    hist_inner.add(comp::chat_message(m, [this](const std::string&) { toast("已复制"); },
                                      [this, i](const std::string&) {
      update([&](AppState& s) {
        if (i < s.messages.size() && !s.messages[i].tools.empty()) {
          // toggle the first block (prototype: single block toggling)
          s.messages[i].tools[0].expanded = !s.messages[i].tools[0].expanded;
        }
      });
    }));
  }
  hist.add(hist_inner);
  center.add(hist);

  // ---- composer (embedded in the center column, under the history) ----
  H comp_box = el("div", "composer");
  H ta = comp::textarea(st_.session_input, "输入消息…（Enter 发送，Shift+Enter 换行）",
                        [this](const std::string& v) {
    st_.session_input = v;  // no re-render while typing (avoid caret jumps)
  },
                        [this](const std::string&) { send_message(); });
  ta.key("session-input");  // send_message() clears it directly after sending
  comp_box.add(ta);
  H tools = el("div", "composer-tools");
  // left: session functions, compact buttons (icon + label; label collapses
  // to icon-only when the toolbar is crowded, per user feedback)
  // 文件 = upload an attachment. The click is handled by the DOM adapter
  // (bind_file_picker, wired in render()) because only the browser can open a
  // file chooser; there is no app-side click handler here on purpose.
  H upload = comp::icon_text_button("paperclip", tr("文件"), nullptr, "ghost");
  upload.key("upload-btn");
  tools.add(upload);
  tools.add(comp::icon_text_button("wand-magic-sparkles", "命令", [this](const std::string&) {
    // list skills; picking one inserts "/<name>" into the composer
    call_async("skill.list", Json::object(), [this](const BackendResult& r) {
      if (!r.ok) return;
      std::vector<std::string> names;
      const Json& arr = r.data.contains("skills") ? r.data["skills"] : r.data;
      if (arr.is_array()) {
        for (size_t i = 0; i < arr.size(); ++i) {
          std::string n = arr[i].is_string()
                              ? arr[i].get<std::string>()
                              : jsonutil::get<std::string>(arr[i], "name", "");
          if (!n.empty()) names.push_back(n);
        }
      }
      update([&](AppState& s) {
        s.skill_menu = names;
        s.skill_menu_open = true;
      });
    });
  }, "ghost"));
  tools.add(comp::icon_text_button("robot", "子代理", [this](const std::string&) {
    if (active_sid().empty()) {
      toast("请先打开一个会话");
      return;
    }
    open_or_focus_side_tab("subagents");
  }, "ghost"));
  tools.add(comp::icon_text_button("diagram-project", "规划", [this](const std::string&) {
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
  tools.add(comp::icon_text_button("terminal", "终端", [this](const std::string&) {
    bool opening = !st_.terminal_open;
    update([&](AppState& s) { s.terminal_open = opening; });
    if (opening && !active_sid().empty()) load_terminals(active_sid());
  }, "ghost"));
  // middle: session settings (selects styled compactly)
  H grow = el("span", "grow");
  tools.add(grow);
  H model_sel = el("div", "bar-group");
  std::vector<std::string> models;
  models.push_back("gpt-4o-mini");
  models.push_back("");
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
  // "/" skill picker: an overlay floating ABOVE the composer (it must not
  // push the input around), dismissed by picking / clicking outside / Esc
  if (st_.skill_menu_open) {
    H dismiss = el("div", "skill-menu-backdrop");
    dismiss.on(EventKind::kClick, [this](const std::string&) {
      update([&](AppState& s) { s.skill_menu_open = false; });
    });
    comp_box.add(dismiss);
    H pop = el("div", "skill-menu");
    pop.key("skill-menu");
    if (st_.skill_menu.empty()) {
      pop.add(el("div", "skill-menu-item dim").text("（暂无 skill）"));
    }
    for (size_t i = 0; i < st_.skill_menu.size(); ++i) {
      std::string name = st_.skill_menu[i];
      H it = el("div", "skill-menu-item");
      it.on(EventKind::kClick, [this, name](const std::string&) {
        update([&](AppState& s) {
          s.skill_menu_open = false;
          if (!s.session_input.empty() && s.session_input.back() != ' ') s.session_input += " ";
          s.session_input += "/" + name;
        });
        // write the new value straight to the textarea (no render while typing)
        VNodePtr ta2 = find_by_key(tree_, "session-input");
        if (ta2 && ta2->dom.valid()) dom_.set_attr(ta2->dom, "value", st_.session_input);
      });
      it.add(el("span", "skill-menu-name").text("/" + name));
      pop.add(it);
    }
    H close = el("div", "skill-menu-item dim");
    close.on(EventKind::kClick, [this](const std::string&) {
      update([&](AppState& s) { s.skill_menu_open = false; });
    });
    close.add(el("span").text("取消（Esc）"));
    pop.add(close);
    comp_box.add(pop);
  }
  center.add(comp_box);

  // plan panel floats over the history's left edge (absolute, inside center)
  if (st_.plan_open) {
    H plan = el("div", "plan-float");
    plan.anim("pop");
    H phead = el("div", "plan-head");
    phead.add(el("span", "plan-title").text("任务规划"));
    phead.add(comp::icon_button("\u2715", "收起", [this](const std::string&) {
      update([&](AppState& s) { s.plan_open = false; });
    }));
    plan.add(phead);
    if (st_.plan_steps.empty()) {
      plan.add(el("div", "plan-empty dim").text("暂无规划步骤"));
    }
    for (size_t i = 0; i < st_.plan_steps.size(); ++i) {
      H step = el("div", "plan-step");
      step.add(el("span", "plan-no").text(itos(static_cast<int>(i + 1))));
      step.add(el("span", "plan-text").text(st_.plan_steps[i]));
      plan.add(step);
    }
    center.add(plan);
  }

  mid.add(center);

  // ---- divider + collapsible side panel ----
  H split = el("div", st_.side_collapsed ? "side-splitter collapsed" : "side-splitter");
  split.key("side-splitter");
  H handle = el("div", "splitter-handle");
  handle.attr("title", st_.side_collapsed ? "展开侧栏" : "收起侧栏");
  handle.text(st_.side_collapsed ? "\u2039" : "\u203a");
  handle.on(EventKind::kClick, [this](const std::string&) {
    update([&](AppState& s) { s.side_collapsed = !s.side_collapsed; });
  });
  split.add(handle);
  mid.add(split);
  if (!st_.side_collapsed) mid.add(build_side_panel());
  top.add(mid);
  wrap.add(top);

  // ---- bottom row: terminal (collapsible bar) ----
  wrap.add(build_terminal_drawer());
  return wrap.get();
}

// Side panel: free-form tabs (add/close), each renders its own content.
VNodePtr UiApp::build_side_panel() {
  H side = el("div", "side-panel");
  side.key("side-panel");  // find_by_key: splitter drag target
  // width lives in state so it survives tab switches and node re-creation
  side.style("width", itos(st_.side_width) + "px");
  side.style("flex", "0 0 auto");
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
    if (kind == "files" && !st_.active_pid.empty()) {
      load_files(st_.active_pid, st_.files_path);
    }
  },
                            [this](const std::string& k) { close_side_tab(k); },
                            [this](const std::string&) {
    // "+" opens a temporary page that aggregates every panel kind (with a
    // description); it must never silently default to one page
    update([&](AppState& s) { s.side_menu_open = !s.side_menu_open; });
  }));

  // ---- the "+" page: pick a panel to add ----
  // A page rather than a popup menu: there are enough kinds that each needs a
  // one-line explanation, and a menu cannot carry that. Picking one adds the
  // panel (and, like every panel, it is a real tab that can be closed again).
  if (st_.side_menu_open) {
    H page = el("div", "side-add");
    page.anim(anim::kEnter);
    page.add(el("div", "side-add-head").text(tr("添加面板")));
    page.add(comp::hint(tr("选一个面板加到右侧栏；已打开的直接跳过去。")));
    struct KindInfo {
      const char* kind;
      const char* desc;
    };
    static const KindInfo kKinds[] = {
        {"files", "项目文件树与预览"},
        {"ops", "本会话的系统操作记录"},
        {"calls", "模型发起的工具调用"},
        {"subagents", "子代理运行状态与引导"},
        {"terminal", "终端列表（含模型启动的）"},
        {"usage", "上下文水位与本会话成本"},
        {"git", "Git 状态与变更（待接线）"},
    };
    H grid = el("div", "side-add-grid");
    for (size_t i = 0; i < sizeof(kKinds) / sizeof(kKinds[0]); ++i) {
      std::string kind = kKinds[i].kind;
      H card = el("div", "side-add-card");
      // an already-open panel is shown as such and jumps instead of duplicating
      bool open = false;
      for (size_t k = 0; k < st_.side_tabs.size(); ++k) {
        if (st_.side_tabs[k].kind == kind) open = true;
      }
      if (open) card.cls("open");
      card.on(EventKind::kClick, [this, kind](const std::string&) {
        update([&](AppState& s) { s.side_menu_open = false; });
        open_or_focus_side_tab(kind);
      });
      card.add(el("div", "side-add-name").text(side_title(kind)));
      card.add(el("div", "side-add-desc").text(tr(kKinds[i].desc)));
      if (open) card.add(el("div", "side-add-tag").text(tr("已打开")));
      grid.add(card);
    }
    page.add(grid);
    H acts = el("div", "side-add-acts");
    acts.add(comp::button(tr("取消"), [this](const std::string&) {
      update([&](AppState& s) { s.side_menu_open = false; });
    }, "ghost"));
    page.add(acts);
    side.add(page);
    H sbody = el("div", "side-body");
    side.add(sbody);
    return side.get();
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
    if (st_.active_pid.empty()) {
      body.add(comp::empty_state("先打开一个会话，文件树会显示该项目的文件。", "", nullptr));
    } else {
      // breadcrumb / up row
      H nav = el("div", "files-nav");
      if (!st_.files_path.empty()) {
        nav.add(comp::button("↑ 上级", [this](const std::string&) {
          std::string p = st_.files_path;
          size_t cut = p.find_last_of("/\\");
          std::string parent = cut == std::string::npos ? "" : p.substr(0, cut);
          load_files(st_.active_pid, parent);
        }, "ghost"));
      }
      nav.add(el("span", "files-cwd dim").text(
          st_.files_path.empty() ? "/" : ("/" + st_.files_path)));
      nav.add(comp::button("刷新", [this](const std::string&) {
        load_files(st_.active_pid, st_.files_path);
      }, "ghost"));
      body.add(nav);
      if (st_.file_entries.empty()) {
        body.add(comp::empty_state("（空目录）", "", nullptr));
      }
      for (size_t i = 0; i < st_.file_entries.size(); ++i) {
        const AppState::FileEntry& e = st_.file_entries[i];
        H row = el("div", "file-row");
        std::string name = e.name;
        bool isdir = e.is_dir;
        row.on(EventKind::kClick, [this, name, isdir](const std::string&) {
          std::string rel = st_.files_path.empty() ? name : (st_.files_path + "/" + name);
          if (isdir) {
            load_files(st_.active_pid, rel);
          } else {
            preview_file(rel);
          }
        });
        row.add(el("span", "file-ico").text(e.is_dir ? "▸" : "·"));
        row.add(el("span", "file-name").text(e.name));
        if (!e.is_dir) {
          row.add(el("span", "file-size dim").text(
              e.size > 1024 ? (itos(static_cast<int>(e.size / 1024)) + "K")
                            : (itos(static_cast<int>(e.size)) + "B")));
        }
        body.add(row);
      }
      if (!st_.file_preview_path.empty()) {
        H pv = el("div", "file-preview");
        pv.add(el("div", "file-preview-head").text(st_.file_preview_path));
        H pre = el("pre", "file-preview-body");
        pre.text(st_.file_preview.empty() ? "（空文件）" : st_.file_preview);
        pv.add(pre);
        body.add(pv);
      }
    }
  } else if (kind == "git") {
    body.add(comp::empty_state("Git 状态（需要项目为 git 仓库；git 工具尚未接线）",
                               "", nullptr));
  } else if (kind == "ops") {
    if (st_.session_ops.empty()) {
      body.add(comp::empty_state("暂无操作记录（systemOps）。", "", nullptr));
    }
    for (size_t i = 0; i < st_.session_ops.size(); ++i) {
      H row = el("div", "op-row");
      row.add(el("span", "op-name").text(st_.session_ops[i].op));
      row.add(el("span", "op-detail dim").text(st_.session_ops[i].detail));
      body.add(row);
    }
  } else if (kind == "calls") {
    if (st_.session_calls.empty()) {
      body.add(comp::empty_state("暂无工具调用记录。", "", nullptr));
    }
    for (size_t i = 0; i < st_.session_calls.size(); ++i) {
      H row = el("div", "call-row");
      row.add(el("span", "call-name").text(st_.session_calls[i].name));
      row.add(el("span", "call-args dim").text(st_.session_calls[i].args));
      body.add(row);
    }
  } else {
    body.add(comp::empty_state("面板内容", "", nullptr));
  }
  side.add(body);
  return side.get();
}

// Bottom bar: terminal. Left-right split: left is the screen of the selected
// terminal ("所有终端" lists every stream as text), right is a vertical tab strip
// whose first entry is always 所有终端, followed by one tab per terminal and a
// 新增 entry. The title row toggles the bar; the strip on the TOP edge drags
// the height.
//
// A terminal is a GRID, not a text box: the engine (fs_vt, backend side) parses
// the VT stream into styled cells and this draws them, with the keys going back
// through the DOM adapter's key-input binding. That is why there is no <input>
// here - a real terminal cannot be driven through one (Ctrl+C is not 'c').
//
// 新增 creates a terminal in one click (the shell comes from settings; the name
// is edited on the tab afterwards), and model-started terminals appear in the
// same strip.
VNodePtr UiApp::build_terminal_drawer() {
  H drawer = el("div", st_.terminal_open ? "terminal-bar open" : "terminal-bar");
  drawer.key("term-panel");
  if (st_.terminal_open) {
    drawer.style("height", itos(st_.term_height) + "px");
    // declared here so opening the drawer animates by declaration, not by a
    // class someone remembered to add
    drawer.anim(anim::kDrawer);
  }

  // top edge: drag handle (the whole bar is a flex column, so this is the very
  // first child and sits above the title row). Only while open - a collapsed
  // bar has no height to drag.
  if (st_.terminal_open) {
    H grip = el("div", "td-resize");
    grip.key("term-splitter");
    grip.attr("title", tr("拖拽调整终端高度"));
    drawer.add(grip);
  }

  H head = el("div", "td-head");
  head.on(EventKind::kClick, [this](const std::string&) {
    bool opening = !st_.terminal_open;
    update([&](AppState& s) { s.terminal_open = opening; });
    // refresh on open so terminals started while the bar was collapsed appear
    if (opening && !active_sid().empty()) {
      load_terminals(active_sid());
      if (st_.term_tab != "all") load_terminal_grid(st_.term_tab);
    }
  });
  head.add(el("span", "td-caret").text(st_.terminal_open ? "\u25be" : "\u25b8"));
  head.add(el("span", "td-title").text(tr("终端")));
  if (!st_.terminals.empty()) {
    int running = 0;
    for (size_t i = 0; i < st_.terminals.size(); ++i) {
      if (st_.terminals[i].status == "running") ++running;
    }
    head.add(comp::badge(itos(running) + "/" +
                         itos(static_cast<int>(st_.terminals.size()))));
  }
  drawer.add(head);

  if (!st_.terminal_open) return drawer.get();

  H body = el("div", "td-body");

  // ---- left: the selected terminal's screen (or the aggregate overview) ----
  H left = el("div", "td-left");
  if (st_.term_tab == "all") {
    // Overview: one card per terminal showing its LAST FEW LINES (a full screen
    // per terminal would make the list unusable once there are several) and,
    // when clicked, jumping to that terminal. Nothing here is a dead end.
    H list = el("div", "td-all");
    for (size_t i = 0; i < st_.terminals.size(); ++i) {
      const TermItem& t = st_.terminals[i];
      std::string tid = t.tid;
      H item = el("div", "td-all-item");
      item.on(EventKind::kClick, [this, tid](const std::string&) {
        update([&](AppState& s) { s.term_tab = tid; });
        term_focus_pending_ = true;
        load_terminal_grid(tid);
      });
      H ih = el("div", "td-all-head");
      ih.add(el("span", t.status == "running" ? "term-dot running" : "term-dot")
                 .text("●"));
      ih.add(el("span", "td-all-name").text("#" + tid + " " + term_display_name(t)));
      ih.add(el("span", "grow"));
      ih.add(el("span", "dim").text(t.status == "running" ? tr("运行中")
                                                          : tr("已退出")));
      item.add(ih);
      H pre = el("pre", "td-all-out");
      std::map<std::string, Json>::const_iterator gi = st_.term_grids.find(tid);
      std::string text;
      if (gi != st_.term_grids.end()) {
        text = term_grid_tail(gi->second, 5);
      } else {
        std::map<std::string, std::string>::const_iterator it =
            st_.term_outputs.find(tid);
        if (it != st_.term_outputs.end()) text = it->second;
      }
      pre.text(text.empty() ? tr("暂无输出") : text);
      item.add(pre);
      list.add(item);
    }
    if (st_.terminals.empty()) {
      list.add(comp::empty_state(tr("点「新增」启动一个终端"), "", nullptr));
    }
    left.add(list);
  } else {
    // the terminal surface: focusable, gets raw keys, draws the cell grid
    H screen = el("div", "term-screen");
    screen.key("term-screen");
    screen.attr("tabindex", "0");
    const Json* grid = nullptr;
    std::map<std::string, Json>::const_iterator gi = st_.term_grids.find(st_.term_tab);
    if (gi != st_.term_grids.end()) grid = &gi->second;
    if (grid && grid->is_object()) {
      const Json& lines = (*grid)["lines"];
      bool cursor_visible = jsonutil::get<bool>(*grid, "cursorVisible", false);
      int cx = static_cast<int>(jsonutil::get_int(*grid, "cursorX", 0));
      int cy = static_cast<int>(jsonutil::get_int(*grid, "cursorY", 0));
      if (lines.is_array()) {
        for (size_t y = 0; y < lines.size(); ++y) {
          H row = el("div", "ts-row");
          row.style("height", "1.28em");
          const Json& runs = lines[y];
          if (runs.is_array()) {
            for (size_t k = 0; k < runs.size(); ++k) {
              const Json& run = runs[k];
              std::string text = jsonutil::get<std::string>(run, "t", "");
              if (text.empty()) continue;
              H span = el("span", "ts-run");
              // position by column so a run's own width never shifts the row
              int x = static_cast<int>(jsonutil::get_int(run, "x", 0));
              span.style("left", itos(x) + "ch");
              int32_t fg = static_cast<int32_t>(jsonutil::get_int(run, "fg", -1));
              int32_t bg = static_cast<int32_t>(jsonutil::get_int(run, "bg", -1));
              int attr = static_cast<int>(jsonutil::get_int(run, "a", 0));
              if (fg >= 0) span.style("color", color_css(fg));
              if (bg >= 0) span.style("background", color_css(bg));
              if (attr & 1) span.style("font-weight", "600");
              if (attr & 2) span.style("opacity", ".72");
              if (attr & 4) span.style("font-style", "italic");
              if (attr & 8) span.style("text-decoration", "underline");
              if (attr & 32) span.style("filter", "invert(1)");
              if (attr & 64) span.style("text-decoration", "line-through");
              span.text(text);
              row.add(span);
            }
          }
          // The cursor lives INSIDE its own row. Positioning it as a sibling of
          // the rows (absolute, top: Nem) drifts: the row height is 1.28em while
          // `em` in the offset is the font size, and the panel's padding is not
          // part of either. Inside the row it inherits the row's box, so it
          // lines up by construction.
          if (cursor_visible && static_cast<int>(y) == cy) {
            H cur = el("div", "ts-cursor");
            cur.style("left", itos(cx) + "ch");
            row.add(cur);
          }
          screen.add(row);
        }
      }
    } else {
      screen.add(el("div", "ts-empty").text(tr("（暂无输出）")));
    }
    left.add(screen);
  }
  body.add(left);

  // ---- right: vertical tab strip ----
  H side = el("div", "td-tabs");
  // fixed first entry: aggregate view
  H all_tab = el("div", st_.term_tab == "all" ? "td-tab active" : "td-tab");
  all_tab.on(EventKind::kClick, [this](const std::string&) {
    update([&](AppState& s) { s.term_tab = "all"; });
  });
  all_tab.add(el("i", "fa fa-terminal"));
  all_tab.add(el("span", "td-tab-name").text(tr("所有终端")));
  side.add(all_tab);
  for (size_t i = 0; i < st_.terminals.size(); ++i) {
    const TermItem& t = st_.terminals[i];
    std::string tid = t.tid;
    if (st_.term_renaming == tid) {
      // renaming in place: the terminal keeps running, only the label changes.
      // Losing focus commits (the same as Enter) - a rename field that keeps
      // eating keystrokes after you click elsewhere is a trap.
      H form = el("div", "td-tab td-tab-rename");
      H inp = comp::text_input(st_.term_rename_text, term_display_name(t),
                               [this](const std::string& v) { st_.term_rename_text = v; },
                               [this, tid](const std::string&) {
        rename_terminal(tid, st_.term_rename_text);
      });
      inp.key("term-rename-input");
      inp.on(EventKind::kBlur, [this, tid](const std::string&) {
        if (st_.term_renaming != tid) return;  // already committed
        rename_terminal(tid, st_.term_rename_text);
      });
      form.add(inp);
      side.add(form);
      continue;
    }
    H tb = el("div", t.tid == st_.term_tab ? "td-tab active" : "td-tab");
    tb.on(EventKind::kClick, [this, tid](const std::string&) {
      update([&](AppState& s) { s.term_tab = tid; });
      term_focus_pending_ = true;  // switching to a terminal focuses it
      load_terminal_grid(tid);
    });
    tb.add(el("span", t.status == "running" ? "term-dot running" : "term-dot").text("●"));
    tb.add(el("span", "td-tab-name").text("#" + t.tid + " " + term_display_name(t)));
    // rename (the name is cosmetic; the shell keeps running)
    tb.add(comp::icon_button("\u270e", tr("重命名"), [this, tid](const std::string&) {
      update([&](AppState& s) {
        s.term_renaming = tid;
        s.term_rename_text.clear();
      });
    }));
    // close the TAB without ending the terminal (the model may still be using
    // it); 停止 is the one that actually kills it
    tb.add(comp::icon_button("\u00d7", tr("关闭标签（终端继续运行）"),
                             [this, tid](const std::string&) {
      close_terminal_tab(tid);
    }));
    // 停止 ends the process (there is deliberately no "clear")
    tb.add(comp::icon_button("\u25a0", tr("停止"), [this, tid](const std::string&) {
      call_async("terminal.kill", Json{{"tid", tid}}, [this, tid](const BackendResult&) {
        update([&](AppState& s) {
          for (size_t k = 0; k < s.terminals.size(); ++k) {
            if (s.terminals[k].tid == tid) s.terminals[k].status = "exited";
          }
        });
      });
    }));
    side.add(tb);
  }
  // 新增: one click. The shell comes from settings (that is what "a terminal"
  // means here) and the name is edited on the tab afterwards, so there is
  // nothing to fill in.
  H add = el("div", "td-tab td-tab-add");
  add.on(EventKind::kClick, [this](const std::string&) {
    start_terminal("", "");
  });
  add.add(el("i", "fa fa-plus"));
  add.add(el("span", "td-tab-name").text(tr("新增")));
  side.add(add);
  body.add(side);
  drawer.add(body);
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
    {"模型", "session_defaults", "会话默认"},
    {"模型", "subagents", "子代理"},
    {"模型", "skills", "MCP 与 Skill"},
    {"模型", "memory", "长期记忆"},
    {"权限", "network", "网络"},
    {"权限", "terminal", "终端"},
    {"权限", "files", "文件"},
    {"权限", "environment", "环境"},
    {"权限", "browser", "浏览器控制"},
    {"权限", "computer", "电脑控制"},
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

// settings values arrive either bare or wrapped as {value, source}; return the
// inner value object for map-typed settings (empty object when absent).
Json sv_obj(const Json& vals, const std::string& key) {
  if (!vals.is_object() || !vals.contains(key)) return Json::object();
  const Json& v = vals[key];
  if (v.is_object() && v.contains("value") && v["value"].is_object()) return v["value"];
  return v.is_object() ? v : Json::object();
}

// read a string[] setting as one-newline-per-item text (textarea draft)
std::string join_lines(const Json& vals, const std::string& key) {
  if (!vals.is_object() || !vals.contains(key)) return "";
  const Json& v = vals[key];
  const Json* arr = &v;
  if (v.is_object() && v.contains("value")) arr = &v["value"];
  if (!arr->is_array()) return "";
  std::string out;
  for (size_t i = 0; i < arr->size(); ++i) {
    if (!(*arr)[i].is_string()) continue;
    if (!out.empty()) out += "\n";
    out += (*arr)[i].get<std::string>();
  }
  return out;
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
  // 界面语言是 UI 进程内的状态：写盘的同时立即生效，不用等重启
  if (key == "ui.language" && value.is_string()) {
    i18n::set_lang(i18n::lang_from_setting(value.get<std::string>()));
  }
  // re-derive the cached views (long_memory / env_vars / cli_tools / history)
  // so a settings write reflects immediately without a full dump round-trip
  apply_settings(st_.settings_values);
  render();
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
  // ui.language drives this process's own i18n table (the backend persists it
  // and pushes it back on every dump, so a reload keeps the choice)
  {
    std::string ui_lang = sv_str(st_.settings_values, "ui.language", "");
    if (!ui_lang.empty()) {
      st_.ui_lang = ui_lang;
      i18n::set_lang(i18n::lang_from_setting(ui_lang));
    }
  }
  st_.history_recent =
      std::to_string(sv_int(st_.settings_values, "history.recentTurns", 20));
  st_.history_budget =
      std::to_string(sv_int(st_.settings_values, "history.tokenBudget", 8000));
  // file black/white lists: stored as arrays; join for the textarea draft
  st_.file_blacklist_text = join_lines(st_.settings_values, "permissions.files.blacklist");
  st_.file_whitelist_text = join_lines(st_.settings_values, "permissions.files.whitelist");
  // long memory / env / cli maps (values may be wrapped as {value,source})
  st_.long_memory.clear();
  {
    Json m = sv_obj(st_.settings_values, "model.longMemory");
    for (Json::const_iterator it = m.begin(); it != m.end(); ++it) {
      if (it.value().is_string()) {
        st_.long_memory.push_back(
            std::make_pair(it.key(), it.value().get<std::string>()));
      }
    }
  }
  st_.env_vars.clear();
  {
    Json m = sv_obj(st_.settings_values, "environment.vars");
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
  {
    Json m = sv_obj(st_.settings_values, "environment.cliTools");
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
  if (section == "providers" && !st_.provider_templates_loaded) {
    load_provider_templates();
  }
  if (section == "subagents" && !st_.presets_loaded) {
    load_preset_agents();
  }
  if (section == "skills" && !st_.skills_loaded) {
    load_skills();
  }
  if (section == "skills" && !st_.mcp_loaded) {
    load_mcp_tools();
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
    std::vector<AppState::SkillRow> rows;
    if (r.ok) {
      const Json& arr = r.data.contains("skills") ? r.data["skills"] : r.data;
      if (arr.is_array()) {
        for (size_t i = 0; i < arr.size(); ++i) {
          const Json& s = arr[i];
          AppState::SkillRow row;
          if (s.is_string()) {
            row.name = s.get<std::string>();
          } else {
            row.name = jsonutil::get<std::string>(s, "name", "");
            row.title = jsonutil::get<std::string>(s, "title", "");
            row.description = jsonutil::get<std::string>(s, "description", "");
            row.loadable = jsonutil::get<bool>(s, "loadable", true);
          }
          if (!row.name.empty()) rows.push_back(row);
        }
      }
    }
    update([&](AppState& s) {
      s.skills = rows;
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
      nav.add(el("div", "snav-grp").text(tr(it.group)));
      last_group = it.group;
    }
    std::string key = it.key;
    H n = el("div", key == st_.settings_nav ? "snav-item active" : "snav-item");
    n.on(EventKind::kClick, [this, key](const std::string&) {
      update([&](AppState& s) { s.settings_nav = key; });
      if (backend_) load_settings_section(key);
    });
    n.add(el("span").text(tr(it.title)));
    nav.add(n);
  }
  wrap.add(nav);

  H body = el("div", "settings-body");
  const std::string& sec = st_.settings_nav;
  if (backend_) load_settings_section(sec);

  if (sec == "providers") {
    // --- 供应商：左列表（选择/新建），右侧编辑或模板向导 ---
    H two = el("div", "pv-two");
    H list = el("div", "pv-list");
    H lhead = el("div", "pv-list-head");
    lhead.add(el("span", "pv-list-title").text("供应商"));
    lhead.add(comp::button("新建", [this](const std::string&) {
      update([&](AppState& s) {
        s.provider_creating = true;
        s.provider_selected.clear();
      });
    }, "primary"));
    list.add(lhead);
    if (st_.providers.empty()) {
      list.add(el("div", "pv-empty").text("还没有供应商。点「新建」选择模板。"));
    }
    for (size_t i = 0; i < st_.providers.size(); ++i) {
      const AppState::ProviderRow& p = st_.providers[i];
      bool sel = (!st_.provider_creating && st_.provider_selected == p.id);
      H it = el("div", sel ? "pv-item active" : "pv-item");
      std::string id = p.id;
      it.on(EventKind::kClick, [this, id](const std::string&) { select_provider(id); });
      H top = el("div", "pv-item-top");
      top.add(el("span", "pv-item-name").text(p.name.empty() ? p.id : p.name));
      if (!p.enabled) top.add(comp::badge("停用"));
      if (!p.has_key) top.add(comp::badge("无密钥"));
      it.add(top);
      it.add(el("div", "pv-item-kind").text(p.kind));
      it.add(el("div", "pv-item-url").text(p.endpoint));
      list.add(it);
    }
    two.add(list);

    H detail = el("div", "pv-detail");

    // The form is shared by "new" and "edit": 格式 is a fixed dropdown, the
    // template only pre-fills endpoint + headers, and 高级 holds headers/models.
    auto build_form = [this, &detail](bool creating) {
      detail.add(comp::section_title(creating ? "新建供应商" : "编辑供应商"));
      if (creating) {
        detail.add(comp::hint("模板只是预填 Endpoint 与特殊 Header；格式可随时改。"));
      }
      detail.add(comp::field("名称", comp::text_input(st_.pv_name, "显示名",
                                                      [this](const std::string& v) {
        st_.pv_name = v;
      })));
      // 格式：请求形态（与供应商品牌无关）
      std::vector<std::string> kinds;
      kinds.push_back("OpenAI Chat");
      kinds.push_back("OpenAI Response");
      kinds.push_back("Anthropic");
      kinds.push_back("自定义");
      const char* kind_ids[4] = {"openai-chat", "openai-response", "anthropic",
                                 "custom"};
      int kind_idx = 0;
      for (int k = 0; k < 4; ++k) {
        if (st_.pv_kind == kind_ids[k]) kind_idx = k;
      }
      detail.add(comp::field("格式", comp::select(kinds, kind_idx,
                                                  [this](const std::string& v) {
        int i = std::atoi(v.c_str());
        const char* ids[4] = {"openai-chat", "openai-response", "anthropic", "custom"};
        st_.pv_kind = ids[i < 0 || i > 3 ? 0 : i];
      }), "请求长什么样（endpoint 由模板/下面的地址决定）"));
      detail.add(comp::field("Endpoint", comp::text_input(st_.pv_base_url, "",
                                                          [this](const std::string& v) {
        st_.pv_base_url = v;
      }), "如 https://api.openai.com/v1"));
      detail.add(comp::field("API Key", comp::text_input(st_.pv_key,
                                                         creating ? "sk-…" : "留空 = 不修改",
                                                         [this](const std::string& v) {
        st_.pv_key = v;
      }), creating ? "" : "回显为脱敏值；不改动则留空"));

      // ---- 高级（折叠）----
      H adv = el("div", "pv-advanced");
      H ahead = el("div", "pv-adv-head");
      ahead.on(EventKind::kClick, [this](const std::string&) {
        update([&](AppState& s) { s.pv_advanced = !s.pv_advanced; });
      });
      ahead.add(el("span", "pv-adv-caret").text(st_.pv_advanced ? "\u25be" : "\u25b8"));
      ahead.add(el("span").text("高级（自定义 Header / 模型）"));
      adv.add(ahead);
      if (st_.pv_advanced) {
        H abody = el("div", "pv-adv-body");
        // 会话亲和头：opencode 系端点需要，与"格式"无关
        abody.add(comp::field("发送会话亲和头", comp::toggle(st_.pv_send_session, "",
                                                             [this](const std::string&) {
          update([&](AppState& s) { s.pv_send_session = !s.pv_send_session; });
        }), "opencode 系端点需要 x-opencode-session（会话 id）"));
        abody.add(comp::hint("自定义 Header：仅当端点需要额外请求头时添加。"));
        for (size_t i = 0; i < st_.pv_headers.size(); ++i) {
          H row = el("div", "settings-row");
          row.add(el("span", "sr-name").text(st_.pv_headers[i].first));
          row.add(el("span", "dim grow").text(st_.pv_headers[i].second));
          size_t idx = i;
          row.add(comp::button("删除", [this, idx](const std::string&) {
            update([&](AppState& s) {
              if (idx < s.pv_headers.size()) s.pv_headers.erase(s.pv_headers.begin() + idx);
            });
          }, "ghost"));
          abody.add(row);
        }
        H addh = el("div", "pv-header-add");
        addh.add(comp::text_input(st_.pv_new_header_k, "Header 名",
                                  [this](const std::string& v) { st_.pv_new_header_k = v; }));
        addh.add(comp::text_input(st_.pv_new_header_v, "值",
                                  [this](const std::string& v) { st_.pv_new_header_v = v; }));
        addh.add(comp::button("添加", [this](const std::string&) {
          if (st_.pv_new_header_k.empty()) return;
          update([&](AppState& s) {
            s.pv_headers.push_back(std::make_pair(s.pv_new_header_k, s.pv_new_header_v));
            s.pv_new_header_k.clear();
            s.pv_new_header_v.clear();
          });
        }, "ghost"));
        abody.add(addh);

        // 模型：获取列表（预置/API 两级回退）
        abody.add(comp::section_title("模型"));
        H macts = el("div", "settings-actions");
        std::string sel = st_.provider_selected;
        if (!sel.empty()) {
          macts.add(comp::button("获取模型列表", [this, sel](const std::string&) {
            fetch_provider_models(sel);
          }));
        } else {
          macts.add(comp::hint("保存后可获取模型列表。"));
        }
        abody.add(macts);
        if (!st_.provider_models.empty()) {
          H ml = el("div", "pv-models");
          for (size_t i = 0; i < st_.provider_models.size(); ++i) {
            ml.add(el("span", "pv-model").text(st_.provider_models[i]));
          }
          abody.add(ml);
          abody.add(el("div", "hint").text(
              "级别：" + st_.provider_models_level + "（user > api > preset > 保守缺省）"));
        }
        adv.add(abody);
      }
      detail.add(adv);

      if (!st_.provider_status.empty()) {
        detail.add(el("div", "pv-status").text(st_.provider_status));
      }
      H acts = el("div", "settings-actions");
      std::string sel = st_.provider_selected;
      acts.add(comp::button("保存", [this](const std::string&) { save_provider(); }, "primary"));
      if (!creating) {
        acts.add(comp::button("测试连接", [this, sel](const std::string&) {
          test_provider(sel);
        }));
        acts.add(comp::button("删除", [this, sel](const std::string&) {
          delete_provider(sel);
        }, "danger"));
      }
      acts.add(comp::button("取消", [this](const std::string&) {
        update([&](AppState& s) {
          s.provider_creating = false;
          s.pv_template_picked = false;
          s.pv_advanced = false;
          s.pv_name.clear();
          s.pv_kind = "openai-chat";
          s.pv_base_url.clear();
          s.pv_key.clear();
          s.pv_headers.clear();
          s.provider_status.clear();
          s.provider_models.clear();
        });
      }, "ghost"));
      detail.add(acts);
    };

    if (st_.provider_creating && !st_.pv_template_picked) {
      // step 1: pick a template (it only pre-fills endpoint + headers)
      detail.add(comp::section_title("新建供应商"));
      detail.add(comp::hint("选择一个模板作为起点（只预填 Endpoint 与特殊 Header）。"));
      H tpls = el("div", "pv-templates");
      for (size_t i = 0; i < st_.provider_templates.size(); ++i) {
        const AppState::ProviderTemplate& t = st_.provider_templates[i];
        H c = el("div", "pv-tpl");
        std::string tid = t.id;
        c.on(EventKind::kClick, [this, tid](const std::string&) {
          begin_new_provider(tid);
        });
        c.add(el("div", "pv-tpl-name").text(t.name));
        c.add(el("div", "pv-tpl-desc").text(t.description));
        c.add(el("div", "pv-tpl-url").text(t.base_url));
        tpls.add(c);
      }
      detail.add(tpls);
    } else if (st_.provider_creating) {
      // step 2: the form (template already applied)
      H back = el("div", "pv-back");
      back.add(comp::button("\u2190 重新选择模板", [this](const std::string&) {
        update([&](AppState& s) { s.pv_template_picked = false; });
      }, "ghost"));
      detail.add(back);
      build_form(true);
    } else if (st_.provider_selected.empty()) {
      detail.add(comp::empty_state("从左侧选择一个供应商，或点「新建」。", "", nullptr));
    } else {
      build_form(false);
    }
    two.add(detail);
    body.add(two);
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
      row.add(el("span", "sr-name").text(
          st_.skills[i].title.empty() ? st_.skills[i].name : st_.skills[i].title));
      row.add(el("span", "dim grow").text(st_.skills[i].description));
      if (!st_.skills[i].loadable) row.add(comp::badge("常驻"));
      std::string name = st_.skills[i].name;
      row.add(comp::button("查看", [this, name](const std::string&) {
        call_async("skill.load", Json{{"name", name}},
                   [this, name](const BackendResult& r) {
          toast(r.ok ? ("已加载 skill: " + name) : ("加载失败：" + r.message));
        });
      }, "ghost"));
      body.add(row);
    }

    // ---- 外部工具包（MCP/Skill 文件夹导入）----
    body.add(comp::section_title("外部工具包"));
    body.add(comp::hint("从外部文件夹导入工具包（claude code / opencode / 自定义目录），"
                        "可启用或停用；启用态随设置持久化。"));
    H macts = el("div", "settings-actions");
    macts.add(comp::button("自动探测", [this](const std::string&) { detect_mcp(); }, "primary"));
    body.add(macts);
    if (st_.mcp_tools.empty()) {
      body.add(comp::empty_state("暂无外部工具包。点「自动探测」或在下方新增。", "", nullptr));
    }
    for (size_t i = 0; i < st_.mcp_tools.size(); ++i) {
      const AppState::McpRow& m = st_.mcp_tools[i];
      H row = el("div", "settings-row");
      row.add(el("span", "sr-name").text(m.name));
      row.add(el("span", "dim grow").text(m.dir));
      std::string name = m.name;
      bool en = m.enabled;
      row.add(comp::toggle(en, "", [this, name, en](const std::string&) {
        toggle_mcp(name, !en);
      }));
      row.add(comp::button("移除", [this, name](const std::string&) {
        remove_mcp(name);
      }, "ghost"));
      body.add(row);
    }
    H addm = el("div", "pv-header-add");
    addm.add(comp::text_input(st_.mcp_new_name, "名称",
                              [this](const std::string& v) { st_.mcp_new_name = v; }));
    addm.add(comp::text_input(st_.mcp_new_dir, "目录路径",
                              [this](const std::string& v) { st_.mcp_new_dir = v; }));
    addm.add(comp::button("新增", [this](const std::string&) { add_mcp(); }, "ghost"));
    addm.add(comp::button("浏览目录…", [this](const std::string&) {
      call_async("project.pickFolder", Json::object(), [this](const BackendResult& r) {
        if (!r.ok) return;
        std::string path = jsonutil::get<std::string>(r.data, "path", "");
        if (path.empty()) return;
        update([&](AppState& s) { s.mcp_new_dir = path; });
      });
    }, "ghost"));
    body.add(addm);
  } else if (sec == "memory") {
    // --- 长期记忆 ---
    body.add(comp::section_title("长期记忆"));
    body.add(comp::hint("键值对存于 model.longMemory，随设置持久化，模型可读；"
                        "会话内持久要点见 session.summarize（turn.keyPoints）。"));
    for (size_t i = 0; i < st_.long_memory.size(); ++i) {
      H row = el("div", "settings-row");
      row.add(el("span", "sr-name").text(st_.long_memory[i].first));
      row.add(el("span", "dim grow").text(st_.long_memory[i].second));
      std::string k = st_.long_memory[i].first;
      row.add(comp::button("删除", [this, k](const std::string&) {
        remove_kv("model.longMemory", k);
      }, "ghost"));
      body.add(row);
    }
    if (st_.long_memory.empty()) {
      body.add(comp::empty_state("暂无长期记忆条目。", "", nullptr));
    }
    H madd = el("div", "pv-header-add");
    madd.add(comp::text_input(st_.mem_new_key, "键",
                              [this](const std::string& v) { st_.mem_new_key = v; }));
    madd.add(comp::text_input(st_.mem_new_value, "值",
                              [this](const std::string& v) { st_.mem_new_value = v; }));
    madd.add(comp::button("添加", [this](const std::string&) {
      add_kv("model.longMemory", st_.mem_new_key, st_.mem_new_value);
    }, "ghost"));
    body.add(madd);
  } else if (sec == "appearance") {
    body.add(comp::section_title("外观"));
    body.add(comp::field("主题", comp::select(
        std::vector<std::string>{"跟随系统", "浅色", "深色"}, 0,
        [this](const std::string& v) {
      int idx = std::atoi(v.c_str());
      const char* themes[3] = {"default", "light", "dark"};
      set_setting("ui.theme", themes[idx < 0 || idx > 2 ? 0 : idx]);
    }), "网页端深浅色经 prefers-color-scheme 自动；桌面端接 WhaleUI 主题。"));
    body.add(comp::field(tr("语言"), comp::select(
        std::vector<std::string>{tr("跟随系统"), "中文", "English"},
        st_.ui_lang == "zh" ? 1 : st_.ui_lang == "en" ? 2 : 0,
        [this](const std::string& v) {
      int idx = std::atoi(v.c_str());
      const char* langs[3] = {"auto", "zh", "en"};
      set_setting("ui.language", langs[idx < 0 || idx > 2 ? 0 : idx]);
    }), tr("界面语言；「跟随系统」按操作系统 locale 选择")));
    body.add(comp::field("代码高亮", comp::toggle(
        sv_bool(st_.settings_values, "ui.codeHighlight", true), "",
        [this](const std::string&) {
      bool cur = sv_bool(st_.settings_values, "ui.codeHighlight", true);
      set_setting("ui.codeHighlight", !cur);
    })));
    body.add(comp::section_title("字体"));
    body.add(comp::field("界面字体", comp::text_input(
        sv_str(st_.settings_values, "ui.font.family", ""), "系统默认",
        [this](const std::string& v) { set_setting("ui.font.family", v); }),
        "ui.font.family（空 = 系统默认）"));
    body.add(comp::field("字号", comp::text_input(
        std::to_string(sv_int(st_.settings_values, "ui.font.size", 13)), "",
        [this](const std::string& v) {
      int n = std::atoi(v.c_str());
      set_setting("ui.font.size", n > 0 ? n : 13);
    }), "ui.font.size（px）"));
    body.add(comp::field("等宽字体", comp::text_input(
        sv_str(st_.settings_values, "ui.font.monoFamily", ""), "系统默认",
        [this](const std::string& v) { set_setting("ui.font.monoFamily", v); }),
        "ui.font.monoFamily（代码块）"));
  } else if (sec == "session_defaults") {
    body.add(comp::section_title("会话默认"));
    body.add(comp::hint("新会话创建时的默认值（session.default.*）；已有会话不受影响。"));
    body.add(comp::field("默认模型", comp::text_input(
        sv_str(st_.settings_values, "session.default.model", ""), "继承全局",
        [this](const std::string& v) { set_setting("session.default.model", v); })));
    body.add(comp::field("默认思考档", comp::select(
        std::vector<std::string>{"继承", "关", "低", "中", "高"},
        [&]() {
          std::string t = sv_str(st_.settings_values, "session.default.thinking", "");
          return t == "off" ? 1 : t == "low" ? 2 : t == "medium" ? 3
                 : t == "high" ? 4 : 0;
        }(),
        [this](const std::string& v) {
      int idx = std::atoi(v.c_str());
      const char* vals[5] = {"", "off", "low", "medium", "high"};
      set_setting("session.default.thinking", vals[idx < 0 || idx > 4 ? 0 : idx]);
    })));
    body.add(comp::field("默认文件权限", comp::select(
        std::vector<std::string>{"approve", "permissive", "full"},
        [&]() {
          std::string p = sv_str(st_.settings_values, "session.default.filePerm", "permissive");
          return p == "approve" ? 0 : p == "full" ? 2 : 1;
        }(),
        [this](const std::string& v) {
      int idx = std::atoi(v.c_str());
      const char* vals[3] = {"approve", "permissive", "full"};
      set_setting("session.default.filePerm", vals[idx < 0 || idx > 2 ? 1 : idx]);
    })));
    body.add(comp::field("默认自动模式", comp::toggle(
        sv_bool(st_.settings_values, "session.default.autoMode", false), "",
        [this](const std::string&) {
      bool cur = sv_bool(st_.settings_values, "session.default.autoMode", false);
      set_setting("session.default.autoMode", !cur);
    }), "自动模式下移除 ask 工具、高风险降级"));
    body.add(comp::field("Enter 行为", comp::select(
        std::vector<std::string>{"Enter 发送", "Enter 换行"},
        sv_str(st_.settings_values, "session.default.enterSend", "enter") == "newline" ? 1 : 0,
        [this](const std::string& v) {
      set_setting("session.default.enterSend", std::atoi(v.c_str()) == 1 ? "newline" : "enter");
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
    body.add(comp::section_title("代理"));
    body.add(comp::hint("httplib 不读系统代理环境变量；需要代理时在此显式设置（留空 = 直连）。"));
    body.add(comp::field("代理主机", comp::text_input(
        sv_str(st_.settings_values, "permissions.network.proxyHost", ""), "127.0.0.1",
        [this](const std::string& v) {
      set_setting("permissions.network.proxyHost", v);
    }), "如 127.0.0.1"));
    body.add(comp::field("代理端口", comp::text_input(
        std::to_string(sv_int(st_.settings_values, "permissions.network.proxyPort", 0)), "",
        [this](const std::string& v) {
      int p = std::atoi(v.c_str());
      set_setting("permissions.network.proxyPort", p > 0 ? p : 0);
    }), "如 7890（0 = 不用代理）"));
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
    body.add(comp::section_title("黑名单（拒绝访问）"));
    body.add(comp::hint("优先级：黑名单 > 白名单 > 三档默认。一行一个路径前缀。"));
    body.add(comp::textarea(st_.file_blacklist_text, "每行一个路径…",
                            [this](const std::string& v) {
      st_.file_blacklist_text = v;  // 提交时拆分成数组
    }, nullptr));
    body.add(comp::section_title("白名单（放行）"));
    body.add(comp::textarea(st_.file_whitelist_text, "每行一个路径…",
                            [this](const std::string& v) {
      st_.file_whitelist_text = v;
    }, nullptr));
    H fsave = el("div", "settings-actions");
    fsave.add(comp::button("保存名单", [this](const std::string&) {
      Json blist = split_lines(st_.file_blacklist_text);
      Json wlist = split_lines(st_.file_whitelist_text);
      call_async("settings.set", Json{{"key", "permissions.files.blacklist"}, {"value", blist}});
      call_async("settings.set", Json{{"key", "permissions.files.whitelist"}, {"value", wlist}},
                 [this](const BackendResult& r) { toast(r.ok ? "已保存名单" : "保存失败"); });
    }, "primary"));
    body.add(fsave);
  } else if (sec == "environment") {
    body.add(comp::section_title("环境"));
    body.add(comp::hint("全局环境变量 + 项目 project.json 覆盖（可 ignoreGlobal）；"
                        "终端 spawn 时合并注入。"));
    for (size_t i = 0; i < st_.env_vars.size(); ++i) {
      H row = el("div", "settings-row");
      row.add(el("span", "sr-name").text(st_.env_vars[i].first));
      row.add(el("span", "dim grow").text(st_.env_vars[i].second));
      std::string k = st_.env_vars[i].first;
      row.add(comp::button("删除", [this, k](const std::string&) {
        remove_kv("environment.vars", k);
      }, "ghost"));
      body.add(row);
    }
    if (st_.env_vars.empty()) {
      body.add(comp::empty_state("暂无环境变量。", "", nullptr));
    }
    H eadd = el("div", "pv-header-add");
    eadd.add(comp::text_input(st_.env_new_key, "名称（如 PATH）",
                              [this](const std::string& v) { st_.env_new_key = v; }));
    eadd.add(comp::text_input(st_.env_new_value, "值",
                              [this](const std::string& v) { st_.env_new_value = v; }));
    eadd.add(comp::button("添加", [this](const std::string&) {
      add_kv("environment.vars", st_.env_new_key, st_.env_new_value);
    }, "ghost"));
    body.add(eadd);
    body.add(comp::section_title("CLI 工具路径"));
    body.add(comp::hint("key-value 表；运行时把 key 告诉模型，模型按 key 调用。"));
    for (size_t i = 0; i < st_.cli_tools.size(); ++i) {
      H row = el("div", "settings-row");
      row.add(el("span", "sr-name").text(st_.cli_tools[i].first));
      row.add(el("span", "dim grow").text(st_.cli_tools[i].second));
      std::string k = st_.cli_tools[i].first;
      row.add(comp::button("删除", [this, k](const std::string&) {
        remove_kv("environment.cliTools", k);
      }, "ghost"));
      body.add(row);
    }
    if (st_.cli_tools.empty()) {
      body.add(comp::empty_state("暂无 CLI 工具条目。", "", nullptr));
    }
    H cadd = el("div", "pv-header-add");
    cadd.add(comp::text_input(st_.cli_new_key, "key",
                              [this](const std::string& v) { st_.cli_new_key = v; }));
    cadd.add(comp::text_input(st_.cli_new_value, "路径",
                              [this](const std::string& v) { st_.cli_new_value = v; }));
    cadd.add(comp::button("添加", [this](const std::string&) {
      add_kv("environment.cliTools", st_.cli_new_key, st_.cli_new_value);
    }, "ghost"));
    body.add(cadd);
  } else if (sec == "shortcuts") {
    body.add(comp::section_title("快捷键"));
    body.add(comp::hint("键位绑定（shortcuts 设置键）；桌面端接入后生效。"));
    const char* keys[][2] = {
        {"发送消息", "Enter"},    {"换行", "Shift+Enter"},
        {"关闭标签页", "Ctrl+W"}, {"新建标签页", "Ctrl+T"},
    };
    for (int i = 0; i < 4; ++i) {
      H row = el("div", "settings-row");
      std::string action = keys[i][0];
      row.add(el("span", "sr-name").text(action));
      row.add(comp::text_input(sv_str(st_.settings_values, "shortcuts." + action,
                                      keys[i][1]),
                               "", [this, action](const std::string& v) {
        set_setting("shortcuts." + action, v);
      }));
      body.add(row);
    }
  } else if (sec == "browser") {
    body.add(comp::section_title("浏览器控制"));
    body.add(comp::hint("模型通过 browser-control skill 驱动外部浏览器；"
                        "skill 由模型按需加载（/browser-load）。"));
    body.add(comp::field("启用浏览器控制", comp::toggle(
        sv_bool(st_.settings_values, "browser.enabled", true), "",
        [this](const std::string&) {
      bool cur = sv_bool(st_.settings_values, "browser.enabled", true);
      set_setting("browser.enabled", !cur);
    })));
    body.add(comp::field("驱动路径", comp::text_input(
        sv_str(st_.settings_values, "browser.driver", ""), "自动（Playwright/内置）",
        [this](const std::string& v) { set_setting("browser.driver", v); }),
        "browser.driver（空 = 自动探测）"));
    body.add(comp::field("允许联网", comp::toggle(
        sv_bool(st_.settings_values, "browser.permissions.net", true), "",
        [this](const std::string&) {
      bool cur = sv_bool(st_.settings_values, "browser.permissions.net", true);
      set_setting("browser.permissions.net", !cur);
    })));
    body.add(comp::field("允许操作证书", comp::toggle(
        sv_bool(st_.settings_values, "browser.permissions.certs", false), "",
        [this](const std::string&) {
      bool cur = sv_bool(st_.settings_values, "browser.permissions.certs", false);
      set_setting("browser.permissions.certs", !cur);
    })));
    H bacts = el("div", "settings-actions");
    bacts.add(comp::button("加载 skill 说明", [this](const std::string&) {
      call_async("skill.load", Json{{"name", "browser-control"}},
                 [this](const BackendResult& r) {
        toast(r.ok ? "已加载 browser-control skill" : ("加载失败：" + r.message));
      });
    }, "primary"));
    body.add(bacts);
  } else if (sec == "computer") {
    body.add(comp::section_title("电脑控制"));
    body.add(comp::hint("截图 / 鼠标键盘控制需要平台后端；当前为配置预留。"));
    body.add(comp::field("启用电脑控制", comp::toggle(
        sv_bool(st_.settings_values, "computer.enabled", false), "",
        [this](const std::string&) {
      bool cur = sv_bool(st_.settings_values, "computer.enabled", false);
      set_setting("computer.enabled", !cur);
    })));
    body.add(comp::field("允许截图", comp::toggle(
        sv_bool(st_.settings_values, "computer.screenshot", false), "",
        [this](const std::string&) {
      bool cur = sv_bool(st_.settings_values, "computer.screenshot", false);
      set_setting("computer.screenshot", !cur);
    })));
    body.add(comp::hint("截图/图像解码后端未接线，相关工具调用返回 not-supported。"));
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

// ===========================================================================
// Settings actions: 供应商
// ===========================================================================
void UiApp::select_provider(const std::string& id) {
  if (id.empty()) return;
  call_async("provider.getProvider", Json{{"providerId", id}}, [this, id](const BackendResult& r) {
    if (!r.ok) return;
    update([&](AppState& s) {
      s.provider_creating = false;
      s.provider_selected = id;
      s.pv_name = jsonutil::get<std::string>(r.data, "name", "");
      s.pv_kind = jsonutil::get<std::string>(r.data, "kind", "openai-chat");
      s.pv_base_url = jsonutil::get<std::string>(r.data, "baseUrl", "");
      s.pv_key.clear();  // 留空 = 不修改（后端保留原密钥）
      s.pv_send_session = jsonutil::get<bool>(r.data, "sendSession", false);
      s.pv_headers.clear();
      s.pv_new_header_k.clear();
      s.pv_new_header_v.clear();
      s.provider_models.clear();
      s.provider_models_level.clear();
      s.provider_status.clear();
      if (r.data.contains("customHeaders") && r.data["customHeaders"].is_object()) {
        const Json& h = r.data["customHeaders"];
        for (Json::const_iterator it = h.begin(); it != h.end(); ++it) {
          if (it.value().is_string()) {
            s.pv_headers.push_back(std::make_pair(it.key(), it.value().get<std::string>()));
          }
        }
      }
    });
  });
}

void UiApp::begin_new_provider(const std::string& template_id) {
  // template picked: pre-fill endpoint + headers, then show the form. The
  // template only seeds values; 格式 stays user-editable in the dropdown.
  for (size_t i = 0; i < st_.provider_templates.size(); ++i) {
    const AppState::ProviderTemplate& t = st_.provider_templates[i];
    if (t.id != template_id) continue;
    update([&](AppState& s) {
      s.provider_creating = true;
      s.pv_template_picked = true;
      s.provider_selected.clear();
      s.pv_name = t.name;
      // shape defaults to OpenAI Chat; only the endpoint/headers come from the
      // template (opencode-style ones additionally need the session header,
      // which the backend sends itself - so it is not kept as a custom header)
      s.pv_kind = "openai-chat";
      s.pv_base_url = t.base_url;
      s.pv_key.clear();
      s.pv_headers.clear();
      s.pv_send_session = false;
      for (size_t k = 0; k < t.headers.size(); ++k) {
        const std::string& hk = t.headers[k].first;
        if (hk == "x-opencode-client" || hk == "x-opencode-session") {
          s.pv_send_session = true;
          continue;
        }
        s.pv_headers.push_back(t.headers[k]);
      }
      s.pv_advanced = false;
      s.provider_status.clear();
      s.provider_models.clear();
    });
    return;
  }
}

void UiApp::save_provider() {
  if (st_.pv_base_url.empty()) {
    toast("请填写 Endpoint");
    return;
  }
  Json conn = Json::object();
  if (!st_.provider_selected.empty()) conn["id"] = st_.provider_selected;
  conn["name"] = st_.pv_name;
  conn["kind"] = st_.pv_kind.empty() ? "openai-chat" : st_.pv_kind;
  conn["baseUrl"] = st_.pv_base_url;
  conn["enabled"] = true;
  conn["sendSession"] = st_.pv_send_session;
  if (!st_.pv_key.empty()) conn["apiKey"] = st_.pv_key;  // 空 = 后端保留原值
  Json h = Json::object();
  for (size_t i = 0; i < st_.pv_headers.size(); ++i) {
    h[st_.pv_headers[i].first] = st_.pv_headers[i].second;
  }
  conn["customHeaders"] = h;
  call_async("provider.addProvider", Json{{"conn", conn}}, [this](const BackendResult& r) {
    if (!r.ok) return;
    toast("已保存供应商");
    update([&](AppState& s) {
      s.provider_creating = false;
      s.pv_template_picked = false;
      s.pv_key.clear();
    });
    load_providers();
  });
}

void UiApp::delete_provider(const std::string& id) {
  if (id.empty()) return;
  call_async("provider.removeProvider", Json{{"providerId", id}},
             [this](const BackendResult&) {
    update([&](AppState& s) {
      s.provider_selected.clear();
      s.pv_name.clear();
      s.pv_base_url.clear();
      s.pv_key.clear();
      s.pv_headers.clear();
    });
    load_providers();
  });
}

void UiApp::test_provider(const std::string& id) {
  if (id.empty()) return;
  update([&](AppState& s) { s.provider_status = "测试中…"; });
  call_async("provider.testConnection", Json{{"providerId", id}},
             [this](const BackendResult& r) {
    update([&](AppState& s) {
      if (r.ok) {
        s.provider_status = "连接成功（" +
            std::to_string(jsonutil::get_int(r.data, "latencyMs", 0)) + "ms）";
      } else {
        s.provider_status = "连接失败：" + r.message;
      }
    });
  });
}

void UiApp::fetch_provider_models(const std::string& id) {
  if (id.empty()) return;
  call_async("provider.listModels", Json{{"providerId", id}},
             [this](const BackendResult& r) {
    if (!r.ok) {
      toast("获取失败：" + r.message);
      return;
    }
    std::vector<std::string> models;
    if (r.data.contains("models") && r.data["models"].is_array()) {
      const Json& arr = r.data["models"];
      for (size_t i = 0; i < arr.size(); ++i) {
        std::string mid = jsonutil::get<std::string>(arr[i], "id", "");
        if (!mid.empty()) models.push_back(mid);
      }
    }
    std::string level = jsonutil::get<std::string>(r.data, "level", "");
    std::string message = jsonutil::get<std::string>(r.data, "message", "");
    update([&](AppState& s) {
      s.provider_models = models;
      s.provider_models_level = level;
      s.provider_status = "获取到 " + std::to_string(models.size()) + " 个模型（级别：" +
                          level + "）" + (message.empty() ? "" : ("，" + message));
    });
  });
}

// ===========================================================================
// Settings actions: MCP / 外部工具
// ===========================================================================
void UiApp::detect_mcp() {
  call_async("mcp.detect", Json::object(), [this](const BackendResult& r) {
    if (!r.ok) return;
    size_t n = r.data.contains("scanned") && r.data["scanned"].is_array()
                   ? r.data["scanned"].size() : 0;
    toast(n > 0 ? ("探测到 " + std::to_string(n) + " 个目录") : "未发现常见工具目录");
    load_mcp_tools();
  });
}

void UiApp::add_mcp() {
  if (st_.mcp_new_name.empty() || st_.mcp_new_dir.empty()) {
    toast("请填写名称与目录");
    return;
  }
  std::string name = st_.mcp_new_name;
  std::string dir = st_.mcp_new_dir;
  call_async("mcp.add", Json{{"name", name}, {"dir", dir}, {"enabled", true}},
             [this](const BackendResult& r) {
    if (!r.ok) return;
    update([&](AppState& s) {
      s.mcp_new_name.clear();
      s.mcp_new_dir.clear();
    });
    load_mcp_tools();
  });
}

void UiApp::remove_mcp(const std::string& name) {
  if (name.empty()) return;
  call_async("mcp.remove", Json{{"name", name}}, [this](const BackendResult&) {
    load_mcp_tools();
  });
}

void UiApp::toggle_mcp(const std::string& name, bool enabled) {
  if (name.empty()) return;
  call_async("mcp.setEnabled", Json{{"name", name}, {"enabled", enabled}},
             [this](const BackendResult&) {
    load_mcp_tools();
  });
}

void UiApp::add_kv(const std::string& key_setting, const std::string& k,
                   const std::string& v) {
  if (k.empty()) {
    toast("请填写键");
    return;
  }
  // merge into the existing map so a single entry is added, not the whole table
  Json cur = sv_obj(st_.settings_values, key_setting);
  cur[k] = v;
  update([&](AppState& s) {
    s.mem_new_key.clear();
    s.mem_new_value.clear();
    s.env_new_key.clear();
    s.env_new_value.clear();
    s.cli_new_key.clear();
    s.cli_new_value.clear();
  });
  set_setting(key_setting, cur);
}

void UiApp::remove_kv(const std::string& key_setting, const std::string& k) {
  if (k.empty()) return;
  Json cur = sv_obj(st_.settings_values, key_setting);
  cur.erase(k);
  set_setting(key_setting, cur);
}

// One entry point for everything dropped on the window. The three cases are
// genuinely different, so the adapter tells us which one it is:
//   dir  -> open as a project (browsers only expose the name, so the backend
//           searches the usual roots for it)
//   file -> we only have the name; insert it as an attachment reference
//   url  -> insert the link into the composer
//   text -> insert the text into the composer
//
// Creating a terminal is one click: it runs the shell from settings, and the
// name is edited on the tab afterwards (nothing to type up front).
void UiApp::start_terminal(const std::string& shell, const std::string& label) {
  std::string sid = active_sid();
  if (sid.empty()) {
    toast(tr("请先打开一个会话"));
    return;
  }
  Json args = Json::object();
  args["sid"] = sid;
  if (!shell.empty()) args["shell"] = shell;
  if (!label.empty()) args["label"] = label;
  // ask for a size up front: the shell prints its first prompt for a console of
  // this width, and ConPTY tells the child about it
  if (st_.term_cols > 0 && st_.term_rows > 0) {
    args["cols"] = st_.term_cols;
    args["rows"] = st_.term_rows;
  }
  call_async("terminal.spawn", args, [this](const BackendResult& r) {
    if (!r.ok) {
      toast(tr("启动失败") + "：" + r.message);
      return;
    }
    std::string tid = jsonutil::get<std::string>(r.data, "tid", "");
    update([&](AppState& s) {
      if (!tid.empty()) s.term_tab = tid;  // jump to what we just started
    });
    term_focus_pending_ = true;  // the new terminal takes the keyboard
    load_terminals(active_sid());
    if (!tid.empty()) load_terminal_grid(tid);
  });
}

// A terminal is driven by keys, not by lines of text: Ctrl+C is a control byte,
// arrows depend on the program's cursor-key mode, and paste must be bracketed.
// The term engine owns that encoding (it is terminal knowledge); here we only
// forward the key press.
void UiApp::send_terminal_key(const KeyPress& k) {
  if (st_.term_tab == "all") return;  // the aggregate view has no single stdin
  std::string tid = st_.term_tab;
  std::string bytes;
  if (k.paste) {
    // a paste is not typing: bracketed paste is how a shell knows to skip
    // auto-indent and not run the text as commands
    bool bracketed = false;
    std::map<std::string, Json>::iterator it = st_.term_grids.find(tid);
    if (it != st_.term_grids.end()) {
      bracketed = jsonutil::get<bool>(it->second, "bracketedPaste", false);
    }
    bytes = vt::encode_paste(k.text, bracketed);
  } else {
    // arrows switch between SS3 and CSI depending on the mode the program set
    bool app_keys = false;
    std::map<std::string, Json>::iterator it = st_.term_grids.find(tid);
    if (it != st_.term_grids.end()) {
      app_keys = jsonutil::get<bool>(it->second, "appKeys", false);
    }
    st_.term_app_keys = app_keys;
    bytes = vt::encode_key(k.key, k.ctrl, k.alt, k.shift, app_keys);
  }
  if (bytes.empty()) return;
  // Queue and flush: keystrokes are bytes whose ORDER carries the meaning, and
  // the transport is async, so writing them in parallel lets a fast typist's
  // last character overtake the Enter. One write in flight at a time.
  term_key_queue_ += bytes;
  flush_terminal_keys();
}

// Send the queued keystrokes, one request at a time, preserving order. The next
// chunk goes out only when the previous one has been acknowledged (or failed),
// so the shell always sees the keys in the order they were pressed.
void UiApp::flush_terminal_keys() {
  if (term_key_inflight_) return;
  if (term_key_queue_.empty()) return;
  if (st_.term_tab == "all") {
    term_key_queue_.clear();
    return;
  }
  std::string tid = st_.term_tab;
  std::string chunk;
  chunk.swap(term_key_queue_);
  term_key_inflight_ = true;
  call_async("terminal.write", Json{{"tid", tid}, {"text", chunk}},
             [this](const BackendResult& r) {
    term_key_inflight_ = false;
    if (!r.ok) {
      term_key_queue_.clear();  // the terminal is gone; do not keep retrying
      toast(tr("发送失败") + "：" + r.message);
      return;
    }
    flush_terminal_keys();
  });
}

// The panel draws the engine's screen grid, not the raw byte stream: escape
// sequences are already resolved into cells by the time it gets here.
void UiApp::load_terminal_grid(const std::string& tid) {
  if (tid.empty()) return;
  term_grid_inflight_ = true;
  call_async("terminal.grid", Json{{"tid", tid}},
             [this, tid](const BackendResult& r) {
    term_grid_inflight_ = false;
    if (!r.ok) return;
    // Skip the re-render when nothing changed: this runs ~20x/sec while a
    // terminal is visible, and an idle terminal's screen is byte-identical.
    std::map<std::string, Json>::iterator it = st_.term_grids.find(tid);
    if (it != st_.term_grids.end() && it->second == r.data) return;
    update([&](AppState& s) { s.term_grids[tid] = r.data; });
  });
}
// Report how many character cells fit. The backend resizes the pseudo console,
// which is what lets a full-screen program lay itself out correctly.
void UiApp::resize_terminal(int cols, int rows) {
  if (cols <= 0 || rows <= 0) return;
  if (cols == st_.term_cols && rows == st_.term_rows) return;
  st_.term_cols = cols;
  st_.term_rows = rows;
  if (st_.term_tab == "all") return;
  std::string tid = st_.term_tab;
  call_async("terminal.resize", Json{{"tid", tid}, {"cols", cols}, {"rows", rows}});
  load_terminal_grid(tid);
}

// Renaming a terminal only touches its label (the shell keeps running).
void UiApp::rename_terminal(const std::string& tid, const std::string& label) {
  if (tid.empty()) return;
  update([&](AppState& s) {
    for (size_t i = 0; i < s.terminals.size(); ++i) {
      if (s.terminals[i].tid == tid) s.terminals[i].label = label;
    }
    s.term_renaming.clear();
    s.term_rename_text.clear();
  });
  call_async("terminal.rename", Json{{"tid", tid}, {"label", label}});
}

void UiApp::handle_drop(const std::string& kind, const std::string& value) {
  if (value.empty()) return;
  if (kind == "dir") {
    handle_dropped_folder(value);
    return;
  }
  std::string insert;
  if (kind == "url") {
    insert = value;
    toast(tr("已插入链接"));
  } else {
    // a bare file name: keep it as an attachment reference the model can read
    insert = "@" + value;
    toast(tr("已插入文件名"));
  }
  if (st_.active_tab == "home" || st_.active_tab == "settings") {
    // nothing to insert into yet: say so instead of silently doing nothing
    toast(st_.active_tab == "home" ? tr("请先打开一个会话") : tr("设置页无法插入"));
    return;
  }
  update([&](AppState& s) {
    if (!s.session_input.empty() && s.session_input.back() != ' ') s.session_input += ' ';
    s.session_input += insert;
  });
  // keep the caret in the composer so the user can keep typing
  VNodePtr ta = find_by_key(tree_, "session-input");
  if (ta && ta->dom.valid()) dom_.set_attr(ta->dom, "value", st_.session_input);
}

// An upload finished. The files were stored inside the project by the backend,
// so the model can read them with its ordinary file tools - what it needs is the
// path. We insert the relative paths into the composer (and say so), rather than
// silently attaching something the user cannot see.
void UiApp::handle_upload_reply(const std::string& reply_json) {
  Json j;
  try {
    j = Json::parse(reply_json);
  } catch (...) {
    toast(tr("上传失败"));
    return;
  }
  const Json& files = j.contains("files") ? j["files"] : Json::array();
  if (!files.is_array() || files.empty()) {
    std::string err = jsonutil::get<std::string>(j, "error", "");
    toast(err.empty() ? tr("未选择文件") : (tr("上传失败") + "：" + err));
    return;
  }
  if (st_.active_tab == "home" || st_.active_tab == "settings") {
    toast(tr("请先打开一个会话"));
    return;
  }
  std::string insert;
  for (size_t i = 0; i < files.size(); ++i) {
    std::string rel = jsonutil::get<std::string>(files[i], "rel", "");
    if (rel.empty()) continue;
    if (!insert.empty()) insert += " ";
    insert += "@" + rel;
  }
  if (insert.empty()) return;
  update([&](AppState& s) {
    if (!s.session_input.empty() && s.session_input.back() != ' ') s.session_input += ' ';
    s.session_input += insert;
  });
  VNodePtr ta = find_by_key(tree_, "session-input");
  if (ta && ta->dom.valid()) dom_.set_attr(ta->dom, "value", st_.session_input);
  toast(tr("已添加附件") + "（" + itos(static_cast<int>(files.size())) + "）");
}

// Remove a terminal from the strip without ending it. The process keeps running
// (the model may still be using it) - only 停止 kills it. We just stop showing
// its tab, and it comes back if it prints again (which is what the user wants:
// "remove the tab", not "kill the terminal").
void UiApp::close_terminal_tab(const std::string& tid) {
  if (tid.empty()) return;
  update([&](AppState& s) {
    for (size_t i = 0; i < s.terminals.size(); ++i) {
      if (s.terminals[i].tid == tid) {
        s.terminals.erase(s.terminals.begin() + i);
        break;
      }
    }
    // remember it so the next refresh does not bring the tab back (the process
    // is still running - it is only the tab that is gone)
    bool known = false;
    for (size_t i = 0; i < s.term_hidden.size(); ++i) {
      if (s.term_hidden[i] == tid) { known = true; break; }
    }
    if (!known) s.term_hidden.push_back(tid);
    if (s.term_tab == tid) s.term_tab = "all";
    if (s.term_renaming == tid) {
      s.term_renaming.clear();
      s.term_rename_text.clear();
    }
  });
}

void UiApp::handle_dropped_folder(const std::string& name) {  if (name.empty()) return;
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
      if (kind == "files" && !st_.active_pid.empty()) {
        load_files(st_.active_pid, st_.files_path);
      }
      return;
    }
  }
  open_side_tab(kind);
  if (kind == "subagents") load_subagents(active_sid());
  if (kind == "terminal") load_terminals(active_sid());
  if (kind == "files" && !st_.active_pid.empty()) {
    load_files(st_.active_pid, st_.files_path);
  }
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
