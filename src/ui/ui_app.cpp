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

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <regex>

#include "core/settings_store.hpp"
#include "fs_i18n.hpp"
#include "provider/model_naming.hpp"
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
std::string term_grid_text(const Json& grid) {  if (!grid.is_object()) return std::string();
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

// Side-panel kinds. Terminals are NOT here: they live in the bottom terminal
// drawer now (which shows every terminal, running or not), so a second terminal
// panel in the sidebar was just a worse duplicate.
//
// `multi` says whether the kind can be opened more than once. Files and git
// genuinely can (two directories, two diffs); a usage summary or a subagent
// list cannot - a second copy would show the same thing, so those focus the
// existing tab instead of adding another.
struct SideKind {
  const char* kind;
  const char* title;
  const char* desc;
  bool multi;
};
const SideKind kSideKinds[] = {
    {"usage", "用量", "上下文水位与本会话成本", false},
    {"subagents", "子代理", "子代理运行状态与引导", false},
    {"files", "文件", "项目文件树（可多开）", true},
    // A preview is its OWN tab, not an inline block under the tree: reading a
    // file and browsing the tree are two things you want to see at once, and an
    // inline block pushed the tree out of view.
    {"preview", "预览", "文件内容（可多开）", true},
    {"git", "Git", "Git 状态与变更（可多开）", true},
    {"ops", "操作", "本会话的系统操作记录", true},
    {"calls", "调用", "模型发起的工具调用", true},
};
const size_t kSideKindCount = sizeof(kSideKinds) / sizeof(kSideKinds[0]);

const SideKind* side_kind(const std::string& kind) {
  for (size_t i = 0; i < kSideKindCount; ++i) {
    if (kind == kSideKinds[i].kind) return &kSideKinds[i];
  }
  return nullptr;
}

std::string side_title(const std::string& kind) {
  const SideKind* k = side_kind(kind);
  return k ? k->title : kind;
}

// Template id -> Lobe Icons brand key. The backend's template ids are endpoint
// presets ("opencode-zen", "openai", "gemini", ...); the icon set uses its own
// keys, so map the few that differ and pass the rest through (the icon simply
// does not exist for a name that has no brand, which the caller handles).
std::string provider_brand_for_template(const std::string& id) {
  if (id == "opencode-zen" || id == "opencode-go") return "opencode";
  if (id == "openai") return "openai";
  if (id == "anthropic") return "anthropic";
  if (id == "gemini") return "gemini";
  if (id == "deepseek") return "deepseek";
  if (id == "moonshot") return "moonshot";
  if (id == "zhipu") return "zhipu";
  if (id == "dashscope") return "alibaba";
  if (id == "siliconflow") return "siliconcloud";
  if (id == "openrouter") return "openrouter";
  if (id == "ollama") return "ollama";
  if (id == "lmstudio") return "lmstudio";
  if (id == "vllm") return "vllm";
  if (id == "azure") return "azure";
  return std::string();  // "custom" and anything unknown: letter fallback
}

// last path segment (both separators; the backend may be on another OS)
std::string folder_basename(const std::string& path) {
  size_t end = path.size();
  while (end > 0 && (path[end - 1] == '\\' || path[end - 1] == '/')) --end;
  size_t cut = end;
  while (cut > 0 && path[cut - 1] != '\\' && path[cut - 1] != '/') --cut;
  return path.substr(cut, end - cut);
}

// Read a string setting from the cached section values (default when absent).
// The dump wraps values as {value, source}; a bare value is accepted too.
std::string sv_str_inner(const Json& vals, const std::string& key,
                         const std::string& def) {
  if (!vals.is_object() || !vals.contains(key)) return def;
  const Json& v = vals[key];
  if (v.is_string()) return v.get<std::string>();
  if (v.is_object() && v.contains("value") && v["value"].is_string()) {
    return v["value"].get<std::string>();
  }
  return def;
}
std::string sv_str(const Json& vals, const std::string& key,
                   const std::string& def) {
  return sv_str_inner(vals, key, def);
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
  if (!v) return;
  v->anim = intent;
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

// The strip shows a search box only once terminals are numerous enough to get
// lost in; below that it is just noise.
const size_t kTermSearchThreshold = 10;

std::string lower_ascii(const std::string& s) {
  std::string o = s;
  for (size_t i = 0; i < o.size(); ++i) {
    o[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(o[i])));
  }
  return o;
}

// Does `pat` contain regex syntax? If it does the user means a pattern; if not,
// a plain/fuzzy substring match is what they want ("bld" should still find
// "build-log").
bool looks_like_regex(const std::string& pat) {
  return pat.find_first_of(".^$*+?()[]{}|\\") != std::string::npos;
}

// Fuzzy subsequence match: every character of the pattern appears in order.
bool fuzzy_match(const std::string& hay_lower, const std::string& pat_lower) {
  if (pat_lower.empty()) return true;
  size_t p = 0;
  for (size_t i = 0; i < hay_lower.size() && p < pat_lower.size(); ++i) {
    if (hay_lower[i] == pat_lower[p]) ++p;
  }
  return p == pat_lower.size();
}

// Match `pat` against one haystack.
//   * looks like a regex -> case-insensitive regex search
//   * a regex that does not compile (the user is mid-typing "foo[") -> the
//     metacharacters are dropped and the rest is fuzzy-matched. Blanking the
//     list on a half-typed pattern is the most annoying possible behavior, and
//     "foo[" clearly means "foo".
//   * anything else -> fuzzy subsequence, so "bld" finds "build-log"
bool term_match_one(const std::string& hay, const std::string& pat) {
  if (pat.empty()) return true;
  std::string hp = lower_ascii(hay);
  std::string pp = lower_ascii(pat);
  if (looks_like_regex(pat)) {
    try {
      std::regex re(pat, std::regex::icase);
      return std::regex_search(hay, re);
    } catch (...) {
      std::string stripped;
      for (size_t i = 0; i < pp.size(); ++i) {
        if (std::string(".^$*+?()[]{}|\\").find(pp[i]) == std::string::npos) {
          stripped.push_back(pp[i]);
        }
      }
      if (stripped.empty()) return true;  // only metacharacters so far
      return fuzzy_match(hp, stripped);
    }
  }
  return fuzzy_match(hp, pp);
}

// a session that has never been opened starts from these defaults
SessionView default_session_view() {
  SessionView v;
  v.side_tabs.push_back(SideTab{"usage", "用量", "usage"});
  v.side_tabs.push_back(SideTab{"subagents", "子代理", "subagents"});
  // No terminal panel: terminals live in the bottom drawer now, so a sidebar
  // copy would be a worse duplicate of the same thing.
  v.side_seq = 2;
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

// 思考档位标签：协议值 → 界面中文。这些是**预置翻译**，未知档位原样显示，
// 所以模型可以有自己的档位名而不被这里限制住。
std::string thinking_label(const std::string& v) {
  if (v == "off" || v == "disable" || v == "disabled") return "关";
  if (v == "on" || v == "enable" || v == "enabled") return "开";
  if (v == "minimal") return "最低";
  if (v == "low") return "低";
  if (v == "medium") return "中";
  if (v == "high") return "高";
  if (v == "xhigh" || v == "extra-high" || v == "extra_high") return "超高";
  if (v == "max") return "最大";
  if (v == "auto") return "自动";
  return v;
}

// 相对时间（"3 分钟前"）。列表里给一个大致时间就够了，绝对时间戳反而要花力气
// 去读；只算到"天"，再久就显示日期。
//
// 注意基准：会话时间来自后端 full.json 的 mtime（真实墙上时间），而这里的
// now_ms() 是**单调时钟**（steady_clock / performance.now()），两者原点不同。
// 所以这里用系统墙上时间做差，不能拿 now_ms() 去减 mtime，否则会算出天量偏差。
std::string rel_time(int64_t ms) {
  if (ms <= 0) return std::string();
  int64_t now_wall = static_cast<int64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
  int64_t d = now_wall - ms;
  if (d < 0) d = 0;
  int64_t sec = d / 1000;
  if (sec < 60) return tr("刚刚");
  int64_t min = sec / 60;
  if (min < 60) return itos(static_cast<int>(min)) + tr(" 分钟前");
  int64_t hr = min / 60;
  if (hr < 24) return itos(static_cast<int>(hr)) + tr(" 小时前");
  int64_t day = hr / 24;
  if (day < 30) return itos(static_cast<int>(day)) + tr(" 天前");
  // older than a month: an absolute date is more useful than "87 天前"
  time_t t = static_cast<time_t>(ms / 1000);
  struct tm* g = localtime(&t);
  if (!g) return std::string();
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", g->tm_year + 1900,
                g->tm_mon + 1, g->tm_mday);
  return std::string(buf);
}

}  // namespace

// ===========================================================================
// AppState
// ===========================================================================
AppState AppState::make() {
  AppState s;
  s.tab_ids.push_back("home");
  // 设置不是标签页（见 modal_page 的说明）：它点开就显示、关掉就销毁。
  // side panel starts with the panels that have real data today; others are
  // added on demand via the "+" button. Terminals are not here - they live in
  // the bottom drawer.
  s.side_tabs.push_back(SideTab{"usage", "用量", "usage"});
  s.side_tabs.push_back(SideTab{"subagents", "子代理", "subagents"});
  s.side_seq = 2;
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
  // Push the appearance defaults immediately: the CSS keeps a media query for
  // "never told", but once the app is up the mode must be explicit (the user can
  // pin light/dark regardless of the OS).
  apply_appearance();
  if (backend_) {
    unsubscribe_events_ = backend_->subscribe(
        [this](const std::string& e, const Json& p) { on_backend_event(e, p); });
    load_projects();
    // Appearance and the tab layout live in settings, so they must be loaded at
    // STARTUP: waiting until the settings page is opened meant a saved
    // ui.tabs=left (or palette/font) was ignored on every fresh launch.
    load_settings_section("");
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
  // drop subtrees whose exit animation is over (they are outside the tree, so
  // only this timer can clean them up)
  sweep_exits(dom_, now_ms());
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
  if (st_.active_tab == "home") return "";
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
      // A closed terminal is still LISTED (it keeps running; closing only
      // removed its tab), so it is not filtered out here - the tab strip skips
      // it and the overview shows it as an inactive row.
      terms.push_back(it);
    }
    st_.terminals.swap(terms);
    // The backend sorts by tid; the strip shows them in the user's own order
    // (see term_order). tids not seen before are appended, and tids that are
    // gone are dropped, so the order stays in sync without re-sorting what the
    // user arranged.
    {
      std::vector<std::string> order;
      for (size_t i = 0; i < st_.term_order.size(); ++i) {
        for (size_t k = 0; k < st_.terminals.size(); ++k) {
          if (st_.terminals[k].tid == st_.term_order[i]) {
            order.push_back(st_.term_order[i]);
            break;
          }
        }
      }
      for (size_t k = 0; k < st_.terminals.size(); ++k) {
        bool known = false;
        for (size_t i = 0; i < order.size(); ++i) {
          if (order[i] == st_.terminals[k].tid) { known = true; break; }
        }
        if (!known) order.push_back(st_.terminals[k].tid);
      }
      st_.term_order.swap(order);
    }
    if (!st_.terminals.empty() && st_.active_term.empty()) {
      st_.active_term = st_.terminals[0].tid;
    }
    render();
  });
}

// Terminals in display order: the strip and the overview both use this so they
// can never disagree about what exists or in what order.
std::vector<const TermItem*> UiApp::ordered_terminals() const {
  std::vector<const TermItem*> out;
  for (size_t i = 0; i < st_.term_order.size(); ++i) {
    for (size_t k = 0; k < st_.terminals.size(); ++k) {
      if (st_.terminals[k].tid == st_.term_order[i]) {
        out.push_back(&st_.terminals[k]);
        break;
      }
    }
  }
  // a terminal that arrived without going through load_terminals (an event for a
  // terminal the model just started) still has to show up
  for (size_t k = 0; k < st_.terminals.size(); ++k) {
    bool known = false;
    for (size_t i = 0; i < out.size(); ++i) {
      if (out[i]->tid == st_.terminals[k].tid) { known = true; break; }
    }
    if (!known) out.push_back(&st_.terminals[k]);
  }
  return out;
}

void UiApp::save_session_view(const std::string& sid) {
  if (sid.empty()) return;
  SessionView& v = views_.get_or_create(sid);
  v.messages = st_.messages;
  v.session_loaded = st_.session_loaded;
  v.session_input = st_.session_input;
  v.model = st_.model;
  v.model_index = st_.model_index;
  v.permission = st_.permission;
  v.perm_index = st_.perm_index;
  v.auto_mode = st_.auto_mode;
  v.thinking = st_.thinking;
  v.attachments = st_.attachments;
  v.suspended = st_.suspended;
  v.context_pct = st_.context_pct;
  v.session_cost = st_.session_cost;
  v.side_tabs = st_.side_tabs;
  v.active_side = st_.active_side;
  v.side_seq = st_.side_seq;
  v.side_menu_open = st_.side_menu_open;
  v.side_width = st_.side_width;
  v.active_pid = st_.active_pid;
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
  v.term_order = st_.term_order;
  v.term_hidden = st_.term_hidden;
  v.subagents = st_.subagents;
  v.plan_open = st_.plan_open;
  v.plan_steps = st_.plan_steps;
}

void UiApp::load_session_view(const std::string& sid) {
  SessionView v = default_session_view();
  views_.restore(sid, &v);  // keeps the defaults when the key is new
  st_.messages = v.messages;
  st_.session_loaded = v.session_loaded;
  st_.session_input = v.session_input;
  st_.model = v.model;
  // 会话自己还没选过模型时，用设置里的会话默认模型（如果配了）。仍然为空就
  // 保持为空，按钮显示"选择模型"——不再凭空塞一个 gpt-4o-mini。
  if (st_.model.empty() && st_.settings_values.is_object() &&
      st_.settings_values.contains("session.default.model")) {
    const Json& dv = st_.settings_values["session.default.model"];
    if (dv.is_string()) st_.model = dv.get<std::string>();
    else if (dv.is_object() && dv.contains("value") && dv["value"].is_string()) {
      st_.model = dv["value"].get<std::string>();
    }
  }
  st_.model_index = v.model_index;
  st_.permission = v.permission;
  st_.perm_index = v.perm_index;
  st_.auto_mode = v.auto_mode;
  st_.thinking = v.thinking;
  st_.attachments = v.attachments;
  st_.suspended = v.suspended;
  st_.context_pct = v.context_pct;
  st_.session_cost = v.session_cost;
  st_.side_tabs = v.side_tabs;
  st_.active_side = v.active_side;
  st_.side_seq = v.side_seq;
  st_.side_menu_open = false;  // transient popovers never survive a switch
  st_.side_width = v.side_width;
  st_.active_pid = v.active_pid;
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
  st_.term_order = v.term_order;
  st_.term_hidden = v.term_hidden;
  st_.subagents = v.subagents;
  st_.plan_open = v.plan_open;
  st_.plan_steps = v.plan_steps;
}

void UiApp::switch_tab(const std::string& id, int dir) {
  // The flat session fields belong to the tab we are leaving. Prefer the
  // tracked key, but fall back to active_tab: if something set active_tab
  // directly, the fields still have to be stashed under the right session or
  // its history would be lost on the way back.
  std::string leaving = last_session_key_;
  if (leaving.empty() && st_.active_tab != "home") {
    leaving = st_.active_tab;
  }
  if (id == st_.active_tab && last_session_key_ == leaving && leaving == id) return;
  if (!leaving.empty()) save_session_view(leaving);
  // Any open transient popup belongs to the tab being left. Leaving it up is how
  // the model picker "stayed glued" across a tab switch.
  st_.model_picker_open = false;
  st_.skill_menu_open = false;
  st_.side_menu_open = false;
  // Switching tabs also dismisses a modal page: it was opened ON TOP of a tab,
  // so navigating away means it is done with.
  st_.modal_page.clear();
  // Record where we came from so closing this tab can return here. The SAME
  // stack tells us the direction of this navigation: if the target is exactly
  // the page we just came FROM (the stack's top), this is an undo of the last
  // move, so the page slides in from the left. Any other move - including going
  // back to a tab visited long ago - is forward and comes from the right.
  // Closing a tab pops the stack before calling us, so it passes dir=-1.
  if (!st_.active_tab.empty() && st_.active_tab != id) {
    if (dir == 0) {
      dir = (!st_.tab_history.empty() && st_.tab_history.back() == id) ? -1 : 1;
    }
    st_.tab_history.push_back(st_.active_tab);
    if (st_.tab_history.size() > 64) st_.tab_history.erase(st_.tab_history.begin());
    page_dir_ = dir < 0 ? "-1" : "1";
  } else {
    page_dir_.clear();
  }
  st_.active_tab = id;
  // Only a session tab carries a per-session view; the home page has none.
  std::string sid = (id == "home") ? "" : id;
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
      st.title = title.empty() ? (tr("会话") + sid) : title;
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
      // the page's animation replay token: the page animates again now that
      // there is actually something to look at (a slow load used to play the
      // animation over an empty page, so it was over before content arrived)
      st_.session_loaded = true;
      render();
    }
    load_terminals(sid);
    // file panels load their own directory when they become active (see
    // load_side_tab), so nothing to prefetch here
  });
}

void UiApp::load_sessions(size_t project_index) {
  if (project_index >= st_.projects.size()) return;
  std::string pid = st_.projects[project_index].pid;
  if (pid.empty()) return;
  call_async("session.list", Json{{"pid", pid}}, [this, project_index](const BackendResult& r) {
    if (!r.ok || !r.data.is_array()) return;
    std::vector<ProjectCard::SessionRow> rows;
    for (size_t i = 0; i < r.data.size(); ++i) {
      const Json& s = r.data[i];
      ProjectCard::SessionRow row;
      row.sid = jsonutil::get<std::string>(s, "sid", "");
      row.title = jsonutil::get<std::string>(s, "title", "");
      if (row.title.empty()) row.title = tr("会话 ") + row.sid;  // untitled: readable
      row.updated_ms = jsonutil::get_int(s, "updatedAt", 0);
      row.running = jsonutil::get<bool>(s, "running", false);
      if (!row.sid.empty()) rows.push_back(row);
    }
    // Most recently touched first: the list is a history, and the thing you want
    // is almost always the one you were just in. A session with no mtime sorts
    // last rather than jumping to the top as if it were newest.
    std::stable_sort(rows.begin(), rows.end(),
                     [](const ProjectCard::SessionRow& a,
                        const ProjectCard::SessionRow& b) {
      return a.updated_ms > b.updated_ms;
    });
    update([&](AppState& st) {
      if (project_index < st.projects.size()) {
        // Keep an in-progress rename alive: a refresh (which happens after a
        // rename of ANOTHER row, or on any project reload) must not yank the
        // input out from under the user.
        std::string renaming_sid;
        std::string rename_text;
        for (size_t k = 0; k < st.projects[project_index].sessions.size(); ++k) {
          const ProjectCard::SessionRow& old = st.projects[project_index].sessions[k];
          if (old.renaming) {
            renaming_sid = old.sid;
            rename_text = old.rename_text;
          }
        }
        for (size_t k = 0; k < rows.size() && !renaming_sid.empty(); ++k) {
          if (rows[k].sid == renaming_sid) {
            rows[k].renaming = true;
            rows[k].rename_text = rename_text;
          }
        }
        st.projects[project_index].sessions = rows;
        st.projects[project_index].session_count = static_cast<int>(rows.size());
        st.projects[project_index].sessions_loaded = true;
      }
    });
  });
}

void UiApp::load_usage() {
  // The filter is real: a provider scope and a time range both narrow the query,
  // so the numbers on the page are the numbers that were asked for.
  Json filter = Json::object();
  if (!st_.usage_scope.empty()) filter["provider"] = st_.usage_scope;
  static const int kDays[4] = {1, 7, 30, 0};
  int days = kDays[st_.usage_range < 0 || st_.usage_range > 3 ? 1 : st_.usage_range];
  if (days > 0) {
    int64_t now = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    filter["from"] = now - static_cast<int64_t>(days) * 86400 * 1000;
  }
  call_async("usage.query", Json{{"filter", filter}}, [this](const BackendResult& r) {
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
    bool picker_wants_default = false;
    std::string select_default;
    update([&](AppState& s) {
      s.providers = rows;
      // set even on failure: the load is render-driven, a retry loop would hang
      s.providers_loaded = true;
      // The model picker can be opened before providers have ever been loaded
      // (a session page, fresh start): it then had no provider to select and
      // showed an empty right pane. Now that the list exists, pick the first
      // one and load its models.
      if (s.model_picker_open && s.model_picker_provider.empty() && !rows.empty()) {
        s.model_picker_provider = rows[0].id;
        s.model_picker_loaded = false;
        picker_wants_default = true;
      }
      // Opening 供应商 with nothing selected showed an empty right pane and made
      // you click the only item in the list. Select the first provider instead.
      if (!s.provider_creating && s.provider_selected.empty() && !rows.empty()) {
        select_default = rows[0].id;
      }
    });
    if (picker_wants_default) load_picker_models(rows[0].id);
    if (!select_default.empty()) select_provider(select_default);
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
  // In-flight guard: this is called from a BUILD (the explore page needs the
  // list), so without it every render fired another request until the first
  // reply landed - a request storm that starved the event loop and made every
  // click look dead.
  if (mcp_inflight_) return;
  mcp_inflight_ = true;
  call_async("mcp.list", Json::object(), [this](const BackendResult& r) {
    mcp_inflight_ = false;
    std::vector<AppState::McpRow> rows;
    const Json& arr = r.ok && r.data.contains("tools") ? r.data["tools"] : Json::array();
    if (arr.is_array()) {
      for (size_t i = 0; i < arr.size(); ++i) {
        const Json& t = arr[i];
        AppState::McpRow row;
        row.name = jsonutil::get<std::string>(t, "name", "");
        row.dir = jsonutil::get<std::string>(t, "dir", "");
        row.description = jsonutil::get<std::string>(t, "description", "");
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
    std::vector<AppState::ShellRow> rows;
    const Json& arr = r.ok ? (r.data.contains("shells") ? r.data["shells"] : r.data)
                           : Json::array();
    if (arr.is_array()) {
      for (size_t i = 0; i < arr.size(); ++i) {
        AppState::ShellRow row;
        row.name = jsonutil::get<std::string>(arr[i], "name", "");
        row.path = jsonutil::get<std::string>(arr[i], "path", "");
        if (!row.name.empty()) rows.push_back(row);
      }
    }
    // set even when empty: the render-driven guard stops retrying either way
    update([&](AppState& s) { s.shell_rows = rows; });
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

// Load a file panel's directory. The panel is identified by `tab_id` and keeps
// its own path/entries/preview, so two file panels can sit in two different
// directories at once.
void UiApp::load_files(const std::string& tab_id, const std::string& pid,
                       const std::string& rel) {
  if (pid.empty()) return;
  call_async("project.files", Json{{"pid", pid}, {"path", rel}},
             [this, tab_id, rel](const BackendResult& r) {
    std::vector<SideTab::FileEntry> rows;
    const Json& arr = r.ok && r.data.contains("entries") ? r.data["entries"] : Json::array();
    if (arr.is_array()) {
      for (size_t i = 0; i < arr.size(); ++i) {
        SideTab::FileEntry e;
        e.name = jsonutil::get<std::string>(arr[i], "name", "");
        e.is_dir = jsonutil::get<bool>(arr[i], "isDir", false);
        e.size = jsonutil::get_int(arr[i], "size", 0);
        if (!e.name.empty()) rows.push_back(e);
      }
    }
    update([&](AppState& s) {
      for (size_t i = 0; i < s.side_tabs.size(); ++i) {
        if (s.side_tabs[i].id != tab_id) continue;
        s.side_tabs[i].entries = rows;
        s.side_tabs[i].path = rel;
        s.side_tabs[i].loaded = true;
        s.side_tabs[i].preview_path.clear();
        s.side_tabs[i].preview.clear();
        return;
      }
    });
  });
}

// Preview one file inside a specific file panel.
void UiApp::preview_file(const std::string& tab_id, const std::string& rel) {
  if (st_.active_pid.empty() || rel.empty()) return;
  std::string pid = st_.active_pid;
  call_async("project.readFile", Json{{"pid", pid}, {"path", rel}},
             [this, tab_id, rel](const BackendResult& r) {
    update([&](AppState& s) {
      for (size_t i = 0; i < s.side_tabs.size(); ++i) {
        if (s.side_tabs[i].id != tab_id) continue;
        s.side_tabs[i].path = rel;
        s.side_tabs[i].preview_path = rel;
        s.side_tabs[i].loaded = true;
        s.side_tabs[i].preview =
            r.ok ? jsonutil::get<std::string>(r.data, "content", "")
                 : (tr("（无法预览：") + r.message + "）");
        return;
      }
    });
  });
}

// Open a file's contents in its OWN side tab. Reading a file and browsing the
// tree are two things you want visible at once, which an inline block under the
// tree cannot do.
void UiApp::open_file_preview(const std::string& rel) {
  if (rel.empty()) return;
  // Already open for this file? Focus it instead of stacking duplicates.
  for (size_t i = 0; i < st_.side_tabs.size(); ++i) {
    if (st_.side_tabs[i].kind == "preview" && st_.side_tabs[i].path == rel) {
      std::string id = st_.side_tabs[i].id;
      update([&](AppState& s) {
        if (!s.active_side.empty() && s.active_side != id) {
          s.side_history.push_back(s.active_side);
        }
        s.active_side = id;
      });
      return;
    }
  }
  std::string id;
  update([&](AppState& s) {
    s.side_seq++;
    SideTab t;
    t.id = "sb" + itos(s.side_seq);
    t.kind = "preview";
    t.path = rel;
    // The tab is named after the FILE, not the kind: with several previews open
    // a row of identical "预览" tabs tells you nothing.
    t.title = folder_basename(rel);
    s.side_tabs.push_back(t);
    // Programmatic focus still has to record where we came from, or closing the
    // preview falls back to the first panel instead of the file tree.
    if (!s.active_side.empty() && s.active_side != t.id) {
      s.side_history.push_back(s.active_side);
      if (s.side_history.size() > 64) s.side_history.erase(s.side_history.begin());
    }
    s.active_side = t.id;
    id = t.id;
  });
  preview_file(id, rel);
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
      // register it in the display order too, or the strip would show it at a
      // position the order list does not know about
      std::vector<std::string>& order = is_active ? st_.term_order : vp->term_order;
      bool in_order = false;
      for (size_t i = 0; i < order.size(); ++i) {
        if (order[i] == tid) { in_order = true; break; }
      }
      if (!in_order) order.push_back(tid);
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
    // Exit direction belongs to the MOVE, not to the page: a page's exit_anim
    // was baked in when it was CREATED, so a page opened by a forward move
    // would always slide out to the left even when you go back. Stamp the
    // outgoing page here, where both the old and the new page are known.
    {
      VNodePtr old_slot = find_by_key(tree_, "page-slot");
      VNodePtr new_slot = find_by_key(neu, "page-slot");
      if (old_slot && new_slot && !old_slot->children.empty() &&
          !new_slot->children.empty()) {
        VNodePtr op = old_slot->children[0];
        VNodePtr np = new_slot->children[0];
        if (op && np && op->key != np->key) {
          op->exit_anim = (page_dir_ == "-1") ? anim::kExitRight : anim::kExitLeft;
          // Stagger: the outgoing page gets a head start, the incoming one waits
          // until it is ~80% gone. Starting both at t=0 made the two pages cross
          // and the whole switch read as noise.
          add_class(np, "page-stagger");
        }
      }
    }
    patch(dom_, tree_, neu, root_);
    tree_ = neu;
    last_rendered_tab_ = st_.active_tab;
    // drop handlers from the previous generation (ids are monotonic, so no
    // collision with the still-bound listeners of the current tree)
    std::vector<int> live;
    collect_handler_ids(tree_, &live);
    bus().retain(live);
    // (re)wire the side-panel drag divider; bind_splitter is idempotent, so
    // this works whether the node survived a patch or was just created.
    //
    // Only while the panel is SHOWN: a collapsed divider is just a thin handle
    // strip, and dragging it had no visible effect but still reported a width -
    // which then came back as the panel's width on expand and made it look
    // broken. With no binding there is nothing to drag and nothing to persist.
    VNodePtr bar = find_by_key(tree_, "side-splitter");
    VNodePtr side = find_by_key(tree_, "side-panel");
    if (bar && side && !st_.side_collapsed && bar->dom.valid() && side->dom.valid()) {
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
    } else if (bar && bar->dom.valid()) {
      // clear any binding left over from when the panel was open
      dom_.bind_splitter(bar->dom, Node{0}, nullptr);
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
    // top tab strip: drag a tab onto another to reorder it
    VNodePtr tabstrip = find_by_key(tree_, "top-tabs");
    if (tabstrip && tabstrip->dom.valid()) {
      dom_.bind_tab_drag(tabstrip->dom, [this](const std::string& payload) {
        size_t tab = payload.find('\t');
        if (tab == std::string::npos) return;
        move_tab(payload.substr(0, tab), payload.substr(tab + 1));
      });
    }
    // project nav rail divider (left mode): its own binding, so dragging it
    // never moves the side panel
    VNodePtr navbar = find_by_key(tree_, "nav-splitter");
    VNodePtr navrail = find_by_key(tree_, "nav-rail");
    if (navbar && navrail && navbar->dom.valid() && navrail->dom.valid()) {
      dom_.bind_nav_splitter(navbar->dom, navrail->dom, [this](int w) {
        if (w > 0) st_.nav_width = w;
      });
    } else if (navbar && navbar->dom.valid()) {
      dom_.bind_nav_splitter(navbar->dom, Node{0}, nullptr);
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
      // Keep the view pinned to the bottom as output arrives - but only when the
      // user is already at the bottom. Scrolling up to read history must not be
      // yanked away by the next line of output.
      dom_.bind_scroll_pin(term->dom, [this](bool at_bottom) {
        term_at_bottom_ = at_bottom;
      });
      if (term_at_bottom_) dom_.scroll_to_bottom(term->dom);
    }
    // rename field: focus it so the user can type immediately
    VNodePtr ren = find_by_key(tree_, "term-rename-input");
    if (ren && ren->dom.valid()) dom_.focus(ren->dom);
    // inline session rename: same treatment, but only on the render that
    // started it (see session_rename_focus_)
    if (session_rename_focus_) {
      VNodePtr sren = find_by_key(tree_, "session-rename-input");
      if (sren && sren->dom.valid()) {
        dom_.focus(sren->dom);
        session_rename_focus_ = false;
      }
    }
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
    // drop exit-animated nodes whose animation has finished (they are no longer
    // part of the tree, so nothing else would ever remove them)
    sweep_exits(dom_, now_ms());
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
    // the "+" tab is a page of its own (it lists projects to pick from)
    {"new", anim::kView},
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

  // 设置/新建是"整窗"级别的页面：盖在标签栏之上，而不是缩在标签栏下面。
  // 它们作为最后一层（在 overlays 之上）叠进来，底下的 shell 保持原样：
  // 于是打开/关闭都有真正的进出动画，而不是整个 window 被换掉。
  H shell = el("div", "shell");
  shell.key("shell");
  build_shell_into(shell);
  win.add(shell);
  win.add(build_overlays());
  // 设置是整窗页面（盖住标签栏）；"新建"不是——它只是页面区里的一个页面，
  // 标签栏照常可见可点（用户明确要求新建页不要覆盖标签栏）。
  if (st_.modal_page == "settings") {
    H overlay = el("div", "modal-layer");
    overlay.key("modal-layer");
    // The LAYER is what disappears when the modal closes, so it is the layer
    // that must fade out: fading only the inner page would leave the opaque
    // backdrop covering the shell until the node is dropped.
    overlay.exit(anim::kExitFade);
    VNodePtr page = build_settings();
    page->key = "page:settings";
    add_anim(page, anim::kView);
    page->exit_anim = anim::kExitFade;
    page->anim_key = "modal:settings";
    overlay.add(page);
    win.add(overlay);
  }
  return win.get();
}

// The normal window content (tab strip / nav rail + page slot). Split out of
// build_shell so a modal page can sit ON TOP of it instead of replacing it.
void UiApp::build_shell_into(H& win) {
  // ---- top tab strip ----
  // 标签位置=左侧时，标签条不再是一排可关闭的工作标签，而是"项目导航栏"：
  // 按项目列出全部会话（见 build_nav_rail）。两者是两种不同的导航模式，不能
  // 混用——所以这里按设置二选一，而不是把同一排标签竖过来。
  const bool left_tabs = sv_str(st_.settings_values, "ui.tabs", "top") == "left";
  if (left_tabs) {
    H row = el("div", "nav-row");
    row.key("nav-row");
    if (!st_.nav_collapsed) {
      row.add(build_nav_rail());
      H navsplit = el("div", "nav-splitter");
      navsplit.key("nav-splitter");
      row.add(navsplit);
    } else {
      // 最小化：只留一条窄条，一个展开按钮。项目导航仍然"在"，只是收起。
      H mini = el("div", "nav-mini");
      mini.key("nav-mini");
      mini.add(comp::icon_button("chevron-right", tr("展开项目栏"), [this](const std::string&) {
        update([&](AppState& s) { s.nav_collapsed = false; });
      }));
      row.add(mini);
    }
    H col = el("div", "nav-main");
    col.key("nav-main");
    col.add(build_page_slot());
    row.add(col);
    win.add(row);
    return;
  }

  std::vector<TabSpec> tabs;
  for (size_t i = 0; i < st_.tab_ids.size(); ++i) {
    const std::string& id = st_.tab_ids[i];
    TabSpec t;
    t.key = id;
    if (id == "home") {
      t.title = "主页";
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
      [this](const std::string&) { open_new_tab(); });
  strip.key("top-tabs");
  // 侧栏开关放在标签条最右端：这是"整窗"级别的控制，属于标题栏区域，而不是
  // 夹在会话内容中间的那条细缝。原来只有分隔条上那个小箭头，太难点。
  if (st_.active_tab != "home") {
    H side_toggle = comp::icon_button(
        st_.side_collapsed ? "chevron-left" : "chevron-right",
        st_.side_collapsed ? tr("展开侧栏") : tr("收起侧栏"),
        [this](const std::string&) {
      update([&](AppState& s) { s.side_collapsed = !s.side_collapsed; });
    });
    side_toggle.cls("tabbar-side-toggle");
    side_toggle.key("tabbar-side-toggle");
    strip.add(side_toggle);
  }
  win.add(strip);

  // ---- page ----
  // keyed by tab so switching pages replaces the subtree instead of patching
  // unrelated content into it
  //
  // A modal page (设置 / 新建) covers whatever tab is underneath instead of
  // becoming a tab of its own: it is opened, used, and closed, and having it in
  // the strip (and in the visit stack) made closing tabs fall back into settings.
  win.add(build_page_slot());
}

// The page area is its own slot: a leaving page is positioned absolutely INSIDE
// it for the two-view transition, so it must not cover the tab strip.
VNodePtr UiApp::build_page_slot() {
  std::string page_id;
  VNodePtr page;
  if (st_.modal_page == "new") {
    // "新建" is a PAGE inside the tab area, not a full-window modal: the tab
    // strip stays visible (you can still switch tabs from it).
    page_id = "new";
    page = build_new_tab();
  } else if (st_.active_tab == "home") {
    page_id = "home";
    page = build_home();
  } else {
    page_id = "session";
    page = build_session();
  }
  page->key = "page:" + page_id + (page_id == "new" ? "" : st_.active_tab);
  // The framework applies the animation, looked up from the page's declaration
  // above - so a page switch animates because it is DECLARED, not because a
  // builder remembered to add a class. The key change makes the diff recreate
  // the node, which is what fires the animation.
  //
  // The token also carries the load state: a session page whose history has
  // arrived animates again, so a slow load does not spend the animation on an
  // empty page (which is how an animation could look like it never ran).
  //
  // Direction: a backward move (back into the tab we came from) slides in from
  // the LEFT, a forward one from the right, so the motion matches the navigation
  // instead of always coming from the same side.
  const char* pg_anim = page_anim_for(page_id);
  if (pg_anim && std::strcmp(pg_anim, anim::kView) == 0 && !page_dir_.empty()) {
    pg_anim = (page_dir_ == "-1") ? anim::kSlideRight : anim::kSlideLeft;
  }
  add_anim(page, pg_anim ? pg_anim : anim::kView);
  page->exit_anim = (page_id == "new")
                        ? anim::kExitFade
                        : ((page_dir_ == "-1") ? anim::kExitRight : anim::kExitLeft);
  {
    std::string token = page_id;
    if (page_id == "session" && st_.session_loaded) token += ":loaded";
    page->anim_key = token;
  }
  H slot = el("div", "page-slot");
  slot.key("page-slot");
  slot.add(page);
  return slot.get();
}
// ===========================================================================
// 左侧栏模式：项目导航
// ===========================================================================
// 这不是"竖过来的标签条"。顶栏模式的标签是一个可以关掉的工作集；左侧栏是
// 导航：永远按项目列出全部会话（和主页·项目一致），点一个就打开，不关闭、
// 不入工作集、不因为开得多而变长。宽度独立（可拖拽），可最小化成一条窄条。
VNodePtr UiApp::build_nav_rail() {
  H rail = el("div", "nav-rail");
  rail.key("nav-rail");
  rail.style("width", itos(st_.nav_width) + "px");
  rail.style("flex", "0 0 auto");

  H head = el("div", "nav-head");
  head.add(el("span", "nav-title").text(tr("项目")));
  head.add(el("span", "grow"));
  // 新建：打开"新建"页（和顶栏 "+" 同一个页面），不是直接在当前项目里开会话
  // ——"新建"要先选项目，这个选择不能替用户做。
  head.add(comp::icon_button("square-plus", tr("新建"), [this](const std::string&) {
    open_new_tab();
  }));
  head.add(comp::icon_button("chevron-left", tr("收起侧栏"), [this](const std::string&) {
    update([&](AppState& s) { s.nav_collapsed = true; });
  }));
  rail.add(head);

  // 主页入口：左栏模式下没有标签条，主页必须能从导航里进去
  H home = el("div", st_.active_tab == "home" && st_.modal_page.empty()
                          ? "nav-home active" : "nav-home");
  home.on(EventKind::kClick, [this](const std::string&) { show("home"); });
  home.add(el("i", "fa fa-house"));
  home.add(el("span").text(tr("主页")));
  rail.add(home);

  H body = el("div", "nav-body");
  if (st_.projects.empty()) {
    body.add(comp::empty_state(tr("还没有项目。"), tr("打开文件夹"),
                               [this](const std::string&) { pick_and_open_folder(); }));
  }
  for (size_t i = 0; i < st_.projects.size(); ++i) {
    const ProjectCard& p = st_.projects[i];
    H grp = el("div", "nav-proj");
    H ph = el("div", "nav-proj-head");
    ph.add(el("span", "nav-proj-mono").text(
        p.name.empty() ? "?" : p.name.substr(0, p.name.size() > 1 ? 2 : 1)));
    ph.add(el("span", "nav-proj-name").text(p.name));
    std::string pid = p.pid;
    H plus = comp::icon_button("square-plus", tr("在这个项目里新建会话"),
                               [this, pid](const std::string&) {
      new_session_in_project(pid);
    });
    plus.cls("nav-proj-add");
    ph.add(plus);
    grp.add(ph);
    for (size_t k = 0; k < p.sessions.size(); ++k) {
      const ProjectCard::SessionRow& s = p.sessions[k];
      std::string sid = s.sid;
      std::string title = s.title;
      H row = el("div", s.sid == st_.active_tab ? "nav-session active" : "nav-session");
      row.on(EventKind::kClick, [this, sid, title](const std::string&) {
        open_session(sid, title);
      });
      row.add(el("span", s.running ? "nav-dot running" : "nav-dot"));
      row.add(el("span", "nav-session-title").text(s.title));
      grp.add(row);
    }
    if (p.sessions.empty()) {
      grp.add(el("div", "nav-empty").text(tr("暂无会话")));
    }
    body.add(grp);
  }
  rail.add(body);

  H foot = el("div", "nav-foot");
  foot.add(comp::button(tr("打开文件夹"), [this](const std::string&) {
    pick_and_open_folder();
  }, "ghost"));
  foot.add(comp::icon_button("gear", tr("设置"), [this](const std::string&) {
    open_modal_page("settings");
  }));
  rail.add(foot);
  return rail.get();
}

// ===========================================================================
// Home
// ===========================================================================
VNodePtr UiApp::build_home() {
  H wrap = el("div", "page home-page");
  wrap.attr("data-home-tab", itos(st_.home_tab));  // diagnostics + tests
  // centered title buttons: 探索 / 项目 / 用量统计
  std::vector<std::string> items;
  items.push_back(tr("探索"));
  items.push_back(tr("项目"));
  items.push_back(tr("用量统计"));
  H head = el("div", "home-head");
  head.add(comp::title_tabs(items, st_.home_tab, [this](const std::string& v) {
    int idx = std::atoi(v.c_str());
    update([&](AppState& s) { s.home_tab = idx; });
    if (idx == 2) {
      // 用量页要显示"按供应商"的范围列表：那来自 provider.listProviders，
      // 以前只有打开过设置或模型选择器才会加载，所以第一次进用量页左侧是空的。
      if (!st_.providers_loaded) load_providers();
      if (!st_.usage_loaded) load_usage();
    }
  }));
  wrap.add(head);

  H body = el("div", "home-body");
  switch (st_.home_tab) {
    case 0: {
      // The explore page lists MCP + skills, so make sure they are loaded
      // (they are normally fetched by the settings pages).
      if (!st_.skills_loaded) load_skills();
      if (!st_.mcp_loaded) load_mcp_tools();
      VNodePtr p = build_home_explore();
      p->key = "home:explore";
      body.add(p);
      break;
    }
    case 2: {
      // Directly opening the page (home_tab=2 restored / tests) must load the
      // same data the tab click does: providers (the left scope list) and usage.
      if (!st_.providers_loaded) load_providers();
      if (!st_.usage_loaded) load_usage();
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
  foot.add(comp::button(tr("打开文件夹"), [this](const std::string&) {
    pick_and_open_folder();
  }, "primary"));
  foot.add(comp::button(tr("新建项目"), [this](const std::string&) {
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
  foot.add(comp::button(tr("设置"), [this](const std::string&) {
    open_modal_page("settings");
  }, "ghost"));
  wrap.add(foot);
  return wrap.get();
}

// 顶层 "+" 打开的页面：项目卡片 + 每个项目的会话列表（点会话即打开、点
// "新建会话"即建）。它**不是标签页**：盖在当前标签页之上，关掉就消失。
VNodePtr UiApp::build_new_tab() {
  H wrap = el("div", "page new-page");
  H head = el("div", "new-head");
  head.add(el("div", "new-title").text(tr("新建")));
  head.add(el("div", "hint").text(
      tr("选一个项目新建会话，或打开一个已有的会话。")));
  H acts = el("div", "new-acts");
  acts.add(comp::button(tr("打开文件夹"), [this](const std::string&) {
    pick_and_open_folder();
  }, "primary"));
  acts.add(comp::button(tr("设置"), [this](const std::string&) {
    open_modal_page("settings");
  }, "ghost"));
  acts.add(el("span", "grow"));
  acts.add(comp::icon_button("\u2715", tr("关闭"), [this](const std::string&) {
    close_modal_page();
  }));
  head.add(acts);
  wrap.add(head);
  H body = el("div", "new-body");
  VNodePtr cards = build_home_projects();
  cards->key = "new:projects";
  body.add(cards);
  wrap.add(body);
  return wrap.get();
}

// 探索（goal.md：未来作为 MCP / Skill / 插件的浏览器）。
// It is the "what can this agent do" page: everything the model can invoke, in
// one searchable list. MCP entries are model-invoked only; skills are the one
// kind the USER can type as /name, so they are marked differently.
VNodePtr UiApp::build_home_explore() {
  H wrap = el("div", "explore");
  H box = el("div", "explore-box");
  box.add(el("div", "explore-title").text(tr("探索")));
  box.add(el("div", "explore-sub").text(
      tr("这台机器上的能力：MCP 工具与 Skill。")));
  H search = comp::search_box(st_.explore_query, tr("搜索名称或说明…"),
                              [this](const std::string& v) {
    // must re-render: the filter decides which rows exist
    update([&](AppState& s) { s.explore_query = v; });
  });
  box.add(search);
  wrap.add(box);

  const std::string q = lower_ascii(st_.explore_query);
  auto hit = [&q](const std::string& a, const std::string& b) {
    if (q.empty()) return true;
    return lower_ascii(a + " " + b).find(q) != std::string::npos;
  };

  size_t shown = 0;
  H list = el("div", "explore-list");

  // --- MCP ---
  H mcp_sec = el("div", "explore-sec");
  H mcp_head = el("div", "explore-sec-head");
  mcp_head.add(el("i", "fa fa-plug"));
  mcp_head.add(el("span").text(tr("MCP")));
  mcp_head.add(comp::badge(tr("模型调用")));
  mcp_sec.add(mcp_head);
  size_t mcp_shown = 0;
  for (size_t i = 0; i < st_.mcp_tools.size(); ++i) {
    const AppState::McpRow& m = st_.mcp_tools[i];
    if (!hit(m.name, m.description + " " + m.dir)) continue;
    ++mcp_shown;
    ++shown;
    H row = el("div", m.enabled ? "explore-row" : "explore-row off");
    row.add(el("span", "er-name").text(m.name));
    row.add(el("span", "er-desc dim").text(m.description));
    row.add(el("span", "grow"));
    if (!m.enabled) row.add(comp::badge(tr("已停用")));
    row.add(comp::icon_button("sliders", tr("在设置中管理"), [this](const std::string&) {
      update([&](AppState& s) { s.settings_nav = "mcp"; });
      open_modal_page("settings");
    }));
    mcp_sec.add(row);
  }
  if (mcp_shown == 0) {
    mcp_sec.add(el("div", "explore-empty").text(
        st_.mcp_tools.empty() ? tr("还没有 MCP。可在设置里探测或导入。")
                              : tr("没有匹配的 MCP。")));
  }
  list.add(mcp_sec);

  // --- Skill ---
  H sk_sec = el("div", "explore-sec");
  H sk_head = el("div", "explore-sec-head");
  sk_head.add(el("i", "fa fa-wand-magic-sparkles"));
  sk_head.add(el("span").text(tr("Skill")));
  sk_head.add(comp::badge(tr("可 /调用")));
  sk_sec.add(sk_head);
  size_t sk_shown = 0;
  for (size_t i = 0; i < st_.skills.size(); ++i) {
    const AppState::SkillRow& s = st_.skills[i];
    std::string label = s.title.empty() ? s.name : s.title;
    if (!hit(label, s.name + " " + s.description)) continue;
    ++sk_shown;
    ++shown;
    H row = el("div", "explore-row");
    row.add(el("span", "er-name").text(label));
    row.add(el("span", "er-slash").text("/" + s.name));
    row.add(el("span", "er-desc dim").text(s.description));
    row.add(el("span", "grow"));
    std::string name = s.name;
    row.add(comp::icon_button("eye", tr("查看内容"), [this, name](const std::string&) {
      call_async("skill.load", Json{{"name", name}}, [this, name](const BackendResult& r) {
        toast(r.ok ? (tr("已加载 skill：") + name) : (tr("加载失败：") + r.message));
      });
    }));
    sk_sec.add(row);
  }
  if (sk_shown == 0) {
    sk_sec.add(el("div", "explore-empty").text(
        st_.skills.empty() ? tr("还没有 Skill。可在设置里探测或导入。")
                           : tr("没有匹配的 Skill。")));
  }
  list.add(sk_sec);

  if (shown == 0 && !q.empty()) {
    wrap.add(comp::empty_state(tr("没有匹配的能力。"), "", nullptr));
  } else {
    wrap.add(list);
  }
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

    // ---- left: identity block ----
    // A monogram + name + path, then the numbers that matter, then the primary
    // action. The old version stacked two full-width labelled buttons here,
    // which is what made the column look like a form.
    H top = el("div", "proj-top");
    H idrow = el("div", "proj-id");
    H mono = el("span", "proj-mono");
    // Two letters at most; a path-like name otherwise collapses to one glyph.
    mono.text(p.name.empty() ? "?" : p.name.substr(0, p.name.size() > 1 ? 2 : 1));
    idrow.add(mono);
    H idtext = el("div", "proj-idtext");
    idtext.add(el("div", "proj-name").text(p.name));
    idtext.add(el("div", "proj-path").text(p.path));
    idrow.add(idtext);
    top.add(idrow);

    H stats = el("div", "proj-stats");
    H st1 = el("div", "proj-stat");
    st1.add(el("span", "psv").text(itos(p.session_count)));
    st1.add(el("span", "psl").text(tr("会话")));
    stats.add(st1);
    if (!p.updated.empty()) {
      H st2 = el("div", "proj-stat");
      st2.add(el("span", "psv").text(p.updated));
      st2.add(el("span", "psl").text(tr("最近")));
      stats.add(st2);
    }
    top.add(stats);

    H actions = el("div", "proj-actions");
    std::string pid = p.pid;
    // 新建会话 is the reason you open this page, so it is a real button in the
    // identity block rather than the last row of a list you have to scroll to.
    H mk = comp::button(tr("新建会话"), [this, pid](const std::string&) {
      new_session_in_project(pid);
    }, "primary");
    mk.cls("proj-new");
    actions.add(mk);
    if (!pid.empty()) {
      H reveal = comp::icon_button("folder-open", tr("在资源管理器中打开"),
                                   [this, pid](const std::string&) {
        call_async("project.reveal", Json{{"pid", pid}}, [this](const BackendResult& r) {
          if (!r.ok) toast(tr("无法打开文件管理器"));
        });
      });
      actions.add(reveal);
      H rm = comp::icon_button("trash", tr("移出列表（不删除文件）"),
                               [this, pid](const std::string&) {
        call_async("project.remove", Json{{"pid", pid}}, [this](const BackendResult&) {
          load_projects();
        });
      });
      actions.add(rm);
    }
    top.add(actions);
    card.add(top);

    // ---- right: session list ----
    // The list is ALWAYS built - CSS decides how it is shown (wide: directly,
    // with its own height and scroll; narrow: stacked below and collapsed until
    // the header is clicked).
    H right = el("div", p.expanded ? "proj-right open" : "proj-right");
    H rhead = el("div", "proj-right-head");
    rhead.on(EventKind::kClick, [this, i](const std::string&) {
      update([&](AppState& s) {
        if (i < s.projects.size()) s.projects[i].expanded = !s.projects[i].expanded;
      });
    });
    rhead.add(el("span", "prh-title").text(tr("会话")));
    rhead.add(el("span", "prh-count").text(itos(p.session_count)));
    rhead.add(el("span", "grow"));
    rhead.add(el("span", "prh-toggle").text(p.expanded ? tr("收起 \u25be")
                                                       : tr("展开 \u25b8")));
    right.add(rhead);
    H list = el("div", "proj-sessions");
    for (size_t k = 0; k < p.sessions.size(); ++k) {
      const ProjectCard::SessionRow& s = p.sessions[k];
      std::string sid = s.sid;
      if (s.renaming) {
        // Inline rename: the row turns into an input. Enter commits, blur also
        // commits (clicking away is the normal way to finish), Esc cancels.
        H rrow = el("div", "proj-session renaming");
        rrow.add(el("span", s.running ? "ps-dot running" : "ps-dot"));
        H inp = comp::text_input(s.rename_text, tr("会话名称"),
                                 [this, sid](const std::string& v) {
          update([&](AppState& st2) {
            for (size_t a = 0; a < st2.projects.size(); ++a) {
              for (size_t b = 0; b < st2.projects[a].sessions.size(); ++b) {
                if (st2.projects[a].sessions[b].sid == sid) {
                  st2.projects[a].sessions[b].rename_text = v;
                }
              }
            }
          });
        }, [this, sid](const std::string&) { commit_rename_session(sid); });
        inp.cls("ps-rename-input");
        inp.key("session-rename-input");
        // Enter commits; so does losing focus (clicking away is the normal way
        // to finish a rename). Esc is not bound here - the adapter's text input
        // has no per-node Esc, and committing is the safer default.
        inp.on(EventKind::kBlur, [this, sid](const std::string&) {
          commit_rename_session(sid);
        });
        rrow.add(inp);
        list.add(rrow);
        continue;
      }
      H row = el("div", "proj-session");
      std::string title = s.title;
      row.on(EventKind::kClick, [this, sid, title](const std::string&) {
        open_session(sid, title);
      });
      H dot = el("span", s.running ? "ps-dot running" : "ps-dot");
      dot.attr("title", s.running ? tr("正在运行") : tr("空闲"));
      row.add(dot);
      row.add(el("span", "ps-title").text(s.title));
      row.add(el("span", "grow"));
      if (s.updated_ms > 0) {
        row.add(el("span", "ps-time dim").text(rel_time(s.updated_ms)));
      }
      // Hover actions: rename + delete. Hidden until hover so a long list does
      // not read as a toolbar; the click must not open the session either.
      H acts = el("span", "ps-acts");
      H ren = comp::icon_button("pen", tr("重命名"), [this, sid](const std::string&) {
        begin_rename_session(sid);
      });
      acts.add(ren);
      H del = comp::icon_button("\u2715", tr("删除会话（不可恢复）"),
                                [this, sid, title](const std::string&) {
        confirm_delete_session(sid, title);
      });
      acts.add(del);
      row.add(acts);
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
  // 每个区块整块滑入，行/柱子在下面再各自生长。原来整个页面渲染完就"啪"地全
  // 出现，图表尤其像是静态图片：现在柱子和进度条从 0 长出来，数字行错开进场。
  wrap.anim(anim::kEnter).anim_key("usage:" + itos(st_.usage_range) + ":" +
                                   st_.usage_scope + (st_.usage_loaded ? ":1" : ":0"));

  // ---- left: real scopes (总计 + one per provider) ----
  // The old list was a hardcoded array of three strings, one of which was
  // literally "自定义" - it could not select anything.
  H left = el("div", "usage-scopes");
  H tot = el("div", st_.usage_scope.empty() ? "scope sel" : "scope");
  tot.on(EventKind::kClick, [this](const std::string&) {
    update([&](AppState& s) { s.usage_scope.clear(); });
    load_usage();
  });
  tot.add(el("span").text(tr("总计")));
  left.add(tot);
  for (size_t i = 0; i < st_.providers.size(); ++i) {
    const AppState::ProviderRow& p = st_.providers[i];
    bool sel = st_.usage_scope == p.id;
    H it = el("div", sel ? "scope sel" : "scope");
    std::string pid = p.id;
    it.on(EventKind::kClick, [this, pid](const std::string&) {
      update([&](AppState& s) { s.usage_scope = pid; });
      load_usage();
    });
    it.add(el("span").text(p.name.empty() ? p.id : p.name));
    left.add(it);
  }
  wrap.add(left);

  H panel = el("div", "usage-panel");

  // range tabs: the query is filtered by `from`, not by a fake bar chart.
  // Labels go through tr() so an English UI does not show "1 天".
  H ranges = el("div", "usage-ranges");
  const char* kRangeLabels[4] = {"1 天", "7 天", "30 天", "全部"};
  for (int i = 0; i < 4; ++i) {
    int idx = i;
    H b = el("div", i == st_.usage_range ? "usage-range active" : "usage-range");
    b.on(EventKind::kClick, [this, idx](const std::string&) {
      update([&](AppState& s) { s.usage_range = idx; });
      load_usage();
    });
    b.add(el("span").text(tr(kRangeLabels[i])));
    ranges.add(b);
  }
  panel.add(ranges);

  // ---- totals ----
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
  head.add(comp::key_value(tr("调用次数"), calls));
  head.add(comp::key_value(tr("输入 tokens"), in_tok));
  head.add(comp::key_value(tr("输出 tokens"), out_tok));
  head.add(comp::key_value(tr("总成本"), cost));
  panel.add(head);

  // ---- by model: a proportional bar per model (the pie could not show values) ----
  Json pm = st_.usage_loaded && st_.usage.contains("byProviderModel") &&
                    st_.usage["byProviderModel"].is_object()
                ? st_.usage["byProviderModel"]
                : Json::object();
  std::vector<std::pair<std::string, int64_t> > rows;
  int64_t total_tok = 0;
  for (Json::const_iterator it = pm.begin(); it != pm.end(); ++it) {
    int64_t n = jsonutil::get_int(it.value(), "in", 0) +
                jsonutil::get_int(it.value(), "out", 0);
    if (n <= 0) n = jsonutil::get_int(it.value(), "calls", 0);  // tokens 未知时按次数
    if (n <= 0) continue;
    std::string model = jsonutil::get<std::string>(it.value(), "model", "");
    std::string prov = jsonutil::get<std::string>(it.value(), "provider", "");
    rows.push_back(std::make_pair(prov.empty() ? model : (prov + " / " + model), n));
    total_tok += n;
  }
  std::sort(rows.begin(), rows.end(),
            [](const std::pair<std::string, int64_t>& a,
               const std::pair<std::string, int64_t>& b) { return a.second > b.second; });

  // ---- by day: a real column chart over byDay ----
  // The backend has always returned byDay; nobody drew it. It is the one view
  // that shows WHEN the tokens went out, and it reads as a shape rather than
  // another table. Empty days inside the selected window are kept as zero
  // columns - dropping them would compress the time axis and lie about usage.
  {
    Json bd = st_.usage_loaded && st_.usage.contains("byDay") &&
                      st_.usage["byDay"].is_object()
                  ? st_.usage["byDay"]
                  : Json::object();
    std::map<int64_t, int64_t> by_day;
    std::map<int64_t, int64_t> by_day_calls;
    int64_t any_tokens = 0;
    for (Json::const_iterator it = bd.begin(); it != bd.end(); ++it) {
      int64_t day = std::strtoll(it.key().c_str(), nullptr, 10);
      int64_t n = jsonutil::get_int(it.value(), "in", 0) +
                  jsonutil::get_int(it.value(), "out", 0);
      by_day[day] = n;
      by_day_calls[day] = jsonutil::get_int(it.value(), "calls", 0);
      any_tokens += n;
    }
    // 供应商没回报 usage 时 token 全是 0。此时按调用次数画图，而不是画一排
    // 0 高度的柱子——"有 7 次调用"和"什么都没发生"是两件事。
    bool by_tokens = any_tokens > 0;
    static const int kDays2[4] = {1, 7, 30, 0};
    int win = kDays2[st_.usage_range < 0 || st_.usage_range > 3 ? 1 : st_.usage_range];
    int64_t today = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count()) / 86400000;
    std::vector<int64_t> days;
    if (win > 0) {
      for (int i = win - 1; i >= 0; --i) days.push_back(today - i);
    } else {
      for (std::map<int64_t, int64_t>::const_iterator it = by_day.begin();
           it != by_day.end(); ++it) {
        days.push_back(it->first);
      }
      // 全部 can span years; keep the recent tail, which is what is read
      if (days.size() > 60) days.erase(days.begin(), days.end() - 60);
    }
    int64_t peak = 0, sum = 0;
    for (size_t i = 0; i < days.size(); ++i) {
      std::map<int64_t, int64_t>::const_iterator f = by_day.find(days[i]);
      int64_t n = f == by_day.end() ? 0 : f->second;
      if (!by_tokens) {
        std::map<int64_t, int64_t>::const_iterator c = by_day_calls.find(days[i]);
        n = c == by_day_calls.end() ? 0 : c->second;
      }
      if (n > peak) peak = n;
      sum += n;
    }
    H chart = el("div", "usage-chart usage-byday");
    H ctop = el("div", "chart-head");
    ctop.add(el("div", "chart-title").text(
        by_tokens ? tr("按天（输入 + 输出 tokens）") : tr("按天（调用次数）")));
    ctop.add(el("span", "grow"));
    ctop.add(el("span", "chart-total dim").text(
        sum > 0 ? (tr("合计 ") + itos(static_cast<int>(sum)) +
                   (by_tokens ? tr(" tokens") : tr(" 次"))) : std::string()));
    chart.add(ctop);
    H plot = el("div", "bar-chart");
    // The plot is keyed by the filters: changing the range or the provider
    // recreates the bars, which is what replays the growth animation.
    plot.key("ud:" + st_.usage_scope + ":" + itos(st_.usage_range) + ":" +
             itos(static_cast<int>(sum)));
    if (days.empty()) {
      plot.add(el("div", "hint").text(tr("暂无用量记录")));
    }
    for (size_t i = 0; i < days.size(); ++i) {
      std::map<int64_t, int64_t>::const_iterator f = by_day.find(days[i]);
      int64_t n = f == by_day.end() ? 0 : f->second;
      if (!by_tokens) {
        std::map<int64_t, int64_t>::const_iterator c = by_day_calls.find(days[i]);
        n = c == by_day_calls.end() ? 0 : c->second;
      }
      int pct = peak > 0 ? static_cast<int>(n * 100 / peak) : 0;
      if (n > 0 && pct < 2) pct = 2;  // a non-zero day must stay visible
      H bar = el("div", n > 0 ? "bar" : "bar zero");
      bar.style("--grow", itos(pct) + "%");
      bar.style("animation-delay", itos(static_cast<int>(i) * 16) + "ms");
      // 日期 + 数值：柱状图没有数值就只剩形状，读不出量
      time_t t = static_cast<time_t>(days[i] * 86400);
      struct tm* g = localtime(&t);
      char buf[80];
      if (g) {
        std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d · %ld %s",
                      g->tm_year + 1900, g->tm_mon + 1, g->tm_mday,
                      static_cast<long>(n), by_tokens ? "tokens" : "calls");
      } else {
        std::snprintf(buf, sizeof(buf), "%ld %s", static_cast<long>(n),
                      by_tokens ? "tokens" : "calls");
      }
      bar.attr("title", buf);
      plot.add(bar);
    }
    chart.add(plot);
    // date labels under the columns (only when they do not collide)
    if (!days.empty()) {
      H axis = el("div", "bar-axis");
      bool dense = days.size() <= 12;
      for (size_t i = 0; i < days.size(); ++i) {
        time_t t = static_cast<time_t>(days[i] * 86400);
        struct tm* g = localtime(&t);
        char lab[16] = "";
        if (g) std::snprintf(lab, sizeof(lab), "%d/%d", g->tm_mon + 1, g->tm_mday);
        bool show = dense || i == 0 || i + 1 == days.size() || (i % 5 == 0);
        H sp = el("span", show ? "ba-lab" : "ba-lab off");
        sp.text(show ? lab : std::string());
        axis.add(sp);
      }
      chart.add(axis);
    }
    panel.add(chart);
  }

  H bymodel = el("div", "usage-chart usage-bymodel");
  bymodel.add(el("div", "chart-title").text(tr("按模型")));
  if (rows.empty()) {
    bymodel.add(el("div", "hint").text(tr("暂无用量记录")));
  } else {
    for (size_t i = 0; i < rows.size() && i < 8; ++i) {
      int pct = total_tok > 0 ? static_cast<int>(rows[i].second * 100 / total_tok) : 0;
      H r = el("div", "ub-row");
      r.style("animation-delay", itos(static_cast<int>(i) * 30) + "ms");
      r.add(el("span", "ub-name").text(rows[i].first));
      H track = el("div", "ub-track");
      H fill = el("div", "ub-fill");
      // 从 0 长出来：--grow 是终值，动画从 width:0 走到它
      fill.style("--grow", itos(pct) + "%");
      fill.style("animation-delay", itos(120 + static_cast<int>(i) * 55) + "ms");
      track.add(fill);
      r.add(track);
      r.add(el("span", "ub-val dim").text(itos(pct) + "%"));
      bymodel.add(r);
    }
  }
  panel.add(bymodel);

  // ---- table: real byProviderModel rows, else an explicit empty state ----
  H tbl = el("table", "stat-table");
  H thead = el("tr");
  const char* hd[5] = {"模型", "输入", "输出", "缓存", "成本"};
  for (int i = 0; i < 5; ++i) thead.add(el("th").text(tr(hd[i])));
  tbl.add(thead);
  bool any = false;
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
  if (!any) {
    H tr = el("tr");
    for (int c = 0; c < 5; ++c) {
      tr.add(el("td").text(c == 0 ? std::string("（暂无用量记录）") : std::string()));
    }
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
    hist_inner.add(comp::empty_state(tr("开始新的对话。输入消息后 Enter 发送。"), "", nullptr));
  }
  for (size_t i = 0; i < st_.messages.size(); ++i) {
    comp::ChatMsg m = st_.messages[i];
    hist_inner.add(comp::chat_message(m, [this](const std::string&) { toast(tr("已复制")); },
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
  // ---- attachments: ABOVE the input, never inside it ----
  // An attachment is not text: mixing @paths into the textarea meant you could
  // not tell what was typed from what was attached, and the "/" menu had to
  // compete with a line of @-references.
  if (!st_.attachments.empty()) {
    H att = el("div", "composer-attach");
    for (size_t i = 0; i < st_.attachments.size(); ++i) {
      H chip = el("span", "att-chip");
      chip.add(comp::file_icon(st_.attachments[i], false));
      chip.add(el("span", "att-name").text(folder_basename(st_.attachments[i])));
      size_t idx = i;
      chip.add(comp::icon_button("\u2715", tr("移除附件"), [this, idx](const std::string&) {
        update([&](AppState& s) {
          if (idx < s.attachments.size()) s.attachments.erase(s.attachments.begin() + idx);
        });
      }));
      att.add(chip);
    }
    comp_box.add(att);
  }

  H ta = comp::textarea(st_.session_input, tr("输入消息…（Enter 发送，Shift+Enter 换行）"),
                        [this](const std::string& v) {
    st_.session_input = v;  // no re-render while typing (avoid caret jumps)
  },
                        [this](const std::string&) { send_message(); });
  ta.key("session-input");  // send_message() clears it directly after sending
  comp_box.add(ta);

  H tools = el("div", "composer-tools");
  // LEFT: what this message does (attach / command / plan / auto / permission).
  H left_tools = el("div", "ct-left");
  // 文件 = upload an attachment. The click is handled by the DOM adapter
  // (bind_file_picker, wired in render()) because only the browser can open a
  // file chooser; there is no app-side click handler here on purpose.
  H upload = comp::icon_text_button("paperclip", tr("文件"), nullptr, "ghost");
  upload.key("upload-btn");
  left_tools.add(upload);
  left_tools.add(comp::icon_text_button("wand-magic-sparkles", tr("命令"),
                                        [this](const std::string&) {
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
  left_tools.add(comp::icon_text_button("diagram-project", tr("规划"),
                                        [this](const std::string&) {
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
  // 自动挡: a toggle button, not a select - it is one bit and it is switched
  // often. The label says which mode is active.
  // 顺序：自动挡 在 批准模式 前面。先决定"谁来做"（人/AI），再决定"做到什么
  // 程度能自己动手"（权限级别），读起来是从粗到细；反过来就成了"先定权限再定
  // 谁来跑"，很别扭。批准模式也因此在最外侧——它点开的下拉朝外展开。
  {
    H auto_btn = comp::icon_text_button(st_.auto_mode ? "robot" : "hand",
                                        st_.auto_mode ? tr("自动") : tr("手动"),
                                        [this](const std::string&) {
      bool next = !st_.auto_mode;
      std::string sid = active_sid();
      update([&](AppState& s) { s.auto_mode = next; });
      if (!sid.empty()) {
        call_async("session.setAutoMode", Json{{"sid", sid}, {"autoMode", next}});
      }
    }, st_.auto_mode ? "ghost on" : "ghost");
    auto_btn.attr("title", st_.auto_mode ? tr("自动模式（点击切到手动）")
                                        : tr("手动模式（点击切到自动）"));
    left_tools.add(auto_btn);
  }
  // 权限: a compact select, but with the three levels spelled out (the values
  // alone - approve/permissive/full - mean nothing). The wording states what
  // each level actually gates: 宽松 = 项目内放行、项目外/系统操作要问。
  {
    H perm_sel = el("div", "bar-group");
    std::vector<std::string> perms;
    perms.push_back(tr("逐次批准"));
    perms.push_back(tr("宽松"));
    perms.push_back(tr("完全放行"));
    H sel = comp::select(perms, st_.perm_index, [this](const std::string& v) {
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
    });
    // One lookup key per tooltip: splitting a sentence across adjacent literals
    // makes the translation table miss it (the concatenated text is what the
    // user sees, but each literal is looked up separately).
    sel.attr("title", tr("文件权限：逐次批准 = 每次写文件都问；宽松 = 项目内直接做、系统操作（其他文件夹读写/终端/网络）询问；完全放行 = 不再询问"));
    perm_sel.add(sel);
    left_tools.add(perm_sel);
  }
  tools.add(left_tools);
  H grow = el("span", "grow");
  tools.add(grow);

  // RIGHT: what the message is sent with (model / thinking / suspend / send).
  H right_tools = el("div", "ct-right");
  // 模型不是一个下拉列表：真实安装里有多个供应商、每个几十个模型，还有能力
  // 和思考档位的差异，所以点开是一个真正的选择器（左供应商 / 右模型）。
  H model_btn = el("button", "model-pick-btn");
  model_btn.attr("type", "button");
  model_btn.attr("title", st_.model.empty() ? tr("还没有选择模型")
                                            : tr("选择模型"));
  model_btn.on(EventKind::kClick, [this](const std::string&) {
    open_model_picker("composer", "");
  });
  model_btn.add(comp::model_icon("", st_.model));
  model_btn.add(el("span", "mpb-name").text(st_.model.empty() ? tr("选择模型") : st_.model));
  model_btn.add(el("span", "mpb-caret").text("\u25be"));
  // 上下文水位：贴在模型按钮底边的一小段线，而不是输入框下面横贯整行的一条
  // （那是"进度条"的语汇，放在这里既抢眼又和发送按钮没有关系）。它是按钮
  // 自身的装饰，宽度跟着按钮走，水位高了颜色才变，所以平时几乎看不见。
  {
    H mb = el("div", "model-btn-wrap");
    mb.key("model-btn-wrap");
    mb.add(model_btn);
    int pct = st_.context_pct < 0 ? 0 : (st_.context_pct > 100 ? 100 : st_.context_pct);
    H lvl = el("div", "model-level");
    H lvl_fill = el("div", "model-level-fill");
    lvl_fill.style("width", itos(pct) + "%");
    if (st_.context_pct >= 85) lvl_fill.cls("hot");
    else if (st_.context_pct >= 60) lvl_fill.cls("warm");
    lvl.add(lvl_fill);
    lvl.attr("title", tr("上下文水位 ") + itos(st_.context_pct) + "%");
    mb.add(lvl);
    right_tools.add(mb);
  }
  // 思考档位: only shown when the current model actually has levels - a control
  // with no options is worse than no control.
  {
    std::vector<std::string> levels = thinking_levels_for(st_.model);
    if (!levels.empty()) {
      std::vector<std::string> opts;
      opts.push_back(tr("默认"));
      int sel = 0;
      for (size_t i = 0; i < levels.size(); ++i) {
        opts.push_back(thinking_label(levels[i]));
        if (levels[i] == st_.thinking) sel = static_cast<int>(i) + 1;
      }
      H th = el("div", "bar-group");
      H thsel = comp::select(opts, sel, [this, levels](const std::string& v) {
        int idx = std::atoi(v.c_str());
        std::string val = (idx <= 0 || idx > static_cast<int>(levels.size()))
                              ? std::string() : levels[idx - 1];
        std::string sid = active_sid();
        update([&](AppState& s) { s.thinking = val; });
        if (!sid.empty()) {
          call_async("session.setThinking", Json{{"sid", sid}, {"thinking", val}});
        }
      });
      thsel.attr("title", tr("思考档位"));
      th.add(thsel);
      right_tools.add(th);
    }
  }
  // 挂起 sits right next to 发送. Both read as a WORD when there is room and as
  // an icon when there is not. Their height never changes, so crossing the
  // threshold cannot reflow the row.
  //
  // NOTE: 挂起 no longer flips optimistically. The backend owns the state (the
  // loop may already be parked), so the button waits for its reply and only then
  // changes label - a button that says "paused" before anything paused is a lie.
  H suspend = comp::icon_text_button(
      st_.suspended ? "play" : "pause", st_.suspended ? tr("恢复") : tr("挂起"),
      [this](const std::string&) {
    bool next = !st_.suspended;
    std::string sid = active_sid();
    if (sid.empty()) {
      update([&](AppState& s) { s.suspended = next; });
      return;
    }
    call_async(next ? "session.suspend" : "session.resume", Json{{"sid", sid}},
               [this, next](const BackendResult& r) {
      if (!r.ok) {
        toast(tr("操作失败：") + r.message);
        return;
      }
      update([&](AppState& s) { s.suspended = next; });
    });
  }, "ghost");
  suspend.key("suspend-btn").cls("text-wide");
  right_tools.add(suspend);
  H send = comp::icon_text_button("paper-plane", tr("发送"),
                                  [this](const std::string&) { send_message(); },
                                  "primary");
  send.key("send-btn").cls("text-wide");
  right_tools.add(send);
  tools.add(right_tools);
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
      pop.add(el("div", "skill-menu-item dim").text(tr("（暂无 skill）")));
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
    close.add(el("span").text(tr("取消（Esc）")));
    pop.add(close);
    comp_box.add(pop);
  }
  // ---- 模型选择器 ----
  // Anchored to the composer, exactly like the "/" menu above: it opens from the
  // model button, so it has to come out of it (a dialog in the middle of the
  // screen has no visible connection to what opened it). It also must NOT live
  // in the overlay layer, which is `pointer-events: none` - a backdrop placed
  // there cannot receive the click that is supposed to dismiss it, which is why
  // the picker used to stay glued open.
  //
  // The popup itself is a shared component (build_model_picker): the same one is
  // used by the settings fields, so "which model" is answered identically
  // everywhere and there is only one copy to keep working.
  if (st_.model_picker_open && st_.model_picker_owner == "composer") {
    H back = el("div", "picker-backdrop");
    back.on(EventKind::kClick, [this](const std::string&) {
      update([&](AppState& s) {
        s.model_picker_open = false;
        s.model_picker_owner.clear();
      });
    });
    comp_box.add(back);
    comp_box.add(build_model_picker("composer", st_.model));
  }
  center.add(comp_box);

  // plan panel floats over the history's left edge (absolute, inside center)
  if (st_.plan_open) {
    H plan = el("div", "plan-float");
    plan.anim("pop");
    H phead = el("div", "plan-head");
    phead.add(el("span", "plan-title").text(tr("任务规划")));
    phead.add(comp::icon_button("\u2715", "收起", [this](const std::string&) {
      update([&](AppState& s) { s.plan_open = false; });
    }));
    plan.add(phead);
    if (st_.plan_steps.empty()) {
      plan.add(el("div", "plan-empty dim").text(tr("暂无规划步骤")));
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
    update([&](AppState& s) {
      // remember where we came from so closing this panel returns there
      if (!s.active_side.empty() && s.active_side != k) {
        s.side_history.push_back(s.active_side);
        if (s.side_history.size() > 64) s.side_history.erase(s.side_history.begin());
      }
      s.active_side = k;
    });
    // refresh that panel's own data when it becomes active
    load_side_tab(k);
  },
                            [this](const std::string& k) { close_side_tab(k); },
                            [this](const std::string&) {
    // "+" opens a temporary page that aggregates every panel kind (with a
    // description); it must never silently default to one page.
    //
    // Opening it also EXPANDS the side panel: the "+" lives inside the panel, so
    // if the panel were collapsed the click would look like it did nothing (and
    // with a stale patch it once opened this page behind a hidden panel).
    update([&](AppState& s) {
      s.side_collapsed = false;
      s.side_menu_open = !s.side_menu_open;
    });
  }));

  // ---- the "+" page: pick a panel to add ----
  // A page rather than a popup menu: there are enough kinds that each needs a
  // one-line explanation, and a menu cannot carry that. Picking one adds the
  // panel (and, like every panel, it is a real tab that can be closed again).
  if (st_.side_menu_open) {
    H page = el("div", "side-add");
    page.anim(anim::kEnter);
    page.add(el("div", "side-add-head").text(tr("添加面板")));
    page.add(comp::hint(tr("选一个面板加到右侧栏；文件/Git 可以多开。")));
    H grid = el("div", "side-add-grid");
    for (size_t i = 0; i < kSideKindCount; ++i) {
      std::string kind = kSideKinds[i].kind;
      bool multi = kSideKinds[i].multi;
      H card = el("div", "side-add-card");
      // a singleton kind that is already open is marked and jumps instead of
      // duplicating; a multi kind always adds a new panel
      bool open = false;
      for (size_t k = 0; k < st_.side_tabs.size(); ++k) {
        if (st_.side_tabs[k].kind == kind) open = true;
      }
      if (open && !multi) card.cls("open");
      card.on(EventKind::kClick, [this, kind](const std::string&) {
        update([&](AppState& s) { s.side_menu_open = false; });
        open_or_focus_side_tab(kind);
      });
      card.add(el("div", "side-add-name").text(tr(kSideKinds[i].title)));
      card.add(el("div", "side-add-desc").text(tr(kSideKinds[i].desc)));
      if (open && !multi) card.add(el("div", "side-add-tag").text(tr("已打开")));
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
  // 侧栏面板内容按面板 key 化：切换面板时整块重新创建并播放进场动画。原来
  // body 是一个无 key 的稳定节点，切换只是原地换字，看起来像"闪了一下"。
  body.key("side-body:" + st_.active_side);
  body.anim(anim::kEnter).anim_key("side:" + st_.active_side);
  // The active panel, so each panel can read ITS OWN state (two file panels sit
  // in two different directories; sharing one set of fields would make them
  // fight over it).
  std::string kind = "usage";
  const SideTab* panel = nullptr;
  for (size_t i = 0; i < st_.side_tabs.size(); ++i) {
    if (st_.side_tabs[i].id == st_.active_side) {
      kind = st_.side_tabs[i].kind;
      panel = &st_.side_tabs[i];
    }
  }
  const std::string panel_id = panel ? panel->id : std::string();

  if (kind == "usage") {
    body.add(comp::key_value(tr("上下文水位"), itos(st_.context_pct) + "%"));
    body.add(comp::progress(st_.context_pct));
    body.add(comp::key_value(tr("本会话成本"), st_.session_cost));
    body.add(comp::key_value(tr("本轮 tokens"), "12.4K / 3.1K"));
  } else if (kind == "subagents") {
    if (st_.subagents.empty()) {
      body.add(comp::empty_state(tr("没有子代理在运行。"), "", nullptr));
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
      acts.add(comp::button(tr("引导"), [this, i](const std::string&) {
        if (i >= st_.subagents.size()) return;
        std::string id = st_.subagents[i].id;
        call_async("subagent.steer", Json{{"id", id}, {"text", "继续"}},
                   [this](const BackendResult&) { toast(tr("已发送引导")); });
      }, "ghost"));
      acts.add(comp::button(tr("取消"), [this, i](const std::string&) {
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
  } else if (kind == "files") {
    if (st_.active_pid.empty()) {
      body.add(comp::empty_state(tr("先打开一个会话，文件树会显示该项目的文件。"), "", nullptr));
    } else {
      const std::string& path = panel ? panel->path : std::string();
      // breadcrumb / up row
      H nav = el("div", "files-nav");
      if (!path.empty()) {
        nav.add(comp::button(tr("↑ 上级"), [this, panel_id, path](const std::string&) {
          size_t cut = path.find_last_of("/\\");
          std::string parent = cut == std::string::npos ? "" : path.substr(0, cut);
          load_files(panel_id, st_.active_pid, parent);
        }, "ghost"));
      }
      nav.add(el("span", "files-cwd dim").text(path.empty() ? "/" : ("/" + path)));
      nav.add(comp::button(tr("刷新"), [this, panel_id, path](const std::string&) {
        load_files(panel_id, st_.active_pid, path);
      }, "ghost"));
      body.add(nav);
      const std::vector<SideTab::FileEntry>& entries =
          panel ? panel->entries : std::vector<SideTab::FileEntry>();
      if (entries.empty()) {
        body.add(comp::empty_state(tr("（空目录）"), "", nullptr));
      }
      for (size_t i = 0; i < entries.size(); ++i) {
        const SideTab::FileEntry& e = entries[i];
        H row = el("div", "file-row");
        std::string name = e.name;
        bool isdir = e.is_dir;
        row.on(EventKind::kClick, [this, panel_id, path, name, isdir](const std::string&) {
          std::string rel = path.empty() ? name : (path + "/" + name);
          if (isdir) {
            load_files(panel_id, st_.active_pid, rel);
          } else {
            // Opening a file opens a PREVIEW TAB. An inline block under the tree
            // pushed the tree off screen, so you could not read a file and keep
            // browsing at the same time.
            open_file_preview(rel);
          }
        });
        row.add(comp::file_icon(e.name, e.is_dir));
        row.add(el("span", "file-name").text(e.name));
        if (!e.is_dir) {
          row.add(el("span", "file-size dim").text(
              e.size > 1024 ? (itos(static_cast<int>(e.size / 1024)) + "K")
                            : (itos(static_cast<int>(e.size)) + "B")));
        }
        body.add(row);
      }
    }
  } else if (kind == "preview") {
    // One file's contents, in its own tab.
    const std::string& rel = panel ? panel->path : std::string();
    if (rel.empty()) {
      body.add(comp::empty_state(tr("没有选中的文件。"), "", nullptr));
    } else {
      H head = el("div", "files-nav");
      head.add(el("span", "files-cwd").text(folder_basename(rel)));
      head.add(el("span", "grow"));
      head.add(comp::button(tr("刷新"), [this, panel_id, rel](const std::string&) {
        preview_file(panel_id, rel);
      }, "ghost"));
      body.add(head);
      H pre = el("pre", "file-preview-body");
      pre.text(panel->loaded ? (panel->preview.empty() ? tr("（空文件）") : panel->preview)
                             : tr("加载中…"));
      body.add(pre);
    }
  } else if (kind == "git") {
    body.add(comp::empty_state(tr("Git 状态（需要项目为 git 仓库；git 工具尚未接线）"),
                               "", nullptr));
  } else if (kind == "ops") {
    if (st_.session_ops.empty()) {
      body.add(comp::empty_state(tr("暂无操作记录（systemOps）。"), "", nullptr));
    }
    for (size_t i = 0; i < st_.session_ops.size(); ++i) {
      H row = el("div", "op-row");
      row.add(el("span", "op-name").text(st_.session_ops[i].op));
      row.add(el("span", "op-detail dim").text(st_.session_ops[i].detail));
      body.add(row);
    }
  } else if (kind == "calls") {
    if (st_.session_calls.empty()) {
      body.add(comp::empty_state(tr("暂无工具调用记录。"), "", nullptr));
    }
    for (size_t i = 0; i < st_.session_calls.size(); ++i) {
      H row = el("div", "call-row");
      row.add(el("span", "call-name").text(st_.session_calls[i].name));
      row.add(el("span", "call-args dim").text(st_.session_calls[i].args));
      body.add(row);
    }
  } else {
    body.add(comp::empty_state(tr("面板内容"), "", nullptr));
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
    // Declared here, and the token changes with the OPEN state so the framework
    // replays it on the existing node - a keyed node is patched, not recreated,
    // so the class alone would only ever animate the first time.
    drawer.anim(anim::kDrawer).anim_key("open");
  } else {
    drawer.anim(anim::kDrawer).anim_key("closed");
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
    // With enough terminals to get lost in, the overview grows a live filter
    // (see build_terminal_search) - this is the page you come to in order to
    // find one.
    if (st_.terminals.size() >= kTermSearchThreshold) {
      list.add(build_terminal_search());
    }
    std::vector<const TermItem*> shown = filtered_terminals();
    if (!st_.term_filter.empty() && shown.empty()) {
      list.add(comp::empty_state(tr("无匹配终端"), "", nullptr));
    }
    for (size_t i = 0; i < shown.size(); ++i) {
      const TermItem& t = *shown[i];
      std::string tid = t.tid;
      bool closed = term_tab_closed(tid);
      H item = el("div", closed ? "td-all-item closed" : "td-all-item");
      // Clicking a card opens it: for a closed terminal that means bringing its
      // tab back, so the overview IS the way to reopen one (no separate entry).
      item.on(EventKind::kClick, [this, tid, closed](const std::string&) {
        if (closed) reopen_terminal_tab(tid);
        update([&](AppState& s) { s.term_tab = tid; });
        term_focus_pending_ = true;
        load_terminal_grid(tid);
      });
      H ih = el("div", "td-all-head");
      ih.add(el("span", t.status == "running" ? "term-dot running" : "term-dot")
                 .text("●"));
      ih.add(el("span", "td-all-name").text("#" + tid + " " + term_display_name(t)));
      ih.add(el("span", "grow"));
      // three states, and the tab one matters: a closed terminal keeps running,
      // so "running" alone would not tell you why it has no tab
      std::string state = closed ? tr("标签已关闭")
                                 : (t.status == "running" ? tr("运行中") : tr("已退出"));
      ih.add(el("span", "dim").text(state));
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
    // The terminal surface: focusable, gets raw keys, draws the cell grid, and
    // SCROLLS - a terminal you cannot scroll back through cannot show you what
    // just went past, which is most of what you scroll for. The grid reply
    // includes history above the live screen (backend side), so this is a plain
    // scroll container over history + screen.
    H screen = el("div", "term-screen");
    screen.key("term-screen");   // stable key: the key binding must survive
    screen.attr("tabindex", "0");
    // The rows are keyed by terminal: switching terminals then swaps the subtree
    // (recreating it, so the entry animation fires) instead of patching text
    // into rows that happen to have the same position. That was why switching to
    // a terminal with MORE lines animated and switching to one with fewer did
    // not - the rows were reused, so nothing was ever created.
    H lines_box = el("div", "ts-lines");
    lines_box.key("term-lines:" + st_.term_tab);
    lines_box.anim(anim::kEnter);
    const Json* grid = nullptr;
    std::map<std::string, Json>::const_iterator gi = st_.term_grids.find(st_.term_tab);
    if (gi != st_.term_grids.end()) grid = &gi->second;
    if (grid && grid->is_object()) {
      const Json& lines = (*grid)["lines"];
      bool cursor_visible = jsonutil::get<bool>(*grid, "cursorVisible", false);
      int cx = static_cast<int>(jsonutil::get_int(*grid, "cursorX", 0));
      int cy = static_cast<int>(jsonutil::get_int(*grid, "cursorY", 0));
      int from = static_cast<int>(jsonutil::get_int(*grid, "from", 0));
      int scrollback = static_cast<int>(jsonutil::get_int(*grid, "scrollback", 0));
      // the cursor's line in the combined buffer, then its index in `lines`
      int cursor_line = scrollback + cy - from;
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
          if (cursor_visible && static_cast<int>(y) == cursor_line) {
            H cur = el("div", "ts-cursor");
            cur.style("left", itos(cx) + "ch");
            row.add(cur);
          }
          lines_box.add(row);
        }
      }
    } else {
      lines_box.add(el("div", "ts-empty").text(tr("（暂无输出）")));
    }
    screen.add(lines_box);
    left.add(screen);
  }
  body.add(left);

  // ---- right: vertical tab strip ----
  // Every entry is KEYED. Unkeyed list items are matched positionally by the
  // diff, so removing one tab made its DOM node get reused for the next item -
  // and the click that was still propagating then landed on a DIFFERENT
  // element's handler. That is what made closing the last terminal instantly
  // spawn a new one (the click reached the 新增 entry after the diff had
  // recycled the node into it).
  H side = el("div", "td-tabs");
  // fixed first entry: aggregate view
  H all_tab = el("div", st_.term_tab == "all" ? "td-tab active" : "td-tab");
  all_tab.key("term-tab:all");
  all_tab.on(EventKind::kClick, [this](const std::string&) {
    update([&](AppState& s) { s.term_tab = "all"; });
  });
  all_tab.add(el("i", "fa fa-terminal"));
  all_tab.add(el("span", "td-tab-name").text(tr("所有终端")));
  side.add(all_tab);
  // The strip lists every terminal that has a TAB, in the user's own order
  // (ordered_terminals). A closed terminal is not a tab - it is a row in the
  // overview list (it keeps running; closing only removes the tab), which is
  // where you find it again. The filter belongs to the overview page: applying
  // it here would hide the tab of a terminal you just created whose name does
  // not match.
  std::vector<const TermItem*> strip = ordered_terminals();
  for (size_t i = 0; i < strip.size(); ++i) {
    const TermItem& t = *strip[i];
    std::string tid = t.tid;
    if (term_tab_closed(tid)) continue;
    if (st_.term_renaming == tid) {
      // renaming in place: the terminal keeps running, only the label changes.
      // Losing focus commits (the same as Enter) - a rename field that keeps
      // eating keystrokes after you click elsewhere is a trap.
      H form = el("div", "td-tab td-tab-rename");
      form.key("term-tab:" + tid);
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
    tb.key("term-tab:" + tid);
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
  // nothing to fill in. Keyed like the rest of the strip (see above).
  H add = el("div", "td-tab td-tab-add");
  add.key("term-tab:add");
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
    {"通用", "browser", "浏览器控制"},
    {"通用", "computer", "电脑控制"},
    {"模型", "providers", "供应商"},
    {"模型", "session_defaults", "会话默认"},
    {"模型", "model_prefs", "模型偏好"},
    {"模型", "subagents", "子代理"},
    // MCP 与 Skill 分开：两者都是"可能有大量条目"的列表，各自的探测/导入/
    // 新建流程也不一样，塞在一页里只会让两边都挤。
    {"模型", "mcp", "MCP"},
    {"模型", "skills", "Skill"},
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
  apply_saved_tab_order();
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
  // ---- appearance ----
  // ui.theme is "<palette>" or "<palette>:<mode>" is NOT used: palette and mode
  // are separate concerns (which hues vs light/dark), so they are separate keys.
  // ui.theme keeps its historical meaning (the palette name) and ui.mode is new.
  {
    std::string theme = sv_str(st_.settings_values, "ui.theme", "default");
    if (theme == "light" || theme == "dark") {
      // legacy values from the first pass, where the theme WAS the mode
      st_.ui_mode = theme;
      st_.ui_theme = "default";
    } else {
      st_.ui_theme = theme.empty() ? "default" : theme;
      st_.ui_mode = sv_str(st_.settings_values, "ui.mode", "auto");
    }    st_.ui_font = sv_str(st_.settings_values, "ui.font.family", "");
    st_.ui_mono = sv_str(st_.settings_values, "ui.font.monoFamily", "");
    st_.ui_font_size = sv_int(st_.settings_values, "ui.font.size", 0);
    apply_appearance();
  }
}

// Hand the appearance to the DOM adapter, which owns the CSS variables. Kept
// separate from apply_settings so a change can re-theme without re-deriving
// every cached list.
void UiApp::apply_appearance() {
  IDom::Appearance a;
  a.palette = st_.ui_theme;
  a.mode = st_.ui_mode;
  a.font = st_.ui_font;
  a.mono = st_.ui_mono;
  a.font_size = st_.ui_font_size;
  a.code_highlight = sv_bool(st_.settings_values, "ui.codeHighlight", true);
  a.tabs = sv_str(st_.settings_values, "ui.tabs", "top");
  dom_.set_appearance(a);
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
  if (section == "mcp" && !st_.mcp_loaded) {
    load_mcp_tools();
  }
  if (section == "terminal" && st_.shell_rows.empty()) {
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
  // Same in-flight guard as load_mcp_tools: this is called from a build.
  if (skills_inflight_) return;
  skills_inflight_ = true;
  call_async("skill.list", Json::object(), [this](const BackendResult& r) {
    skills_inflight_ = false;
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
            row.dir = jsonutil::get<std::string>(s, "dir", "");
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
  // 设置不是标签页，所以它需要一个明确的关闭出口（没有标签可点 ×）。
  H head = el("div", "settings-head");
  head.add(el("div", "settings-head-title").text(tr("设置")));
  head.add(el("span", "grow"));
  head.add(comp::icon_button("\u2715", tr("关闭设置"), [this](const std::string&) {
    close_modal_page();
  }));
  wrap.add(head);
  H cols = el("div", "settings-cols");
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
  cols.add(nav);

  H body = el("div", "settings-body");
  const std::string& sec = st_.settings_nav;
  // The body is declared as an entry-animated panel keyed by SECTION, so
  // switching sections replays the animation (the node would otherwise be
  // patched in place and never animate). This is the per-page animation the
  // settings pages were missing.
  body.anim(anim::kEnter).anim_key("sec:" + sec);
  if (backend_) load_settings_section(sec);

  if (sec == "providers") {
    // --- 供应商：左列表（选择/新建），右侧编辑或模板向导 ---
    H two = el("div", "pv-two");
    H list = el("div", "pv-list");
    H lhead = el("div", "pv-list-head");
    lhead.add(el("span", "pv-list-title").text(tr("供应商")));
    lhead.add(comp::button(tr("新建"), [this](const std::string&) {
      update([&](AppState& s) {
        s.provider_creating = true;
        s.provider_selected.clear();
      });
    }, "primary"));
    list.add(lhead);
    if (st_.providers.empty()) {
      list.add(el("div", "pv-empty").text(tr("还没有供应商。点「新建」选择模板。")));
    }
    for (size_t i = 0; i < st_.providers.size(); ++i) {
      const AppState::ProviderRow& p = st_.providers[i];
      bool sel = (!st_.provider_creating && st_.provider_selected == p.id);
      H it = el("div", sel ? "pv-item active" : "pv-item");
      std::string id = p.id;
      it.on(EventKind::kClick, [this, id](const std::string&) { select_provider(id); });
      H top = el("div", "pv-item-top");
      top.add(el("span", "pv-item-name").text(p.name.empty() ? p.id : p.name));
      if (!p.enabled) top.add(comp::badge(tr("停用")));
      if (!p.has_key) top.add(comp::badge(tr("无密钥")));
      it.add(top);
      it.add(el("div", "pv-item-kind").text(p.kind));
      it.add(el("div", "pv-item-url").text(p.endpoint));
      list.add(it);
    }
    two.add(list);

    H detail = el("div", "pv-detail");

    // The form is shared by "new" and "edit": 格式 is a fixed dropdown, the
    // template only pre-fills endpoint + headers, and 高级 holds headers.
    //
    // Layout: the fields and the model list are two PANELS in a wrapping flex
    // row, so a wide detail puts the models beside the form (using the right
    // half, which used to be empty) and a narrow one drops them below in the
    // same column. No media query is needed for either case.
    auto build_form = [this, &detail](bool creating) {
      H cols = el("div", "pv-detail-cols");
      H form = el("div", "pv-form");
      form.add(comp::section_title(creating ? "新建供应商" : "编辑供应商"));
      form.add(comp::field(tr("名称"), comp::text_input(st_.pv_name, "显示名",
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
      form.add(comp::field(tr("格式"), comp::select(kinds, kind_idx,
                                                [this](const std::string& v) {
        int i = std::atoi(v.c_str());
        const char* ids[4] = {"openai-chat", "openai-response", "anthropic", "custom"};
        st_.pv_kind = ids[i < 0 || i > 3 ? 0 : i];
      })));
      form.add(comp::field("Endpoint", comp::text_input(st_.pv_base_url, "",
                                                        [this](const std::string& v) {
        st_.pv_base_url = v;
      }), "如 https://api.openai.com/v1"));
      form.add(comp::field("API Key", comp::text_input(st_.pv_key,
                                                       creating ? "sk-…" : "留空 = 不修改",
                                                       [this](const std::string& v) {
        st_.pv_key = v;
      }), creating ? "" : "回显为脱敏值；不改动则留空"));

      // ---- 高级（折叠）：自定义请求头 + 自定义 body ----
      H adv = el("div", "pv-advanced");
      H ahead = el("div", "pv-adv-head");
      ahead.on(EventKind::kClick, [this](const std::string&) {
        update([&](AppState& s) { s.pv_advanced = !s.pv_advanced; });
      });
      ahead.add(el("span", "pv-adv-caret").text(st_.pv_advanced ? "\u25be" : "\u25b8"));
      ahead.add(el("span").text(tr("高级（请求头 / 请求体）")));
      adv.add(ahead);
      if (st_.pv_advanced) {
        H abody = el("div", "pv-adv-body");
        // 自定义请求头。opencode 的会话亲和头不再是一个特殊开关：它只是模板
        // 预填的一条普通头（x-opencode-session: {{SESSION}}），后端把
        // {{SESSION}} 换成会话 id。这样 Azure 的 api-key 和 opencode 的亲和头
        // 走的是同一条路径，没有"某种供应商才有的隐藏开关"。
        for (size_t i = 0; i < st_.pv_headers.size(); ++i) {
          H row = el("div", "settings-row");
          row.add(el("span", "sr-name").text(st_.pv_headers[i].first));
          row.add(el("span", "dim grow").text(st_.pv_headers[i].second));
          size_t idx = i;
          row.add(comp::button(tr("删除"), [this, idx](const std::string&) {
            update([&](AppState& s) {
              if (idx < s.pv_headers.size()) s.pv_headers.erase(s.pv_headers.begin() + idx);
            });
          }, "ghost"));
          abody.add(row);
        }
        H addh = el("div", "pv-header-add");
        addh.add(comp::text_input(st_.pv_new_header_k, tr("Header 名"),
                                  [this](const std::string& v) { st_.pv_new_header_k = v; }));
        addh.add(comp::text_input(st_.pv_new_header_v, tr("值"),
                                  [this](const std::string& v) { st_.pv_new_header_v = v; }));
        addh.add(comp::button(tr("添加"), [this](const std::string&) {
          if (st_.pv_new_header_k.empty()) return;
          update([&](AppState& s) {
            s.pv_headers.push_back(std::make_pair(s.pv_new_header_k, s.pv_new_header_v));
            s.pv_new_header_k.clear();
            s.pv_new_header_v.clear();
          });
        }, "ghost"));
        abody.add(addh);
        // 自定义 body：浅合并进默认请求体。有些网关要求供应商特有的字段，
        // 没有这个就只能改代码。
        //
        // 标题与输入框上下排：这个字段的标签和提示都长，左右排会把输入框挤成
        // 一条细缝（JSON 需要宽度，不是高度）。所以它不用 .field，而是自己
        // 的竖排块。
        H bodyfield = el("div", "pv-body-field");
        bodyfield.add(el("div", "pv-body-label").text(tr("请求体覆盖（JSON）")));
        bodyfield.add(comp::textarea(
            st_.pv_body_text, tr("{\"key\": \"value\"}"), [this](const std::string& v) {
          st_.pv_body_text = v;
        }, nullptr));
        abody.add(bodyfield);
        adv.add(abody);
      }
      form.add(adv);

      if (!st_.provider_status.empty()) {
        form.add(el("div", "pv-status").text(st_.provider_status));
      }
      H acts = el("div", "settings-actions");
      std::string sel = st_.provider_selected;
      acts.add(comp::button(tr("保存"), [this](const std::string&) { save_provider(); }, "primary"));
      if (!creating) {
        acts.add(comp::button(tr("测试连接"), [this, sel](const std::string&) {
          test_provider(sel);
        }));
        acts.add(comp::button(tr("删除"), [this, sel](const std::string&) {
          delete_provider(sel);
        }, "danger"));
      }
      acts.add(comp::button(tr("取消"), [this](const std::string&) {
        update([&](AppState& s) {
          s.provider_creating = false;
          s.pv_template_picked = false;
          s.pv_advanced = false;
          s.pv_name.clear();
          s.pv_kind = "openai-chat";
          s.pv_base_url.clear();
          s.pv_key.clear();
          s.pv_headers.clear();
          s.pv_body_text.clear();
          s.provider_status.clear();
          s.provider_models.clear();
        });
      }, "ghost"));
      form.add(acts);
      cols.add(form);

      // ---- 模型：独立面板（宽屏在右侧，窄屏落到表单下面）----
      // The panel is itself split in two: the scrollable model list on top
      // (enable / test / delete / add, pick one to edit) and that model's
      // advanced settings underneath. Two scroll regions in one panel would be
      // confusing, so only the list scrolls.
      H mpanel = el("div", "pv-models-panel");
      H mhead = el("div", "md-head");
      mhead.add(el("span", "md-title").text(tr("模型")));
      mhead.add(el("span", "grow"));
      if (!sel.empty()) {
        mhead.add(comp::icon_button("\u21bb", tr("获取模型列表"), [this, sel](const std::string&) {
          fetch_provider_models(sel);
        }));
      }
      mhead.add(comp::icon_button("+", tr("新增模型"), [this](const std::string&) {
        begin_new_model();
      }));
      mpanel.add(mhead);
      // Bulk enable/disable: with dozens of models, flipping them one dot at a
      // time is the wrong unit of work.
      if (!creating && !st_.models.empty()) {
        H bulk = el("div", "md-bulk");
        bulk.add(comp::button(tr("全选"), [this](const std::string&) {
          for (size_t i = 0; i < st_.models.size(); ++i) {
            if (!st_.models[i].enabled) toggle_model_enabled(st_.models[i].id, true);
          }
        }, "ghost"));
        bulk.add(comp::button(tr("全不选"), [this](const std::string&) {
          for (size_t i = 0; i < st_.models.size(); ++i) {
            if (st_.models[i].enabled) toggle_model_enabled(st_.models[i].id, false);
          }
        }, "ghost"));
        // Where the list came from matters: preset entries are a guess, not
        // what the endpoint actually serves.
        bool any_api = false, any_preset = false;
        for (size_t i = 0; i < st_.models.size(); ++i) {
          if (st_.models[i].from_api) any_api = true;
          if (st_.models[i].from_preset) any_preset = true;
        }
        bulk.add(el("span", "grow"));
        if (any_api) bulk.add(el("span", "md-src").text(tr("来自 API")));
        else if (any_preset) bulk.add(el("span", "md-src warn").text(tr("内置预置（非 API）")));
        mpanel.add(bulk);
      }

      if (creating) {
        mpanel.add(el("div", "hint").text(tr("保存供应商后可管理模型")));
      } else {
        // search only pays for itself once the list is long
        if (st_.models.size() > 8) {
          mpanel.add(comp::search_box(st_.model_filter, tr("筛选模型"),
                                      [this](const std::string& v) {
            // A filter has to re-render to have any effect (unlike the composer
            // textarea, where suppressing the render is what keeps the caret
            // from jumping). The input is focused, so patch_attrs leaves its
            // value alone and the caret stays put.
            update([&](AppState& s) { s.model_filter = v; });
          }));
        }
        H mlist = el("div", "md-list");
        size_t shown = 0;
        for (size_t i = 0; i < st_.models.size(); ++i) {
          const AppState::ModelRow& m = st_.models[i];
          if (!st_.model_filter.empty() &&
              m.id.find(st_.model_filter) == std::string::npos &&
              m.display.find(st_.model_filter) == std::string::npos) {
            continue;
          }
          ++shown;
          bool is_sel = !st_.model_creating && st_.model_selected == m.id;
          H row = el("div", is_sel ? "md-row active" : "md-row");
          row.key("md-" + m.id);
          std::string mid = m.id;
          row.on(EventKind::kClick, [this, mid](const std::string&) {
            select_model(mid);
          });
          // enable toggle: the model must be switchable without opening it
          H en = el("span", m.enabled ? "md-en on" : "md-en");
          en.attr("title", m.enabled ? tr("已启用（点击停用）") : tr("已停用（点击启用）"));
          en.on(EventKind::kClick, [this, mid, on = m.enabled](const std::string&) {
            toggle_model_enabled(mid, !on);
          });
          row.add(en);
          row.add(comp::model_icon(m.icon, m.id));
          H nm = el("div", "md-name");
          nm.add(el("span", "md-id").text(m.display.empty() ? m.id : m.display));
          // capability badges: only the ones that are set, so the row stays short
          H caps = el("span", "md-caps");
          bool any_cap = false;
          if (m.vision) { caps.add(el("i", "fa fa-eye").attr("title", tr("视觉"))); any_cap = true; }
          if (m.audio) { caps.add(el("i", "fa fa-volume-high").attr("title", tr("音频"))); any_cap = true; }
          if (m.video) { caps.add(el("i", "fa fa-video").attr("title", tr("视频"))); any_cap = true; }
          if (m.pdf) { caps.add(el("i", "fa fa-file-pdf").attr("title", tr("PDF"))); any_cap = true; }
          if (m.structured) { caps.add(el("i", "fa fa-brackets-curly").attr("title", tr("结构化输出"))); any_cap = true; }
          if (!m.thinking_levels.empty()) { caps.add(el("i", "fa fa-brain").attr("title", tr("思考档位"))); any_cap = true; }
          if (any_cap) nm.add(caps);
          row.add(nm);
          row.add(el("span", "grow"));
          if (m.user_set) row.add(el("span", "md-tag").text(tr("自定义")));
          H acts2 = el("span", "md-acts");
          acts2.add(comp::icon_button("\u26a1", tr("测试"), [this, mid](const std::string&) {
            test_model(mid);
          }));
          if (m.user_set) {
            acts2.add(comp::icon_button("\u2715", tr("删除"), [this, mid](const std::string&) {
              delete_model(mid);
            }));
          }
          row.add(acts2);
          mlist.add(row);
        }
        if (st_.models.empty()) {
          mpanel.add(comp::empty_state(tr("还没有模型。点右上「↻」从 API 获取，或「+」手动新增。"),
                                       "", nullptr));
        } else if (shown == 0) {
          mpanel.add(comp::empty_state(tr("没有匹配的模型。"), "", nullptr));
        } else {
          mpanel.add(mlist);
        }
      }

      // ---- 下半部分：选中模型的高级设置 ----
      if (st_.model_creating || !st_.model_selected.empty()) {
        H adv2 = el("div", "md-adv");
        // The replay token is the model id, EXCEPT while creating: there the
        // "id" is whatever is being typed, so keying on it would replay the
        // animation on every keystroke.
        adv2.anim(anim::kEnter).anim_key(
            st_.model_creating ? std::string("md:new") : ("md:" + st_.model_selected));
        AppState::ModelRow& d = st_.model_draft;
        adv2.add(el("div", "md-adv-title").text(
            st_.model_creating ? tr("新增模型") : tr("模型设置")));
        if (st_.model_creating) {
          adv2.add(comp::field(tr("模型名"), comp::text_input(
              st_.model_new_id, "如 gpt-4o-mini",
              [this](const std::string& v) {
          // must re-render: the thinking-level preview below is derived from
          // this name (the focused value is not patched back, so no caret jump)
          update([&](AppState& s) { s.model_new_id = v; });
        }), tr("发给 API 的 model 字段")));
        }
        adv2.add(comp::field(tr("显示名"), comp::text_input(
            d.display, tr("缺省 = 模型名"),
            [this](const std::string& v) { st_.model_draft.display = v; })));
        adv2.add(comp::field(tr("启用"), comp::toggle(d.enabled, "",
            [this](const std::string&) {
          update([&](AppState& s) { s.model_draft.enabled = !s.model_draft.enabled; });
        }), tr("停用后不出现在会话的模型选择器里")));

        adv2.add(comp::section_title(tr("能力")));
        // Two per row: five full-width toggle rows made this block needlessly
        // tall, and the panel is already the taller of the two columns.
        // 工具调用 is NOT here: every model this app can use supports it, so a
        // per-model switch for it was a decision nobody makes.
        H caps_grid = el("div", "md-cap-grid");
        auto cap = [&](const char* label, bool value, const char* icon,
                       std::function<void()> flip) {
          H c = el("div", "md-cap");
          c.add(el("i", "fa fa-" + std::string(icon)));
          c.add(el("span", "md-cap-name").text(tr(label)));
          c.add(comp::toggle(value, "", [flip](const std::string&) { flip(); }));
          caps_grid.add(c);
        };
        cap("视觉", d.vision, "eye", [this]() {
          update([&](AppState& s) { s.model_draft.vision = !s.model_draft.vision; });
        });
        cap("音频", d.audio, "volume-high", [this]() {
          update([&](AppState& s) { s.model_draft.audio = !s.model_draft.audio; });
        });
        cap("视频", d.video, "video", [this]() {
          update([&](AppState& s) { s.model_draft.video = !s.model_draft.video; });
        });
        cap("PDF", d.pdf, "file-pdf", [this]() {
          update([&](AppState& s) { s.model_draft.pdf = !s.model_draft.pdf; });
        });
        cap("结构化输出", d.structured, "brackets-curly", [this]() {
          update([&](AppState& s) { s.model_draft.structured = !s.model_draft.structured; });
        });
        adv2.add(caps_grid);

        adv2.add(comp::section_title(tr("长度")));
        // 上下文长度必填：0/空会让压缩与水位都算不出来（除以零或永远 0%），
        // 所以这里要求一个数，保存时再校验一次。
        adv2.add(comp::field(tr("上下文长度"), comp::text_input(
            d.context_len > 0 ? itos(static_cast<int>(d.context_len)) : std::string(),
            tr("必填，如 128000"), [this](const std::string& v) {
          st_.model_draft.context_len = static_cast<long long>(std::atoi(v.c_str()));
        }), tr("必填：上下文窗口大小（token）")));
        adv2.add(comp::field(tr("最大输出"), comp::text_input(
            d.max_output > 0 ? itos(static_cast<int>(d.max_output)) : std::string(),
            "如 8192", [this](const std::string& v) {
          st_.model_draft.max_output = static_cast<long long>(std::atoi(v.c_str()));
        })));

        adv2.add(comp::section_title(tr("思考档位")));
        // Levels are a property of the MODEL, not a fixed global enum. Editing
        // them as one comma-separated line matches how people think about it
        // ("off, low, high") and is far less clicking than add-one-at-a-time.
        // The protocol values stay as typed; the Chinese names are only a
        // display translation used by the dropdowns elsewhere.
        std::vector<std::string> shown_levels = d.thinking_levels;
        if (st_.model_creating && shown_levels.empty() && !st_.model_new_id.empty()) {
          shown_levels = thinking_levels_for(st_.model_new_id);
        }
        std::string levels_text;
        for (size_t i = 0; i < shown_levels.size(); ++i) {
          if (!levels_text.empty()) levels_text += ", ";
          levels_text += shown_levels[i];
        }
        if (st_.model_creating) {
          // not saved yet: show what the coarse match will produce
          adv2.add(comp::field(tr("档位"), el("div", "hint").text(
              levels_text.empty() ? tr("该模型名没有匹配到档位")
                                  : (levels_text + tr("（保存后生效）")))));
        } else {
          adv2.add(comp::field(tr("档位"), comp::text_input(
              levels_text, tr("以逗号分隔，如 off, low, medium, high"),
              [this](const std::string& v) {
            // Parse on every keystroke: the text is the source of truth, and a
            // half-typed line ("off, lo") is still a valid list of one level.
            update([&](AppState& s) {
              s.model_draft.thinking_levels.clear();
              size_t i = 0;
              while (i <= v.size()) {
                size_t e = v.find(',', i);
                if (e == std::string::npos) e = v.size();
                std::string part = v.substr(i, e - i);
                size_t a = 0, b = part.size();
                while (a < b && (part[a] == ' ' || part[a] == '\t')) ++a;
                while (b > a && (part[b - 1] == ' ' || part[b - 1] == '\t')) --b;
                part = part.substr(a, b - a);
                if (!part.empty()) s.model_draft.thinking_levels.push_back(part);
                if (e == v.size()) break;
                i = e + 1;
              }
              // keep the default valid: if it vanished from the list, fall back
              // to the first level so saving cannot write a dangling value
              bool still = false;
              for (size_t k = 0; k < s.model_draft.thinking_levels.size(); ++k) {
                if (s.model_draft.thinking_levels[k] == s.model_draft.thinking_default) {
                  still = true;
                }
              }
              if (!still) {
                s.model_draft.thinking_default = s.model_draft.thinking_levels.empty()
                                                     ? std::string()
                                                     : s.model_draft.thinking_levels[0];
              }
            });
          }), levels_text.empty() ? tr("留空 = 不可配思考档") : ""));
          // Default level: a dropdown over the levels that actually exist, with
          // the Chinese preset names (off/low/high/xhigh/max ...).
          if (!d.thinking_levels.empty()) {
            std::vector<std::string> opts;
            int sel = 0;
            for (size_t i = 0; i < d.thinking_levels.size(); ++i) {
              opts.push_back(thinking_label(d.thinking_levels[i]));
              if (d.thinking_levels[i] == d.thinking_default) sel = static_cast<int>(i);
            }
            adv2.add(comp::field(tr("默认档位"), comp::select(
                opts, sel, [this](const std::string& v) {
              int idx = std::atoi(v.c_str());
              update([&](AppState& s) {
                if (idx >= 0 && idx < static_cast<int>(s.model_draft.thinking_levels.size())) {
                  s.model_draft.thinking_default = s.model_draft.thinking_levels[idx];
                }
              });
            })));
          }
        }

        adv2.add(comp::section_title(tr("失败重试")));
        adv2.add(comp::field(tr("重试次数"), comp::text_input(
            d.retry_max > 0 ? itos(d.retry_max) : std::string(), "0 = 用全局默认",
            [this](const std::string& v) {
          st_.model_draft.retry_max = std::atoi(v.c_str());
        })));
        adv2.add(comp::field(tr("退避毫秒"), comp::text_input(
            d.retry_backoff_ms > 0 ? itos(d.retry_backoff_ms) : std::string(), "如 500",
            [this](const std::string& v) {
          st_.model_draft.retry_backoff_ms = std::atoi(v.c_str());
        }), tr("首次退避，之后指数增长")));

        if (!st_.model_status.empty()) {
          adv2.add(el("div", "pv-status").text(st_.model_status));
        }
        H macts2 = el("div", "settings-actions");
        macts2.add(comp::button(tr("保存"), [this](const std::string&) { save_model(); },
                                "primary"));
        if (!st_.model_creating) {
          std::string mid = st_.model_selected;
          macts2.add(comp::button(tr("测试"), [this, mid](const std::string&) {
            test_model(mid);
          }));
        }
        macts2.add(comp::button(tr("取消"), [this](const std::string&) {
          update([&](AppState& s) {
            s.model_creating = false;
            s.model_selected.clear();
            s.model_draft = AppState::ModelRow();
            s.model_status.clear();
          });
        }, "ghost"));
        adv2.add(macts2);
        mpanel.add(adv2);
      }
      cols.add(mpanel);

      detail.add(cols);
    };

    if (st_.provider_creating && !st_.pv_template_picked) {
      // step 1: pick a template (it only pre-fills endpoint + headers). The grid
      // is auto-fill, so a wider window shows more templates per row instead of
      // stretching two of them across the whole page.
      detail.add(comp::section_title(tr("新建供应商")));
      detail.add(comp::hint(tr("模板只预填 Endpoint 与请求头；格式可随时改")));
      H tpls = el("div", "pv-templates");
      for (size_t i = 0; i < st_.provider_templates.size(); ++i) {
        const AppState::ProviderTemplate& t = st_.provider_templates[i];
        H c = el("div", "pv-tpl");
        std::string tid = t.id;
        c.on(EventKind::kClick, [this, tid](const std::string&) {
          begin_new_provider(tid);
        });
        // A brand mark from the local Lobe Icons set: the template id is the
        // brand key (openai/anthropic/gemini/...), so a grid of templates is
        // scannable by logo instead of by reading every name. Unknown ids fall
        // back to the first letter.
        H mark = el("span", "pv-tpl-mark");
        std::string brand = provider_brand_for_template(t.id);
        if (!brand.empty()) {
          mark = el("span", "pv-tpl-mark brand");
          mark.add(comp::model_icon(brand, t.name, "mi-lg"));
        } else {
          mark.text(t.name.empty() ? "?" : t.name.substr(0, 1));
        }
        c.add(mark);
        H body = el("div", "pv-tpl-body");
        body.add(el("div", "pv-tpl-name").text(t.name));
        body.add(el("div", "pv-tpl-desc").text(t.description));
        body.add(el("div", "pv-tpl-url").text(t.base_url));
        c.add(body);
        tpls.add(c);
      }
      detail.add(tpls);
    } else if (st_.provider_creating) {
      // step 2: the form (template already applied)
      H back = el("div", "pv-back");
      back.add(comp::button(tr("\u2190 重新选择模板"), [this](const std::string&) {
        update([&](AppState& s) { s.pv_template_picked = false; });
      }, "ghost"));
      detail.add(back);
      build_form(true);
    } else if (st_.provider_selected.empty()) {
      detail.add(comp::empty_state(tr("从左侧选择一个供应商，或点「新建」"), "", nullptr));
    } else {
      build_form(false);
    }
    two.add(detail);
    body.add(two);
  } else if (sec == "subagents") {
    // --- 子代理 ---
    body.add(comp::section_title(tr("子代理")));
    for (size_t i = 0; i < st_.preset_agents.size(); ++i) {
      const AppState::PresetRow& a = st_.preset_agents[i];
      H row = el("div", "settings-row");
      row.add(el("span", "sr-name").text(a.name));
      row.add(el("span", "dim grow").text(a.description));
      if (a.read_only) row.add(comp::badge(tr("只读")));
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
      // Which model this agent uses. Same picker component as everywhere else,
      // plus "inherit the session's model" as an explicit choice - a text field
      // here let a typo look like a valid model name.
      std::string cur = sv_str(st_.settings_values, "subagents." + name + ".model", "");
      std::string owner = "subagent_" + name;
      row.add(build_model_field(owner, "subagents." + name + ".model", cur,
                                tr("继承会话")));
      body.add(row);
    }
    body.add(comp::section_title(tr("上限")));
    H par_row = el("div", "stepper");
    par_row.add(comp::icon_button("\u2212", tr("减少"), [this](const std::string&) {
      int n = sv_int(st_.settings_values, "subagents.maxParallel", 4);
      set_setting("subagents.maxParallel", n > 1 ? n - 1 : 1);
    }));
    par_row.add(el("span", "stepper-value").text(
        itos(sv_int(st_.settings_values, "subagents.maxParallel", 4)) + tr(" 个")));
    par_row.add(comp::icon_button("+", tr("增加"), [this](const std::string&) {
      int n = sv_int(st_.settings_values, "subagents.maxParallel", 4);
      set_setting("subagents.maxParallel", n < 16 ? n + 1 : 16);
    }));
    body.add(comp::field(tr("并行上限"), par_row));
    H dep_row = el("div", "stepper");
    dep_row.add(comp::icon_button("\u2212", tr("减少"), [this](const std::string&) {
      int n = sv_int(st_.settings_values, "subagents.maxDepth", 3);
      set_setting("subagents.maxDepth", n > 1 ? n - 1 : 1);
    }));
    dep_row.add(el("span", "stepper-value").text(
        itos(sv_int(st_.settings_values, "subagents.maxDepth", 3)) + tr(" 层")));
    dep_row.add(comp::icon_button("+", tr("增加"), [this](const std::string&) {
      int n = sv_int(st_.settings_values, "subagents.maxDepth", 3);
      set_setting("subagents.maxDepth", n < 8 ? n + 1 : 8);
    }));
    body.add(comp::field(tr("嵌套深度"), dep_row));
} else if (sec == "model_prefs") {
    // --- 模型偏好 ---
    // NOTE: 子代理's own model lives on the 子代理 page (each agent picks its
    // own); a second, global "subagent default" here just duplicated it.
    body.add(comp::section_title(tr("语言")));
    // 模型输出语言：默认"不指定"，让模型自己决定。以前是"跟随系统"，但那等于
    // 把界面语言当成模型指令，用户并没有这么要求过。
    body.add(comp::field(tr("输出语言"), comp::select(
        std::vector<std::string>{tr("不指定"), "中文", "English"},
        sv_str(st_.settings_values, "model.outputLang", "") == "zh" ? 1
            : sv_str(st_.settings_values, "model.outputLang", "") == "en" ? 2 : 0,
        [this](const std::string& v) {
      int idx = std::atoi(v.c_str());
      const char* langs[3] = {"", "zh", "en"};
      set_setting("model.outputLang", langs[idx < 0 || idx > 2 ? 0 : idx]);
    }), tr("不指定 = 由模型自行决定")));
    body.add(comp::field(tr("思考语言"), comp::select(
        std::vector<std::string>{tr("不指定"), "中文", "English"},
        st_.thinking_lang == "zh" ? 1 : st_.thinking_lang == "en" ? 2 : 0,
        [this](const std::string& v) {
      int idx = std::atoi(v.c_str());
      const char* langs[3] = {"", "zh", "en"};
      std::string l = langs[idx < 0 || idx > 2 ? 0 : idx];
      update([&](AppState& s) { s.thinking_lang = l; });
      set_setting("model.thinkingLang", l);
    }), tr("影响工具/skill 说明的双语选择")));

    body.add(comp::section_title(tr("压缩")));
    // 自动压缩阈值：会话视图长度达到窗口的 N% 就开始压缩。给档位而不是让用户
    // 填一个百分比，因为"多少算满"本来就是个经验值。
    body.add(comp::field(tr("自动压缩阈值"), comp::select(
        std::vector<std::string>{"60%", "70%", "80%", "90%", tr("关闭")},
        [&]() {
          int n = sv_int(st_.settings_values, "history.compressPct", 80);
          return n <= 0 ? 4 : n <= 60 ? 0 : n <= 70 ? 1 : n <= 80 ? 2 : 3;
        }(),
        [this](const std::string& v) {
      int idx = std::atoi(v.c_str());
      const int vals[5] = {60, 70, 80, 90, 0};
      set_setting("history.compressPct", vals[idx < 0 || idx > 4 ? 2 : idx]);
    }), tr("上下文占用达到该比例时自动压缩较早的轮次")));

    body.add(comp::section_title(tr("摘要")));
    // 摘要默认继承会话模型：只有在需要便宜模型时才有必要单独指定
    body.add(comp::field(tr("摘要模型"),
                         build_model_field("model_pref_summary",
                                           "model.pref.summary.model",
                                           sv_str(st_.settings_values,
                                                  "model.pref.summary.model", ""),
                                           tr("继承会话"))));
  } else if (sec == "mcp") {
    // --- MCP（外部工具包）---
    // MCP 和 Skill 分成两页：两者都可能有很长的列表，探测/导入/新建的流程也不
    // 一样，塞在一页里两边都挤。这一页只管"模型能自主调用的外部工具"。
    H head = el("div", "settings-actions");
    head.add(comp::button(tr("自动探测"), [this](const std::string&) { detect_mcp(); },
                          "primary"));
    head.add(comp::button(tr("手动导入"), [this](const std::string&) {
      call_async("project.pickFolder", Json::object(), [this](const BackendResult& r) {
        if (!r.ok) return;
        std::string path = jsonutil::get<std::string>(r.data, "path", "");
        if (path.empty()) return;
        update([&](AppState& s) { s.mcp_new_dir = path; });
        add_mcp();
      });
    }, "ghost"));
    body.add(head);
    body.add(comp::search_box(st_.mcp_filter, tr("搜索 MCP"),
                              [this](const std::string& v) {
      update([&](AppState& s) { s.mcp_filter = v; });
    }));
    size_t shown = 0;
    H list = el("div", "mcp-list");
    for (size_t i = 0; i < st_.mcp_tools.size(); ++i) {
      const AppState::McpRow& m = st_.mcp_tools[i];
      if (!st_.mcp_filter.empty() &&
          m.name.find(st_.mcp_filter) == std::string::npos &&
          m.dir.find(st_.mcp_filter) == std::string::npos) {
        continue;
      }
      ++shown;
      H card = el("div", m.enabled ? "mcp-card" : "mcp-card off");
      H top = el("div", "mcp-card-top");
      top.add(el("span", "mcp-card-name").text(m.name));
      top.add(el("span", "grow"));
      std::string name = m.name;
      bool en = m.enabled;
      top.add(comp::toggle(en, "", [this, name, en](const std::string&) {
        toggle_mcp(name, !en);
      }));
      card.add(top);
      card.add(el("div", "mcp-card-path").text(m.dir));
      if (!m.description.empty()) {
        card.add(el("div", "mcp-card-desc").text(m.description));
      }
      H acts = el("div", "mcp-card-acts");
      acts.add(comp::button(tr("移除"), [this, name](const std::string&) {
        remove_mcp(name);
      }, "ghost"));
      card.add(acts);
      list.add(card);
    }
    if (st_.mcp_tools.empty()) {
      // 空列表只说"暂无"，不再在下面挂一个新建表单——那看起来像表单才是主体
      body.add(comp::empty_state(
          tr("暂无 MCP。点「自动探测」扫描常见位置，或用「手动导入」选择目录。"),
          "", nullptr));
    } else if (shown == 0) {
      body.add(comp::empty_state(tr("没有匹配的 MCP。"), "", nullptr));
    } else {
      body.add(list);
    }
  } else if (sec == "skills") {
    // --- Skill ---
    // Skills are the ONLY "/命令" the user can type themselves (MCP commands and
    // subagents are model-invoked only), so this page is about what you can call.
    H head = el("div", "settings-actions");
    head.add(comp::button(tr("自动探测"), [this](const std::string&) { detect_mcp(); },
                          "primary"));
    head.add(comp::button(tr("手动导入"), [this](const std::string&) {
      call_async("project.pickFolder", Json::object(), [this](const BackendResult& r) {
        if (!r.ok) return;
        std::string path = jsonutil::get<std::string>(r.data, "path", "");
        if (path.empty()) return;
        update([&](AppState& s) { s.skill_new_dir = path; });
        add_skill();
      });
    }, "ghost"));
    body.add(head);
    body.add(comp::hint(tr("Skill 是唯一可以由你输入 /名称 调用的命令")));
    body.add(comp::search_box(st_.skill_filter, tr("搜索 Skill"),
                              [this](const std::string& v) {
      update([&](AppState& s) { s.skill_filter = v; });
    }));
    size_t shown = 0;
    H list = el("div", "mcp-list");
    for (size_t i = 0; i < st_.skills.size(); ++i) {
      const AppState::SkillRow& sk = st_.skills[i];
      std::string label = sk.title.empty() ? sk.name : sk.title;
      if (!st_.skill_filter.empty() &&
          label.find(st_.skill_filter) == std::string::npos &&
          sk.name.find(st_.skill_filter) == std::string::npos) {
        continue;
      }
      ++shown;
      H card = el("div", "mcp-card");
      H top = el("div", "mcp-card-top");
      top.add(el("span", "mcp-card-name").text(label));
      if (!sk.loadable) top.add(comp::badge(tr("常驻")));
      top.add(el("span", "grow"));
      std::string name = sk.name;
      top.add(comp::icon_button("eye", tr("查看内容"), [this, name](const std::string&) {
        call_async("skill.load", Json{{"name", name}},
                   [this, name](const BackendResult& r) {
          toast(r.ok ? (tr("已加载 skill：") + name) : (tr("加载失败：") + r.message));
        });
      }));
      card.add(top);
      card.add(el("div", "mcp-card-path").text("/" + sk.name +
                                               (sk.dir.empty() ? "" : ("  ·  " + sk.dir))));
      if (!sk.description.empty()) {
        card.add(el("div", "mcp-card-desc").text(sk.description));
      }
      list.add(card);
    }
    if (st_.skills.empty()) {
      body.add(comp::empty_state(
          tr("暂无 Skill。点「自动探测」扫描常见位置，或用「手动导入」选择目录。"),
          "", nullptr));
    } else if (shown == 0) {
      body.add(comp::empty_state(tr("没有匹配的 Skill。"), "", nullptr));
    } else {
      body.add(list);
    }
  } else if (sec == "memory") {
    // --- 长期记忆 ---
    // 这不是一张用户维护的 KV 表：它是**模型自己以 note 形式写下的提醒**，
    // 用户在这里只是复核/删除/手动补一条。所以按"便签"呈现（内容为主），而不是
    // 键值对表格。
    body.add(comp::section_title(tr("长期记忆")));
    body.add(comp::hint(tr("模型在对话中自行记下的提醒；你可以编辑或删除")));
    if (st_.long_memory.empty()) {
      // 空状态不再带一句解释文字：上面那行 hint 已经把这件事说清楚了，
      // 两句"还没有…"叠在一起只是噪音。
      body.add(comp::empty_state(tr(""), "", nullptr));
    }
    for (size_t i = 0; i < st_.long_memory.size(); ++i) {
      H card = el("div", "note-card");
      H top = el("div", "note-top");
      top.add(el("span", "note-title").text(st_.long_memory[i].first));
      top.add(el("span", "grow"));
      std::string k = st_.long_memory[i].first;
      top.add(comp::icon_button("trash", tr("删除"), [this, k](const std::string&) {
        remove_kv("model.longMemory", k);
      }));
      card.add(top);
      card.add(el("div", "note-body").text(st_.long_memory[i].second));
      body.add(card);
    }
    H madd = el("div", "pv-header-add");
    madd.add(comp::text_input(st_.mem_new_key, tr("标题"),
                              [this](const std::string& v) { st_.mem_new_key = v; }));
    madd.add(comp::text_input(st_.mem_new_value, tr("内容"),
                              [this](const std::string& v) { st_.mem_new_value = v; }));
    madd.add(comp::button(tr("记一条"), [this](const std::string&) {
      add_kv("model.longMemory", st_.mem_new_key, st_.mem_new_value);
    }, "ghost"));
    body.add(madd);
  } else if (sec == "appearance") {
    // 外观：调色板 / 深浅 / 语言 / 字体 / 代码高亮。每一项都直接生效（写设置 +
    // 立即 apply_appearance），所以没有"保存"按钮，也不需要解释性文字。
    body.add(comp::section_title(tr("主题")));
    struct PaletteRow {
      const char* id;
      const char* label;
    };
    static const PaletteRow kPalettes[] = {
        {"default", "默认蓝"}, {"slate", "石板灰"},
        {"forest", "森林绿"}, {"plum", "紫罗兰"}, {"amber", "琥珀橙"},
    };
    H swatches = el("div", "palette-row");
    for (size_t i = 0; i < sizeof(kPalettes) / sizeof(kPalettes[0]); ++i) {
      std::string id = kPalettes[i].id;
      bool active = st_.ui_theme == id;
      H sw = el("div", active ? "palette-swatch active" : "palette-swatch");
      sw.on(EventKind::kClick, [this, id](const std::string&) {
        update([&](AppState& s) { s.ui_theme = id; });
        set_setting("ui.theme", id);
        apply_appearance();
      });
      H dot = el("span", "palette-dot palette-preview-" + id);
      sw.add(dot);
      sw.add(el("span", "palette-name").text(tr(kPalettes[i].label)));
      swatches.add(sw);
    }
    body.add(swatches);
    body.add(comp::field(tr("深浅"), comp::select(
        std::vector<std::string>{tr("跟随系统"), tr("浅色"), tr("深色")},
        st_.ui_mode == "light" ? 1 : st_.ui_mode == "dark" ? 2 : 0,
        [this](const std::string& v) {
      int idx = std::atoi(v.c_str());
      const char* modes[3] = {"auto", "light", "dark"};
      std::string m = modes[idx < 0 || idx > 2 ? 0 : idx];
      update([&](AppState& s) { s.ui_mode = m; });
      set_setting("ui.mode", m);
      apply_appearance();
    })));

    body.add(comp::section_title(tr("语言")));
    body.add(comp::field(tr("界面语言"), comp::select(
        std::vector<std::string>{tr("跟随系统"), "中文", "English"},
        st_.ui_lang == "zh" ? 1 : st_.ui_lang == "en" ? 2 : 0,
        [this](const std::string& v) {
      int idx = std::atoi(v.c_str());
      const char* langs[3] = {"auto", "zh", "en"};
      set_setting("ui.language", langs[idx < 0 || idx > 2 ? 0 : idx]);
    }), tr("只影响界面文字，不影响模型输出")));
    // NOTE: 思考语言 lives under 模型偏好 (it is a model instruction, not a UI
    // appearance setting), together with 输出语言.

    body.add(comp::section_title(tr("字体")));
    // Font choices are free text with a datalist of common families: a picker
    // cannot know what the OS has, and typing a family is what a desktop app
    // does anyway. Empty = the built-in stack.
    H ui_font_in = comp::text_input(st_.ui_font, tr("系统默认"),
                                    [this](const std::string& v) {
      st_.ui_font = v;
    }, [this](const std::string&) {
      set_setting("ui.font.family", st_.ui_font);
      apply_appearance();
    });
    ui_font_in.on(EventKind::kBlur, [this](const std::string&) {
      set_setting("ui.font.family", st_.ui_font);
      apply_appearance();
    });
    body.add(comp::field(tr("界面字体"), ui_font_in));
    H mono_in = comp::text_input(st_.ui_mono, tr("系统默认"),
                                 [this](const std::string& v) { st_.ui_mono = v; },
                                 [this](const std::string&) {
      set_setting("ui.font.monoFamily", st_.ui_mono);
      apply_appearance();
    });
    mono_in.on(EventKind::kBlur, [this](const std::string&) {
      set_setting("ui.font.monoFamily", st_.ui_mono);
      apply_appearance();
    });
    body.add(comp::field(tr("等宽字体"), mono_in));
    // A size stepper rather than a free number: the value has a sane range and
    // stepping is how people actually adjust it.
    H size_row = el("div", "stepper");
    size_row.add(comp::icon_button("\u2212", tr("减小"), [this](const std::string&) {
      int n = st_.ui_font_size > 0 ? st_.ui_font_size : 14;
      n = n > 10 ? n - 1 : 10;
      update([&](AppState& s) { s.ui_font_size = n; });
      set_setting("ui.font.size", n);
      apply_appearance();
    }));
    size_row.add(el("span", "stepper-value").text(
        itos(st_.ui_font_size > 0 ? st_.ui_font_size : 14) + " px"));
    size_row.add(comp::icon_button("+", tr("增大"), [this](const std::string&) {
      int n = st_.ui_font_size > 0 ? st_.ui_font_size : 14;
      n = n < 24 ? n + 1 : 24;
      update([&](AppState& s) { s.ui_font_size = n; });
      set_setting("ui.font.size", n);
      apply_appearance();
    }));
    body.add(comp::field(tr("字号"), size_row));

    body.add(comp::section_title(tr("代码")));
    body.add(comp::field(tr("语法高亮"), comp::toggle(
        sv_bool(st_.settings_values, "ui.codeHighlight", true), "",
        [this](const std::string&) {
      bool cur = sv_bool(st_.settings_values, "ui.codeHighlight", true);
      set_setting("ui.codeHighlight", !cur);
      apply_appearance();
    })));
  } else if (sec == "session_defaults") {
    body.add(comp::section_title(tr("会话默认")));
    body.add(comp::hint(tr("只影响新会话；已有会话不受影响")));
    // 模型：用同一套模型选择器组件（和输入框上的那个是同一个），而不是让用户
    // 手打模型名——手打的名字拼错了只有到调用时才知道。
    body.add(comp::field(tr("默认模型"),
                         build_model_field("session_default_model",
                                           "session.default.model",
                                           sv_str(st_.settings_values,
                                                  "session.default.model", ""),
                                           tr("继承全局"))));
    // 思考档位因模型而异，所以这里不写死 off/low/medium/high：档位取自当前
    // 默认模型粗匹配出来的那份表（没设默认模型时给会话模型的档位）。
    {
      std::string model_for_levels = sv_str(st_.settings_values, "session.default.model", "");
      if (model_for_levels.empty()) model_for_levels = st_.model;
      std::vector<std::string> lv = thinking_levels_for(model_for_levels);
      std::vector<std::string> opts;
      opts.push_back(tr("继承模型默认"));
      for (size_t i = 0; i < lv.size(); ++i) opts.push_back(thinking_label(lv[i]));
      std::string cur = sv_str(st_.settings_values, "session.default.thinking", "");
      int sel = 0;
      for (size_t i = 0; i < lv.size(); ++i) {
        if (lv[i] == cur) sel = static_cast<int>(i) + 1;
      }
      H lvl = comp::select(opts, sel, [this, lv](const std::string& v) {
        int idx = std::atoi(v.c_str());
        std::string val = (idx <= 0 || idx > static_cast<int>(lv.size()))
                              ? std::string() : lv[idx - 1];
        set_setting("session.default.thinking", val);
      });
      body.add(comp::field(tr("默认思考档"), lvl,
                           lv.empty() ? tr("该模型没有可配档位") : ""));
    }
    // 文件权限：三档 + "记忆上次"。做成带说明的选择列表而不是下拉框——光看
    // "permissive" 没人知道它到底放行了什么。
    {
      std::vector<comp::ChoiceOption> perms;
      comp::ChoiceOption o;
      o.value = "approve";
      o.icon = "hand";
      o.title = tr("逐次批准");
      o.desc = tr("每次写文件、执行命令都先问一次");
      perms.push_back(o);
      o.value = "permissive";
      o.icon = "folder";
      o.title = tr("宽松");
      o.desc = tr("项目内操作直接放行；系统操作（其他文件夹读写、终端、网络）询问");
      perms.push_back(o);
      o.value = "full";
      o.icon = "bolt";
      o.title = tr("完全放行");
      o.desc = tr("不再询问（仅在你信任当前项目时使用）");
      perms.push_back(o);
      o.value = "remember";
      o.icon = "clock";
      o.title = tr("记忆上次");
      o.desc = tr("新会话沿用上一次会话的选择");
      perms.push_back(o);
      body.add(comp::section_title(tr("文件权限")));
      // The stored value is the LEVEL; "记住上次" is a separate flag. Reading the
      // level back directly meant picking 记忆上次 wrote the literal string
      // "remember" into a permission field and the next render could not match
      // any option, so the choice never showed as selected.
      std::string perm_sel = sv_bool(st_.settings_values,
                                     "session.default.filePermRemember", false)
                                 ? "remember"
                                 : sv_str(st_.settings_values,
                                          "session.default.filePerm", "permissive");
      body.add(comp::choice_list(perms, perm_sel, [this](const std::string& v) {
        if (v == "remember") {
          set_setting("session.default.filePermRemember", true);
          return;
        }
        set_setting("session.default.filePermRemember", false);
        set_setting("session.default.filePerm", v);
      }));
    }
    // 自动模式：同样三态 + 记忆，语义和文件权限一致。
    {
      std::vector<comp::ChoiceOption> modes;
      comp::ChoiceOption o;
      o.value = "auto";
      o.icon = "robot";
      o.title = tr("自动");
      o.desc = tr("移除 ask 工具，高风险操作降级处理");
      modes.push_back(o);
      o.value = "manual";
      o.icon = "hand";
      o.title = tr("手动");
      o.desc = tr("每轮都由你决定下一步");
      modes.push_back(o);
      o.value = "remember";
      o.icon = "clock";
      o.title = tr("记忆上次");
      o.desc = tr("新会话沿用上一次会话的选择");
      modes.push_back(o);
      body.add(comp::section_title(tr("自动模式")));
      // Same split as 文件权限: the mode is one setting, "记住上次" is a flag.
      // Reading the flag as a mode made the selected option impossible to match.
      std::string cur_auto =
          sv_bool(st_.settings_values, "session.default.autoModeRemember", false)
              ? "remember"
              : (sv_bool(st_.settings_values, "session.default.autoMode", false)
                     ? "auto"
                     : "manual");
      body.add(comp::choice_list(modes, cur_auto, [this](const std::string& v) {
        if (v == "remember") {
          set_setting("session.default.autoModeRemember", true);
          return;
        }
        set_setting("session.default.autoModeRemember", false);
        set_setting("session.default.autoMode", v == "auto");
      }));
    }
    // NOTE: "Enter 行为" was removed. It was a setting with no visible effect
    // (the composer always sends on Enter) that also contradicted the 快捷键
    // page, which claims Enter is the send key.
  } else if (sec == "network") {
    body.add(comp::section_title(tr("网络")));
    body.add(comp::field(tr("网络总开关"), comp::toggle(
        sv_bool(st_.settings_values, "permissions.network.enabled", true),
        tr("允许模型发起网络请求（fetch/search）"), [this](const std::string&) {
      bool cur = sv_bool(st_.settings_values, "permissions.network.enabled", true);
      set_setting("permissions.network.enabled", !cur);
    })));
    body.add(comp::field(tr("校验证书"), comp::toggle(
        sv_bool(st_.settings_values, "permissions.network.verifyCert", true), "",
        [this](const std::string&) {
      bool cur = sv_bool(st_.settings_values, "permissions.network.verifyCert", true);
      set_setting("permissions.network.verifyCert", !cur);
    })));
    // 代理：三态。以前只有"主机 + 端口"，于是"跟随系统"和"关闭"这两种最常见
    // 的意图都表达不出来（留空到底是直连还是跟随系统？）。
    body.add(comp::section_title(tr("代理")));
    {
      std::vector<comp::ChoiceOption> px;
      comp::ChoiceOption o;
      o.value = "system";
      o.icon = "gear";
      o.title = tr("跟随系统");
      o.desc = tr("读取系统代理设置");
      px.push_back(o);
      o.value = "custom";
      o.icon = "server";
      o.title = tr("自定义");
      o.desc = tr("手动填写主机与端口");
      px.push_back(o);
      o.value = "off";
      o.icon = "plug";
      o.title = tr("关闭");
      o.desc = tr("直连，不使用代理");
      px.push_back(o);
      std::string mode = sv_str(st_.settings_values, "permissions.network.proxyMode", "system");
      body.add(comp::choice_list(px, mode, [this](const std::string& v) {
        set_setting("permissions.network.proxyMode", v);
      }));
      if (mode == "custom") {
        body.add(comp::field(tr("代理主机"), comp::text_input(
            sv_str(st_.settings_values, "permissions.network.proxyHost", ""), "127.0.0.1",
            [this](const std::string& v) {
          set_setting("permissions.network.proxyHost", v);
        }), "如 127.0.0.1"));
        body.add(comp::field(tr("代理端口"), comp::text_input(
            std::to_string(sv_int(st_.settings_values, "permissions.network.proxyPort", 0)), "",
            [this](const std::string& v) {
          int p = std::atoi(v.c_str());
          set_setting("permissions.network.proxyPort", p > 0 ? p : 0);
        }), "如 7890"));
      }
    }
  } else if (sec == "terminal") {
    body.add(comp::section_title(tr("终端")));
    // 探测到的 shell 列表（"探测"按钮随时重扫；每一项都能直接选中当默认）
    H tacts = el("div", "settings-actions");
    tacts.add(comp::button(tr("重新探测"), [this](const std::string&) {
      load_shells();
    }, "ghost"));
    body.add(tacts);
    if (st_.shell_rows.empty()) {
      body.add(comp::empty_state(tr("没有探测到可用的 shell。"), "", nullptr));
    } else {
      std::vector<comp::ChoiceOption> shells;
      comp::ChoiceOption auto_o;
      auto_o.value = "";
      auto_o.icon = "wand-magic-sparkles";
      auto_o.title = tr("自动选择");
      auto_o.desc = tr("按优先级挑一个可用的");
      shells.push_back(auto_o);
      for (size_t i = 0; i < st_.shell_rows.size(); ++i) {
        comp::ChoiceOption o;
        o.value = st_.shell_rows[i].name;
        o.icon = "terminal";
        o.title = st_.shell_rows[i].name;
        // 显示解析到的路径：两个都叫 pwsh 的安装否则分不出来
        o.desc = st_.shell_rows[i].path;
        shells.push_back(o);
      }
      std::string cur = sv_str(st_.settings_values, "terminal.shell", "");
      body.add(comp::choice_list(shells, cur, [this](const std::string& v) {
        set_setting("terminal.shell", v);
        call_async("settings.setShell", Json{{"name", v}}, [this](const BackendResult& r) {
          toast(r.ok ? tr("已设置 shell") : (tr("设置失败：") + r.message));
        });
      }));
    }
    body.add(comp::field(tr("编码"), comp::select(
        std::vector<std::string>{"系统默认", "UTF-8", "GBK"}, 0,
        [this](const std::string& v) {
      int idx = std::atoi(v.c_str());
      const char* enc[3] = {"", "UTF-8", "GBK"};
      set_setting("terminal.encoding", enc[idx < 0 || idx > 2 ? 0 : idx]);
    }), "终端 spawn 时使用"));
  } else if (sec == "files") {
    body.add(comp::section_title(tr("文件")));
    body.add(comp::field(tr("自动 git init"), comp::toggle(
        sv_bool(st_.settings_values, "permissions.files.autoGitInit", false),
        "非 git 项目打开时自动 git init（默认关，permissions.md §3）",
        [this](const std::string&) {
      bool cur = sv_bool(st_.settings_values, "permissions.files.autoGitInit", false);
      set_setting("permissions.files.autoGitInit", !cur);
    })));
    body.add(comp::section_title(tr("黑名单（拒绝访问）")));
    body.add(comp::hint(tr("优先级：黑名单 > 白名单 > 默认档；一行一个路径前缀")));
    body.add(comp::textarea(st_.file_blacklist_text, "每行一个路径…",
                            [this](const std::string& v) {
      st_.file_blacklist_text = v;  // 提交时拆分成数组
    }, nullptr));
    body.add(comp::section_title(tr("白名单（放行）")));
    body.add(comp::textarea(st_.file_whitelist_text, "每行一个路径…",
                            [this](const std::string& v) {
      st_.file_whitelist_text = v;
    }, nullptr));
    H fsave = el("div", "settings-actions");
    fsave.add(comp::button(tr("保存名单"), [this](const std::string&) {
      Json blist = split_lines(st_.file_blacklist_text);
      Json wlist = split_lines(st_.file_whitelist_text);
      call_async("settings.set", Json{{"key", "permissions.files.blacklist"}, {"value", blist}});
      call_async("settings.set", Json{{"key", "permissions.files.whitelist"}, {"value", wlist}},
                 [this](const BackendResult& r) { toast(r.ok ? "已保存名单" : "保存失败"); });
    }, "primary"));
    body.add(fsave);
  } else if (sec == "environment") {
    // --- 环境 ---
    // 只有一张表：CLI 工具路径。原来还有一张"环境变量（PATH 追加）"，但它和
    // CLI 工具路径表达的是同一件事（把某个目录/可执行文件交给 agent），两张表
    // 让用户不知道该填哪张。
    H eacts = el("div", "settings-actions");
    eacts.add(comp::button(tr("自动探测"), [this](const std::string&) {
      detect_cli_tools();
    }, "primary"));
    body.add(eacts);
    if (st_.cli_tools.empty()) {
      body.add(comp::empty_state(
          tr("还没有条目。点「自动探测」扫描 PATH 上的常见工具，或手动添加。"),
          "", nullptr));
    }
    for (size_t i = 0; i < st_.cli_tools.size(); ++i) {
      H row = el("div", "settings-row");
      row.add(el("span", "sr-name").text(st_.cli_tools[i].first));
      row.add(el("span", "dim grow").text(st_.cli_tools[i].second));
      std::string k = st_.cli_tools[i].first;
      // 路径可以直接改成另一个文件：给一个"选文件"按钮，省得手打路径
      row.add(comp::icon_button("folder-open", tr("选择文件"), [this, k](const std::string&) {
        pick_cli_tool_path(k);
      }));
      row.add(comp::button(tr("删除"), [this, k](const std::string&) {
        remove_kv("environment.cliTools", k);
      }, "ghost"));
      body.add(row);
    }
    H cadd = el("div", "pv-header-add");
    cadd.add(comp::text_input(st_.cli_new_key, tr("名称"),
                              [this](const std::string& v) { st_.cli_new_key = v; }));
    cadd.add(comp::text_input(st_.cli_new_value, tr("路径"),
                              [this](const std::string& v) { st_.cli_new_value = v; }));
    cadd.add(comp::button(tr("浏览…"), [this](const std::string&) {
      pick_cli_tool_draft();
    }, "ghost"));
    cadd.add(comp::button(tr("添加"), [this](const std::string&) {
      add_kv("environment.cliTools", st_.cli_new_key, st_.cli_new_value);
    }, "ghost"));
    body.add(cadd);
  } else if (sec == "shortcuts") {
    body.add(comp::section_title(tr("快捷键")));
    body.add(comp::hint(tr("桌面端接入后生效")));
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
    body.add(comp::section_title(tr("浏览器控制")));
    body.add(comp::field(tr("启用浏览器控制"), comp::toggle(
        sv_bool(st_.settings_values, "browser.enabled", true), "",
        [this](const std::string&) {
      bool cur = sv_bool(st_.settings_values, "browser.enabled", true);
      set_setting("browser.enabled", !cur);
    })));
    // 驱动路径：给一个"选文件"按钮。手打一个可执行文件路径基本不可能打对，
    // 而且这里要的是文件，用目录选择框也选不中。
    {
      H drv = el("div", "model-field");
      drv.add(comp::text_input(sv_str(st_.settings_values, "browser.driver", ""),
                               tr("自动（内置驱动）"),
                               [this](const std::string& v) {
        set_setting("browser.driver", v);
      }));
      drv.add(comp::icon_button("folder-open", tr("选择文件"), [this](const std::string&) {
        pick_file_into("browser.driver", tr("可执行文件"),
                       "*.exe;*.cmd;*.bat;*.ps1;*");
      }));
      body.add(comp::field(tr("驱动路径"), drv, tr("空 = 自动探测内置驱动")));
    }
    body.add(comp::field(tr("允许联网"), comp::toggle(
        sv_bool(st_.settings_values, "browser.permissions.net", true), "",
        [this](const std::string&) {
      bool cur = sv_bool(st_.settings_values, "browser.permissions.net", true);
      set_setting("browser.permissions.net", !cur);
    })));
    body.add(comp::field(tr("允许操作证书"), comp::toggle(
        sv_bool(st_.settings_values, "browser.permissions.certs", false), "",
        [this](const std::string&) {
      bool cur = sv_bool(st_.settings_values, "browser.permissions.certs", false);
      set_setting("browser.permissions.certs", !cur);
    })));
    // NOTE: "加载 skill 说明" was removed. Loading a skill is something the MODEL
    // decides when it needs it; a button that pushes a skill into context is not
    // a user-facing action.
  } else if (sec == "computer") {
    body.add(comp::section_title(tr("电脑控制")));
    body.add(comp::field(tr("启用电脑控制"), comp::toggle(
        sv_bool(st_.settings_values, "computer.enabled", false), "",
        [this](const std::string&) {
      bool cur = sv_bool(st_.settings_values, "computer.enabled", false);
      set_setting("computer.enabled", !cur);
    })));
    body.add(comp::field(tr("允许截图"), comp::toggle(
        sv_bool(st_.settings_values, "computer.screenshot", false), "",
        [this](const std::string&) {
      bool cur = sv_bool(st_.settings_values, "computer.screenshot", false);
      set_setting("computer.screenshot", !cur);
    })));
  } else if (sec == "about") {
    body.add(comp::section_title(tr("关于")));
    body.add(comp::key_value(tr("版本"), st_.about_version));
    body.add(comp::key_value(tr("构建"), "C++14 · xmake · wasm(emcc)"));
    body.add(comp::key_value(tr("界面"), "FlashSight UI (C++ VNode)"));
  } else {
    // --- 常规：界面显示 ---
    // Per rules/goal.md, 通用·常规 is "界面显示等" - UI behaviour. The context
    // window / compression knobs are agent behaviour and belong to the model
    // section, not here.
    body.add(comp::section_title(tr("界面显示")));
    // Tab placement: a real layout switch (the strip becomes a vertical side
    // strip), applied as a data attribute so CSS owns the layout.
    body.add(comp::field(tr("标签位置"), comp::select(
        std::vector<std::string>{tr("顶部"), tr("左侧")},
        sv_str(st_.settings_values, "ui.tabs", "top") == "left" ? 1 : 0,
        [this](const std::string& v) {
      set_setting("ui.tabs", std::atoi(v.c_str()) == 1 ? "left" : "top");
      apply_appearance();  // a layout switch has to take effect immediately
    })));
    body.add(comp::field(tr("会话名提示"), comp::toggle(
        sv_bool(st_.settings_values, "ui.tabTooltip", true), "",
        [this](const std::string&) {
      bool cur = sv_bool(st_.settings_values, "ui.tabTooltip", true);
      set_setting("ui.tabTooltip", !cur);
    })));
    // NOTE: 会话标签上限 was removed on purpose. The strip scrolls, and 主页 is
    // always there, so a cap only hid tabs the user had open.

    body.add(comp::section_title(tr("会话行为")));
    body.add(comp::field(tr("过拟合检测"), comp::toggle(
        sv_bool(st_.settings_values, "session.overfitEnabled", false), "",
        [this](const std::string&) {
      bool cur = sv_bool(st_.settings_values, "session.overfitEnabled", false);
      set_setting("session.overfitEnabled", !cur);
    }), tr("连续多轮输出高度相似时提示，避免模型原地打转")));

    body.add(comp::section_title(tr("数据保留")));
    H days_row = el("div", "stepper");
    days_row.add(comp::icon_button("\u2212", tr("减少"), [this](const std::string&) {
      int n = sv_int(st_.settings_values, "usage.retainDays", 90);
      n = n > 7 ? n - 7 : 7;
      set_setting("usage.retainDays", n);
    }));
    days_row.add(el("span", "stepper-value").text(
        itos(sv_int(st_.settings_values, "usage.retainDays", 90)) + tr(" 天")));
    days_row.add(comp::icon_button("+", tr("增加"), [this](const std::string&) {
      int n = sv_int(st_.settings_values, "usage.retainDays", 90);
      n = n < 3650 ? n + 7 : 3650;
      set_setting("usage.retainDays", n);
    }));
    body.add(comp::field(tr("用量记录保留"), days_row));
  }
  cols.add(body);
  wrap.add(cols);
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
  // Generic confirm modal (destructive actions). Its own branch so a session
  // delete and a permission prompt can never be confused for each other.
  if (!st_.confirm_kind.empty()) {
    std::vector<H> body;
    body.push_back(el("div").text(st_.confirm_text));
    std::string kind = st_.confirm_kind;
    std::string id = st_.confirm_id;
    layer.add(comp::modal(tr("请确认"), body, tr("删除"),
                          [this, kind, id](const std::string&) {
      update([&](AppState& s) {
        s.confirm_kind.clear();
        s.confirm_id.clear();
        s.confirm_text.clear();
      });
      if (kind == "session") delete_session(id);
    },
                          [this](const std::string&) {
      update([&](AppState& s) {
        s.confirm_kind.clear();
        s.confirm_id.clear();
        s.confirm_text.clear();
      });
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
  // A message may be attachments only (dropping a file and hitting send).
  if (st_.session_input.empty() && st_.attachments.empty()) return;
  std::string text = st_.session_input;
  // Attachments travel with the message as explicit paths. The model reads them
  // with its file tools, so the text carries the list rather than a mystery
  // "@file" token the UI cannot show after sending.
  std::string shown = text;
  for (size_t i = 0; i < st_.attachments.size(); ++i) {
    if (!shown.empty()) shown += "\n";
    shown += "@" + st_.attachments[i];
  }
  if (text.empty()) text = shown;   // attachments only: the paths are the message
  std::string sid = active_sid();
  // echo the user's message immediately (optimistic; the assistant text streams in)
  update([&](AppState& s) {
    comp::ChatMsg m;
    m.role = "user";
    m.text = shown;
    m.time = "刚刚";
    s.messages.push_back(m);
    s.session_input.clear();
    s.attachments.clear();
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
  Json attachments = Json::array();
  for (size_t i = 0; i < st_.attachments.size(); ++i) attachments.push_back(st_.attachments[i]);
  // 发送时把当前模型一起带上：会话页上换过的模型必须真的用于这次请求，而不是
  // 只改了按钮上的字。
  Json args = Json{{"sid", sid}, {"text", text}, {"attachments", attachments}};
  if (!st_.model.empty()) args["model"] = st_.model;
  if (!st_.thinking.empty()) args["thinking"] = st_.thinking;
  call_async("session.send", args,
             [this](const BackendResult& r) {
    if (!r.ok) toast(tr("发送失败：") + r.message);
  });
}

std::string UiApp::rel_time_for_test(int64_t ms) { return rel_time(ms); }

void UiApp::open_new_session() {
  // a real session needs a project; without one, fall back to a local tab
  if (!backend_ || st_.projects.empty()) {
    update([&](AppState& s) {
      s.session_seq++;
      std::string id = "s" + itos(s.session_seq);
      SessionTab t;
      t.id = id;
      t.title = tr("会话") + itos(s.session_seq);
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

// 顶层 "+"：打开"新建"页面（内容同主页·项目：点会话即开、点新建即建）。
// 它不是标签页，所以关掉就消失，也不会出现在访问栈里。
void UiApp::open_new_tab() { open_modal_page("new"); }

// 设置/新建这类页面：盖在当前标签页之上，不占标签、不进访问栈。
void UiApp::open_modal_page(const std::string& page) {
  if (page.empty()) return;
  // any transient popup belongs to the page we are leaving
  st_.model_picker_open = false;
  st_.skill_menu_open = false;
  st_.side_menu_open = false;
  st_.modal_page = page;
  // A modal is not a tab navigation: it always enters the same way, regardless
  // of what the tab stack says about the page underneath.
  page_dir_.clear();
  render();
}

void UiApp::close_modal_page() {
  if (st_.modal_page.empty()) return;
  st_.modal_page.clear();
  page_dir_.clear();
  render();
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
             [this, pid](const BackendResult& r) {
    if (!r.ok) return;
    std::string sid = jsonutil::get<std::string>(r.data, "sid", "");
    if (sid.empty()) sid = jsonutil::get<std::string>(r.data, "id", "");
    if (sid.empty()) return;
    update([&](AppState& s) {
      SessionTab t;
      t.id = sid;
      t.title = tr("会话") + itos(static_cast<int>(s.sessions.size() + 1));
      s.sessions.push_back(t);
      s.tab_ids.push_back(sid);
      // remember where we came from so closing this tab returns there
      if (!s.active_tab.empty() && s.active_tab != sid) {
        s.tab_history.push_back(s.active_tab);
      }
      s.active_tab = sid;
      s.messages.clear();
      s.session_input.clear();
    });
    load_terminals(sid);
    // The card's list must show the new session (and its count), so re-read the
    // project's sessions rather than waiting for the next full refresh.
    for (size_t i = 0; i < st_.projects.size(); ++i) {
      if (st_.projects[i].pid == pid) load_sessions(i);
    }
  });
}

void UiApp::confirm_delete_session(const std::string& sid, const std::string& title) {
  if (sid.empty()) return;
  // Deleting a session removes its files. That is not undoable, so it goes
  // through the same modal the permission gate uses instead of firing straight
  // from the click.
  st_.confirm_kind = "session";
  st_.confirm_id = sid;
  st_.confirm_text = tr("删除会话「") + (title.empty() ? sid : title) +
                     tr("」？记录与补丁会一并删除，无法恢复。");
  render();
}

void UiApp::delete_session(const std::string& sid) {
  if (sid.empty()) return;
  call_async("session.delete", Json{{"sid", sid}}, [this, sid](const BackendResult& r) {
    if (!r.ok) {
      toast(tr("删除失败：") + r.message);
      return;
    }
    toast(tr("已删除会话"));
    // If it was open, close its tab first (the visit stack then decides where
    // we land), and drop its cached view.
    bool was_open = false;
    for (size_t i = 0; i < st_.tab_ids.size(); ++i) {
      if (st_.tab_ids[i] == sid) was_open = true;
    }
    if (was_open) {
      close_tab(sid);
    } else {
      views_.erase(sid);
    }
    load_projects();  // refresh counts + lists
  });
}

// 重命名：行内编辑。开始编辑时把当前标题放进草稿，Enter/失焦提交。
void UiApp::begin_rename_session(const std::string& sid) {
  update([&](AppState& s) {
    for (size_t i = 0; i < s.projects.size(); ++i) {
      for (size_t k = 0; k < s.projects[i].sessions.size(); ++k) {
        ProjectCard::SessionRow& r = s.projects[i].sessions[k];
        // only one row is ever renaming; clear the others so two inputs cannot
        // both hold focus state
        if (r.sid == sid) {
          r.renaming = true;
          r.rename_text = r.title;
        } else {
          r.renaming = false;
        }
      }
    }
  });
  session_rename_focus_ = true;
  render();
}

void UiApp::commit_rename_session(const std::string& sid) {
  std::string title;
  update([&](AppState& s) {
    for (size_t i = 0; i < s.projects.size(); ++i) {
      for (size_t k = 0; k < s.projects[i].sessions.size(); ++k) {
        ProjectCard::SessionRow& r = s.projects[i].sessions[k];
        if (r.sid != sid) continue;
        title = r.rename_text;
        r.renaming = false;
      }
    }
  });
  if (sid.empty() || title.empty()) return;
  // optimistic: the row shows the new name immediately; the reply only decides
  // whether we have to revert.
  update([&](AppState& s) {
    for (size_t i = 0; i < s.projects.size(); ++i) {
      for (size_t k = 0; k < s.projects[i].sessions.size(); ++k) {
        if (s.projects[i].sessions[k].sid == sid) {
          s.projects[i].sessions[k].title = title;
        }
      }
    }
    // the open tab's title comes from `sessions`, not from the project card
    for (size_t i = 0; i < s.sessions.size(); ++i) {
      if (s.sessions[i].id == sid) s.sessions[i].title = title;
    }
  });
  call_async("session.rename", Json{{"sid", sid}, {"title", title}},
             [this](const BackendResult& r) {
    if (!r.ok) {
      toast(tr("重命名失败：") + r.message);
      load_projects();
    }
  });
}

void UiApp::pick_and_open_folder() {
  // the OS dialog runs in the native process; the browser gets the path back
  call_async("project.pickFolder", Json::object(), [this](const BackendResult& r) {
    if (!r.ok) {
      toast(tr("打开文件夹选择框失败"));
      return;
    }
    std::string path = jsonutil::get<std::string>(r.data, "path", "");
    if (path.empty()) return;  // user cancelled
    call_async("project.open", Json{{"path", path}}, [this](const BackendResult& r2) {
      if (r2.ok) toast(tr("已打开项目"));
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
      // 换了供应商 -> 上一个供应商的模型列表和草稿都作废
      s.models.clear();
      s.models_loaded = false;
      s.models_provider.clear();
      s.model_selected.clear();
      s.model_creating = false;
      s.model_filter.clear();
      s.model_status.clear();
      s.model_draft = AppState::ModelRow();
      if (r.data.contains("customHeaders") && r.data["customHeaders"].is_object()) {
        const Json& h = r.data["customHeaders"];
        for (Json::const_iterator it = h.begin(); it != h.end(); ++it) {
          if (it.value().is_string()) {
            s.pv_headers.push_back(std::make_pair(it.key(), it.value().get<std::string>()));
          }
        }
      }
      // 请求体覆盖：回显成可编辑的 JSON 文本
      s.pv_body_text.clear();
      if (r.data.contains("bodyOverride") && r.data["bodyOverride"].is_object() &&
          !r.data["bodyOverride"].empty()) {
        s.pv_body_text = r.data["bodyOverride"].dump(2);
      }
    });
    // 选中即加载该供应商的模型列表（列表 + 高级设置都用它）
    load_models(id);
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
      s.pv_body_text.clear();
      // 模板的 header 原样进表。opencode 的会话亲和头不再是特殊开关：模板给的
      // 就是一条普通头（x-opencode-session: {{SESSION}}），后端替换占位符。
      for (size_t k = 0; k < t.headers.size(); ++k) {
        s.pv_headers.push_back(t.headers[k]);
      }
      s.pv_advanced = false;
      s.provider_status.clear();
      s.provider_models.clear();
      s.models.clear();
      s.models_loaded = false;
      s.models_provider.clear();
      s.model_selected.clear();
      s.model_creating = false;
      s.model_filter.clear();
      s.model_status.clear();
      s.model_draft = AppState::ModelRow();
    });
    return;
  }
}

void UiApp::save_provider() {
  if (st_.pv_base_url.empty()) {
    toast(tr("请填写 Endpoint"));
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
  // 请求体覆盖：必须是合法 JSON 对象，否则后端会静默丢掉它（比报错更糟）。
  if (!st_.pv_body_text.empty()) {
    Json body;
    bool parsed = true;
    try {
      body = Json::parse(st_.pv_body_text);
    } catch (...) {
      parsed = false;
    }
    if (!parsed || !body.is_object()) {
      toast(tr("请求体覆盖必须是 JSON 对象"));
      return;
    }
    conn["bodyOverride"] = body;
  }
  call_async("provider.addProvider", Json{{"conn", conn}}, [this](const BackendResult& r) {
    if (!r.ok) return;
    toast(tr("已保存供应商"));
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
             [this, id](const BackendResult& r) {
    if (!r.ok) {
      toast(tr("获取失败：") + r.message);
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
    // The fetched list is now the provider's model set, so re-read it through
    // model.list: that path merges API models with the user's own overrides and
    // fills in thinking levels + icons, which the raw listModels reply does not.
    load_models(id);
  });
}

// ===========================================================================
// Settings actions: 逐模型（能力 / 档位 / 重试 / 启用 / 图标）
// ===========================================================================
namespace {

// One ModelEntry JSON -> the flat UI row. Kept as a free function so both the
// settings list and the session picker parse replies the same way.
void parse_model_row(const Json& j, AppState::ModelRow* out) {
  if (!out) return;
  out->id = jsonutil::get<std::string>(j, "id", "");
  out->display = jsonutil::get<std::string>(j, "display", out->id);
  out->icon = jsonutil::get<std::string>(j, "icon", "");
  out->enabled = jsonutil::get<bool>(j, "enabled", true);
  out->user_set = jsonutil::get<bool>(j, "userSet", false);
  out->from_api = jsonutil::get<bool>(j, "fromApi", false);
  out->from_preset = jsonutil::get<bool>(j, "fromPreset", false);
  Json c = j.contains("caps") && j["caps"].is_object() ? j["caps"] : Json::object();
  out->vision = jsonutil::get<bool>(c, "vision", false);
  out->audio = jsonutil::get<bool>(c, "audio", false);
  out->video = jsonutil::get<bool>(c, "video", false);
  out->pdf = jsonutil::get<bool>(c, "pdf", false);
  out->structured = jsonutil::get<bool>(c, "structuredOutput", false);
  out->tool_use = jsonutil::get<bool>(c, "toolUse", false);
  out->thinking_cap = jsonutil::get<bool>(c, "thinking", false);
  out->context_len = jsonutil::get_int(c, "contextLen", 0);
  out->max_output = jsonutil::get_int(c, "maxOutput", 0);
  out->thinking_levels.clear();
  if (j.contains("thinking") && j["thinking"].is_object()) {
    const Json& t = j["thinking"];
    if (t.contains("levels") && t["levels"].is_array()) {
      for (size_t i = 0; i < t["levels"].size(); ++i) {
        if (t["levels"][i].is_string()) {
          out->thinking_levels.push_back(t["levels"][i].get<std::string>());
        }
      }
    }
    out->thinking_default = jsonutil::get<std::string>(t, "default", "");
  } else {
    out->thinking_default.clear();
  }
  out->retry_max = 0;
  out->retry_backoff_ms = 0;
  if (j.contains("retry") && j["retry"].is_object()) {
    out->retry_max = static_cast<int>(jsonutil::get_int(j["retry"], "max", 0));
    out->retry_backoff_ms =
        static_cast<int>(jsonutil::get_int(j["retry"], "backoffMs", 0));
  }
}

Json model_row_to_json(const AppState::ModelRow& m) {
  Json j = Json::object();
  j["id"] = m.id;
  j["display"] = m.display.empty() ? m.id : m.display;
  j["enabled"] = m.enabled;
  if (!m.icon.empty()) j["icon"] = m.icon;
  Json c = Json::object();
  c["vision"] = m.vision;
  c["audio"] = m.audio;
  c["video"] = m.video;
  c["pdf"] = m.pdf;
  c["structuredOutput"] = m.structured;
  c["toolUse"] = m.tool_use;
  c["thinking"] = m.thinking_cap || !m.thinking_levels.empty();
  c["contextLen"] = m.context_len;
  c["maxOutput"] = m.max_output;
  j["caps"] = c;
  if (!m.thinking_levels.empty()) {
    Json t = Json::object();
    Json lv = Json::array();
    for (size_t i = 0; i < m.thinking_levels.size(); ++i) lv.push_back(m.thinking_levels[i]);
    t["levels"] = lv;
    t["default"] = m.thinking_default;
    j["thinking"] = t;
  }
  if (m.retry_max > 0 || m.retry_backoff_ms > 0) {
    Json r = Json::object();
    r["max"] = m.retry_max;
    r["backoffMs"] = m.retry_backoff_ms;
    j["retry"] = r;
  }
  return j;
}

}  // namespace

void UiApp::load_models(const std::string& provider_id) {
  if (provider_id.empty()) return;
  call_async("model.list", Json{{"providerId", provider_id}},
             [this, provider_id](const BackendResult& r) {
    if (!r.ok) return;
    std::vector<AppState::ModelRow> rows;
    if (r.data.contains("models") && r.data["models"].is_array()) {
      const Json& arr = r.data["models"];
      for (size_t i = 0; i < arr.size(); ++i) {
        AppState::ModelRow row;
        parse_model_row(arr[i], &row);
        if (!row.id.empty()) rows.push_back(row);
      }
    }
    update([&](AppState& s) {
      // Only accept the reply if it is still the provider we are showing: a
      // slow reply for provider A must not overwrite the list for B.
      if (s.provider_selected != provider_id) return;
      s.models = rows;
      s.models_loaded = true;
      s.models_provider = provider_id;
      // keep the draft in sync with the refreshed data (it may carry new caps)
      if (!s.model_selected.empty()) {
        for (size_t i = 0; i < s.models.size(); ++i) {
          if (s.models[i].id == s.model_selected) {
            AppState::ModelRow keep = s.models[i];
            s.models[i] = keep;
          }
        }
      }
    });
  });
}

void UiApp::select_model(const std::string& model_id) {
  update([&](AppState& s) {
    s.model_creating = false;
    s.model_status.clear();
    s.model_selected = model_id;
    for (size_t i = 0; i < s.models.size(); ++i) {
      if (s.models[i].id == model_id) {
        s.model_draft = s.models[i];
        return;
      }
    }
    s.model_draft = AppState::ModelRow();
    s.model_draft.id = model_id;
    s.model_draft.display = model_id;
  });
}

void UiApp::begin_new_model() {
  update([&](AppState& s) {
    s.model_creating = true;
    s.model_selected.clear();
    s.model_status.clear();
    s.model_draft = AppState::ModelRow();
    s.model_new_id.clear();
  });
}

void UiApp::save_model() {
  std::string pid = st_.provider_selected;
  if (pid.empty()) {
    toast(tr("请先保存供应商"));
    return;
  }
  AppState::ModelRow m = st_.model_draft;
  if (st_.model_creating) m.id = st_.model_new_id;
  if (m.id.empty()) {
    toast(tr("请填写模型名"));
    return;
  }
  // 上下文长度必填：压缩阈值和上下文水位都要用它算比例，0 会让两者都失真。
  if (m.context_len <= 0) {
    update([&](AppState& s) {
      s.model_status = tr("请填写上下文长度（必填）");
    });
    return;
  }
  if (m.display.empty()) m.display = m.id;
  Json body = model_row_to_json(m);
  call_async("model.save", Json{{"providerId", pid}, {"model", body}},
             [this, pid, mid = m.id](const BackendResult& r) {
    if (!r.ok) {
      update([&](AppState& s) { s.model_status = "保存失败：" + r.message; });
      return;
    }
    update([&](AppState& s) {
      s.model_creating = false;
      s.model_new_id.clear();
      s.model_selected = mid;
      s.model_status = "已保存";
    });
    load_models(pid);
  });
}

void UiApp::delete_model(const std::string& model_id) {
  std::string pid = st_.provider_selected;
  if (pid.empty() || model_id.empty()) return;
  call_async("model.remove", Json{{"providerId", pid}, {"modelId", model_id}},
             [this, pid](const BackendResult& r) {
    if (!r.ok) {
      // Preset/API models cannot be deleted - only disabled. Say so instead of
      // failing silently.
      update([&](AppState& s) { s.model_status = "删除失败：" + r.message; });
      return;
    }
    update([&](AppState& s) {
      s.model_selected.clear();
      s.model_draft = AppState::ModelRow();
      s.model_status = "已删除";
    });
    load_models(pid);
  });
}

void UiApp::toggle_model_enabled(const std::string& model_id, bool enabled) {
  std::string pid = st_.provider_selected;
  if (pid.empty() || model_id.empty()) return;
  // optimistic: the checkbox should not lag a round trip
  update([&](AppState& s) {
    for (size_t i = 0; i < s.models.size(); ++i) {
      if (s.models[i].id == model_id) s.models[i].enabled = enabled;
    }
    if (s.model_draft.id == model_id) s.model_draft.enabled = enabled;
  });
  call_async("model.setEnabled",
             Json{{"providerId", pid}, {"modelId", model_id}, {"enabled", enabled}},
             [this, pid](const BackendResult& r) {
    if (!r.ok) load_models(pid);  // revert by re-reading
  });
}

void UiApp::test_model(const std::string& model_id) {
  std::string pid = st_.provider_selected;
  if (pid.empty() || model_id.empty()) return;
  update([&](AppState& s) { s.model_status = "测试中…（" + model_id + "）"; });
  call_async("model.test", Json{{"providerId", pid}, {"modelId", model_id}},
             [this](const BackendResult& r) {
    update([&](AppState& s) {
      if (r.ok) {
        s.model_status = "可用（" +
            std::to_string(jsonutil::get_int(r.data, "latencyMs", 0)) + "ms）";
      } else {
        s.model_status = "测试失败：" + r.message;
      }
    });
  });
}

// ===========================================================================
// 模型选择器（左供应商 / 右模型）
// ===========================================================================
// It is ONE component reused by every "pick a model" affordance: the composer's
// model button and the settings fields (会话默认 / 模型偏好 / 子代理). `owner`
// says which widget is showing it (only that widget renders the popup, so it can
// never appear twice), and `target` says what a pick means: "" = the session's
// model, otherwise a settings key.
void UiApp::open_model_picker(const std::string& owner, const std::string& target) {
  // The picker lists providers, so make sure they are loaded before it opens
  // (it can be opened from the session page without ever visiting settings).
  if (!st_.providers_loaded) load_providers();
  update([&](AppState& s) {
    s.model_picker_open = true;
    s.model_picker_owner = owner;
    s.model_picker_target = target;
    s.model_picker_query.clear();
    // Default to the provider being edited in settings, else the first one -
    // opening onto an empty right pane would look broken.
    if (s.model_picker_provider.empty()) {
      s.model_picker_provider = !s.provider_selected.empty()
                                    ? s.provider_selected
                                    : (s.providers.empty() ? std::string()
                                                           : s.providers[0].id);
    }
    s.model_picker_models.clear();
    s.model_picker_loaded = false;
    s.model_picker_status.clear();
  });
  load_picker_models(st_.model_picker_provider);
}

void UiApp::load_picker_models(const std::string& provider_id) {
  if (provider_id.empty()) return;
  call_async("model.list", Json{{"providerId", provider_id}},
             [this, provider_id](const BackendResult& r) {
    if (!r.ok) {
      update([&](AppState& s) { s.model_picker_status = "加载失败：" + r.message; });
      return;
    }
    std::vector<AppState::ModelRow> rows;
    if (r.data.contains("models") && r.data["models"].is_array()) {
      const Json& arr = r.data["models"];
      for (size_t i = 0; i < arr.size(); ++i) {
        AppState::ModelRow row;
        parse_model_row(arr[i], &row);
        if (!row.id.empty() && row.enabled) rows.push_back(row);
      }
    }
    update([&](AppState& s) {
      if (s.model_picker_provider != provider_id) return;
      s.model_picker_models = rows;
      s.model_picker_loaded = true;
      s.model_picker_status.clear();
    });
  });
}

void UiApp::pick_model(const std::string& provider_id, const std::string& model_id) {
  (void)provider_id;
  if (model_id.empty()) return;
  std::string target = st_.model_picker_target;
  std::string sid = active_sid();
  update([&](AppState& s) {
    s.model_picker_open = false;
    s.model_picker_owner.clear();
    s.model_picker_target.clear();
  });
  if (target.empty()) {
    // the session's model
    update([&](AppState& s) { s.model = model_id; });
    if (!sid.empty()) {
      call_async("session.setModel", Json{{"sid", sid}, {"model", model_id}});
    }
    toast(tr("已切换到 ") + model_id);
    return;
  }
  // a settings field: write the key (the field reads it back from settings)
  set_setting(target, model_id);
  toast(tr("已设为 ") + model_id);
}

// A settings field's model button. It renders the same popup as the composer,
// so "which model" is answered by the same component everywhere.
H UiApp::build_model_field(const std::string& owner, const std::string& setting_key,
                           const std::string& current, const std::string& placeholder) {
  H box = el("div", "model-field");
  H btn = el("button", "model-pick-btn");
  btn.attr("type", "button");
  btn.attr("title", tr("选择模型"));
  std::string key = setting_key;
  btn.on(EventKind::kClick, [this, owner, key](const std::string&) {
    open_model_picker(owner, key);
  });
  btn.add(comp::model_icon("", current));
  btn.add(el("span", "mpb-name").text(current.empty() ? placeholder : current));
  btn.add(el("span", "mpb-caret").text("\u25be"));
  box.add(btn);
  if (!current.empty()) {
    btn.attr("title", current + "（点击更换）");
    box.add(comp::icon_button("\u2715", tr("清除（继承）"), [this, key](const std::string&) {
      set_setting(key, std::string());
    }));
  }
  if (st_.model_picker_open && st_.model_picker_owner == owner) {
    box.add(build_model_picker(owner, current));
  }
  return box;
}

H UiApp::build_model_picker(const std::string& owner, const std::string& current) {
  H pop = el("div", "model-picker");
  pop.key("model-picker:" + owner);
  pop.anim(anim::kPop);
  H head = el("div", "mp-head");
  head.add(el("span", "mp-title").text(tr("选择模型")));
  head.add(el("span", "grow"));
  head.add(comp::icon_button("\u2715", tr("关闭"), [this](const std::string&) {
    update([&](AppState& s) {
      s.model_picker_open = false;
      s.model_picker_owner.clear();
    });
  }));
  pop.add(head);

  H cols = el("div", "mp-cols");
  // left: providers
  H plist = el("div", "mp-providers");
  for (size_t i = 0; i < st_.providers.size(); ++i) {
    const AppState::ProviderRow& p = st_.providers[i];
    bool active = p.id == st_.model_picker_provider;
    H item = el("div", active ? "mp-prov active" : "mp-prov");
    item.key("mp-" + p.id);
    std::string pid = p.id;
    item.on(EventKind::kClick, [this, pid](const std::string&) {
      update([&](AppState& s) {
        s.model_picker_provider = pid;
        s.model_picker_models.clear();
        s.model_picker_loaded = false;
        s.model_picker_status.clear();
        // A query typed against one provider's models should not silently
        // filter the next provider's list down to nothing.
        s.model_picker_query.clear();
      });
      load_picker_models(pid);
    });
    item.add(el("div", "mp-prov-name").text(p.name.empty() ? p.id : p.name));
    item.add(el("div", "mp-prov-kind dim").text(p.kind));
    plist.add(item);
  }
  if (st_.providers.empty()) {
    plist.add(comp::empty_state(tr("还没有供应商。"), "", nullptr));
  }
  cols.add(plist);

  // right: models of the selected provider
  H mside = el("div", "mp-models");
  H search = comp::search_box(st_.model_picker_query, tr("搜索模型"),
                              [this](const std::string& v) {
    // must re-render: the filter decides which rows exist
    update([&](AppState& s) { s.model_picker_query = v; });
  });
  // Keyed by provider: switching providers clears the query, but a focused
  // input's value is deliberately NOT patched (that is what keeps a caret from
  // jumping mid-typing), so the cleared state would never reach the DOM. A new
  // key recreates the node, which is the framework-level way to say "start over".
  search.key("mp-search:" + owner + ":" + st_.model_picker_provider);
  mside.add(search);
  // Filtering happens here rather than in the backend: the list is already in
  // memory and a round trip per keystroke would make typing feel laggy.
  std::vector<const AppState::ModelRow*> vis;
  for (size_t i = 0; i < st_.model_picker_models.size(); ++i) {
    const AppState::ModelRow& m = st_.model_picker_models[i];
    if (!st_.model_picker_query.empty()) {
      std::string ql = lower_ascii(st_.model_picker_query);
      if (lower_ascii(m.id + " " + m.display).find(ql) == std::string::npos) continue;
    }
    vis.push_back(&m);
  }
  // Above the threshold the list is sorted and grouped by initial, which only
  // means anything if the order is alphabetical.
  const bool grouped = vis.size() > 20;
  if (grouped) {
    std::sort(vis.begin(), vis.end(),
              [](const AppState::ModelRow* a, const AppState::ModelRow* b) {
      return lower_ascii(a->display.empty() ? a->id : a->display) <
             lower_ascii(b->display.empty() ? b->id : b->display);
    });
  }
  if (!st_.model_picker_loaded) {
    mside.add(el("div", "hint").text(tr("加载中…")));
  } else if (vis.empty()) {
    mside.add(comp::empty_state(
        st_.model_picker_query.empty() ? tr("这个供应商还没有启用中的模型。")
                                       : tr("没有匹配的模型。"),
        "", nullptr));
  } else {
    std::string last_group;
    H list = el("div", "mp-list");
    for (size_t i = 0; i < vis.size(); ++i) {
      const AppState::ModelRow& m = *vis[i];
      std::string label = m.display.empty() ? m.id : m.display;
      if (grouped) {
        char c = label.empty() ? '#' : label[0];
        std::string g;
        if (c >= 'a' && c <= 'z') g = std::string(1, static_cast<char>(c - 'a' + 'A'));
        else if (c >= 'A' && c <= 'Z') g = std::string(1, c);
        else if (c >= '0' && c <= '9') g = "#";
        else g = "·";
        if (g != last_group) {
          list.add(el("div", "mp-group").text(g));
          last_group = g;
        }
      }
      bool cur = m.id == current;
      H row = el("div", cur ? "mp-model active" : "mp-model");
      row.key("mpm-" + m.id);
      std::string mid = m.id;
      std::string ppid = st_.model_picker_provider;
      row.on(EventKind::kClick, [this, ppid, mid](const std::string&) {
        pick_model(ppid, mid);
      });
      row.add(comp::model_icon(m.icon, m.id));
      H nm = el("div", "mp-model-name");
      nm.add(el("span", "mp-model-id").text(label));
      H caps = el("span", "md-caps");
      bool any_cap = false;
      if (m.vision) { caps.add(el("i", "fa fa-eye").attr("title", tr("视觉"))); any_cap = true; }
      if (m.audio) { caps.add(el("i", "fa fa-volume-high").attr("title", tr("音频"))); any_cap = true; }
      if (m.video) { caps.add(el("i", "fa fa-video").attr("title", tr("视频"))); any_cap = true; }
      if (m.pdf) { caps.add(el("i", "fa fa-file-pdf").attr("title", tr("PDF"))); any_cap = true; }
      if (m.structured) { caps.add(el("i", "fa fa-brackets-curly").attr("title", tr("结构化输出"))); any_cap = true; }
      if (any_cap) nm.add(caps);
      row.add(nm);
      if (m.context_len > 0) {
        row.add(el("span", "mp-ctx dim").text(
            itos(static_cast<int>(m.context_len / 1000)) + "K"));
      }
      if (cur) row.add(el("i", "fa fa-check"));
      list.add(row);
    }
    mside.add(list);
  }
  if (!st_.model_picker_status.empty()) {
    mside.add(el("div", "pv-status").text(st_.model_picker_status));
  }
  cols.add(mside);
  pop.add(cols);
  return pop;
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
    toast(tr("请填写名称与目录"));
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

// A skill is imported from a directory (same flow as MCP: pick a folder, the
// backend reads the manifest out of it).
void UiApp::add_skill() {
  if (!backend_) return;
  std::string dir = st_.skill_new_dir;
  if (dir.empty()) {
    toast(tr("请先选择目录"));
    return;
  }
  call_async("skill.import", Json{{"dir", dir}}, [this](const BackendResult& r) {
    if (!r.ok) {
      toast(tr("导入失败：") + r.message);
      return;
    }
    update([&](AppState& s) { s.skill_new_dir.clear(); });
    load_skills();
    toast(tr("已导入 Skill"));
  });
}

// CLI 工具探测：在 PATH 上找常见的开发工具（git/node/python/cargo/...），把
// 找到的路径写进 environment.cliTools。只列出**找到的**，其余留给用户自己加。
void UiApp::detect_cli_tools() {
  call_async("environment.detectCliTools", Json::object(), [this](const BackendResult& r) {
    if (!r.ok) {
      toast(tr("探测失败：") + r.message);
      return;
    }
    const Json& found = r.data.contains("tools") ? r.data["tools"] : r.data;
    if (!found.is_object()) return;
    // 合并进已有表（用户手填的不能被探测结果冲掉）
    Json cur = sv_obj(st_.settings_values, "environment.cliTools");
    size_t added = 0;
    for (Json::const_iterator it = found.begin(); it != found.end(); ++it) {
      if (!it.value().is_string()) continue;
      if (cur.contains(it.key())) continue;
      cur[it.key()] = it.value();
      ++added;
    }
    if (added == 0) {
      toast(tr("没有发现新的 CLI 工具"));
      return;
    }
    set_setting("environment.cliTools", cur);
    toast(tr("发现 ") + itos(static_cast<int>(added)) + tr(" 个 CLI 工具"));
  });
}

// 选文件 -> 写进某个设置键。驱动路径这类字段需要的是文件本身，用目录选择框
// 选不中，所以这里走 dialog.pickFile。
void UiApp::pick_file_into(const std::string& setting_key,
                           const std::string& filter_desc,
                           const std::string& filter_spec) {
  Json args = Json::object();
  if (!filter_desc.empty()) args["filterDesc"] = filter_desc;
  if (!filter_spec.empty()) args["filterSpec"] = filter_spec;
  call_async("dialog.pickFile", args, [this, setting_key](const BackendResult& r) {
    if (!r.ok) {
      toast(tr("打开文件选择框失败"));
      return;
    }
    std::string path = jsonutil::get<std::string>(r.data, "path", "");
    if (path.empty()) return;  // cancelled
    set_setting(setting_key, path);
  });
}

// CLI 工具路径的两种选文件场景：改已有条目（合并进 map）和填新增草稿（只改草稿
// 输入框，等用户点"添加"）。分开是因为前者写设置、后者只动草稿。
void UiApp::pick_cli_tool_path(const std::string& key) {
  Json args = Json::object();
  args["filterDesc"] = tr("可执行文件");
  args["filterSpec"] = "*.exe;*.cmd;*.bat;*.ps1;*";
  call_async("dialog.pickFile", args, [this, key](const BackendResult& r) {
    if (!r.ok) {
      toast(tr("打开文件选择框失败"));
      return;
    }
    std::string path = jsonutil::get<std::string>(r.data, "path", "");
    if (path.empty()) return;
    Json cur = sv_obj(st_.settings_values, "environment.cliTools");
    cur[key] = path;
    set_setting("environment.cliTools", cur);
  });
}

void UiApp::pick_cli_tool_draft() {
  Json args = Json::object();
  args["filterDesc"] = tr("可执行文件");
  args["filterSpec"] = "*.exe;*.cmd;*.bat;*.ps1;*";
  call_async("dialog.pickFile", args, [this](const BackendResult& r) {
    if (!r.ok) return;
    std::string path = jsonutil::get<std::string>(r.data, "path", "");
    if (path.empty()) return;
    update([&](AppState& s) { s.cli_new_value = path; });
  });
}

void UiApp::add_kv(const std::string& key_setting, const std::string& k,
                   const std::string& v) {
  if (k.empty()) {
    toast(tr("请填写键"));
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
      // You just asked for a terminal, so you must be able to see it: a filter
      // left over from an earlier search would otherwise hide its card on the
      // overview page.
      s.term_filter.clear();
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
  // Remember what was typed into this terminal. "输入匹配" searches these, and
  // a terminal's commands are not visible in its name or (once the screen has
  // scrolled) necessarily on it either. Enter starts a new line of input.
  {
    std::string& log = st_.term_input_log[tid];
    if (bytes == "\r") {
      log += "\n";
    } else if (bytes.size() == 1 && bytes[0] == 0x7f) {
      if (!log.empty() && log[log.size() - 1] != '\n') log.erase(log.size() - 1);
    } else if (bytes.size() == 1 && bytes[0] >= 0x20) {
      log += bytes;
    } else if (bytes.size() > 1 && bytes[0] >= 0x20) {
      log += bytes;  // pasted text
    }
    if (log.size() > 8192) log.erase(0, log.size() - 8192);
  }
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
  if (st_.active_tab == "home") {
    toast(tr("请先打开一个会话"));
    return;
  }
  if (kind == "url") {
    // A link IS text: it belongs in the message, where it can be read in context.
    update([&](AppState& s) {
      if (!s.session_input.empty() && s.session_input.back() != ' ') s.session_input += ' ';
      s.session_input += value;
    });
    VNodePtr ta = find_by_key(tree_, "session-input");
    if (ta && ta->dom.valid()) dom_.set_attr(ta->dom, "value", st_.session_input);
    toast(tr("已插入链接"));
    return;
  }
  // A dropped FILE is an attachment, not text: it becomes a chip like an
  // uploaded one, so the model still gets a readable path but the input stays
  // clean.
  update([&](AppState& s) {
    bool dup = false;
    for (size_t k = 0; k < s.attachments.size(); ++k) {
      if (s.attachments[k] == value) dup = true;
    }
    if (!dup) s.attachments.push_back(value);
  });
  toast(tr("已添加附件"));
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
  if (st_.active_tab == "home") {
    toast(tr("请先打开一个会话"));
    return;
  }
  // Attachments become chips above the input, NOT @paths in the text: a path is
  // not text, and mixing the two made it impossible to see what was attached.
  size_t added = 0;
  update([&](AppState& s) {
    for (size_t i = 0; i < files.size(); ++i) {
      std::string rel = jsonutil::get<std::string>(files[i], "rel", "");
      if (rel.empty()) continue;
      bool dup = false;
      for (size_t k = 0; k < s.attachments.size(); ++k) {
        if (s.attachments[k] == rel) dup = true;
      }
      if (dup) continue;
      s.attachments.push_back(rel);
      ++added;
    }
  });
  if (added == 0) return;
  toast(tr("已添加附件") + "（" + itos(static_cast<int>(added)) + "）");
}

// Terminals matching the current filter, in display order. An empty filter means
// "everything", so the strip and the overview both go through here.
//
// Three haystacks, because "find that terminal" means different things:
//   name  (always)  the tab label, the shell, and the #id
//   input (opt-in)  the commands typed into it
//   text  (opt-in)  its whole screen, output included
std::vector<const TermItem*> UiApp::filtered_terminals() const {
  std::vector<const TermItem*> out;
  const std::string& pat = st_.term_filter;
  std::vector<const TermItem*> all = ordered_terminals();
  for (size_t i = 0; i < all.size(); ++i) {
    const TermItem& t = *all[i];
    if (pat.empty()) {
      out.push_back(&t);
      continue;
    }
    std::string name = "#" + t.tid + " " + term_display_name(t) + " " + t.shell;
    bool hit = term_match_one(name, pat);
    if (!hit && st_.term_filter_input) {
      std::map<std::string, std::string>::const_iterator it =
          st_.term_input_log.find(t.tid);
      if (it != st_.term_input_log.end()) hit = term_match_one(it->second, pat);
    }
    if (!hit && st_.term_filter_text) {
      std::map<std::string, Json>::const_iterator it = st_.term_grids.find(t.tid);
      if (it != st_.term_grids.end()) hit = term_match_one(term_grid_text(it->second), pat);
    }
    if (hit) out.push_back(&t);
  }
  return out;
}

// Search row for the terminal overview, shown once there are enough terminals to
// get lost in. Matching is live (every keystroke re-filters) and the two toggles
// widen it; they are icon-only because the strip is narrow, and their titles
// carry the explanation.
H UiApp::build_terminal_search() {
  H box = el("div", "td-search");
  box.key("term-search");
  // update() (not a bare assignment): the filter must re-render the list as you
  // type. The input keeps focus because patch only rewrites `value` when it
  // actually changed, and it now matches what the user typed.
  H inp = comp::text_input(st_.term_filter, tr("筛选终端…"),
                           [this](const std::string& v) {
    update([&](AppState& s) { s.term_filter = v; });
  });
  inp.key("term-filter-input");
  box.add(inp);
  // Icon-only toggles: the strip is narrow, so labels would not fit. The title
  // (tooltip) carries the full meaning.
  box.add(comp::icon_button("\u2318", tr("输入匹配：也匹配该终端执行过的命令"),
                            [this](const std::string&) {
    update([&](AppState& s) { s.term_filter_input = !s.term_filter_input; });
  }).cls(st_.term_filter_input ? "on" : ""));
  box.add(comp::icon_button("\u2261", tr("全文匹配：也匹配终端屏幕上的全部内容（含输出）"),
                            [this](const std::string&) {
    update([&](AppState& s) { s.term_filter_text = !s.term_filter_text; });
  }).cls(st_.term_filter_text ? "on" : ""));
  return box;
}

// Close a terminal's TAB without ending it. The process keeps running (the model
// may still be using it) - only 停止 kills it.
//
// The terminal stays in the list: `terminals` is what BOTH the strip and the
// overview read, and only the STRIP skips closed ones (term_tab_closed). Removing
// it here is what made a closed terminal vanish from 所有 as well, so it looked
// gone until the next backend refresh put it back.
void UiApp::close_terminal_tab(const std::string& tid) {
  if (tid.empty()) return;
  update([&](AppState& s) {
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

// Bring back a closed terminal's tab. Closing only hides the tab (the process
// keeps running), and the overview card does this on click, so the tab is
// recoverable from the list where the terminal is visible.
//
// The reopened tab goes to the END of the strip: reopening is "give me this
// back", and it should appear where a new tab would - not jump into numeric
// position in the middle of the strip (which is what the backend's tid sort
// would do, and it makes the tab you just asked for hard to find).
void UiApp::reopen_terminal_tab(const std::string& tid) {
  if (tid.empty()) return;
  update([&](AppState& s) {
    for (size_t i = 0; i < s.term_hidden.size(); ++i) {
      if (s.term_hidden[i] == tid) {
        s.term_hidden.erase(s.term_hidden.begin() + i);
        break;
      }
    }
    for (size_t i = 0; i < s.term_order.size(); ++i) {
      if (s.term_order[i] == tid) {
        s.term_order.erase(s.term_order.begin() + i);
        break;
      }
    }
    s.term_order.push_back(tid);
  });
}

void UiApp::handle_dropped_folder(const std::string& name) {  if (name.empty()) return;
  call_async("project.resolveDrop", Json{{"name", name}}, [this, name](const BackendResult& r) {
    if (!r.ok) return;
    if (jsonutil::get<bool>(r.data, "notFound", false)) {
      toast(tr("未找到文件夹「") + name + "」，请用「打开文件夹」手动选择");
      return;
    }
    std::string path = jsonutil::get<std::string>(r.data, "path", "");
    if (path.empty()) return;
    call_async("project.open", Json{{"path", path}}, [this](const BackendResult& r2) {
      if (r2.ok) toast(tr("已打开拖入的项目"));
      load_projects();
    });
  });
}

void UiApp::close_tab(const std::string& id) {
  if (id == "home") return;
  // Where to go after closing: back through the visit stack, skipping tabs that
  // are gone. Only when the stack has nothing usable do we fall back to home -
  // jumping to a fixed tab is what made closing feel random.
  std::string next;
  bool closing_active = (id == st_.active_tab);
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
    if (!closing_active) return;
    // pop the stack until we find a tab that still exists
    while (!s.tab_history.empty()) {
      std::string cand = s.tab_history.back();
      s.tab_history.pop_back();
      if (cand == id) continue;
      bool alive = false;
      for (size_t i = 0; i < s.tab_ids.size(); ++i) {
        if (s.tab_ids[i] == cand) alive = true;
      }
      if (alive) {
        next = cand;
        break;
      }
    }
    if (next.empty()) next = "home";
  });
  if (closing_active) {
    // The session view of the closed tab is dead weight (and its sid could be
    // reused), so drop it.
    views_.erase(id);
    if (last_session_key_ == id) last_session_key_.clear();
    switch_tab(next, -1);  // closing always retraces: animate as a "back"
  }
}

// 拖动标签调换顺序：把 from 移到 to 原来的位置（其余标签相对顺序不变）。
// 主页固定在第一位——它不是工作标签，而是一个入口。
// 按设置里的 ui.tabOrder 重排已打开的标签：用户的排列是持久化的安排，重启后
// 新开同一个会话应当回到原来的位置。未知 id 保持原顺序排在后面，主页永远第一。
void UiApp::apply_saved_tab_order() {
  if (!st_.settings_values.is_object() ||
      !st_.settings_values.contains("ui.tabOrder")) {
    return;
  }
  const Json& raw = st_.settings_values["ui.tabOrder"];
  Json order = (raw.is_object() && raw.contains("value")) ? raw["value"] : raw;
  if (order.is_array()) {
    std::vector<std::string> out;
    out.push_back("home");
    for (size_t i = 0; i < order.size(); ++i) {
      if (!order[i].is_string()) continue;
      std::string id = order[i].get<std::string>();
      if (id == "home") continue;
      for (size_t k = 0; k < st_.tab_ids.size(); ++k) {
        if (st_.tab_ids[k] == id) {
          bool dup = false;
          for (size_t j = 0; j < out.size(); ++j) if (out[j] == id) dup = true;
          if (!dup) out.push_back(id);
        }
      }
    }
    for (size_t k = 0; k < st_.tab_ids.size(); ++k) {
      bool known = false;
      for (size_t j = 0; j < out.size(); ++j) if (out[j] == st_.tab_ids[k]) known = true;
      if (!known) out.push_back(st_.tab_ids[k]);
    }
    st_.tab_ids = out;
  }
}

void UiApp::move_tab(const std::string& from, const std::string& to) {
  if (from.empty() || to.empty() || from == to) return;
  if (from == "home" || to == "home") return;
  update([&](AppState& s) {
    int fi = -1, ti = -1;
    for (size_t i = 0; i < s.tab_ids.size(); ++i) {
      if (s.tab_ids[i] == from) fi = static_cast<int>(i);
      if (s.tab_ids[i] == to) ti = static_cast<int>(i);
    }
    if (fi < 0 || ti < 0) return;
    std::string moved = s.tab_ids[fi];
    s.tab_ids.erase(s.tab_ids.begin() + fi);
    // recompute after the erase: indices shift when moving left -> right
    int insert = ti;
    if (fi < ti) insert = ti - 1;
    if (insert < 0) insert = 0;
    if (insert > static_cast<int>(s.tab_ids.size())) insert = static_cast<int>(s.tab_ids.size());
    s.tab_ids.insert(s.tab_ids.begin() + insert, moved);
  });
  // 顺序是用户的安排，重启后应当还在：写回设置。
  Json order = Json::array();
  for (size_t i = 0; i < st_.tab_ids.size(); ++i) order.push_back(st_.tab_ids[i]);
  set_setting("ui.tabOrder", order);
}

void UiApp::open_or_focus_side_tab(const std::string& kind) {
  if (kind.empty()) return;  // never silently default to a tab kind
  const SideKind* k = side_kind(kind);
  // Opening a panel implies wanting to SEE it: a manually collapsed side panel
  // must expand, or the click looks broken (the panel was "opened" but nothing
  // appeared).
  if (st_.side_collapsed) {
    update([&](AppState& s) { s.side_collapsed = false; });
  }
  // A singleton kind focuses its existing tab; a multi kind (files, git) always
  // adds another, because two file panels are two directories.
  if (k && !k->multi) {
    for (size_t i = 0; i < st_.side_tabs.size(); ++i) {
      if (st_.side_tabs[i].kind == kind) {
        std::string key = st_.side_tabs[i].id;
        update([&](AppState& s) {
          if (!s.active_side.empty() && s.active_side != key) {
            s.side_history.push_back(s.active_side);
          }
          s.active_side = key;
        });
        load_side_tab(key);
        return;
      }
    }
  }
  std::string id;
  update([&](AppState& s) {
    s.side_seq++;
    SideTab t;
    t.id = "sb" + itos(s.side_seq);
    t.kind = kind;
    t.title = side_title(t.kind);
    s.side_tabs.push_back(t);
    if (!s.active_side.empty() && s.active_side != t.id) {
      s.side_history.push_back(s.active_side);
    }
    s.active_side = t.id;
    id = t.id;
  });
  load_side_tab(id);
}

// Load (or reload) one panel's data. Each panel loads its OWN state, so two
// file panels can sit in different directories without fighting.
void UiApp::load_side_tab(const std::string& id) {
  std::string kind;
  std::string path;
  for (size_t i = 0; i < st_.side_tabs.size(); ++i) {
    if (st_.side_tabs[i].id == id) {
      kind = st_.side_tabs[i].kind;
      path = st_.side_tabs[i].path;
    }
  }
  if (kind == "subagents") load_subagents(active_sid());
  if (kind == "files" && !st_.active_pid.empty()) load_files(id, st_.active_pid, path);
}
void UiApp::close_side_tab(const std::string& id) {
  std::string next;
  bool closing_active = (id == st_.active_side);
  update([&](AppState& s) {
    for (size_t i = 0; i < s.side_tabs.size(); ++i) {
      if (s.side_tabs[i].id == id) {
        s.side_tabs.erase(s.side_tabs.begin() + i);
        break;
      }
    }
    // Closing a panel while the "+" page is open must not leave that page
    // standing over the panel you just closed: the page belongs to the panel.
    s.side_menu_open = false;
    if (!closing_active) return;
    // Same rule as the top strip: go back to the panel you came from, skipping
    // ones that are gone. Only fall back to the first tab when nothing is left.
    while (!s.side_history.empty()) {
      std::string cand = s.side_history.back();
      s.side_history.pop_back();
      if (cand == id) continue;
      bool alive = false;
      for (size_t i = 0; i < s.side_tabs.size(); ++i) {
        if (s.side_tabs[i].id == cand) alive = true;
      }
      if (alive) {
        next = cand;
        break;
      }
    }
    if (next.empty() && !s.side_tabs.empty()) next = s.side_tabs[0].id;
    s.active_side = next;
  });
  if (closing_active && !next.empty()) load_side_tab(next);
}

}  // namespace ui
}  // namespace fs
