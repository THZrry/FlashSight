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
#include <set>

#include "core/settings_store.hpp"
#include "fs_highlight.hpp"
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

// i18n: 源码写中文，) 按当前语言查表（中文时原样返回）
std::string tr(const std::string& zh) { return i18n::tr(zh); }

// Logs a backend event whose handler (state update + render) took long enough
// to be felt as a freeze. The user's report is precise: the window froze right
// after a tool call, and the system followed seconds later. If the UI thread
// blocks inside a tool event, this names the event and the duration instead of
// leaving another "it froze" with nothing to go on. Opt-in: FS_RES_LOG=1, same
// file as the periodic resource samples.
class SlowEventLog {
 public:
  explicit SlowEventLog(const std::string& name) : name_(name), t0_(mono_ms()) {}
  ~SlowEventLog() {
    int64_t dt = mono_ms() - t0_;
    if (dt < 500) return;
    if (!std::getenv("FS_RES_LOG")) return;
    const char* tmp = std::getenv("TEMP");
    std::string path =
        std::string(tmp && *tmp ? tmp : ".") + "\\flashsight_res.log";
    if (FILE* f = std::fopen(path.c_str(), "a")) {
      std::fprintf(f, "[slow] event=%s ms=%lld\n", name_.c_str(),
                   static_cast<long long>(dt));
      std::fclose(f);
    }
  }

 private:
  static int64_t mono_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
  }
  std::string name_;
  int64_t t0_;
};

// Normalise a shortcut string for comparison: trim, drop spaces around "+",
// lowercase the key name. "Ctrl+W", "ctrl+w" and "Ctrl + W" are the same binding.
std::string normalize_shortcut(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    if (c == ' ') continue;
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    out += c;
  }
  return out;
}

std::string itos(int v) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%d", v);
  return std::string(buf);
}

// Cut text for DISPLAY at a UTF-8 character boundary. Half a CJK character is
// invalid UTF-8, and these strings go straight to the DOM adapter (which
// re-encodes them), so a cut inside a sequence paints a replacement glyph.
std::string cut_display(const std::string& s, size_t max_bytes,
                        const std::string& suffix) {
  if (s.size() <= max_bytes) return s;
  size_t cut = max_bytes;
  while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
  return s.substr(0, cut) + suffix;
}

// Token counts are read at a glance, not compared digit by digit: 12.4K is
// more useful in a 360px panel than 12437.
std::string fmt_tokens(int64_t n) {
  char buf[32];
  if (n >= 1000000) {
    std::snprintf(buf, sizeof(buf), "%.1fM", static_cast<double>(n) / 1000000.0);
  } else if (n >= 1000) {
    std::snprintf(buf, sizeof(buf), "%.1fK", static_cast<double>(n) / 1000.0);
  } else {
    std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(n));
  }
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
    // One subagent's full transcript. A `multi` kind: you may want several open
    // (two running agents side by side). Not listed in the "+" picker (it needs
    // an agent id) - it is opened from a card or a tool row.
    {"subagent", "子代理记录", "单个子代理的完整记录", true},
    {"files", "文件", "项目文件树（可多开）", true},
    // A preview is its OWN tab, not an inline block under the tree: reading a
    // file and browsing the tree are two things you want to see at once, and an
    // inline block pushed the tree out of view.
    {"preview", "预览", "文件内容（可多开）", true},
    {"git", "Git", "Git 状态与变更（可多开）", true},
    {"workflow", "工作流", "长任务的分步执行状态", false},
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
  // ) the label: the panel TITLE (top of the side tab) is user-visible text
  // exactly like the picker card's label, and it used to stay Chinese in the
  // English UI while the card it was opened from was translated.
  return k ? std::string(tr(k->title)) : kind;
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
  // The mainstream endpoints added later: each has a lobe brand mark, so the
  // template grid stays scannable instead of showing a bare first letter.
  if (id == "groq") return "groq";
  if (id == "mistral") return "mistral";
  if (id == "xai") return "xai";
  if (id == "together") return "together";
  if (id == "fireworks") return "fireworks";
  if (id == "cerebras") return "cerebras";
  if (id == "minimax") return "minimax";
  if (id == "stepfun") return "stepfun";
  if (id == "volcengine") return "bytedance";
  return std::string();  // "custom" and anything unknown: letter fallback
}

// Brand mark for a SAVED provider, derived from its endpoint (then its name).
//
// The template id is gone once the provider is created, so the list could not use
// it: a provider added from the 模板 grid showed no logo at all ("新增的供应商没
// 图片"). The endpoint is stable and is what identifies the vendor everywhere else
// (the balance probe uses the same trick).
std::string provider_brand_for_endpoint(const std::string& base_url,
                                        const std::string& name) {
  std::string s = base_url;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] >= 'A' && s[i] <= 'Z') s[i] = static_cast<char>(s[i] - 'A' + 'a');
  }
  std::string n = name;
  for (size_t i = 0; i < n.size(); ++i) {
    if (n[i] >= 'A' && n[i] <= 'Z') n[i] = static_cast<char>(n[i] - 'A' + 'a');
  }
  auto has = [](const std::string& h, const char* k) {
    return h.find(k) != std::string::npos;
  };
  if (has(s, "deepseek")) return "deepseek";
  if (has(s, "moonshot") || has(s, "kimi")) return "moonshot";
  if (has(s, "openrouter")) return "openrouter";
  if (has(s, "siliconflow")) return "siliconcloud";
  if (has(s, "bigmodel") || has(s, "zhipu")) return "zhipu";
  if (has(s, "dashscope") || has(s, "aliyun")) return "alibaba";
  if (has(s, "openai.azure")) return "azure";
  if (has(s, "openai.com")) return "openai";
  if (has(s, "anthropic")) return "anthropic";
  if (has(s, "generativelanguage") || has(s, "gemini")) return "gemini";
  if (has(s, "groq.com")) return "groq";
  if (has(s, "mistral.ai")) return "mistral";
  if (has(s, "x.ai")) return "xai";
  if (has(s, "together")) return "together";
  if (has(s, "fireworks")) return "fireworks";
  if (has(s, "cerebras")) return "cerebras";
  if (has(s, "minimax")) return "minimax";
  if (has(s, "stepfun")) return "stepfun";
  if (has(s, "volces") || has(s, "volcengine")) return "bytedance";
  if (has(s, ":11434")) return "ollama";
  if (has(s, ":1234")) return "lmstudio";
  if (has(s, "opencode")) return "opencode";
  // Fall back to the NAME: a gateway or a renamed provider still says what it is.
  if (has(n, "deepseek")) return "deepseek";
  if (has(n, "claude") || has(n, "anthropic")) return "anthropic";
  if (has(n, "gpt") || has(n, "openai")) return "openai";
  if (has(n, "gemini")) return "gemini";
  if (has(n, "qwen") || has(n, "通义")) return "qwen";
  return std::string();
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

// EPOCH milliseconds (not the monotonic `now_ms()` above). Anything compared with
// a timestamp that came from the BACKEND - a session's updatedAt, a turn's ts -
// must use this: steady_clock starts at boot, so comparing it with epoch values
// silently never matches (the "the blue dot never clears" bug).
int64_t wall_ms() {
  return static_cast<int64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

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
  // The FIRST panel is the one on screen. Leaving active_side empty showed the
  // usage body (the kind falls back to "usage") with NO tab highlighted - it
  // looked like "nothing is open but stats are showing".
  v.active_side = v.side_tabs[0].id;
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
// 所以模型可以有自己的档位名而不被这里限制住。已知档位走 tr()：中文是键，
// 英文界面靠 kEnTable 查表，否则英文 UI 里这里会一直蹦中文。
std::string thinking_label(const std::string& v) {
  if (v == "off" || v == "disable" || v == "disabled") return tr("关");
  if (v == "on" || v == "enable" || v == "enabled") return tr("开");
  if (v == "minimal") return tr("最低");
  if (v == "low") return tr("低");
  if (v == "medium") return tr("中");
  if (v == "high") return tr("高");
  if (v == "xhigh" || v == "extra-high" || v == "extra_high") return tr("超高");
  if (v == "max") return tr("最大");
  if (v == "auto") return tr("自动");
  return v;
}

// FontAwesome icon for a thinking level: ONE distinct glyph per level.
//
// Every name must exist in the subset (web/icons/fa-subset.css). The first
// version returned `brain` for every level except "off", so 低/中/高 all looked
// identical - "图标也只有关闭和其他". Each level now gets its own image, and an
// unknown level name gets a generic bulb: we know it IS a level, just not one we
// have a picture for ("未知的选项就用个通用的灯泡或大脑吧").
const char* thinking_icon(const std::string& v) {
  if (v == "off" || v == "disable" || v == "disabled" || v == "none") {
    return "power-off";
  }
  if (v == "minimal") return "feather";
  if (v == "low") return "gauge";
  if (v == "medium") return "brain";
  if (v == "high") return "fire";
  if (v == "xhigh" || v == "extra-high" || v == "extra_high") {
    return "gauge-high";
  }
  if (v == "max") return "infinity";
  if (v == "auto") return "wand-magic-sparkles";
  if (v == "on" || v == "enable" || v == "enabled") return "brain";
  return "lightbulb";  // unknown level: generic, never blank
}

// 相对时间（"3 分钟前"）。列表里给一个大致时间就够了，绝对时间戳反而要花力气
// 去读；只算到"天"，再久就显示日期。
//
// 注意基准：会话时间来自后端 full.json 的 mtime（真实墙上时间），而这里的
// now_ms() 是**单调时钟**（steady_clock / performance.now()），两者原点不同。
// 所以这里用系统墙上时间做差，不能拿 now_ms() 去减 mtime，否则会算出天量偏差。
// 时长（"1 分 12 秒"）：结束时间减去开始时间；还没结束时用 now。
std::string session_duration(int64_t from_ms, int64_t to_ms) {
  if (from_ms <= 0) return std::string();
  int64_t end = to_ms > 0 ? to_ms : static_cast<int64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
  int64_t d = end - from_ms;
  if (d < 0) d = 0;
  int64_t sec = d / 1000;
  if (sec < 60) return itos(static_cast<int>(sec)) + tr(" 秒");
  int64_t min = sec / 60;
  if (min < 60) {
    int64_t rem = sec % 60;
    return itos(static_cast<int>(min)) + tr(" 分 ") + itos(static_cast<int>(rem)) + tr(" 秒");
  }
  return itos(static_cast<int>(min / 60)) + tr(" 小时 ") +
         itos(static_cast<int>(min % 60)) + tr(" 分");
}

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

// Defined next to the plan/todo parsers further down; load_session is above it
// and needs the declaration here.
std::vector<AppState::TodoItem> parse_todo_items(const Json& items);

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
  s.active_side = s.side_tabs[0].id;
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
  // Flush a delta that the paint floor held back. Deltas normally paint from the
  // SSE frame itself (see apply_session_event) - this is only the tail of a
  // burst, and it is the last paint of a streaming turn.
  if (stream_dirty_) {
    stream_dirty_ = false;
    render();
    follow_history_if_pinned();
  }
  // A subagent's live preview is painted here, not per delta (see
  // apply_session_event): hundreds of deltas per second would each trigger a
  // full re-render and freeze the app.
  if (subagent_stream_dirty_) {
    subagent_stream_dirty_ = false;
    render();
  }
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
  // Countdowns in the wake bar are only correct if they are refreshed. The
  // interval is deliberately coarse: this is a "5 分钟后" label, not a clock.
  if (!st_.pending_wakes.empty()) {
    int64_t now = now_ms();
    if (now - last_wake_refresh_ms_ > 5000) {
      last_wake_refresh_ms_ = now;
      refresh_pending_wakes();
    }
  }
  // Live turn timer. A streaming message shows "how long has this been running",
  // which only moves if something repaints it. Once a second is enough for a
  // counter and cheap: it rebuilds the ONE streaming message, not the transcript.
  {
    int64_t now = now_ms();
    if (now - last_stream_tick_ms_ >= 1000) {
      for (size_t i = 0; i < st_.messages.size(); ++i) {
        if (st_.messages[i].streaming) {
          last_stream_tick_ms_ = now;
          st_.messages[i].now_ms = now;
          st_.messages[i].rev = next_msg_rev_++;
          render();
          break;
        }
      }
    }
  }
  // The project/nav session rows carry their own status (running / terminal /
  // auto round) and come from `session.list`. Nothing in the app refreshed that
  // list on the events that CHANGE it, so a project's dot stayed idle while its
  // agent worked or a terminal printed ("项目那里的小圆点没有反应"). Coalesced:
  // a fast turn emits dozens of events and each refresh is one request per
  // project.
  if (sessions_dirty_) {
    int64_t now = now_ms();
    if (now - sessions_refresh_ms_ >= 800) {
      sessions_dirty_ = false;
      sessions_refresh_ms_ = now;
      refresh_session_rows();
    }
  }
  // ---- status watchdog: "running" must never get stuck ----
  // The backend is the authority. If the snapshot (or this view) says something
  // is still running, re-read the rows every 2 s; when the turn has really ended
  // (a provider error, a parked loop, a missed event) the dot clears on its own.
  // Polling ONLY while something looks busy keeps the cost at zero when idle.
  {
    bool looks_running = st_.turn_running || st_.auto_round > 0;
    if (!looks_running) {
      for (size_t i = 0; i < st_.projects.size() && !looks_running; ++i) {
        for (size_t k = 0; k < st_.projects[i].sessions.size(); ++k) {
          if (st_.projects[i].sessions[k].running) {
            looks_running = true;
            break;
          }
        }
      }
    }
    int64_t now = now_ms();
    if (looks_running && now - status_poll_ms_ >= 2000) {
      status_poll_ms_ = now;
      refresh_session_rows();
      if (!st_.active_tab.empty() && st_.active_tab != "home") {
        refresh_loop_state(st_.active_tab);
      }
    }
  }
  // The 操作记录 panel must follow the log while it is on screen. Coarse (2 s):
  // it is a log, and each refresh is a session.info round trip.
  {
    std::string kind;
    for (size_t i = 0; i < st_.side_tabs.size(); ++i) {
      if (st_.side_tabs[i].id == st_.active_side) kind = st_.side_tabs[i].kind;
    }
    if (kind == "ops" && !st_.side_collapsed) {
      int64_t now = now_ms();
      if (now - last_ops_refresh_ms_ >= 2000) {
        last_ops_refresh_ms_ = now;
        refresh_session_ops();
      }
    }
  }
  // A proposed / advancing workflow: refresh the panel (and the runs the panel
  // reads) shortly after the event, so a plan appears without reopening it.
  if (workflows_dirty_) {
    int64_t now = now_ms();
    if (now - workflows_refresh_ms_ >= 1200) {
      workflows_dirty_ = false;
      workflows_refresh_ms_ = now;
      if (!st_.active_tab.empty() && st_.active_tab != "home") {
        load_workflows(st_.active_tab);
      }
    }
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
        if (st_.terminals[i].status == "running") {
          load_terminal_grid(st_.term_tab);
        } else if (term_replay_done_.find(st_.term_tab) == term_replay_done_.end()) {
          // A RESTORED terminal (a record from a previous run) has a fixed
          // replay: fetch it ONCE. Without this the screen said "暂无输出" until
          // you clicked the tab - the record existed but nothing ever asked for
          // its content ("列表确实没有终端内容，点进去才有").
          term_replay_done_.insert(st_.term_tab);
          load_terminal_grid(st_.term_tab);
        }
        return;
      }
    }
    return;
  }
  // overview: round-robin over the running terminals
  if (st_.terminals.empty()) return;
  size_t n = st_.terminals.size();
  bool any_running = false;
  for (size_t k = 0; k < n; ++k) {
    size_t i = (term_grid_rr_ + k) % n;
    if (st_.terminals[i].status != "running") continue;
    any_running = true;
    term_grid_rr_ = (i + 1) % n;
    load_terminal_grid(st_.terminals[i].tid);
    return;
  }
  // Nothing is running: give the restored ones their one replay fetch each, so
  // the overview cards are not all "暂无输出".
  if (any_running) return;
  for (size_t k = 0; k < n; ++k) {
    const std::string& tid = st_.terminals[k].tid;
    if (term_replay_done_.find(tid) != term_replay_done_.end()) continue;
    term_replay_done_.insert(tid);
    load_terminal_grid(tid);
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
    // The terminal drawer can be opened on a terminal that has never been
    // clicked this session (a restored record, or one the model started): its
    // screen was empty until you re-selected it. Pull the grid for the tab that
    // would be shown.
    if (st_.terminal_open && !st_.term_tab.empty() && st_.term_tab != "all" &&
        st_.term_grids.find(st_.term_tab) == st_.term_grids.end()) {
      load_terminal_grid(st_.term_tab);
    }
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
  v.hist_scroll = st_.hist_scroll;
  v.hist_follow = st_.hist_follow;
  // Anchor to a MESSAGE, not only to pixels: when the view is rebuilt (a
  // reopened session, a page of older turns arriving) the content above can
  // change height, and a pixel offset then lands anywhere. The adapter can tell
  // which message is at the top; stores that message's stable uid so the
  // restore can scroll IT into view. -1 (unsupported adapter) keeps the pixel
  // path as the fallback.
  v.hist_anchor_uid = 0;
  if (!st_.hist_follow) {
    VNodePtr hist = find_by_key(tree_, "chat-history");
    if (hist && hist->dom.valid()) {
      int idx = dom_.scroll_anchor(hist->dom);
      if (idx >= 0 && static_cast<size_t>(idx) < st_.messages.size()) {
        v.hist_anchor_uid = st_.messages[static_cast<size_t>(idx)].uid;
      }
    }
  }
  v.history_total = st_.history_total;
  v.history_start = st_.history_start;
  v.history_has_more = st_.history_has_more;
  v.history_loading_older = false;  // an in-flight page belongs to the old view
  v.session_input = st_.session_input;
  v.edit_turn_id = st_.edit_turn_id;
  v.model = st_.model;
  v.model_index = st_.model_index;
  v.model_available = st_.model_available;
  v.model_thinking_levels = st_.model_thinking_levels;
  v.model_thinking_default = st_.model_thinking_default;
  v.model_provider_label = st_.model_provider_label;
  v.permission = st_.permission;
  v.perm_index = st_.perm_index;
  v.auto_mode = st_.auto_mode;
  v.thinking = st_.thinking;
  v.attachments = st_.attachments;
  v.suspended = st_.suspended;
  v.turn_running = st_.turn_running;
  v.interrupted = st_.interrupted;
  v.interrupt_reason = st_.interrupt_reason;
  v.pending_wakes = st_.pending_wakes;
  v.schedule_open = st_.schedule_open;
  v.schedule_text = st_.schedule_text;
  v.enter_send = st_.enter_send;
  v.context_pct = st_.context_pct;
  v.context_tokens = st_.context_tokens;
  v.context_window = st_.context_window;
  v.cache_hit_pct = st_.cache_hit_pct;
  v.session_cache_tokens = st_.session_cache_tokens;
  v.cache_reported = st_.cache_reported;
  v.session_cost_kind = st_.session_cost_kind;
  v.usage_calls = st_.usage_calls;
  v.session_cost = st_.session_cost;
  v.session_tokens_in = st_.session_tokens_in;
  v.session_tokens_out = st_.session_tokens_out;
  v.session_usage_loaded = st_.session_usage_loaded;
  v.turn_cost_micros = st_.turn_cost_micros;
  v.turn_in = st_.turn_in;
  v.turn_out = st_.turn_out;
  v.turn_cost_kind = st_.turn_cost_kind;
  v.side_tabs = st_.side_tabs;
  v.active_side = st_.active_side;
  v.side_seq = st_.side_seq;
  v.side_menu_open = st_.side_menu_open;
  v.side_width = st_.side_width;
  v.active_pid = st_.active_pid;
  v.session_ops = st_.session_ops;
  v.skill_menu_open = st_.skill_menu_open;
  v.command_menu = st_.command_menu;
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
  v.todos = st_.todos;
  v.todo_open = st_.todo_open;
}

void UiApp::load_session_view(const std::string& sid) {
  SessionView v = default_session_view();
  views_.restore(sid, &v);  // keeps the defaults when the key is new
  // A view can exist WITHOUT the defaults: `views_.get_or_create(sid)` (used for
  // background events and when stashing) constructs a raw SessionView, and
  // restoring that one left the side panel with no tabs and no active tab - it
  // then showed the usage body with nothing highlighted ("默认什么都不开却显示
  // 用量统计"). Put the defaults back where they are missing.
  if (v.side_tabs.empty()) {
    SessionView d = default_session_view();
    v.side_tabs = d.side_tabs;
    if (v.side_seq <= 0) v.side_seq = d.side_seq;
  }
  if (v.active_side.empty()) v.active_side = v.side_tabs[0].id;
  st_.messages = v.messages;
  st_.session_loaded = v.session_loaded;
  st_.hist_scroll = v.hist_scroll;
  st_.hist_anchor_uid = v.hist_anchor_uid;
  st_.hist_follow = v.hist_follow;
  st_.history_total = v.history_total;
  st_.history_start = v.history_start;
  st_.history_has_more = v.history_has_more;
  st_.history_loading_older = false;
  st_.session_input = v.session_input;
  st_.edit_turn_id = v.edit_turn_id;
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
  st_.model_available = v.model_available;
  st_.model_thinking_levels = v.model_thinking_levels;
  st_.model_thinking_default = v.model_thinking_default;
  st_.model_provider_label = v.model_provider_label;
  st_.permission = v.permission;
  st_.perm_index = v.perm_index;
  st_.auto_mode = v.auto_mode;
  st_.thinking = v.thinking;
  st_.attachments = v.attachments;
  st_.suspended = v.suspended;
  st_.turn_running = v.turn_running;
  st_.suspend_pending = false;  // a request in flight does not survive a switch
  st_.interrupted = v.interrupted;
  st_.interrupt_reason = v.interrupt_reason;
  st_.pending_wakes = v.pending_wakes;
  st_.schedule_open = v.schedule_open;
  st_.schedule_text = v.schedule_text;
  st_.enter_send = v.enter_send;
  st_.context_pct = v.context_pct;
  st_.context_tokens = v.context_tokens;
  st_.context_window = v.context_window;
  st_.cache_hit_pct = v.cache_hit_pct;
  st_.session_cache_tokens = v.session_cache_tokens;
  st_.cache_reported = v.cache_reported;
  st_.session_cost_kind = v.session_cost_kind;
  st_.usage_calls = v.usage_calls;
  st_.session_cost = v.session_cost;
  st_.session_tokens_in = v.session_tokens_in;
  st_.session_tokens_out = v.session_tokens_out;
  st_.session_usage_loaded = v.session_usage_loaded;
  st_.turn_cost_micros = v.turn_cost_micros;
  st_.turn_in = v.turn_in;
  st_.turn_out = v.turn_out;
  st_.turn_cost_kind = v.turn_cost_kind;
  st_.side_tabs = v.side_tabs;
  st_.active_side = v.active_side;
  st_.side_seq = v.side_seq;
  st_.side_menu_open = false;  // transient popovers never survive a switch
  st_.side_width = v.side_width;
  st_.active_pid = v.active_pid;
  st_.session_ops = v.session_ops;
  st_.skill_menu_open = false;
  st_.thinking_menu_open = false;
  st_.command_menu = v.command_menu;
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
  st_.todos = v.todos;
  st_.todo_open = v.todo_open;
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
  // Save the chat scroll offset BEFORE stashing the view: the payload is what
  // restore() puts back, so reading it later would store the default.
  //
  // This is a DELIBERATE read, at a moment the user chose to leave the tab, so it
  // is not guarded by restoring_scroll_: that guard exists for the scroll EDGE
  // handler, whose events also fire while a restore is driving the DOM. Guarding
  // here would instead drop the position the user actually scrolled to.
  {
    VNodePtr h = find_by_key(tree_, "chat-history");
    if (h && h->dom.valid()) st_.hist_scroll = dom_.scroll_top(h->dom);
  }
  if (!leaving.empty()) save_session_view(leaving);
  // Any open transient popup belongs to the tab being left. Leaving it up is how
  // the model picker "stayed glued" across a tab switch.
  st_.model_picker_open = false;
  st_.skill_menu_open = false;
  st_.thinking_menu_open = false;
  st_.side_menu_open = false;
  // A context menu belongs to what was right-clicked; navigating away leaves
  // nothing for it to act on.
  st_.ctx_open = false;
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
  // The user is looking at this tab now: its "new output" dot is spent.
  for (size_t i = 0; i < st_.sessions.size(); ++i) {
    if (st_.sessions[i].id == sid) {
      st_.sessions[i].dirty = false;
      break;
    }
  }
  // ...and the "finished / failed" dot on its session row is spent too - it
  // means "something happened since you last looked", which stops being true
  // the moment you look.
  session_seen_ms_[sid] = wall_ms();
  if (session_seen_ms_.size() > 512) session_seen_ms_.clear();
  // ...and persist it: the red failed dot must not return after a restart.
  call_async("session.markSeen", Json{{"sid", sid}, {"ms", session_seen_ms_[sid]}});
  // Re-read the rows' status after a switch: which session is RUNNING is a
  // backend fact, and the snapshot in hand was taken before the switch. Without
  // this, opening another session made the still-running one's indicator look
  // gone until some later event happened to refresh the list.
  mark_sessions_dirty();
  // A returning visit already HAS its transcript in the view, so the render below
  // can position it in the same frame it paints - no flash of the top of the chat
  // while the refresh is in flight. (The refresh then lands and does the same
  // again with the final content; that is idempotent.)
  request_history_scroll_restore();
  render();
  // Scroll restore is NOT done here. The transcript has not been fetched yet, so
  // the container is empty: a programmatic scroll to the saved offset is clamped
  // to 0, and the scroll handler then wrote that 0 back over the saved position -
  // so the SECOND visit to a session landed at the top ("重新点进来却变成顶上").
  // load_session() applies the offset once the messages exist.
  if (st_.messages.empty() && st_.hist_scroll < 0) {
    // First visit of a never-loaded session: start at the bottom.
    st_.hist_follow = true;
  }
  // "Has a stashed view" is NOT the same as "its history was fetched": leaving
  // a tab always stashes a default view, so this used to skip the fetch on the
  // way back to a session that had never loaded (the composer then showed an
  // empty model and no messages). Ask the restored view itself.
  //
  // Re-fetching on every visit is deliberate: the message list is the truth on
  // disk, and another tab (or the model itself) may have appended turns since
  // this view was stashed. The render is a no-op when nothing changed.
  bool loaded = views_.get_or_create(sid).loaded;
  (void)had_view;
  (void)loaded;
  load_session(sid);
  // The side panel was restored from the stashed view, which is as old as the
  // last visit. Re-read whatever it shows: "切标签页还是旧数据" was the restored
  // usage/ops/files/git panel never asking the backend again.
  if (!st_.side_collapsed && !st_.active_side.empty()) load_side_tab(st_.active_side);
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

// Turn JSON -> ChatMsg. `abs_index` is the turn's position in the STORED
// session; it becomes the message's stable identity, so paging backwards does
// not renumber (and therefore rebuild) what is already on screen.
//
// Loaded turns are FROZEN: each gets one revision and never changes again,
// which is what makes reopening a long session cheap (the render cache serves
// every message instead of re-parsing it).
comp::ChatMsg UiApp::turn_to_message(const Json& t, int64_t abs_index) {
  comp::ChatMsg m;
  m.uid = abs_index;
  m.turn_id = jsonutil::get<std::string>(t, "turnId", "");
  m.role = jsonutil::get<std::string>(t, "role", "");
  // An auto-continue round is stored as a user turn whose promptRaw is the
  // system's "Continue." nudge. Rendering that as a user bubble put words in the
  // user's mouth and glued the auto round onto the previous exchange - the
  // "把自动轮的历史和上一轮放到一起" report. Marked rounds render as a system
  // marker instead (divider + round number).
  int auto_round = static_cast<int>(jsonutil::get_int(t, "autoRound", 0));
  bool is_system_turn = jsonutil::get<bool>(t, "system", false);
  if (auto_round > 0 && m.role == "user") {
    m.role = "system";
    m.text = tr("自动继续 · 第 ") + itos(auto_round) + tr(" 轮");
    m.started_at = jsonutil::get_int(t, "ts", 0);
    m.rev = next_msg_rev_++;
    return m;
  }
  if (is_system_turn && m.role == "user") m.role = "system";
  // A stored turn uses a different field per role: a user turn keeps the prompt
  // in `promptRaw`, an assistant turn keeps the reply in `streamSummary`. Reading
  // only `text`/`streamSummary` made every reloaded USER message empty - the
  // transcript showed the answers with no questions above them.
  m.text = jsonutil::get<std::string>(t, "text", "");
  if (m.text.empty()) m.text = jsonutil::get<std::string>(t, "promptRaw", "");
  if (m.text.empty()) m.text = jsonutil::get<std::string>(t, "streamSummary", "");
  // Reasoning is part of the stored turn when the provider returned it.
  m.thinking = jsonutil::get<std::string>(t, "thinking",
                 jsonutil::get<std::string>(t, "reasoning", ""));
  // A turn that stopped on an error was persisted WITH the reason: the reload
  // must say why the answer ended instead of showing a silently clipped reply.
  m.error = jsonutil::get<std::string>(t, "error", "");
  m.started_at = jsonutil::get_int(t, "startedAt", 0);
  m.ended_at = jsonutil::get_int(t, "ts", 0);
  m.rev = next_msg_rev_++;
  // Tool calls become the flow blocks under the message: without this a
  // reloaded session lost every "read file / edit / diff" block and looked
  // like the model answered out of thin air.
  if (m.role == "assistant" && t.contains("toolCalls") && t["toolCalls"].is_array()) {
    const Json& tc = t["toolCalls"];
    for (size_t k = 0; k < tc.size(); ++k) {
      comp::ToolBlock tb;
      tb.name = jsonutil::get<std::string>(tc[k], "name", "");
      // Prefer the stored, already-summarised detail: `args` alone says what was
      // asked for, not what came back. Older turns only have args, so fall back.
      std::string detail = jsonutil::get<std::string>(tc[k], "detail", "");
      tb.summary = jsonutil::get<std::string>(tc[k], "summary", "");
      tb.detail = detail;
      // The call's INPUT is its own section (the block used to show only the
      // result). An empty object carries no information - skip it.
      if (tc[k].contains("args") && tc[k]["args"].is_object() &&
          !tc[k]["args"].empty()) {
        // The stored args are NOT truncated (the model needs them whole), but
        // the transcript does not: a write_file carries the entire new file as
        // its input, and building a message node around a 200KB string on every
        // repaint is pure waste. Display-only cap.
        tb.input = cut_display(dump_safe(tc[k]["args"], 2), 6000,
                               "\n… (输入已截断)");
      }
      if (detail.empty()) detail = tb.input;
      tb.detail = detail;
      tb.ok = jsonutil::get<bool>(tc[k], "ok", true);
      // A subagent tool call must keep its id across a reload, or the "打开记录"
      // button disappears the moment the session is reopened - which is exactly
      // "子代理还是无法在历史记录里打开". The live path reads the id from the tool
      // RESULT; the stored turn keeps that same result in `detail`, so recover it
      // the same way (the id is also written as a first-class field for turns
      // saved after this change).
      if (tb.name == "subagent_spawn" || tb.name == "subagent_follow_up") {
        tb.subagent_id = jsonutil::get<std::string>(tc[k], "subagentId", "");
        if (tb.subagent_id.empty()) {
          try {
            Json d = Json::parse(detail);
            tb.subagent_id = jsonutil::get<std::string>(d, "id", "");
          } catch (...) {
          }
        }
      }
      if (!tb.name.empty()) m.tools.push_back(tb);
    }
  }
  if (m.role == "assistant" && t.contains("keyPoints") && t["keyPoints"].is_array()) {
    for (size_t k = 0; k < t["keyPoints"].size(); ++k) {
      if (!m.summary.empty()) m.summary += "\n";
      if (t["keyPoints"][k].is_string()) m.summary += t["keyPoints"][k].get<std::string>();
    }
  }
  // The model's between-tools narration (see ChatMsg::interim): stored with the
  // turn, rendered in the work log.
  if (m.role == "assistant" && t.contains("interim") && t["interim"].is_array()) {
    for (size_t k = 0; k < t["interim"].size(); ++k) {
      if (t["interim"][k].is_string()) {
        m.interim.push_back(t["interim"][k].get<std::string>());
      }
    }
    // Where each note sits relative to the tool calls. Older stored turns do not
    // have it; the stream then falls back to the note index.
    if (t.contains("interimAfter") && t["interimAfter"].is_array()) {
      for (size_t k = 0; k < t["interimAfter"].size(); ++k) {
        int64_t n = 0;
        if (t["interimAfter"][k].is_number()) n = t["interimAfter"][k].get<int64_t>();
        m.interim_after.push_back(static_cast<size_t>(n < 0 ? 0 : n));
      }
    }
  }
  return m;
}

// fetch a session's INFO + its newest page of history into its view.
//
// Only the newest page is loaded: a session can hold hundreds of turns and the
// screen shows the last few. Older turns are fetched on demand (load_older) and
// then behave exactly like the first page - frozen, cached, never re-parsed.
void UiApp::load_session(const std::string& sid) {
  if (sid.empty()) return;
  // A tab/page switch tore this session's DOM down. The cached message nodes and
  // memoised tool rows still point at those removed elements, so handing them
  // back leaves rows that CANNOT be clicked ("切页面后工具调用点不开"). Drop both
  // caches before the new page is built; re-rendering is the cost of correctness.
  {
    const std::string prefix = "msg:" + sid + ":";
    for (std::map<std::string, MsgNode>::iterator it = msg_nodes_.begin();
         it != msg_nodes_.end();) {
      if (it->first.compare(0, prefix.size(), prefix) == 0) {
        it = msg_nodes_.erase(it);
      } else {
        ++it;
      }
    }
  }
  comp::tool_block_cache_clear();
  // Without a backend there is nothing to fetch (UI tests, the demo shell). The
  // transcript is whatever the view already holds, so the scroll restore still has
  // to be applied - otherwise the "remembered position" path is exercised only in
  // production and the tests cannot cover it.
  if (!backend_) {
    request_history_scroll_restore();
    render();
    return;
  }
  // Two independent requests: the info (small, needed for the composer) and the
  // last page of turns. They land in either order.
  call_async("session.info", Json{{"sid", sid}}, [this, sid](const BackendResult& r) {
    if (!r.ok) return;
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
    SessionView& v = views_.get_or_create(sid);
    v.session_ops = ops;
    if (!pid.empty()) v.active_pid = pid;
    // The session's OWN settings (model / thinking / permission / auto mode) come
    // with the same reply. Without this the composer showed "choose model" on a
    // session that already had one, and sending used the wrong model.
    Json ss = r.data.contains("sessionSettings") && r.data["sessionSettings"].is_object()
                  ? r.data["sessionSettings"]
                  : Json::object();
    std::string m = jsonutil::get<std::string>(ss, "model", "");
    if (!m.empty()) v.model = m;
    v.thinking = jsonutil::get<std::string>(ss, "thinking", "");
    std::string perm = jsonutil::get<std::string>(ss, "filePerm", "");
    if (!perm.empty()) {
      v.permission = perm;
      v.perm_index = perm == "approve" ? 0 : perm == "full" ? 2 : 1;
    }
    if (ss.contains("autoMode")) v.auto_mode = jsonutil::get<bool>(ss, "autoMode", true);
    {
      std::string es = jsonutil::get<std::string>(ss, "enterSend", "enter");
      v.enter_send = es == "ctrlEnter" ? "ctrlEnter" : "enter";
    }
    if (st_.active_tab == sid) {
      st_.session_ops = ops;
      if (!pid.empty()) st_.active_pid = pid;
      st_.model = v.model;
      st_.thinking = v.thinking;
      st_.permission = v.permission;
      st_.perm_index = v.perm_index;
      st_.auto_mode = v.auto_mode;
      st_.enter_send = v.enter_send;
      // The wake list is per session, so it has to be re-read when a session is
      // opened - a scheduled task survives a reload and must still be visible.
      refresh_pending_wakes();
      // The remembered model may have been disabled or its provider deleted
      // while this session was closed.
      refresh_model_availability();
      // The stashed view remembers `turn_running` from whenever it was left. If
      // the process is still alive but the turn ended meanwhile (it failed), the
      // restored value put 中止 back on a stopped task and nothing ever corrected
      // it - the composer must ask the backend when a session is opened.
      refresh_loop_state(sid);
    }
    render();
  });
  // The model's checklist is host memory, not part of the stored turns: read it
  // when the session opens so the floating todo panel still shows a list written
  // before the app was started.
  call_async("session.todos", Json{{"sid", sid}}, [this, sid](const BackendResult& r) {
    if (!r.ok) return;
    std::vector<AppState::TodoItem> list =
        parse_todo_items(r.data.contains("items") ? r.data["items"] : Json::array());
    if (st_.active_tab == sid) {
      bool was_empty = st_.todos.empty();
      st_.todos = list;
      if (!list.empty() && was_empty) st_.todo_open = true;
      render();
    } else {
      SessionView& v = views_.get_or_create(sid);
      if (v.todos.empty() && !list.empty()) v.todo_open = true;
      v.todos = list;
    }
  });
  call_async("session.turns", Json{{"sid", sid}, {"before", -1}, {"limit", kHistoryPage}},
             [this, sid](const BackendResult& r) {
    if (!r.ok) return;
    const Json& turns = r.data.contains("turns") ? r.data["turns"] : Json::array();
    std::vector<comp::ChatMsg> msgs;
    int start = static_cast<int>(jsonutil::get_int(r.data, "start", 0));
    for (size_t i = 0; i < turns.size(); ++i) {
      comp::ChatMsg m = turn_to_message(turns[i], start + static_cast<int64_t>(i));
      // `system` markers (auto-continue round dividers) are kept: dropping them
      // merged the auto round into the previous exchange on reload.
      if (m.role == "user" || m.role == "assistant" || m.role == "system") {
        msgs.push_back(m);
      }
    }
    int total = static_cast<int>(jsonutil::get_int(r.data, "total", msgs.size()));
    bool has_more = jsonutil::get<bool>(r.data, "hasMore", false);
    // write into the session's view; mirror to the flat fields only when the
    // user is still looking at it (a slow reply must not clobber another tab)
    SessionView& v = views_.get_or_create(sid);
    v.loaded = true;
    v.messages = msgs;
    v.history_total = total;
    v.history_start = start;
    v.history_has_more = has_more;
    v.history_loading_older = false;
    if (st_.active_tab == sid) {
      // A reply describes the session as it was ON DISK when the request was
      // made - which can be older than what the user is looking at: a send
      // appends its message optimistically, and the model's deltas append more.
      // Assigning the reply wholesale therefore ERASED a just-sent message (the
      // transcript "did nothing" until a page switch reloaded it from disk).
      // Keep the local messages that the reply does not know about: no stored
      // turn id, and their text is not already in the reply.
      std::vector<comp::ChatMsg> keep;
      for (size_t i = 0; i < st_.messages.size(); ++i) {
        const comp::ChatMsg& lm = st_.messages[i];
        if (!lm.turn_id.empty()) continue;
        bool seen = false;
        for (size_t k = msgs.size(); k-- > 0;) {
          if (msgs[k].role == lm.role && msgs[k].text == lm.text) {
            seen = true;
            break;
          }
        }
        if (!seen) keep.push_back(lm);
      }
      st_.messages = msgs;
      for (size_t i = 0; i < keep.size(); ++i) st_.messages.push_back(keep[i]);
      st_.history_total = total;
      st_.history_start = start;
      st_.history_has_more = has_more;
      st_.history_loading_older = false;
      // the page's animation replay token: the page animates again now that
      // there is actually something to look at (a slow load used to play the
      // animation over an empty page, so it was over before content arrived)
      st_.session_loaded = true;
      // A turn may still be streaming in the backend: the disk transcript ends
      // at the last FINISHED turn, so a tab closed mid-turn and reopened here
      // would show nothing of the answer that is being written right now.
      attach_live_turn(sid);
      // The history just arrived, so whatever we scrolled to before was either
      // an empty page or the previous content's height. Opening a session must
      // land on the latest message, and a return visit must land on the saved
      // offset. The restore is applied by THIS render, not a later tick: the
      // content is already in the tree when it patches, so waiting would show
      // the top of the transcript first and jump afterwards.
      //
      // ONE render, not two: the messages and the restore land in the same paint
      // instead of painting the transcript at the top and then repositioning it.
      request_history_scroll_restore();
      render();
    }
    // Terminals are NOT fetched here: the list can be long, and the session page
    // does not show it until the terminal drawer is opened. `load_terminals` runs
    // when that drawer opens (or after a spawn), which also keeps reopening a
    // session cheap.
  });
}

// Ask the NEXT render to position the transcript at the remembered offset before
// the browser paints it.
//
// Two earlier attempts both failed the same way: they set scrollTop in a LATER
// tick. By then the new page had already been painted at scrollTop 0, so a return
// visit showed the TOP of the transcript and jumped afterwards - and if the
// content had not been laid out yet the assignment was clamped to 0 and the
// resulting scroll event wrote that back over the saved value.
//
// The page is built from the view's own messages, so the content is IN THE TREE
// by the time render() has patched it. Reading the container's geometry forces
// layout right there, which means setting scrollTop in the same task is exact -
// the first paint is already at the right place. That is what "提前把布局完成"
// has to mean in practice: not a deferral, the same frame.
void UiApp::request_history_scroll_restore() {
  pending_scroll_apply_ = true;
  pending_scroll_target_ = st_.hist_scroll;
  pending_scroll_follow_ = st_.hist_follow;
  pending_anchor_uid_ = st_.hist_anchor_uid;
  restoring_scroll_ = true;
}

void UiApp::apply_pending_history_scroll() {
  if (!pending_scroll_apply_) return;
  pending_scroll_apply_ = false;
  VNodePtr h = find_by_key(tree_, "chat-history");
  if (h && h->dom.valid()) {
    // Force layout: the browser has not necessarily computed the height of the
    // content that was just inserted, and scrollTop is clamped against whatever
    // height it currently believes in. Reading the geometry is what makes the
    // assignment below exact instead of "the old, shorter page".
    //
    // NOTE (webview): those reads come from the MIRROR here, which is a frame
    // behind - the adapter sends the assignment as a separate script. See
    // request_history_scroll_restore() for why this is still applied in the same
    // task on the browser build and re-applied there for the native one.
    int sh = dom_.scroll_height(h->dom);
    int ch = dom_.client_height(h->dom);
    if (std::getenv("FS_UI_WEBVIEW_DEBUG")) {
      std::fprintf(stderr,
                   "[scroll] restore target=%d follow=%d height=%d client=%d\n",
                   pending_scroll_target_, static_cast<int>(pending_scroll_follow_),
                   sh, ch);
    }
    if (pending_scroll_follow_ || pending_scroll_target_ < 0) {
      // The user was at the END when they left (or this is a first visit), so
      // the end is what they must see - not the numeric offset they left,
      // which is now somewhere in the middle of a longer transcript. This is
      // the "history 大了进入停在顶上" report: the old code restored the saved
      // pixel offset even when the view was pinned to the bottom.
      dom_.scroll_to_bottom(h->dom);
      st_.hist_follow = true;
    } else if (pending_anchor_uid_ != 0) {
      // ANCHOR TO A MESSAGE. The transcript is rebuilt from the view, so the
      // height above the viewport can differ from when the offset was saved
      // (older pages loaded, tool blocks expanded); a pixel offset then points
      // at the wrong message. Scroll the SAVED message to the top instead.
      std::string key = comp::chat_message_key(st_.active_tab, pending_anchor_uid_);
      VNodePtr msg = find_by_key(tree_, key);
      if (msg && msg->dom.valid()) {
        dom_.scroll_into_view(msg->dom);
      } else {
        dom_.set_scroll_top(h->dom, pending_scroll_target_);
      }
    } else {
      dom_.set_scroll_top(h->dom, pending_scroll_target_);
    }
  }
  // The scroll event this causes is delivered asynchronously; ignore it for a
  // couple of ticks so it cannot overwrite the offset we just restored.
  dom_.schedule(0, [this]() {
    dom_.schedule(0, [this]() { restoring_scroll_ = false; });
  });
}

// Fetch the page BEFORE the oldest loaded turn and prepend it.
//
// The reading position is preserved by anchoring to the distance from the
// BOTTOM: prepending changes scrollHeight but not that distance, so restoring it
// keeps the same line under the reader's eye. (Anchoring to scrollTop would jump
// by exactly the height of the inserted content.)
void UiApp::load_older_history() {
  const std::string sid = st_.active_tab;
  if (sid.empty() || sid == "home" || st_.history_loading_older) return;
  if (!st_.history_has_more) return;
  int before = st_.history_start;
  if (before <= 0) return;
  st_.history_loading_older = true;
  // record the anchor while the layout still reflects the OLD content
  {
    VNodePtr h = find_by_key(tree_, "chat-history");
    if (h && h->dom.valid()) {
      int sh = dom_.scroll_height(h->dom);
      int st_top = dom_.scroll_top(h->dom);
      st_.history_anchor_from_bottom = sh > 0 ? (sh - st_top) : -1;
    }
  }
  call_async("session.turns",
             Json{{"sid", sid}, {"before", before}, {"limit", kHistoryPage}},
             [this, sid](const BackendResult& r) {
    if (!r.ok) {
      update([&](AppState& s) { s.history_loading_older = false; });
      return;
    }
    const Json& turns = r.data.contains("turns") ? r.data["turns"] : Json::array();
    std::vector<comp::ChatMsg> older;
    int start = static_cast<int>(jsonutil::get_int(r.data, "start", 0));
    for (size_t i = 0; i < turns.size(); ++i) {
      comp::ChatMsg m = turn_to_message(turns[i], start + static_cast<int64_t>(i));
      if (m.role == "user" || m.role == "assistant" || m.role == "system") {
        older.push_back(m);
      }
    }
    bool has_more = jsonutil::get<bool>(r.data, "hasMore", false);
    SessionView& v = views_.get_or_create(sid);
    int anchor = -1;
    if (st_.active_tab == sid) {
      anchor = st_.history_anchor_from_bottom;
      // prepend to the flat copy and to the stashed one, so a tab switch keeps it
      st_.messages.insert(st_.messages.begin(), older.begin(), older.end());
      st_.history_has_more = has_more;
      st_.history_start = start;
      st_.history_loading_older = false;
      st_.history_anchor_from_bottom = -1;
      v.messages = st_.messages;
      v.history_has_more = has_more;
      v.history_start = start;
      v.history_loading_older = false;
      render();
      if (anchor >= 0) {
        VNodePtr h = find_by_key(tree_, "chat-history");
        if (h && h->dom.valid()) {
          int sh = dom_.scroll_height(h->dom);
          dom_.set_scroll_top(h->dom, sh - anchor);
        }
      }
      // NOTE: rendering does NOT fire a scroll event, so no further page is
      // requested here; the next request only comes from a real user scroll.
    } else {
      // the user moved on: still keep the page, just do not disturb the view
      v.messages.insert(v.messages.begin(), older.begin(), older.end());
      v.history_has_more = has_more;
      v.history_start = start;
      v.history_loading_older = false;
    }
  });
}

void UiApp::load_sessions(size_t project_index) {
  if (project_index >= st_.projects.size()) return;
  std::string pid = st_.projects[project_index].pid;
  if (pid.empty()) return;
  // Key the reply by PID, not by the array index: the project list is re-sorted
  // by activity (a running session changes lastActive), so by the time this
  // reply lands the index may point at ANOTHER project - its session rows would
  // be overwritten (a row showing the wrong dots, or losing "running").
  call_async("session.list", Json{{"pid", pid}}, [this, pid](const BackendResult& r) {
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
      row.loop_state = jsonutil::get<std::string>(s, "loopState", "");
      row.last_error = jsonutil::get<std::string>(s, "lastError", "");
      row.terminal_count = static_cast<int>(jsonutil::get_int(s, "terminalCount", 0));
      row.auto_round = static_cast<int>(jsonutil::get_int(s, "autoRound", 0));
      if (row.auto_round > 0) row.running = true;  // background work → purple dot
      // "Seen" is a BACKEND fact now: the red failed dot must stay gone after the
      // app restarts (it used to live only in this process's map).
      int64_t seen_at = jsonutil::get_int(s, "seenAt", 0);
      if (!row.sid.empty() && seen_at > 0) {
        std::map<std::string, int64_t>::iterator sit = session_seen_ms_.find(row.sid);
        if (sit == session_seen_ms_.end() || sit->second < seen_at) {
          session_seen_ms_[row.sid] = seen_at;
        }
      }
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
      // Locate the project by PID at APPLY time (see the call above).
      size_t at = st.projects.size();
      for (size_t i = 0; i < st.projects.size(); ++i) {
        if (st.projects[i].pid == pid) { at = i; break; }
      }
      if (at < st.projects.size()) {
        // Keep an in-progress rename alive: a refresh (which happens after a
        // rename of ANOTHER row, or on any project reload) must not yank the
        // input out from under the user.
        std::string renaming_sid;
        std::string rename_text;
        for (size_t k = 0; k < st.projects[at].sessions.size(); ++k) {
          const ProjectCard::SessionRow& old = st.projects[at].sessions[k];
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
        st.projects[at].sessions = rows;
        st.projects[at].session_count = static_cast<int>(rows.size());
        st.projects[at].sessions_loaded = true;
      }
    });
  });
}

// Re-read every project's session rows (the source of the dots).
//
// Called from pump() only, and only when something that changes a dot happened.
// It deliberately does NOT reload the project list: pids and paths cannot change
// from a turn or a terminal, and re-fetching them would also rebuild the cards
// the user may be looking at.
void UiApp::refresh_session_rows() {
  for (size_t i = 0; i < st_.projects.size(); ++i) load_sessions(i);
}

// Re-read the 操作记录 (systemOps) list for the open session.
//
// The panel was filled once, when the session was opened, and never again: with
// the panel visible you could change permission/model/etc. and watch nothing
// appear. pump() calls this while the ops panel is the ACTIVE one, at a coarse
// interval - it is a log, not a live stream.
void UiApp::refresh_session_ops() {
  std::string sid = active_sid();
  if (sid.empty() || !backend_) return;
  call_async("session.info", Json{{"sid", sid}}, [this, sid](const BackendResult& r) {
    if (!r.ok) return;
    if (st_.active_tab != sid) return;  // another tab is in front now
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
    // Same list twice: skip the repaint (this runs on a timer).
    if (ops.size() == st_.session_ops.size()) {
      bool same = true;
      for (size_t i = 0; i < ops.size(); ++i) {
        if (ops[i].op != st_.session_ops[i].op ||
            ops[i].detail != st_.session_ops[i].detail) {
          same = false;
          break;
        }
      }
      if (same) return;
    }
    update([&](AppState& s) { s.session_ops = ops; });
  });
}

// The ACTIVE session's row, with the UI's live state taking precedence.
//
// The list is a snapshot from the backend (up to a second old), while
// `turn_running` / `terminals` change on the event that CAUSED them. Using the
// snapshot for the active row made the dot lag visibly right where the user is
// looking; this override makes it immediate, and the refresh catches the rest.
ProjectCard::SessionRow UiApp::live_session_row(ProjectCard::SessionRow row) const {
  if (row.sid.empty() || row.sid != st_.active_tab) return row;
  // LIVE states can only ADD information, never erase a backend fact. The old
  // `row.running = st_.turn_running` overwrote "the backend says this session is
  // running" with a UI-local flag that is false when the turn was started before
  // this view existed (opening a running session from the project list made its
  // 运行中 dot disappear - pure coupling, as the user put it).
  if (st_.turn_running || st_.auto_round > 0) row.running = true;
  if (st_.auto_round > 0) row.auto_round = st_.auto_round;
  // Terminal count from the LIVE list: opening one shows immediately, and a
  // closed one stops counting even before the next refresh.
  int running_terms = 0;
  for (size_t i = 0; i < st_.terminals.size(); ++i) {
    if (st_.terminals[i].status == "running") ++running_terms;
  }
  row.terminal_count = running_terms;
  // An interrupted turn is a failure the list cannot know about yet.
  if (st_.interrupted && !st_.interrupt_reason.empty()) {
    row.last_error = st_.interrupt_reason;
    row.running = false;
  }
  if (st_.suspended) row.running = false;
  return row;
}

// The dot for one session row.
//
// Two rules on top of the raw snapshot:
//   * the active session reads the LIVE state (see live_session_row);
//   * "已完成 / 失败" means NEWS since you last looked. The backend's loopState
//     stays "turnDone" forever after a turn, so the blue dot used to be permanent
//     - it survived opening the session AND switching tabs ("可查看了不会消失").
// (session_dot is defined further down with the other home builders.)
H session_dot(const std::string& loop_state, const std::string& last_error,
              bool running, int terminal_count, int auto_round);
H UiApp::dot_for(const ProjectCard::SessionRow& row) const {
  ProjectCard::SessionRow r = live_session_row(row);
  if (!r.running && !r.auto_round && !r.loop_state.empty() &&
      r.loop_state != "waiting") {
    std::map<std::string, int64_t>::const_iterator it =
        session_seen_ms_.find(r.sid);
    if (it != session_seen_ms_.end() && r.updated_ms <= it->second) {
      r.loop_state = "idle";
      r.last_error.clear();
    }
  }
  return session_dot(r.loop_state, r.last_error, r.running, r.terminal_count,
                     r.auto_round);
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

// Balance / credit per provider.
//
// Every vendor that exposes one uses a DIFFERENT private endpoint, so the backend
// probes by the configured endpoint and reports one row per provider: the number,
// or why there is none ("不支持" / "获取失败：HTTP 401"). The UI never invents a 0.
void UiApp::load_balances(bool force) {
  if (!backend_ || st_.balances_loading) return;
  int64_t now = now_ms();
  if (st_.balances_at > 0) {
    int64_t since = now - st_.balances_at;
    // "Refresh after every turn" must not become "hammer the vendor": a run of
    // short turns would fire one request per provider each time. The floor is
    // short enough that a normal turn always refreshes.
    if (!force && since < 30000) return;
    if (force && since < 8000) return;
  }
  update([&](AppState& s) { s.balances_loading = true; });
  call_async("provider.balances", Json::object(), [this](const BackendResult& r) {
    std::vector<AppState::BalanceRow> rows;
    if (r.ok && r.data.contains("providers") && r.data["providers"].is_array()) {
      const Json& arr = r.data["providers"];
      for (size_t i = 0; i < arr.size(); ++i) {
        AppState::BalanceRow row;
        row.pid = jsonutil::get<std::string>(arr[i], "pid", "");
        row.name = jsonutil::get<std::string>(arr[i], "name", "");
        row.endpoint = jsonutil::get<std::string>(arr[i], "endpoint", "");
        row.status = jsonutil::get<std::string>(arr[i], "status", "failed");
        row.currency = jsonutil::get<std::string>(arr[i], "currency", "");
        row.balance = jsonutil::get<std::string>(arr[i], "balance", "");
        row.used = jsonutil::get<std::string>(arr[i], "used", "");
        row.limit = jsonutil::get<std::string>(arr[i], "limit", "");
        row.note = jsonutil::get<std::string>(arr[i], "note", "");
        row.enabled = jsonutil::get<bool>(arr[i], "enabled", true);
        rows.push_back(row);
      }
    }
    update([&](AppState& s) {
      s.balances = rows;
      s.balances_loading = false;
      s.balances_at = now_ms();
    });
  });
}

// Per-session usage: the side panel's "context level / cache / this turn's cost
// / this session's cost". Every number is a FACT the provider reported:
//   * context level = the LAST request's prompt tokens / the model's context
//     window (from /models, or the model settings when the API does not say).
//     No estimate from the model name - a made-up denominator is worse than
//     showing nothing.
//   * cache hit = cached prompt tokens / prompt tokens, over the rows that
//     actually reported a cache field. -1 (not 0) when nobody reported one.
//   * cost / tokens = the usage rows recorded for this session.
void UiApp::load_session_usage(const std::string& sid) {
  if (sid.empty()) return;
  call_async("usage.session", Json{{"sid", sid}}, [this, sid](const BackendResult& r) {
    if (!r.ok) return;
    int64_t in = jsonutil::get_int(r.data, "totalIn", 0);
    int64_t out = jsonutil::get_int(r.data, "totalOut", 0);
    int64_t cache = jsonutil::get_int(r.data, "totalCache", 0);
    double cost = jsonutil::get_number(r.data, "totalCost", 0.0);
    std::string kind = jsonutil::get<std::string>(r.data, "costKind", "unknown");
    int64_t calls = jsonutil::get_int(r.data, "calls", 0);
    int64_t last_in = jsonutil::get_int(r.data, "lastIn", 0);
    int64_t window = jsonutil::get_int(r.data, "contextWindow", 0);
    double cache_hit = jsonutil::get_number(r.data, "cacheHit", -1.0);
    bool cache_reported = jsonutil::get<bool>(r.data, "cacheReported", false);
    int pct = -1;
    if (window > 0 && last_in > 0) {
      pct = static_cast<int>((last_in * 100) / window);
      // A real but tiny usage (a few thousand tokens in a 1M window) rounds to 0,
      // which reads the same as "not computed". Keep it visibly non-zero.
      if (pct <= 0) pct = 1;
      if (pct > 100) pct = 100;
    }
    int cache_pct = -1;
    if (cache_reported && cache_hit >= 0.0) {
      cache_pct = static_cast<int>(cache_hit * 100.0 + 0.5);
      if (cache_pct < 0) cache_pct = 0;
      if (cache_pct > 100) cache_pct = 100;
    }
    std::string cost_text;
    if (kind == "money") {
      char buf[32];
      std::snprintf(buf, sizeof(buf), "¥%.2f", cost);
      cost_text = buf;
    } else if (kind == "quota") {
      // Quota providers report cost 0 inside the plan: "¥0.00" would claim the
      // session was free.
      cost_text = tr("套餐内") + " · " + itos(static_cast<int>(calls)) + tr(" 次");
    } else if (calls > 0) {
      // The provider reports tokens but no money. Showing a number computed from
      // list prices would look like a bill and be wrong (discounts, cache rates,
      // price changes), so this stays "unknown" and the token counts are shown
      // separately for anyone who wants to do the arithmetic.
      cost_text = tr("未知") + " · " + itos(static_cast<int>(calls)) + tr(" 次");
    }

    // This turn: the rows recorded since the current turn started. The panel
    // already knows the start time of the live turn, so the split is computed
    // from the recent tail instead of costing another backend round trip.
    int64_t turn_started = 0;
    {
      const SessionView& v = views_.get_or_create(sid);
      if (!v.messages.empty()) {
        turn_started = v.messages.back().started_at;
        if (turn_started <= 0) turn_started = v.messages.back().ended_at;
      }
    }
    int64_t t_in = 0, t_out = 0;
    double t_cost = 0.0;
    std::string t_kind;
    bool has_turn = false;
    if (r.data.contains("recent") && r.data["recent"].is_array()) {
      for (auto& row : r.data["recent"]) {
        int64_t ts = jsonutil::get_int(row, "ts", 0);
        if (turn_started > 0 && ts < turn_started) continue;
        has_turn = true;
        t_in += jsonutil::get_int(row, "in", 0);
        t_out += jsonutil::get_int(row, "out", 0);
        t_cost += jsonutil::get_number(row, "cost", 0.0);
        std::string k = jsonutil::get<std::string>(row, "costKind", "unknown");
        if (t_kind.empty()) t_kind = k;
        else if (k == "money") t_kind = "money";
      }
    }
    int64_t turn_micros = has_turn ? static_cast<int64_t>(t_cost * 1000000.0) : -1;
    bool turn_known = has_turn;

    update([&](AppState& s) {
      // The reply belongs to `sid`; do not paint it onto a different session
      // that the user switched to while the request was in flight.
      SessionView& v = views_.get_or_create(sid);
      if (s.active_tab != sid) {
        v.context_pct = pct;
        v.context_tokens = last_in;
        v.context_window = window;
        v.cache_hit_pct = cache_pct;
        v.session_cache_tokens = cache;
        v.cache_reported = cache_reported;
        v.session_cost_kind = kind;
        v.usage_calls = calls;
        v.session_cost = cost_text;
        v.session_tokens_in = in;
        v.session_tokens_out = out;
        v.session_usage_loaded = true;
        v.turn_cost_micros = turn_micros;
        v.turn_in = turn_known ? t_in : -1;
        v.turn_out = turn_known ? t_out : -1;
        v.turn_cost_kind = t_kind;
        return;
      }
      s.context_pct = pct;
      s.context_tokens = last_in;
      s.context_window = window;
      s.cache_hit_pct = cache_pct;
      s.session_cache_tokens = cache;
      s.cache_reported = cache_reported;
      s.session_cost_kind = kind;
      s.usage_calls = calls;
      s.session_cost = cost_text;
      s.session_tokens_in = in;
      s.session_tokens_out = out;
      s.session_usage_loaded = true;
      s.turn_cost_micros = turn_micros;
      s.turn_in = turn_known ? t_in : -1;
      s.turn_out = turn_known ? t_out : -1;
      s.turn_cost_kind = t_kind;
    });
  });
}
void UiApp::attach_live_turn(const std::string& sid) {
  if (sid.empty()) return;
  call_async("session.liveTurn", Json{{"sid", sid}},
             [this, sid](const BackendResult& r) {
    if (!r.ok) return;
    bool running = jsonutil::get<bool>(r.data, "running", false);
    bool crashed = jsonutil::get<bool>(r.data, "crashed", false);
    std::string text = jsonutil::get<std::string>(r.data, "text", "");
    std::string user_text = jsonutil::get<std::string>(r.data, "userText", "");
    int64_t started = jsonutil::get_int(r.data, "startedAt", 0);
    // A crashed turn is shown as a FINISHED message (nobody is writing it any
    // more) plus the user's question when the transcript does not have it. The
    // process died mid-turn, so the answer exists only in this snapshot.
    if (!running && !crashed) return;
    if (st_.active_tab != sid) return;
    update([&](AppState& s) {
      // The question itself may be missing: a crashed turn's user message was
      // written at turn start, but a crash BEFORE that (or a cleared snapshot)
      // leaves nothing. Do not duplicate it when it is already there.
      bool have_user = false;
      if (!user_text.empty()) {
        for (size_t i = 0; i < s.messages.size(); ++i) {
          if (s.messages[i].role == "user" && s.messages[i].text == user_text) {
            have_user = true;
            break;
          }
        }
        if (!have_user) {
          comp::ChatMsg u;
          u.role = "user";
          u.text = user_text;
          u.rev = next_msg_rev_++;
          u.uid = s.history_total + static_cast<int64_t>(s.messages.size());
          s.messages.push_back(u);
        }
      }
      // Only append if the transcript does not already end with this streaming
      // message (a turn.done may have landed between the request and the reply).
      if (!s.messages.empty() && s.messages.back().role == "assistant" &&
          s.messages.back().streaming) {
        if (s.messages.back().text.size() < text.size()) {
          s.messages.back().text = text;
          s.messages.back().rev = next_msg_rev_++;
        }
        return;
      }
      comp::ChatMsg a;
      a.role = "assistant";
      a.text = text;
      a.streaming = running;  // a crashed turn is not still being written
      a.started_at = started > 0 ? started : now_ms();
      if (!running) a.ended_at = a.started_at;  // no live counter for a dead turn
      a.rev = next_msg_rev_++;
      a.uid = s.history_total + static_cast<int64_t>(s.messages.size());
      s.messages.push_back(a);
    });
    if (crashed) {
      toast(tr("检测到上次运行中断的回合，已恢复已产生的内容"));
    }
    render();
    follow_history_if_pinned();
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
      // you click the only item in the list. Select the first provider instead;
      // with NO providers at all, open the create wizard - an empty page with a
      // "新增" button somewhere is a dead end.
      if (!s.provider_creating && s.provider_selected.empty() && !rows.empty()) {
        select_default = rows[0].id;
      } else if (!s.provider_creating && s.provider_selected.empty() &&
                 rows.empty() && !provider_auto_opened_) {
        provider_auto_opened_ = true;
        s.provider_creating = true;
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
        row.kind = jsonutil::get<std::string>(t, "kind", "");
        row.enabled = jsonutil::get<bool>(t, "enabled", true);
        if (t.contains("server") && t["server"].is_object()) {
          const Json& sv = t["server"];
          row.server_command = jsonutil::get<std::string>(sv, "command", "");
          row.server_url = jsonutil::get<std::string>(sv, "url", "");
          if (sv.contains("args") && sv["args"].is_array()) {
            for (size_t k = 0; k < sv["args"].size(); ++k) {
              if (sv["args"][k].is_string()) {
                row.server_args.push_back(sv["args"][k].get<std::string>());
              }
            }
          }
        }
        rows.push_back(row);
      }
    }
    update([&](AppState& s) {
      s.mcp_tools = rows;
      s.mcp_loaded = true;  // render-driven load: stop retrying either way
    });
  });
}

// Workflows: definitions + runs for the active session.
//
// A workflow is a pipeline of steps, each spawning subagents; the panel answers
// "which step is running, which failed, can I retry/skip/steer it". The backend
// already has all of that (workflow.*), so this is a view over it.
void UiApp::load_workflows(const std::string& sid) {
  if (st_.workflows_inflight) return;
  st_.workflows_inflight = true;
  call_async("workflow.list", Json::object(), [this](const BackendResult& r) {
    std::vector<AppState::WorkflowDefRow> defs;
    if (r.ok && r.data.contains("defs") && r.data["defs"].is_array()) {
      const Json& arr = r.data["defs"];
      for (size_t i = 0; i < arr.size(); ++i) {
        AppState::WorkflowDefRow d;
        d.name = jsonutil::get<std::string>(arr[i], "name", "");
        d.description = jsonutil::get<std::string>(arr[i], "description", "");
        if (arr[i].contains("steps") && arr[i]["steps"].is_array()) {
          d.steps = static_cast<int>(arr[i]["steps"].size());
        }
        if (!d.name.empty()) defs.push_back(d);
      }
    }
    update([&](AppState& s) {
      s.workflow_defs = defs;
      s.workflow_defs_loaded = true;
    });
  });
  call_async("workflow.runs", Json{{"sid", sid}},
             [this](const BackendResult& r) {
    std::vector<AppState::WorkflowRunRow> runs;
    if (r.ok && r.data.contains("runs") && r.data["runs"].is_array()) {
      const Json& arr = r.data["runs"];
      for (size_t i = 0; i < arr.size(); ++i) {
        AppState::WorkflowRunRow run;
        run.id = jsonutil::get<std::string>(arr[i], "id", "");
        run.name = jsonutil::get<std::string>(arr[i], "name", "");
        run.state = jsonutil::get<std::string>(arr[i], "state", "");
        if (arr[i].contains("steps") && arr[i]["steps"].is_array()) {
          const Json& steps = arr[i]["steps"];
          for (size_t k = 0; k < steps.size(); ++k) {
            AppState::WorkflowStepRow st;
            st.id = jsonutil::get<std::string>(steps[k], "id", "");
            st.name = jsonutil::get<std::string>(steps[k], "name", "");
            st.state = jsonutil::get<std::string>(steps[k], "state", "pending");
            st.attempts = static_cast<int>(jsonutil::get_int(steps[k], "attempts", 0));
            st.error = jsonutil::get<std::string>(steps[k], "error", "");
            if (!st.id.empty()) run.steps.push_back(st);
          }
        }
        if (!run.id.empty()) runs.push_back(run);
      }
    }
    update([&](AppState& s) {
      s.workflow_runs = runs;
      s.workflow_runs_loaded = true;
    });
    st_.workflows_inflight = false;
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

// The model's checklist items ({id, task, done}) as the floating panel shows
// them. Shared by the live `todo.updated` event and the on-open fetch, so the
// two paths cannot disagree about the shape.
std::vector<AppState::TodoItem> parse_todo_items(const Json& items) {
  std::vector<AppState::TodoItem> out;
  if (!items.is_array()) return out;
  for (size_t i = 0; i < items.size(); ++i) {
    const Json& e = items[i];
    if (!e.is_object()) continue;
    AppState::TodoItem t;
    t.id = jsonutil::get_int(e, "id", 0);
    t.task = jsonutil::get<std::string>(e, "task",
                                         jsonutil::get<std::string>(e, "text", ""));
    t.done = jsonutil::get<bool>(e, "done", false);
    if (!t.task.empty()) out.push_back(t);
  }
  return out;
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
      row.model = jsonutil::get<std::string>(a, "model", "");
      row.summary = jsonutil::get<std::string>(a, "summary", "");
      row.started_at = jsonutil::get_int(a, "startedAt", 0);
      row.ended_at = jsonutil::get_int(a, "endedAt", 0);
      row.transcript = a.contains("full") ? a["full"] : Json::array();
      // Preserve the user's own row state across the poll-driven refresh: the
      // open fold and the live text (which comes from events, not from this
      // listing). The steer draft lives on the detail page now.
      for (size_t k = 0; k < st_.subagents.size(); ++k) {
        if (st_.subagents[k].id == row.id) {
          row.open = st_.subagents[k].open;
          row.live_text = st_.subagents[k].live_text;
        }
      }
      rows.push_back(row);
    }
    update([&](AppState& s) { s.subagents = rows; });
  });
}

// Steering is a typed instruction. The old button sent a hardcoded "继续", which
// is why it looked like it did nothing: the user had no way to say WHAT to
// change, and the toast confirmed a message they never wrote.
//
// A RUNNING subagent is steered (the text reaches it between tool calls); a
// FINISHED one is followed up - the same id runs again with the earlier
// transcript, which is how "keep guiding it after it reported back" works.
// ---------------------------------------------------------------------------
// Composer inline completion ("/" commands, "/" and "./" paths)
// ---------------------------------------------------------------------------
//
// Rules the user asked for:
//   * a "/" at the VERY START of the input opens the command (skill) list;
//   * a "/" or "./" ANYWHERE ELSE opens file/directory candidates for the
//     fragment, relative to the project ("./" or a "/" that names a project
//     folder) or, in permissive mode, to the filesystem.
// Both are HINTS: nothing is inserted until the user picks (Enter/click) or
// presses Tab.
int UiApp::complete_row_count() const {
  if (st_.complete_kind == AppState::CompleteKind::kCommand) {
    return static_cast<int>(st_.command_menu.size());
  }
  if (st_.complete_kind == AppState::CompleteKind::kPath) {
    return static_cast<int>(st_.complete_items.size());
  }
  return 0;
}

void UiApp::update_complete_from_input(const std::string& text) {
  // A "/" as the FIRST character: the command list. Keep the current one open
  // while the user narrows it ("/br" still lists commands).
  auto is_cmd = [](const std::string& t) { return t.size() > 0 && t[0] == '/'; };
  bool command_mode = is_cmd(text) && text.find(' ') == std::string::npos &&
                      text.find('\n') == std::string::npos;
  if (command_mode) {
    std::string q = text.substr(1);
    bool was = st_.complete_kind == AppState::CompleteKind::kCommand;
    if (!was || st_.complete_query != q) {
      // Load the command list once; it is backend-owned.
      if (st_.command_menu.empty()) {
        call_async("command.list", Json{{"sid", active_sid()}},
                   [this](const BackendResult& r) {
          if (!r.ok) return;
          std::vector<AppState::CommandRow> rows;
          const Json& arr = r.data.contains("commands") ? r.data["commands"] : r.data;
          if (arr.is_array()) {
            for (size_t i = 0; i < arr.size(); ++i) {
              AppState::CommandRow row;
              row.name = jsonutil::get<std::string>(arr[i], "name", "");
              row.title = jsonutil::get<std::string>(arr[i], "title", row.name);
              row.description = jsonutil::get<std::string>(arr[i], "description", "");
              row.action = jsonutil::get<std::string>(arr[i], "action", "");
              row.group = jsonutil::get<std::string>(arr[i], "group", "");
              if (!row.name.empty()) rows.push_back(row);
            }
          }
          update([&](AppState& s) { s.command_menu = rows; });
          update_complete_from_input(st_.session_input);
        });
      }
      update([&](AppState& s) {
        s.complete_kind = AppState::CompleteKind::kCommand;
        s.complete_query = q;
        s.complete_index = 0;
      });
    }
    return;
  }

  // Path mode: find the LAST "/" (or "./") and take the fragment after it.
  size_t slash = text.find_last_of('/');
  if (slash == std::string::npos || slash == 0) {
    // No separator (or the leading one, which is command mode): nothing to hint.
    if (st_.complete_kind != AppState::CompleteKind::kNone) {
      update([&](AppState& s) { s.complete_kind = AppState::CompleteKind::kNone; });
    }
    return;
  }
  // Only hint when the fragment looks like a path being typed: no spaces after
  // the slash (it is a word, not prose).
  std::string frag = text.substr(slash + 1);
  if (frag.find(' ') != std::string::npos || frag.find('\n') != std::string::npos) {
    if (st_.complete_kind != AppState::CompleteKind::kNone) {
      update([&](AppState& s) { s.complete_kind = AppState::CompleteKind::kNone; });
    }
    return;
  }
  // The path starts at the separator's word start: "./a/b" -> "./a/b"; "see a/b"
  // -> "a/b" (the hint is about the path only).
  size_t word = slash;
  while (word > 0 && text[word - 1] != ' ' && text[word - 1] != '\n') --word;
  std::string typed = text.substr(word, text.size() - word);
  bool was = st_.complete_kind == AppState::CompleteKind::kPath;
  if (was && st_.complete_prefix == typed) return;  // nothing changed
  std::string sid = active_sid();
  call_async("complete.path", Json{{"sid", sid}, {"typed", typed}},
             [this, typed](const BackendResult& r) {
    // A reply is only valid while the SAME fragment is still being typed. Without
    // this an answer arriving after the user finished the path (and went on to
    // edit other text) re-opened the popup over what they were doing.
    {
      const std::string& cur = st_.session_input;
      size_t s2 = cur.find_last_of('/');
      bool still = s2 != std::string::npos && s2 != 0;
      if (still) {
        size_t w2 = s2;
        while (w2 > 0 && cur[w2 - 1] != ' ' && cur[w2 - 1] != '\n') --w2;
        still = cur.substr(w2) == typed;
      }
      if (!still) return;
    }
    if (!r.ok) return;
    std::vector<std::pair<std::string, bool> > items;
    const Json& arr = r.data.contains("entries") ? r.data["entries"] : Json::array();
    if (arr.is_array()) {
      for (size_t i = 0; i < arr.size(); ++i) {
        std::string nm = jsonutil::get<std::string>(arr[i], "name", "");
        if (nm.empty()) continue;
        items.push_back(std::make_pair(
            nm, jsonutil::get<bool>(arr[i], "isDir", false)));
      }
    }
    if (items.empty()) {
      // NO MATCHES = NO POPUP. An empty menu still floats above the composer and
      // blocks what is underneath, which is the "它一直呆在那里挡着" report.
      if (st_.complete_kind == AppState::CompleteKind::kPath) {
        update([&](AppState& s) { s.complete_kind = AppState::CompleteKind::kNone; });
      }
      return;
    }
    update([&](AppState& s) {
      s.complete_kind = AppState::CompleteKind::kPath;
      s.complete_prefix = typed;
      s.complete_dir = jsonutil::get<std::string>(r.data, "dir", "");
      s.complete_items = items;
      s.complete_index = 0;
    });
  });
}

void UiApp::accept_complete() {
  if (st_.complete_kind == AppState::CompleteKind::kNone) return;
  int idx = st_.complete_index;
  std::string insert;
  bool is_dir = false;
  if (st_.complete_kind == AppState::CompleteKind::kCommand) {
    if (idx < 0 || idx >= static_cast<int>(st_.command_menu.size())) return;
    insert = "/" + st_.command_menu[static_cast<size_t>(idx)].name + " ";
  } else {
    if (idx < 0 || idx >= static_cast<int>(st_.complete_items.size())) return;
    insert = st_.complete_items[static_cast<size_t>(idx)].first;
    is_dir = st_.complete_items[static_cast<size_t>(idx)].second;
  }
  std::string text = st_.session_input;
  if (st_.complete_kind == AppState::CompleteKind::kCommand) {
    text = insert;  // the command replaces the whole line
  } else {
    // Replace the fragment after the last separator, keeping what came before.
    size_t slash = text.find_last_of('/');
    if (slash == std::string::npos) return;
    size_t word = slash;
    while (word > 0 && text[word - 1] != ' ' && text[word - 1] != '\n') --word;
    text = text.substr(0, word) + text.substr(word, slash + 1 - word) + insert;
    if (is_dir) text += "/";
  }
  AppState::CompleteKind kind = st_.complete_kind;
  update([&](AppState& s) {
    s.session_input = text;
    s.complete_kind = AppState::CompleteKind::kNone;
    s.complete_index = 0;
  });
  // The accepted text has to be written THROUGH to the DOM. The framework
  // deliberately never overwrites a focused textarea's value (that would move
  // the caret on every keystroke), so a state-only update left the box showing
  // the old draft - typing "/" then Tab kept the literal "/" while the menu
  // closed, which is exactly "Tab 没反应". Same fix as send_message's clear.
  {
    VNodePtr ta = find_by_key(tree_, "session-input");
    if (ta && ta->dom.valid()) dom_.set_attr(ta->dom, "value", text);
  }
  // Focus + values live in the DOM mirror: the select is patched by the
  // framework, and the textarea's value follows `session_input`.
  if (kind == AppState::CompleteKind::kPath && is_dir) {
    // A directory was accepted: immediately list its contents.
    update_complete_from_input(text);
  }
}

VNodePtr UiApp::build_complete_menu() {
  H box = el("div", "complete-menu");
  box.key("complete-menu");
  box.anim(anim::kPop);
  int idx = st_.complete_index;
  if (st_.complete_kind == AppState::CompleteKind::kCommand) {
    if (st_.command_menu.empty()) {
      box.add(el("div", "complete-item dim").text(tr("（暂无命令）")));
      return box.get();
    }
    for (size_t i = 0; i < st_.command_menu.size(); ++i) {
      const AppState::CommandRow& c = st_.command_menu[i];
      if (!st_.complete_query.empty()) {
        std::string q = lower_ascii(st_.complete_query);
        if (lower_ascii(c.name).find(q) == std::string::npos &&
            lower_ascii(c.title).find(q) == std::string::npos) {
          continue;
        }
      }
      H it = el("div", static_cast<int>(i) == idx ? "complete-item sel" : "complete-item");
      it.add(el("span", "complete-name").text("/" + c.name));
      if (!c.title.empty() && c.title != c.name) {
        it.add(el("span", "complete-title").text(c.title));
      }
      if (!c.description.empty()) {
        it.add(el("span", "complete-desc dim").text(c.description));
      }
      size_t n = i;
      it.on(EventKind::kClick, [this, n](const std::string&) {
        update([&](AppState& s) { s.complete_index = static_cast<int>(n); });
        accept_complete();
      });
      box.add(it);
    }
  } else {
    if (st_.complete_items.empty()) {
      box.add(el("div", "complete-item dim").text(tr("（没有匹配的文件）")));
      return box.get();
    }
    for (size_t i = 0; i < st_.complete_items.size(); ++i) {
      bool is_dir2 = st_.complete_items[i].second;
      H it = el("div", static_cast<int>(i) == idx ? "complete-item sel" : "complete-item");
      it.add(el("i", is_dir2 ? "fa fa-folder" : "fa fa-file"));
      it.add(el("span", "complete-name").text(st_.complete_items[i].first +
                                              (is_dir2 ? "/" : "")));
      size_t n = i;
      it.on(EventKind::kClick, [this, n](const std::string&) {
        update([&](AppState& s) { s.complete_index = static_cast<int>(n); });
        accept_complete();
      });
      box.add(it);
    }
  }
  return box.get();
}

// Steering is a typed instruction. The old button sent a hardcoded "继续", which
// is why it looked like it did nothing: the user had no way to say WHAT to
// change, and the toast confirmed a message they never wrote.
//
// Addressed by ID: the 引导 box lives on the DETAIL page now, and a finished
// agent may no longer be in the overview list (it was pruned), so an index into
// that list cannot name it.
void UiApp::send_subagent_steer_text(const std::string& id, const std::string& text,
                                     bool running) {
  if (id.empty()) return;
  if (text.empty()) {
    toast(tr("请先输入要发送的引导"));
    return;
  }
  std::string method = running ? "subagent.steer" : "subagent.followUp";
  call_async(method, Json{{"id", id}, {"text", text}},
             [this, id, running](const BackendResult& r) {
    if (!r.ok) {
      toast(tr("引导失败：") + r.message);
      return;
    }
    toast(running ? tr("已发送引导") : tr("已追加任务，子代理正在继续"));
    // Clear the box: the message is queued, and leaving it there would send it
    // twice on the next press.
    update([&](AppState& s) {
      std::map<std::string, AppState::SubagentDetail>::iterator it =
          s.subagent_details.find(id);
      if (it != s.subagent_details.end()) it->second.steer_draft.clear();
    });
    // A follow-up produced a new summary; refresh the row from the backend.
    if (!running) load_subagents(active_sid());
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
        s.side_tabs[i].files_truncated =
            r.ok && jsonutil::get<bool>(r.data, "truncated", false);
        s.side_tabs[i].path = rel;
        s.side_tabs[i].loaded = true;
        s.side_tabs[i].preview_path.clear();
        s.side_tabs[i].preview.clear();
        return;
      }
    });
  });
}

// ---- git panel ----------------------------------------------------------
// The panel is a view of the ACTIVE session's project, addressed by pid. Every
// call refreshes the panel's own copy of the status: two git panels can point
// at two different projects, so nothing here reads global state.
void UiApp::load_git(const std::string& tab_id) {
  if (st_.active_pid.empty()) return;
  std::string pid = st_.active_pid;
  call_async("git.status", Json{{"pid", pid}}, [this, tab_id](const BackendResult& r) {
    std::vector<SideTab::GitChange> rows;
    bool ok = r.ok;
    bool is_repo = false;
    std::string root, branch, upstream, operation, error;
    int ahead = 0, behind = 0;
    bool detached = false;
    if (ok) {
      is_repo = jsonutil::get<bool>(r.data, "isRepo", false);
      root = jsonutil::get<std::string>(r.data, "root", "");
      branch = jsonutil::get<std::string>(r.data, "branch", "");
      upstream = jsonutil::get<std::string>(r.data, "upstream", "");
      operation = jsonutil::get<std::string>(r.data, "operation", "");
      error = jsonutil::get<std::string>(r.data, "error", "");
      ahead = static_cast<int>(jsonutil::get_int(r.data, "ahead", 0));
      behind = static_cast<int>(jsonutil::get_int(r.data, "behind", 0));
      detached = jsonutil::get<bool>(r.data, "detached", false);
      const Json& arr = r.data.contains("changes") ? r.data["changes"] : Json::array();
      if (arr.is_array()) {
        for (size_t i = 0; i < arr.size(); ++i) {
          SideTab::GitChange c;
          c.path = jsonutil::get<std::string>(arr[i], "path", "");
          c.orig_path = jsonutil::get<std::string>(arr[i], "origPath", "");
          std::string idx = jsonutil::get<std::string>(arr[i], "index", " ");
          std::string wk = jsonutil::get<std::string>(arr[i], "work", " ");
          c.index = idx.empty() ? ' ' : idx[0];
          c.work = wk.empty() ? ' ' : wk[0];
          c.staged = jsonutil::get<bool>(arr[i], "staged", false);
          c.untracked = jsonutil::get<bool>(arr[i], "untracked", false);
          c.conflicted = jsonutil::get<bool>(arr[i], "conflicted", false);
          if (!c.path.empty()) rows.push_back(c);
        }
      }
    } else {
      error = r.message;
    }
    update([&](AppState& s) {
      for (size_t i = 0; i < s.side_tabs.size(); ++i) {
        if (s.side_tabs[i].id != tab_id) continue;
        SideTab& t = s.side_tabs[i];
        t.git_loaded = true;
        t.git_repo = is_repo;
        t.git_root = root;
        t.git_branch = branch;
        t.git_upstream = upstream;
        t.git_operation = operation;
        t.git_error = error;
        t.git_ahead = ahead;
        t.git_behind = behind;
        t.git_detached = detached;
        t.git_changes = rows;
        // The open diff may no longer exist (staged away, reverted, committed).
        if (!t.git_diff_path.empty()) {
          bool still = false;
          for (size_t k = 0; k < rows.size(); ++k) {
            if (rows[k].path == t.git_diff_path) still = true;
          }
          if (!still) {
            t.git_diff_path.clear();
            t.git_diff_text.clear();
            t.git_diff_truncated = false;
            t.git_diff_error.clear();
          }
        }
        return;
      }
    });
  });
}

void UiApp::load_git_diff(const std::string& tab_id, const std::string& path,
                          bool staged) {
  if (st_.active_pid.empty() || path.empty()) return;
  std::string pid = st_.active_pid;
  call_async("git.diff", Json{{"pid", pid}, {"path", path}, {"staged", staged}},
             [this, tab_id, path, staged](const BackendResult& r) {
    std::string text;
    std::string error;
    bool truncated = false;
    if (r.ok) {
      text = jsonutil::get<std::string>(r.data, "diff", "");
      truncated = jsonutil::get<bool>(r.data, "truncated", false);
      if (!jsonutil::get<bool>(r.data, "ok", true)) {
        error = jsonutil::get<std::string>(r.data, "error", "");
      }
    } else {
      error = r.message;
    }
    update([&](AppState& s) {
      for (size_t i = 0; i < s.side_tabs.size(); ++i) {
        if (s.side_tabs[i].id != tab_id) continue;
        SideTab& t = s.side_tabs[i];
        t.git_diff_path = path;
        t.git_diff_staged = staged;
        t.git_diff_text = text;
        t.git_diff_truncated = truncated;
        t.git_diff_error = error;
        return;
      }
    });
  });
}

void UiApp::load_git_log(const std::string& tab_id) {
  if (st_.active_pid.empty()) return;
  std::string pid = st_.active_pid;
  call_async("git.log", Json{{"pid", pid}, {"maxCount", 40}},
             [this, tab_id](const BackendResult& r) {
    std::vector<SideTab::GitCommit> rows;
    if (r.ok && r.data.contains("commits") && r.data["commits"].is_array()) {
      const Json& arr = r.data["commits"];
      for (size_t i = 0; i < arr.size(); ++i) {
        SideTab::GitCommit c;
        c.hash = jsonutil::get<std::string>(arr[i], "hash", "");
        c.short_hash = jsonutil::get<std::string>(arr[i], "short", "");
        c.author = jsonutil::get<std::string>(arr[i], "author", "");
        c.subject = jsonutil::get<std::string>(arr[i], "subject", "");
        c.time = jsonutil::get_int(arr[i], "time", 0);
        if (!c.hash.empty()) rows.push_back(c);
      }
    }
    update([&](AppState& s) {
      for (size_t i = 0; i < s.side_tabs.size(); ++i) {
        if (s.side_tabs[i].id != tab_id) continue;
        s.side_tabs[i].git_commits = rows;
        s.side_tabs[i].git_log_loaded = true;
        return;
      }
    });
  });
}

void UiApp::load_git_branches(const std::string& tab_id) {
  if (st_.active_pid.empty()) return;
  std::string pid = st_.active_pid;
  call_async("git.branches", Json{{"pid", pid}}, [this, tab_id](const BackendResult& r) {
    std::vector<SideTab::GitBranch> rows;
    if (r.ok && r.data.contains("branches") && r.data["branches"].is_array()) {
      const Json& arr = r.data["branches"];
      for (size_t i = 0; i < arr.size(); ++i) {
        SideTab::GitBranch b;
        b.name = jsonutil::get<std::string>(arr[i], "name", "");
        b.current = jsonutil::get<bool>(arr[i], "current", false);
        if (!b.name.empty()) rows.push_back(b);
      }
    }
    update([&](AppState& s) {
      for (size_t i = 0; i < s.side_tabs.size(); ++i) {
        if (s.side_tabs[i].id != tab_id) continue;
        s.side_tabs[i].git_branch_rows = rows;
        s.side_tabs[i].git_branches_loaded = true;
        return;
      }
    });
  });
}

// The file's content at HEAD. Diff shows what CHANGED; this shows what it
// changed FROM, which is the other half of reading a modification.
void UiApp::load_git_head(const std::string& tab_id, const std::string& path) {
  if (st_.active_pid.empty() || path.empty()) return;
  std::string pid = st_.active_pid;
  call_async("git.showHead", Json{{"pid", pid}, {"path", path}},
             [this, tab_id](const BackendResult& r) {
    std::string text;
    bool exists = false;
    if (r.ok) {
      exists = jsonutil::get<bool>(r.data, "exists", false);
      text = jsonutil::get<std::string>(r.data, "content", "");
    }
    update([&](AppState& s) {
      for (size_t i = 0; i < s.side_tabs.size(); ++i) {
        if (s.side_tabs[i].id != tab_id) continue;
        s.side_tabs[i].git_show_head = true;
        s.side_tabs[i].git_head_text = text;
        s.side_tabs[i].git_head_exists = exists;
        return;
      }
    });
  });
}

void UiApp::git_stage_paths(const std::string& tab_id,
                            const std::vector<std::string>& paths, bool stage) {
  if (st_.active_pid.empty() || paths.empty()) return;
  Json arr = Json::array();
  for (size_t i = 0; i < paths.size(); ++i) arr.push_back(paths[i]);
  std::string pid = st_.active_pid;
  const char* method = stage ? "git.stage" : "git.unstage";
  call_async(method, Json{{"pid", pid}, {"paths", arr}},
             [this, tab_id](const BackendResult& r) {
    if (!r.ok) {
      toast(tr("Git 操作失败：") + r.message);
    } else if (!jsonutil::get<bool>(r.data, "ok", true)) {
      toast(tr("Git 操作失败：") +
            jsonutil::get<std::string>(r.data, "message", ""));
    }
    load_git(tab_id);
  });
}

void UiApp::git_stage_all(const std::string& tab_id) {
  if (st_.active_pid.empty()) return;
  std::string pid = st_.active_pid;
  call_async("git.stageAll", Json{{"pid", pid}}, [this, tab_id](const BackendResult& r) {
    if (!r.ok || !jsonutil::get<bool>(r.data, "ok", true)) {
      toast(tr("暂存失败：") + (r.ok ? jsonutil::get<std::string>(r.data, "message", "")
                                    : r.message));
    }
    load_git(tab_id);
  });
}

void UiApp::git_discard_paths(const std::string& tab_id,
                              const std::vector<std::string>& paths) {
  if (st_.active_pid.empty() || paths.empty()) return;
  Json arr = Json::array();
  for (size_t i = 0; i < paths.size(); ++i) arr.push_back(paths[i]);
  std::string pid = st_.active_pid;
  call_async("git.discard", Json{{"pid", pid}, {"paths", arr}},
             [this, tab_id](const BackendResult& r) {
    if (!r.ok || !jsonutil::get<bool>(r.data, "ok", true)) {
      toast(tr("撤销失败：") + (r.ok ? jsonutil::get<std::string>(r.data, "message", "")
                                    : r.message));
    }
    load_git(tab_id);
  });
}

void UiApp::git_commit(const std::string& tab_id, const std::string& message,
                       bool amend) {
  if (st_.active_pid.empty()) return;
  if (message.empty()) {
    toast(tr("提交信息不能为空"));
    return;
  }
  std::string pid = st_.active_pid;
  call_async("git.commit", Json{{"pid", pid}, {"message", message}, {"amend", amend}},
             [this, tab_id](const BackendResult& r) {
    bool ok = r.ok && jsonutil::get<bool>(r.data, "ok", true);
    if (ok) {
      std::string hash = jsonutil::get<std::string>(r.data, "hash", "");
      toast(hash.empty() ? tr("已提交") : (tr("已提交 ") + hash));
      update([&](AppState& s) {
        for (size_t i = 0; i < s.side_tabs.size(); ++i) {
          if (s.side_tabs[i].id == tab_id) s.side_tabs[i].git_message.clear();
        }
      });
    } else {
      toast(tr("提交失败：") + (r.ok ? jsonutil::get<std::string>(r.data, "message", "")
                                    : r.message));
    }
    load_git(tab_id);
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
        if (r.ok) {
          std::string kind = jsonutil::get<std::string>(r.data, "kind", "text");
          s.side_tabs[i].preview_kind = kind;
          s.side_tabs[i].preview_size = jsonutil::get_int(r.data, "size", 0);
          s.side_tabs[i].preview_uri =
              kind == "image" ? jsonutil::get<std::string>(r.data, "dataUri", "")
                              : std::string();
          s.side_tabs[i].preview =
              kind == "text"
                  ? jsonutil::get<std::string>(r.data, "content", "")
                  : std::string();
        } else {
          s.side_tabs[i].preview_kind = "text";
          s.side_tabs[i].preview_uri.clear();
          s.side_tabs[i].preview_size = 0;
          s.side_tabs[i].preview = tr("（无法预览：") + r.message + "）";
        }
        return;
      }
    });
  });
}

// Follow a markdown link. The backend decides where it goes: http(s) opens the
// browser, a project-relative path opens the side-panel preview (which is what a
// link to a source file should do), anything else is refused.
void UiApp::open_href(const std::string& href) {
  if (href.empty()) return;
  call_async("link.open", Json{{"sid", active_sid()}, {"href", href}},
             [this](const BackendResult& r) {
    if (!r.ok) {
      toast(tr("无法打开链接：") + r.message);
      return;
    }
    std::string opened = jsonutil::get<std::string>(r.data, "opened", "");
    if (opened == "file") {
      open_file_preview(jsonutil::get<std::string>(r.data, "path", ""));
    }
  });
}

// Open a file's contents in its OWN side tab. Reading a file and browsing the
// tree are two things you want visible at once, which an inline block under the
// tree cannot do.
// Open a subagent's transcript in the SIDEBAR.
//
// It used to open a full-window tab. That was wrong for two reasons the user
// called out: a subagent's history belongs next to the conversation you are
// watching (the sidebar is where "what is the agent doing" already lives), and a
// page could not be closed back to the session without hunting for the tab.
//
// The panel is a real side tab of kind "subagents" carrying `subagent_id`, so it
// coexists with the summary panel and can be closed like any other.
void UiApp::open_subagent_tab(const std::string& subagent_id,
                              const std::string& title) {
  if (subagent_id.empty()) return;
  // Already open for this agent? Focus it.
  for (size_t i = 0; i < st_.side_tabs.size(); ++i) {
    if (st_.side_tabs[i].kind == "subagent" &&
        st_.side_tabs[i].subagent_id == subagent_id) {
      std::string id = st_.side_tabs[i].id;
      update([&](AppState& s) {
        s.side_collapsed = false;
        if (!s.active_side.empty() && s.active_side != id) {
          s.side_history.push_back(s.active_side);
        }
        s.active_side = id;
      });
      load_subagent_detail(subagent_id);
      return;
    }
  }
  std::string id;
  update([&](AppState& s) {
    s.side_collapsed = false;  // asking to see it implies it must be visible
    s.side_seq++;
    SideTab t;
    t.id = "sb" + itos(s.side_seq);
    t.kind = "subagent";
    t.subagent_id = subagent_id;
    t.title = title.empty() ? (tr("子代理 ") + subagent_id) : title;
    s.side_tabs.push_back(t);
    if (!s.active_side.empty() && s.active_side != t.id) {
      s.side_history.push_back(s.active_side);
    }
    s.active_side = t.id;
    id = t.id;
  });
  load_subagent_detail(subagent_id);
}

void UiApp::load_subagent_detail(const std::string& subagent_id) {
  if (subagent_id.empty()) return;
  // Pass the session too: when the in-memory record is gone (the process
  // restarted, or the record was pruned) the backend reads the copy saved next to
  // this session. A bare id cannot name a session - ids restart at sub_1 in every
  // session.
  call_async("subagent.detail",
             Json{{"id", subagent_id}, {"sid", active_sid()}},
             [this, subagent_id](const BackendResult& r) {
    if (!r.ok) return;
    const Json& a = r.data;
    AppState::SubagentRow row;
    row.id = jsonutil::get<std::string>(a, "id", subagent_id);
    row.kind = jsonutil::get<std::string>(a, "kind", "");
    row.status = jsonutil::get<std::string>(a, "status", "");
    row.task = jsonutil::get<std::string>(a, "task", "");
    row.model = jsonutil::get<std::string>(a, "model", "");
    row.summary = jsonutil::get<std::string>(a, "summary", "");
    row.started_at = jsonutil::get_int(a, "startedAt", 0);
    row.ended_at = jsonutil::get_int(a, "endedAt", 0);
    row.transcript = a.contains("full") && a["full"].is_object()
                         ? a["full"].value("transcript", Json::array())
                         : Json::array();
    update([&](AppState& s) {
      AppState::SubagentDetail& d = s.subagent_details[subagent_id];
      d.row = row;
      d.loaded = true;
    });
  });
}

void UiApp::open_file_preview(const std::string& rel) {  if (rel.empty()) return;
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
    m.rev = next_msg_rev_++;
    // Newest message in the session: distinct from every loaded turn.
    m.uid = st_.history_total + static_cast<int64_t>(st_.messages.size());
    st_.messages.push_back(m);
    st_.history_total += 1;
  }
  st_.messages.back().text += text;
  st_.messages.back().streaming = true;
  // The streaming message is the ONE that must be rebuilt on every delta; every
  // message before it keeps its revision and is skipped by the render cache.
  st_.messages.back().rev = next_msg_rev_++;
  follow_history_if_pinned();
}

// New content grows the transcript; if the reader is at the end, stay at the end.
//
// This runs AFTER the render that added the content, because only then does the
// container know its new scrollHeight. `hist_follow` is the whole decision: it is
// set by the scroll-edge report (true at the bottom, false as soon as the user
// scrolls away), so a streamed reply never drags the view down while someone is
// reading older turns.
void UiApp::follow_history_if_pinned() {
  if (!st_.hist_follow) return;
  VNodePtr h = find_by_key(tree_, "chat-history");
  if (h && h->dom.valid()) dom_.scroll_to_bottom(h->dom);
}

// Backend events are per-session (payload.sid). They must land in that
// session's view, not blindly on whatever tab happens to be active - a
// background session can stream while the user is elsewhere.
// Make sure the transcript ends with the assistant message of the CURRENT turn.
//
// WHY: a model often replies with ONLY tool calls and no text (very common for
// the first round of a turn). No text means no `stream.delta`, so no assistant
// message was ever created - and `tool.start` / `tool.update` required
// `m.back().role == "assistant"`. They therefore dropped the block entirely:
//   * the work log was missing ("工作过程依旧没看到"),
//   * and the live timer never appeared, because the timer renders on that
//     message and it did not exist ("有时候不显示计时").
// Creating it here fixes both at the source.
static comp::ChatMsg& ensure_turn_message(std::vector<comp::ChatMsg>& m,
                                          int& total, uint64_t* next_rev,
                                          int64_t now) {
  if (!m.empty() && m.back().role == "assistant" && m.back().streaming) {
    return m.back();
  }
  comp::ChatMsg a;
  a.role = "assistant";
  a.streaming = true;
  // A turn in flight shows its work: the process is the only thing moving, and a
  // folded block made a long tool chain look like a frozen screen. turn.done
  // folds it back.
  a.work_open = true;
  a.rev = (*next_rev)++;
  a.started_at = now;
  a.uid = total + static_cast<int64_t>(m.size());
  m.push_back(a);
  ++total;
  return m.back();
}

void UiApp::apply_session_event(const std::string& sid, const std::string& event,
                                const Json& payload) {
  bool is_active = sid.empty() || sid == st_.active_tab;
  // A SUBAGENT's events are tagged (sid=parent, subagentId, subagentKind). Only
  // a complete-step belongs INLINE in the conversation: it is the current task
  // advancing, and the user should see it happen. Research / explore / check-files
  // are side work - showing their stream and tool rows in the main transcript
  // buried the actual answer, so those stay in the 子代理 sidebar (which is fed
  // by subagent.list / subagent.started / subagent.done separately).
  if (jsonutil::get<bool>(payload, "fromSubagent", false)) {
    std::string sub_id = jsonutil::get<std::string>(payload, "subagentId", "");
    // Feed the SIDEBAR's live view before deciding anything about the main chat:
    // a running subagent's panel row shows what it is doing right now, and that
    // is the only place its work is visible when it is not a complete-step.
    //
    // NO render() here. A subagent generating text emits hundreds of deltas per
    // second, and painting per delta starves the event loop - the SAME bug the
    // main chat hit, and it freezes the whole app seconds after a subagent starts
    // talking ("生成子代理后几秒内卡死"). The data is already recorded above; the
    // paint is deferred to the same 25 ms floor the main transcript uses, and
    // turn.done / the panel's own refresh paints the final state.
    if (!sub_id.empty() && (event == "stream.delta" || event == "tool.start")) {
      std::vector<AppState::SubagentRow>& rows =
          is_active ? st_.subagents : views_.get_or_create(sid).subagents;
      for (size_t k = 0; k < rows.size(); ++k) {
        if (rows[k].id != sub_id) continue;
        if (event == "stream.delta") {
          rows[k].live_text += jsonutil::get<std::string>(payload, "text", "");
          // Keep the tail: a long answer would otherwise grow the row without
          // bound and the panel only shows the latest part anyway.
          if (rows[k].live_text.size() > 2000) {
            rows[k].live_text.erase(0, rows[k].live_text.size() - 2000);
          }
        } else {
          std::string nm = jsonutil::get<std::string>(payload, "name", "");
          if (!nm.empty()) rows[k].live_text += "\n[" + tr("调用") + " " + nm + "]";
        }
        break;
      }
      // Mark dirty. pump() drains this at a human cadence (it runs on the
      // browser's 250 ms timer and from the desktop event loop), which is all a
      // "what is it doing now" preview needs.
      subagent_stream_dirty_ = true;
    }
    std::string skind = jsonutil::get<std::string>(payload, "subagentKind", "");
    if (skind != "complete-step") return;
  }
  // A fast stream delivers hundreds of deltas in one poll burst. Rendering once
  // per delta starves the browser's event loop: the next poll never runs, and
  // the transcript appears frozen until the burst ends - which is exactly the
  // "it is not really streaming" feeling. Deltas only append text, so they are
  // coalesced: mark dirty, let pump() paint at most every ~50 ms. (The `coalesced`
  // flag itself lives in on_backend_event, which owns the outer paint decision;
  // this function paints its own case inline below.)
  // only touch views_ for a background session; a fresh one gets the default
  // panels seeded so opening it later does not look empty
  SessionView* vp = nullptr;
  if (!is_active) vp = &views_.get_or_create(sid);
  if (event == "turn.started") {
    // The turn is in flight now: the composer's primary button becomes 中止, and
    // any parked state is stale (a new turn is running). The interrupt bar must
    // disappear NOW - a new turn means the task is no longer parked.
    if (is_active) {
      update([&](AppState& s) {
        s.turn_running = true;
        s.suspended = false;
        s.interrupted = false;
        s.interrupt_reason.clear();
      });
    } else {
      vp->turn_running = true;
      vp->suspended = false;
      vp->interrupted = false;
      vp->interrupt_reason.clear();
    }
  } else if (event == "loop.resumed") {
    // 继续 starts a turn WITHOUT turn.started, so the interrupt bar used to stay
    // on screen for the whole continuation and every further click started
    // another continue. Clear it (and show the turn as running) the moment the
    // backend confirms the continuation.
    if (is_active) {
      update([&](AppState& s) {
        s.interrupted = false;
        s.interrupt_reason.clear();
        s.turn_running = true;
      });
    } else {
      vp->interrupted = false;
      vp->interrupt_reason.clear();
      vp->turn_running = true;
    }
    render();
  } else if (event == "stream.delta") {
    std::string text = jsonutil::get<std::string>(payload, "text", "");
    std::vector<comp::ChatMsg>& m = is_active ? st_.messages : vp->messages;
    int& total = is_active ? st_.history_total : vp->history_total;
    // A NEW reply starts a NEW message. Testing only `role != "assistant"` was
    // wrong: the previous turn's answer is also an assistant message (just no
    // longer streaming), so the next turn's text was appended onto it - two
    // answers fused into one bubble. ensure_turn_message owns the creation so
    // every event that can open a turn (delta / tool.start / interim) agrees on
    // the message's defaults, including the work fold being open while it runs.
    comp::ChatMsg& msg = ensure_turn_message(m, total, &next_msg_rev_, now_ms());
    msg.text += text;
    msg.streaming = true;
    msg.rev = next_msg_rev_++;
    // PAINT NOW, not on a timer. The SSE frame that delivered this delta is a
    // normal task, so painting from here is not throttled; a timer-driven paint
    // is (Chrome clamps timers in occluded/background pages to ~1 Hz, which made
    // a whole reply appear in one or two lumps after it had finished).
    //
    // The 25 ms floor is self-clocking rather than a timer: while deltas keep
    // arriving, the next one past the floor does the paint; when they stop,
    // turn.done paints the final state. Nothing depends on setInterval.
    int64_t now = now_ms();
    // Stamp the live counter on the same clock that paints it: pump() may be
    // throttled to 1 Hz in an occluded tab, and the elapsed time must still move
    // while deltas keep arriving.
    msg.now_ms = now;
    if (now - last_stream_paint_ms_ >= 25) {
      last_stream_paint_ms_ = now;
      stream_dirty_ = false;
      render();
      follow_history_if_pinned();
    } else {
      stream_dirty_ = true;
    }
  } else if (event == "turn.done") {
    // The final paint of the turn: flush any delta held back by the 25 ms floor.
    stream_dirty_ = false;
    // A turn that finished while you were looking at it is NOT news: mark the
    // session as seen, or its blue "done" dot would appear on a transcript you
    // just watched finish.
    if (is_active && !sid.empty()) {
      session_seen_ms_[sid] = wall_ms();
      call_async("session.markSeen", Json{{"sid", sid}, {"ms", session_seen_ms_[sid]}});
    }
    // A turn just spent credit: refresh the provider balances (the usage page and
    // the sidebar show them). Throttled inside load_balances.
    load_balances(true);
    // The turn is over: the primary button goes back to 发送. Derived from the
    // event, not from a click, so it cannot drift from the backend.
    if (is_active) {
      update([&](AppState& s) { s.turn_running = false; });
    } else {
      vp->turn_running = false;
    }
    std::vector<comp::ChatMsg>& m = is_active ? st_.messages : vp->messages;
    if (!m.empty() && m.back().role == "assistant") {
      m.back().streaming = false;
      // The work is done: fold the process. You watch it while it runs; after
      // that the answer is what you reread, and a long tool log above it buries
      // the reply. A reopened transcript starts folded too, so live and stored
      // now agree. One click reopens it.
      m.back().work_open = false;
      m.back().work_touched = false;
      m.back().work_seq = next_msg_rev_++;  // fold transition (see work_seq)
      // Prefer the backend's own start/end: it knows when the request went out,
      // while the UI only sees the first delta. Falling back to local times
      // keeps the counter correct if an older backend sends no timestamps.
      int64_t be = jsonutil::get_int(payload, "endedAt", 0);
      int64_t bs = jsonutil::get_int(payload, "startedAt", 0);
      m.back().ended_at = be > 0 ? be : now_ms();
      if (bs > 0) m.back().started_at = bs;
      if (m.back().started_at <= 0) m.back().started_at = m.back().ended_at;
      m.back().rev = next_msg_rev_++;
    }
    // The turn just spent tokens, so the side panel's cost/context numbers are
    // stale. Refresh them for the session that actually ran.
    std::string usage_sid = sid.empty() ? st_.active_tab : sid;
    if (!usage_sid.empty()) {
      if (is_active) {
        st_.session_usage_loaded = false;
      } else {
        vp->session_usage_loaded = false;
      }
      load_session_usage(usage_sid);
      // Reconcile the composer with the BACKEND after the turn: a stale
      // session.loopState reply from before the turn could otherwise leave 中止
      // on screen (refresh_loop_state only honours the newest request).
      if (is_active) refresh_loop_state(usage_sid);
    }
  } else if (event == "loop.usage") {
    // Per-CALL usage: the side panel's context level / cache / this-turn numbers
    // move as each model round comes back, instead of jumping once at turn end.
    // Everything here is a FACT the provider reported for THAT call. A background
    // session is skipped: its panel numbers are re-read from usage.session when
    // the tab is opened.
    if (!is_active) return;
    int64_t in = jsonutil::get_int(payload, "in", -1);
    int64_t out = jsonutil::get_int(payload, "out", -1);
    int64_t cached = jsonutil::get_int(payload, "cached", -1);
    bool cache_known = jsonutil::get<bool>(payload, "cacheKnown", false);
    double cost = jsonutil::get_number(payload, "cost", 0.0);
    std::string cost_kind = jsonutil::get<std::string>(payload, "costKind", "");
    update([&](AppState& s) {
      if (in >= 0) {
        s.turn_in = in;
        if (out >= 0) s.turn_out = out;
        s.context_tokens = in;
        if (s.context_window > 0) {
          s.context_pct = static_cast<int>(in * 100 / s.context_window);
        }
        // Between the authoritative usage.session refreshes (turn.done), keep a
        // running total so "累计 tokens" does not freeze mid-turn.
        if (s.session_tokens_in >= 0) s.session_tokens_in += in;
        if (out >= 0 && s.session_tokens_out >= 0) s.session_tokens_out += out;
      }
      // Cache hit = cached input / total input. Only shown when the endpoint
      // actually reported a cache figure; a provider that says nothing is "—",
      // not "0%".
      if (cache_known && cached >= 0 && in > 0) {
        s.cache_reported = true;
        s.session_cache_tokens = cached;
        s.cache_hit_pct = static_cast<int>(cached * 100 / in);
      }
      if (!cost_kind.empty()) {
        s.turn_cost_kind = cost_kind;
        s.turn_cost_micros = static_cast<int64_t>(cost * 1000000.0);
        if (cost_kind == "money" && cost > 0) {
          char buf[48];
          std::snprintf(buf, sizeof(buf), "¥%.4f", cost);
          s.session_cost = buf;
        }
      }
    });
  } else if (event == "assistant.interim") {
    // A round of the turn finished with narration (the model talking to itself
    // between tool calls). File it under the CURRENT assistant message's work log
    // instead of appending it to the answer: it is process, not the answer, and
    // gluing it on made the reply look like it repeated itself.
    std::string text = jsonutil::get<std::string>(payload, "text", "");
    std::vector<comp::ChatMsg>& m = is_active ? st_.messages : vp->messages;
    int& total = is_active ? st_.history_total : vp->history_total;
    if (!text.empty()) {
      comp::ChatMsg& msg = ensure_turn_message(m, total, &next_msg_rev_, now_ms());
      // The same round text ALREADY reached the answer through stream.delta: the
      // provider streams the narration like any other text, and the loop only
      // learns at the end of the round that it was narration. Leaving it there
      // showed the process twice (in the fold AND as the reply), and because a
      // stored turn keeps narration only in `interim`, reopening the session - or
      // switching away and back, which rebuilds from the store - made it vanish.
      // Move it out of the answer so the live transcript matches the saved one.
      std::string& ans = msg.text;
      if (ans.size() >= text.size() &&
          ans.compare(ans.size() - text.size(), text.size(), text) == 0) {
        ans.erase(ans.size() - text.size());
      } else {
        size_t at = ans.rfind(text);
        if (at != std::string::npos) ans.erase(at, text.size());
      }
      msg.interim.push_back(text);
      // The anchor: how many tools had already run when this note arrived.
      int64_t done = jsonutil::get_int(payload, "tools", -1);
      msg.interim_after.push_back(
          done >= 0 ? static_cast<size_t>(done) : msg.tools.size());
      msg.rev = next_msg_rev_++;
      render();
    }
  } else if (event == "tool.start") {
    // A tool was invoked. Show it IMMEDIATELY with its arguments: a slow tool
    // (build, fetch) otherwise leaves the transcript looking frozen, and once it
    // finishes the block already exists so `tool.update` just fills it in.
    //
    // The assistant message is CREATED here when it does not exist yet. A model
    // that opens a turn with a pure tool call (no text) emits no stream.delta, so
    // there was no assistant message - and the old `m.back().role == "assistant"`
    // test then dropped the block entirely. That is why the work log was missing
    // and why the live timer never appeared for those turns.
    std::string name = jsonutil::get<std::string>(payload, "name", "");
    std::vector<comp::ChatMsg>& m = is_active ? st_.messages : vp->messages;
    int& total = is_active ? st_.history_total : vp->history_total;
    if (!name.empty()) {
      comp::ChatMsg& msg = ensure_turn_message(m, total, &next_msg_rev_, now_ms());
      comp::ToolBlock tb;
      tb.name = name;
      tb.summary = tr("运行中…");
      tb.running = true;
      // The ARGUMENTS are this block's input. They used to be written into
      // `detail`, which tool.update then overwrote with the result - so the row
      // looked like it never received a request. An empty object is not an
      // input: `{}` would render as a labelled section holding two braces.
      if (payload.contains("args") && payload["args"].is_object() &&
          !payload["args"].empty()) {
        tb.input = cut_display(dump_safe(payload["args"], 2), 6000,
                               "\n… (输入已截断)");
      }
      // Identity for tool_block_cached: an unchanged row yields the same VNode
      // on later renders instead of copying its strings again.
      tb.rev = next_msg_rev_++;
      msg.tools.push_back(tb);
      msg.rev = next_msg_rev_++;
      render();
    }
  } else if (event == "tool.update") {
    std::string name = jsonutil::get<std::string>(payload, "name", "");
    bool ok = jsonutil::get<bool>(payload, "ok", true);
    std::string summary = jsonutil::get<std::string>(payload, "summary", "");
    std::string detail = jsonutil::get<std::string>(payload, "detail", "");
    // A subagent tool's result carries the id it created, so the work-log row can
    // offer "打开记录". Without it the row only proved a subagent had run.
    std::string subagent_id;
    if (name == "subagent_spawn" || name == "subagent_follow_up") {
      try {
        Json d = Json::parse(detail);
        subagent_id = jsonutil::get<std::string>(d, "id", "");
      } catch (...) {
      }
    }
    std::vector<comp::ChatMsg>& m = is_active ? st_.messages : vp->messages;
    {
      int& total = is_active ? st_.history_total : vp->history_total;
      // Same rule as tool.start: a tool.update can arrive for a turn whose
      // assistant message was never created (pure tool-call round), and the block
      // must still be recorded.
      comp::ChatMsg& msg = ensure_turn_message(m, total, &next_msg_rev_, now_ms());
      // Complete the block `tool.start` created for this call. Matching by name
      // is enough: the loop runs one tool at a time, so the newest unfinished
      // block with that name IS this call. Creating a second block here (the old
      // behaviour) doubled every row: "running" plus "ok".
      comp::ToolBlock* slot = nullptr;
      for (size_t i = msg.tools.size(); i-- > 0;) {
        if (msg.tools[i].running && msg.tools[i].name == name) {
          slot = &msg.tools[i];
          break;
        }
      }
      if (!slot) {
        msg.tools.push_back(comp::ToolBlock());
        slot = &msg.tools.back();
      }
      slot->name = name;
      slot->ok = ok;
      slot->running = false;
      slot->summary = !summary.empty() ? summary : (ok ? "ok" : "failed");
      if (!detail.empty()) slot->detail = detail;
      if (!subagent_id.empty()) slot->subagent_id = subagent_id;
      // The row changed -> new cache identity (see tool_block_cached).
      slot->rev = next_msg_rev_++;
      msg.rev = next_msg_rev_++;
      render();
    }
  } else if (event == "subagent.done") {
    std::string id = jsonutil::get<std::string>(payload, "id", "");
    std::vector<AppState::SubagentRow>& subs = is_active ? st_.subagents : vp->subagents;
    for (size_t i = 0; i < subs.size(); ++i) {
      if (subs[i].id == id) subs[i].status = "done";
    }
    // A transcript tab for this agent should show the final state too (it was
    // opened while the agent was still running, which is the common case).
    std::map<std::string, AppState::SubagentDetail>::iterator d =
        st_.subagent_details.find(id);
    if (d != st_.subagent_details.end() && d->second.loaded) {
      load_subagent_detail(id);
    }
  } else if (event == "subagent.started") {
    // A NEW subagent gets its row here. The panel's rows come from
    // `subagent.list`, which was fetched when the tab was opened - a subagent
    // that starts AFTER that had no row at all, so its live text (the tagged
    // stream.delta events look the row up by id) was dropped and the tab badge
    // stayed at the old count.
    std::string id = jsonutil::get<std::string>(payload, "id", "");
    if (!id.empty()) {
      std::vector<AppState::SubagentRow>& subs =
          is_active ? st_.subagents : vp->subagents;
      bool known = false;
      for (size_t i = 0; i < subs.size(); ++i) {
        if (subs[i].id == id) { known = true; break; }
      }
      if (!known) {
        AppState::SubagentRow row;
        row.id = id;
        row.kind = jsonutil::get<std::string>(payload, "kind", "");
        row.task = jsonutil::get<std::string>(payload, "task", "");
        row.model = jsonutil::get<std::string>(payload, "model", "");
        row.status = "running";
        row.started_at = now_ms();
        subs.push_back(row);
      }
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
      // A terminal appeared (the model started one): the session is now doing
      // background work, so its dot changes. Marked here rather than per output
      // chunk - see the note in on_backend_event.
      mark_sessions_dirty();
    }
  }
}

void UiApp::on_backend_event(const std::string& event, const Json& payload) {
  // Times the whole handler (including its render); a freeze caused by one
  // event is written to the resource log (see SlowEventLog).
  SlowEventLog slow_event_log(event);
  ++event_count_;
  std::string sid = jsonutil::get<std::string>(payload, "sid", "");
  // Anything that changes a session's STATUS also changes a project's dot (and
  // the sidebar rows). The dot used to be fed only by `session.list`, which was
  // fetched on load and never again - so a project looked idle while its agent
  // worked and its terminal printed. Coalesced in pump(); one refresh per burst.
  if (event == "turn.started" || event == "turn.done" ||
      event == "loop.state" || event == "loop.interrupted" ||
      event == "loop.paused" || event == "auto.next" ||
      event == "tool.start" || event == "terminal.exit") {
    mark_sessions_dirty();
  }
  // A workflow event changes BOTH the sidebar panel and the session dot (a run is
  // background work). The panel used to update only when it was reopened.
  if (event.compare(0, 8, "workflow") == 0) {
    workflows_dirty_ = true;
    mark_sessions_dirty();
  }
  // NOTE: terminal.OUTPUT is not in that list on purpose - a chatty terminal
  // would refresh the lists 1.25 times a second forever. Only the appearance of a
  // NEW terminal changes the count, and that is marked where it is registered
  // (see the terminal.output branch).
  // A delta only appends text, and a fast model emits hundreds per poll burst.
  // Repainting per delta starves the browser's event loop (the next poll never
  // runs), which makes the stream LOOK frozen. pump() paints them on a ~50 ms
  // cadence instead; every other event repaints immediately.
  const bool coalesced = (event == "stream.delta");
  // session-scoped events go to their own view (may be a background tab)
  if (!sid.empty() && sid != st_.active_tab) {
    // A background tab that is working gets the dot on its tab: without it the
    // only way to know another session produced something was to click every tab.
    if (event == "turn.started" || event == "turn.done" ||
        event == "loop.interrupted" || event == "stream.delta" ||
        event == "tool.start" || event == "assistant.interim") {
      for (size_t i = 0; i < st_.sessions.size(); ++i) {
        if (st_.sessions[i].id == sid) {
          st_.sessions[i].dirty = true;
          break;
        }
      }
    }
    apply_session_event(sid, event, payload);
    if (!coalesced) render();
    return;
  }
  if (event == "stream.delta" || event == "turn.done" || event == "turn.started" ||
      event == "tool.start" ||
      event == "tool.update" || event == "assistant.interim" ||
      event == "subagent.done" || event == "loop.usage" ||
      event == "terminal.exit" || event == "terminal.output") {
    apply_session_event(sid, event, payload);
  } else if (event == "permission.prompt") {
    st_.ask_modal = true;
    st_.ask_id = 0;  // permission gate, not an ask question
    st_.ask_options.clear();
    st_.ask_text = "允许执行 " + jsonutil::get<std::string>(payload, "tool", "") + "？";
  } else if (event == "ask.prompt") {
    // The model is ASKING and the turn is parked until answered. A reply can
    // carry SEVERAL questions; they are shown in the composer area as "1/3" and
    // answered one at a time (see build_composer), not as a floating dialog that
    // covers the conversation and renders options as plain text.
    st_.asks.clear();
    if (payload.contains("asks") && payload["asks"].is_array()) {
      for (auto& a : payload["asks"]) {
        if (!a.is_object()) continue;
        AppState::AskItem item;
        item.ask_id = static_cast<int>(jsonutil::get_int(a, "askId", 0));
        item.question = jsonutil::get<std::string>(a, "question", "");
        if (a.contains("options") && a["options"].is_array()) {
          for (auto& o : a["options"]) {
            if (o.is_string()) item.options.push_back(o.get<std::string>());
          }
        }
        item.recommended = static_cast<int>(jsonutil::get_int(a, "recommended", 0));
        item.allow_custom = jsonutil::get<bool>(a, "allowCustom", true);
        st_.asks.push_back(item);
      }
    }
    if (st_.asks.empty()) {
      // Older backend: single-question fields.
      AppState::AskItem item;
      item.ask_id = static_cast<int>(jsonutil::get_int(payload, "askId", 0));
      item.question = jsonutil::get<std::string>(payload, "question", "");
      if (payload.contains("options") && payload["options"].is_array()) {
        for (auto& o : payload["options"]) {
          if (o.is_string()) item.options.push_back(o.get<std::string>());
        }
      }
      item.recommended = static_cast<int>(jsonutil::get_int(payload, "recommended", 0));
      item.allow_custom = jsonutil::get<bool>(payload, "allowCustom", true);
      st_.asks.push_back(item);
    }
    st_.ask_index = 0;
    st_.ask_modal = true;
    st_.interrupted = false;  // this is a question, not a failure
    st_.interrupt_reason.clear();
  } else if (event == "auto.next") {
    // An automatic round is starting. Say so: otherwise the next answer simply
    // appeared under the previous one and the session looked stuck.
    int round = static_cast<int>(jsonutil::get_int(payload, "round", 1));
    st_.auto_round = round;
    std::string reason = jsonutil::get<std::string>(payload, "reason", "");
    st_.toast = "自动继续（第 " + itos(round) + " 轮）" +
                (reason.empty() ? "" : "：" + reason);
    st_.toast_deadline_ms = now_ms() + kToastMs;
    // The purple dot means "running in the background": set it on the session
    // row too, so the home/project lists show it without opening the tab.
    // (SessionTab is a tab, not a session row - the rows live on the project
    // cards, so they are refreshed from the backend instead.)
    if (!sid.empty()) {
      for (size_t i = 0; i < st_.projects.size(); ++i) load_sessions(i);
    }
  } else if (event == "auto.halted") {
    st_.auto_round = 0;
    st_.toast = "自动继续已停止（达到轮数上限）";
    st_.toast_deadline_ms = now_ms() + kToastMs;
  } else if (event == "context.compressed") {
    int dropped = static_cast<int>(jsonutil::get_int(payload, "droppedTurns", 0));
    st_.toast = "上下文已压缩" + (dropped > 0 ? "（折叠 " + itos(dropped) + " 条）" : "");
    st_.toast_deadline_ms = now_ms() + kToastMs;
  } else if (event == "subagent.steered") {
    st_.toast = "子代理已收到引导";
    st_.toast_deadline_ms = now_ms() + kToastMs;
  } else if (event == "todo.updated") {
    // The model's own checklist (todo_add/todo_check, one owner per session).
    // It is the ONLY progress view in the composer: the plan is a document the
    // model maintains, not a step tree to render.
    std::vector<AppState::TodoItem> list =
        parse_todo_items(payload.contains("items") ? payload["items"] : Json::array());
    // The host tags the list with its OWNER: a session id for the main agent, a
    // subagent id for a subagent's private tree. The panel is the session's, so
    // a subagent's checklist must not replace it.
    std::string owner = jsonutil::get<std::string>(payload, "owner", "");
    std::string routed = sid.empty() ? st_.active_tab : sid;
    if (!owner.empty() && !routed.empty() && owner != routed) return;
    if (sid.empty() || sid == st_.active_tab) {
      bool was_empty = st_.todos.empty();
      st_.todos = list;
      // A NEW checklist opens its panel: the model just wrote down what it is
      // about to do, and that is exactly the moment the user wants to see it. A
      // list the user has closed stays closed on later updates.
      if (!list.empty() && was_empty) st_.todo_open = true;
    } else {
      SessionView& v = views_.get_or_create(sid);
      bool was_empty = v.todos.empty();
      v.todos = list;
      if (!list.empty() && was_empty) v.todo_open = true;
    }
    render();
  } else if (event == "plan.update") {
    // `plan.update` carries the DOCUMENT (path/status/content), not a step tree
    // to draw. There is no plan panel: the file is the plan, and progress is the
    // task list. Nothing to do here - the event is informational.
  } else if (event == "loop.paused") {
    // A guard tripped (tool-call limit / repeated call). The turn still produced
    // output, so this is a notice, not the "interrupted, continue?" bar: that bar
    // made a safety limit look like a crash.
    st_.toast = jsonutil::get<std::string>(payload, "note", "本轮已暂停");
    st_.toast_deadline_ms = now_ms() + kToastMs * 2;
  } else if (event == "loop.interrupted") {
    // A bare toast is a dead end: the task stopped and the only useful next
    // action ("continue from here") had no button. The flag shows a persistent
    // bar above the composer until the user continues or sends something new.
    //
    // The REASON matters: "任务已中断" alone looks like a UI glitch when the real
    // cause is a rejected API key or a 429. The backend puts it in `error`.
    st_.interrupted = true;
    st_.turn_running = false;  // nothing is running after an interrupt
    st_.interrupt_reason = jsonutil::get<std::string>(payload, "error", "");
    // Watched live = already seen: the row's red dot is for news you have NOT
    // looked at (the error line is in the transcript right here).
    if (!st_.active_tab.empty() && st_.active_tab != "home") {
      session_seen_ms_[st_.active_tab] = wall_ms();
      call_async("session.markSeen",
                 Json{{"sid", st_.active_tab}, {"ms", session_seen_ms_[st_.active_tab]}});
    }
    // The turn ends mid-stream here too (no turn.done), so the last message has
    // to be closed by hand: it kept its "streaming" flag and the transcript
    // showed a spinner + a counting timer next to a send button that had already
    // gone back to 发送 ("history 还显示加载中"). The reason is stored on the
    // message so the failure stays visible after the toast is gone.
    //
    // A turn that failed BEFORE producing anything has no assistant message yet
    // (the provider rejected the very first call). One is created here, because a
    // failure that exists only in a three-second toast is invisible a moment
    // later - and the transcript then looked like the question was swallowed.
    if (st_.messages.empty() || st_.messages.back().role != "assistant") {
      comp::ChatMsg m;
      m.role = "assistant";
      m.rev = next_msg_rev_++;
      m.uid = st_.history_total + static_cast<int64_t>(st_.messages.size());
      m.started_at = now_ms();
      m.ended_at = m.started_at;
      m.error = st_.interrupt_reason.empty() ? tr("未知错误") : st_.interrupt_reason;
      st_.messages.push_back(m);
    } else {
      comp::ChatMsg& m = st_.messages.back();
      m.streaming = false;
      m.now_ms = 0;
      // An interrupted turn is over too: fold the process the same way turn.done
      // does, so the error line and the partial answer are what remain visible.
      m.work_open = false;
      m.work_touched = false;
      m.work_seq = next_msg_rev_++;  // fold transition (see work_seq)
      if (m.ended_at <= 0) m.ended_at = now_ms();
      if (m.started_at <= 0) m.started_at = m.ended_at;
      if (!st_.interrupt_reason.empty()) m.error = st_.interrupt_reason;
      m.rev = next_msg_rev_++;
    }
    st_.toast = st_.interrupt_reason.empty()
                    ? "已中断（可继续）"
                    : ("已中断：" + st_.interrupt_reason);
    st_.toast_deadline_ms = now_ms() + kToastMs;
    // Ask the backend too: the click/state we set above is our best guess, and
    // the fresh reply also invalidates any older in-flight one.
    if (!st_.active_tab.empty()) refresh_loop_state(st_.active_tab);
  } else if (event == "loop.state") {
    std::string state = jsonutil::get<std::string>(payload, "state", "");
    if (state == "resumed" || state == "turnDone" || state == "idle") {
      st_.interrupted = false;
      st_.interrupt_reason.clear();
    }
    // PARKED = the turn stopped on purpose. It ends mid-stream, so no turn.done
    // follows - without this the last assistant message kept its "streaming"
    // flag and the transcript showed a spinner and a running timer for a task
    // that had already stopped ("停止生成/挂起做不完整"). This branch only sees
    // the ACTIVE session (a background one returns earlier), and switching to it
    // reloads the transcript from disk, where streaming is always false.
    if (state == "suspended") {
      st_.turn_running = false;
      std::vector<comp::ChatMsg>& m = st_.messages;
      if (!m.empty() && m.back().role == "assistant" && m.back().streaming) {
        // A park before the first token leaves an empty bubble behind; drop it
        // instead of showing a blank answer. Text that DID arrive stays - it is
        // the part the user is keeping by stopping here.
        if (m.back().text.empty() && m.back().tools.empty() &&
            m.back().interim.empty() && m.back().thinking.empty()) {
          m.pop_back();
        } else {
          m.back().streaming = false;
          m.back().now_ms = 0;
          m.back().ended_at = now_ms();
          if (m.back().started_at <= 0) m.back().started_at = m.back().ended_at;
          m.back().rev = next_msg_rev_++;
        }
      }
    }
    if (!sid.empty()) {
      refresh_loop_state(sid);
      refresh_pending_wakes();
    }
  }
  // Deltas paint inline from apply_session_event (the SSE frame itself is the
  // clock, so occluded-tab timer throttling cannot delay them); this is only the
  // "last delta of a burst was held back by the 25 ms floor" case, and it runs on
  // the next delta or on turn.done.
  if (!coalesced) render();
  if (event == "stream.delta" || event == "tool.update" || event == "turn.done") {
    follow_history_if_pinned();
  }
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

std::string UiApp::debug_stats() const {
  size_t tools = 0;
  for (size_t i = 0; i < st_.messages.size(); ++i) tools += st_.messages[i].tools.size();
  size_t term_bytes = 0;
  for (std::map<std::string, std::string>::const_iterator it = st_.term_outputs.begin();
       it != st_.term_outputs.end(); ++it) {
    term_bytes += it->second.size();
  }
  char buf[512];
  std::snprintf(buf, sizeof(buf),
                "msgs=%d tools=%d msg_nodes=%d views=%d details=%d subagents=%d "
                "ops=%d todos=%d terms=%d term_bytes=%d",
                static_cast<int>(st_.messages.size()), static_cast<int>(tools),
                static_cast<int>(msg_nodes_.size()), static_cast<int>(views_.size()),
                static_cast<int>(st_.subagent_details.size()),
                static_cast<int>(st_.subagents.size()),
                static_cast<int>(st_.session_ops.size()),
                static_cast<int>(st_.todos.size()),
                static_cast<int>(st_.terminals.size()),
                static_cast<int>(term_bytes));
  return std::string(buf);
}

void UiApp::render() {
  // With synchronous backends (NativeBackend), a loader triggered from inside
  // a build can complete and call update()->render() while we are still
  // building. Defer that nested render instead of patching mid-build.
  if (render_in_progress_) {
    render_pending_ = true;
    return;
  }
  // How long a paint takes. On the wasm build this blocks the whole tab, so a
  // slow one IS the "opening a big history freezes" report - log it (stderr
  // reaches the browser console, the native build's stderr goes to its console).
  const int64_t render_t0 = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
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
        }
      }
    }
    patch(dom_, tree_, neu, root_);
    // Ship this frame's DOM writes as one batch before anything below READS the
    // DOM (the scroll restore reads real heights, the wiring binds handlers to
    // live nodes). A bridged adapter buffers per-node writes; without this they
    // would still be sitting in its buffer while those calls execute.
    dom_.flush();
    tree_ = neu;
    last_rendered_tab_ = st_.active_tab;
    // The transcript (if this render created one) is in the DOM now, so a
    // pending restore can be applied against the REAL height in this same task -
    // before the browser paints. Doing it in a later tick is what made a return
    // visit show the top of the transcript and jump afterwards.
    apply_pending_history_scroll();
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
    // (re)wire the chat's scroll edges. Two jobs, both keyed off the SAME
    // report so they cannot disagree:
    //   * at_top    -> the user wants older turns; load the previous page.
    //   * at_bottom -> new output keeps following the end. Following is a
    //     PERSISTENT flag: it is turned on when the user lands at the bottom and
    //     off as soon as they scroll away, so a streamed reply never yanks the
    //     view down while someone is reading history.
    {
      VNodePtr hist = find_by_key(tree_, "chat-history");
      if (hist && hist->dom.valid()) {
        dom_.bind_scroll_edges(hist->dom, [this](bool at_top, bool at_bottom) {
          if (at_bottom) st_.hist_follow = true;
          else st_.hist_follow = false;
          // Keep the remembered offset CURRENT on every scroll, but NOT while a
          // restore is in flight: the restore's own scroll event reports the
          // clamped position and would overwrite the offset we are restoring to.
          if (!restoring_scroll_) {
            VNodePtr h = find_by_key(tree_, "chat-history");
            if (h && h->dom.valid()) st_.hist_scroll = dom_.scroll_top(h->dom);
          }
          if (at_top && st_.history_has_more && !st_.history_loading_older) {
            load_older_history();
          }
        });
      }
    }
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
    // Top tab strip: drag a tab onto another to reorder it.
    VNodePtr tabstrip = find_by_key(tree_, "top-tabs");
    if (tabstrip && tabstrip->dom.valid()) {
      dom_.bind_tab_drag(tabstrip->dom, [this](const std::string& payload) {
        // "from\tto\tafter" - `after` says which half of the target was under
        // the pointer (1 = drop after it).
        size_t a = payload.find('\t');
        if (a == std::string::npos) return;
        size_t b = payload.find('\t', a + 1);
        std::string from = payload.substr(0, a);
        std::string to = b == std::string::npos ? payload.substr(a + 1)
                                                : payload.substr(a + 1, b - a - 1);
        bool after = b != std::string::npos && b + 1 < payload.size() &&
                     payload[b + 1] == '1';
        move_tab(from, to, after);
      });
    }
    // Every open model picker: anchor it to its button (flip down when there is
    // no room above) and dismiss it on a press outside. The owner name can be
    // dynamic (subagent rows), so the popup is found by key prefix.
    {
      VNodePtr pop = find_by_key_prefix(tree_, "model-picker:");
      VNodePtr anchor = find_by_key_prefix(tree_, "model-field:");
      if (pop && !anchor) anchor = find_by_key(tree_, "model-btn-wrap");
      if (pop && anchor && pop->dom.valid() && anchor->dom.valid()) {
        dom_.place_popup(pop->dom, anchor->dom);
      }
      if (pop && pop->dom.valid()) {
        dom_.bind_outside_click(pop->dom, [this]() {
          update([&](AppState& s) {
            s.model_picker_open = false;
            s.model_picker_owner.clear();
          });
        });
      }
    }
    // Context menu: closes on a press outside, WITHOUT swallowing that press -
    // so right-clicking another row closes this menu and immediately opens the
    // menu for what was just clicked.
    //
    // Looked up by KEY, not through the cached `ctx_menu_node_`: that pointer was
    // captured when the menu was first built, and after the menu was rebuilt
    // (another right-click) it referred to a node that is no longer in the tree -
    // so the binding went to a detached element and the visible menu never
    // closed. The key survives the rebuild.
    if (st_.ctx_open) {
      VNodePtr menu = find_by_key(tree_, "ctx-menu");
      if (menu && menu->dom.valid()) {
        dom_.bind_outside_click(menu->dom, [this]() {
          update([&](AppState& s) { s.ctx_open = false; });
        });
      }
    }
    // Modals: losing focus dismisses them, with the exit animation. The node
    // carries the same callbacks the inline backdrop click uses, so Esc, a press
    // on the backdrop and a press anywhere else all take the SAME path (clearing
    // the state) and the framework plays exit_anim because the node is keyed.
    //
    // The backdrop click alone was not enough: the backdrop only covers the
    // viewport of the element that rendered it, so a dialog opened over a tag
    // page did not react to a press on the tag strip above it.
    if (st_.ask_modal) {
      VNodePtr m = find_by_key_prefix(tree_, "modal:");
      if (m && m->dom.valid()) {
        dom_.bind_outside_click(m->dom, [this]() {
          std::string sid = active_sid();
          bool permission_gate = st_.asks.empty();
          update([&](AppState& s) { s.ask_modal = false; });
          if (!permission_gate || sid.empty()) return;
          // A dismissed PERMISSION gate is a refusal, not a no-op: the loop is
          // parked waiting for an answer, and leaving it parked forever would
          // hang the turn. Deny explicitly, exactly like the 取消 button.
          call_async("session.approve", Json{{"sid", sid}, {"yes", false}});
        });
      }
    }
    if (!st_.confirm_kind.empty()) {
      VNodePtr m = find_by_key_prefix(tree_, "modal:");
      if (m && m->dom.valid()) {
        dom_.bind_outside_click(m->dom, [this]() {
          update([&](AppState& s) {
            s.confirm_kind.clear();
            s.confirm_id.clear();
            s.confirm_arg.clear();
            s.confirm_text.clear();
          });
        });
      }
    }
    // Side panel tabs: same drag-to-reorder as the top strip.
    VNodePtr sidetabs = find_by_key(tree_, "side-tabs");
    if (sidetabs && sidetabs->dom.valid()) {
      dom_.bind_tab_drag(sidetabs->dom, [this](const std::string& payload) {
        size_t a = payload.find('\t');
        if (a == std::string::npos) return;
        size_t b = payload.find('\t', a + 1);
        std::string from = payload.substr(0, a);
        std::string to = b == std::string::npos ? payload.substr(a + 1)
                                                : payload.substr(a + 1, b - a - 1);
        bool after = b != std::string::npos && b + 1 < payload.size() &&
                     payload[b + 1] == '1';
        move_side_tab(from, to, after);
      });
    }
    // Project nav rail divider (left mode): its own binding, so dragging it
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
    // document-level keys: app shortcuts + dismissing transient overlays.
    //
    // The shortcut table (Settings > 快捷键) used to save values that NOTHING
    // read back - four text fields and no consumer, so every binding in it was
    // decoration. They are matched here, against the same settings keys the page
    // writes, with modifiers included in the reported key.
    dom_.bind_global_key([this](const std::string& key) {
      if (shortcut_matches(key, "关闭标签页", "Ctrl+W")) {
        if (st_.active_tab != "home" && !st_.active_tab.empty()) {
          close_tab(st_.active_tab);
        }
        return;
      }
      if (shortcut_matches(key, "新建标签页", "Ctrl+T")) {
        open_new_tab();
        return;
      }
      if (key != "Escape") return;
      // Order matters: the topmost transient thing closes first, and a modal
      // (which covers the page) outranks the menus underneath it.
      if (st_.ask_modal) {
        // A permission gate is a park: dismissing it must DENY, or the turn
        // waits for an answer nobody will give.
        std::string sid = active_sid();
        bool permission_gate = st_.asks.empty();
        update([&](AppState& s) { s.ask_modal = false; });
        if (permission_gate && !sid.empty()) {
          call_async("session.approve", Json{{"sid", sid}, {"yes", false}});
        }
        return;
      }
      if (!st_.confirm_kind.empty()) {
        update([&](AppState& s) {
          s.confirm_kind.clear();
          s.confirm_id.clear();
          s.confirm_arg.clear();
          s.confirm_text.clear();
        });
        return;
      }
      if (st_.model_picker_open) {
        update([&](AppState& s) {
          s.model_picker_open = false;
          s.model_picker_owner.clear();
        });
        return;
      }
      if (st_.ctx_open) {
        update([&](AppState& s) { s.ctx_open = false; });
        return;
      }
      if (st_.skill_menu_open || st_.thinking_menu_open || st_.side_menu_open) {
        update([&](AppState& s) {
          s.skill_menu_open = false;
          s.thinking_menu_open = false;
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
  {
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const int64_t dt = now - render_t0;
    if (dt >= 200) {
      std::fprintf(stderr,
                   "[slow] render ms=%lld msgs=%d tab=%s nodes=%d\n",
                   static_cast<long long>(dt),
                   static_cast<int>(st_.messages.size()),
                   st_.active_tab.c_str(), static_cast<int>(msg_nodes_.size()));
    }
  }
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
      [this](const std::string&) { open_new_tab(); },
      [this](const std::string& v) {
    // "<tabKey>\t<x>\t<y>" - the key is prepended by tab_strip because one
    // handler serves every tab.
    size_t t1 = v.find('\t');
    if (t1 == std::string::npos) return;
    std::string key = v.substr(0, t1);
    size_t t2 = v.find('\t', t1 + 1);
    int x = std::atoi(v.substr(t1 + 1, t2 == std::string::npos ? std::string::npos
                                                               : t2 - t1 - 1).c_str());
    int y = t2 == std::string::npos ? 0 : std::atoi(v.substr(t2 + 1).c_str());
    update([&](AppState& s) {
      s.ctx_open = true;
      s.ctx_x = x;
      s.ctx_y = y;
      s.ctx_kind = "tab";
      s.ctx_id = key;
    });
  });
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
  // Stagger: the incoming page waits until the outgoing one is ~80% gone.
  // Expressed INLINE rather than as a class: it is re-applied by every render
  // during the transition (so a mid-transition re-render cannot drop it and make
  // the entrance jump to its end) and the next switch simply overwrites it (so
  // the delay cannot leak into a later animation).
  H pageh(page);
  pageh.style("animation-delay", page_dir_.empty() ? "0s" : ".2s");
  if (!page_dir_.empty()) pageh.style("animation-fill-mode", "backwards");
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
// 会话状态点：运行中=转圈、待确认=黄、报错=红、完成=蓝、空闲=空心。
// 颜色是"这一格现在怎么样"的唯一提示，所以放在标题前面而不是 hover 里。
H session_dot_with(const std::string& base, const std::string& loop_state,
                   const std::string& last_error, bool running,
                   int terminal_count = 0, int auto_round = 0) {
  std::string cls = base;
  std::string title = tr("空闲");
  if (loop_state == "waiting") {
    cls += " waiting";
    title = tr("等待你确认");
  } else if (running) {
    cls += " running";
    title = tr("正在运行");
  } else if (!last_error.empty()) {
    cls += " failed";
    title = tr("上次运行出错");
  } else if (auto_round > 0 || terminal_count > 0) {
    // Purple: background work. An auto-continue round is exactly this - the
    // session is doing work nobody typed - and a live terminal can still print.
    // Without the auto_round term the dot read "idle" while the agent was
    // actually running the next round, which is the "没有紫色小圆点" report.
    cls += " background";
    if (auto_round > 0) {
      title = tr("自动继续中（第 ") + itos(auto_round) + tr(" 轮）");
      if (terminal_count > 0) title += tr("，终端 ") + itos(terminal_count) + tr(" 个");
    } else {
      title = tr("后台运行中（终端 ") + itos(terminal_count) + tr(" 个）");
    }
  } else if (!loop_state.empty() && loop_state != "idle") {
    cls += " done";
    title = tr("已完成");
  }
  H d = el("span", cls);
  d.attr("title", title);
  return d;
}
H session_dot(const std::string& loop_state, const std::string& last_error,
              bool running, int terminal_count = 0, int auto_round = 0) {
  return session_dot_with("ps-dot", loop_state, last_error, running, terminal_count,
                          auto_round);
}

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
  head.add(comp::icon_button("magnifying-glass", tr("搜索"), [this](const std::string&) {
    update([&](AppState& s) { s.home_tab = 3; });
    show("home");
  }));
  head.add(comp::icon_button("chevron-left", tr("收起侧栏"), [this](const std::string&) {
    update([&](AppState& s) { s.nav_collapsed = true; });
  }));
  rail.add(head);

  // 排序/筛选保留在栏里（它很短），但搜索不放在这里：左栏本来就窄，塞一个
  // 输入框会把会话标题挤到只剩几个字。点放大镜跳到全局搜索页。
  H tools = el("div", "nav-tools");
  H sortrow = el("div", "nav-sort");
  const char* sortLabels[3] = {"最近", "名称", "状态"};
  const char* sortVals[3] = {"", "title", "status"};
  for (int i = 0; i < 3; ++i) {
    std::string v = sortVals[i];
    H b = el("div", st_.session_sort == v ? "nav-sort-btn active" : "nav-sort-btn");
    b.on(EventKind::kClick, [this, v](const std::string&) {
      update([&](AppState& s) { s.session_sort = v; });
    });
    b.text(tr(sortLabels[i]));
    sortrow.add(b);
  }
  tools.add(sortrow);
  rail.add(tools);

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
  // 排序：导航栏会越来越长，没有排序只能靠肉眼找。搜索不在这里——见上面
  // 放大镜的说明（左栏太窄，输入框会把标题挤没）。
  size_t nav_hits = 0;
  for (size_t i = 0; i < st_.projects.size(); ++i) {
    const ProjectCard& p = st_.projects[i];
    std::vector<ProjectCard::SessionRow> rows;
    for (size_t k = 0; k < p.sessions.size(); ++k) rows.push_back(p.sessions[k]);
    if (rows.empty()) continue;
    // sort: 最近 (default, already sorted by backend) / 名称 / 状态
    if (st_.session_sort == "title") {
      std::stable_sort(rows.begin(), rows.end(),
                       [](const ProjectCard::SessionRow& a,
                          const ProjectCard::SessionRow& b) { return a.title < b.title; });
    } else if (st_.session_sort == "status") {
      auto rank = [](const ProjectCard::SessionRow& r) {
        if (r.loop_state == "waiting") return 0;
        // An auto round is "running" for the user even though no user message
        // started it, so it sorts with the busy sessions.
        if (r.running || r.auto_round > 0) return 1;
        if (!r.last_error.empty()) return 2;
        return 3;
      };
      std::stable_sort(rows.begin(), rows.end(),
                       [&rank](const ProjectCard::SessionRow& a,
                               const ProjectCard::SessionRow& b) {
        return rank(a) < rank(b);
      });
    }
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
    for (size_t k = 0; k < rows.size(); ++k) {
      const ProjectCard::SessionRow& s = rows[k];
      ++nav_hits;
      std::string sid = s.sid;
      std::string title = s.title;
      H row = el("div", s.sid == st_.active_tab ? "nav-session active" : "nav-session");
      row.on(EventKind::kClick, [this, sid, title](const std::string&) {
        open_session(sid, title);
      });
      // Right-click a session in the rail: same menu as the project card row.
      row.on(EventKind::kContextMenu, [this, sid](const std::string& v) {
        size_t t = v.find('\t');
        int x = std::atoi(v.substr(0, t).c_str());
        int y = t == std::string::npos ? 0 : std::atoi(v.substr(t + 1).c_str());
        update([&](AppState& s2) {
          s2.ctx_open = true;
          s2.ctx_x = x;
          s2.ctx_y = y;
          s2.ctx_kind = "session";
          s2.ctx_id = sid;
        });
      });
      // LIVE state for the active session + "news since you looked" for the rest
      // (see dot_for).
      H nd = dot_for(s);
      nd.cls("nav-dot");  // same colour rules, rail-sized
      row.add(nd);
      row.add(el("span", "nav-session-title").text(s.title));
      grp.add(row);
    }
    body.add(grp);
  }
  if (nav_hits == 0 && !st_.projects.empty()) {
    body.add(el("div", "nav-empty").text(tr("没有匹配的会话")));
  }
  rail.add(body);

  H foot = el("div", "nav-foot");
  foot.add(comp::button(tr("打开文件夹"), [this](const std::string&) {
    pick_and_open_folder();
  }, "ghost"));
  // The home footer (hidden in this mode) carried 新建项目; keep it reachable
  // from the rail so the action is not lost with the bar.
  foot.add(comp::icon_button("folder-plus", tr("新建项目"), [this](const std::string&) {
    call_async("project.pickFolder", Json::object(), [this](const BackendResult& r) {
      if (!r.ok) return;
      std::string path = jsonutil::get<std::string>(r.data, "path", "");
      if (path.empty()) return;
      call_async("project.create", Json{{"path", path}}, [this](const BackendResult&) {
        load_projects();
      });
    });
  }));
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
  // centered title buttons: 探索 / 项目 / 用量统计 (+ a search icon at the right)
  std::vector<std::string> items;
  items.push_back(tr("探索"));
  items.push_back(tr("项目"));
  items.push_back(tr("用量统计"));
  H head = el("div", "home-head");
  head.add(comp::title_tabs(items, st_.home_tab > 2 ? -1 : st_.home_tab,
                            [this](const std::string& v) {
    int idx = std::atoi(v.c_str());
    update([&](AppState& s) { s.home_tab = idx; });
    if (idx == 2) {
      // 用量页要显示"按供应商"的范围列表：那来自 provider.listProviders，
      // 以前只有打开过设置或模型选择器才会加载，所以第一次进用量页左侧是空的。
      if (!st_.providers_loaded) load_providers();
      if (!st_.usage_loaded) load_usage();
    }
  }));
  // 搜索入口放在标题栏最右端（不是紧跟页签）：它是"整页"的动作，右对齐才
  // 不会随着页签文字长度左右浮动。
  H sbtn = comp::icon_button("magnifying-glass", tr("搜索"), [this](const std::string&) {
    update([&](AppState& s) { s.home_tab = 3; });
  });
  sbtn.cls(st_.home_tab == 3 ? "home-search-btn active" : "home-search-btn");
  sbtn.key("home-search-btn");
  head.add(sbtn);
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
      // Sub-page swap: a fade, NOT the row entry animation. The row animation
      // lifts content by a few pixels, which on a whole page reads as "the page
      // jumped up from the bottom" (that is exactly how switching back from 用量
      // to 项目 looked).
      add_anim(p, anim::kFade);
      p->anim_key = "home:" + itos(st_.home_tab);
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
      add_anim(p, anim::kFade);
      p->anim_key = "home:" + itos(st_.home_tab);
      body.add(p);
      break;
    }
    default: {
      if (st_.home_tab == 3) {
        VNodePtr p = build_home_search();
        p->key = "home:search";
        add_anim(p, anim::kFade);
        p->anim_key = "home:" + itos(st_.home_tab);
        body.add(p);
        break;
      }
      VNodePtr p = build_home_projects();
      p->key = "home:projects";
      add_anim(p, anim::kFade);
      p->anim_key = "home:" + itos(st_.home_tab);
      body.add(p);
      break;
    }
  }
  wrap.add(body);

  // footer actions; both flows start from the native folder picker (the
  // browser cannot hand out OS paths, so the backend opens the dialog)
  //
  // In left-rail mode the rail already carries 打开文件夹 / 设置, so this bar
  // would be a second copy of the same controls pinned to the bottom of every
  // page. It is skipped there (the rail has 新建项目 instead).
  if (sv_str(st_.settings_values, "ui.tabs", "top") == "left") return wrap.get();
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
  // 搜索框：新建页同样是"找到某个会话"的入口，所以这里也能直接搜
  H nsearch = comp::search_box(st_.search_query, tr("搜索会话…"),
                               [this](const std::string& v) {
    update([&](AppState& s) { s.search_query = v; });
  });
  nsearch.key("new-search-input");
  acts.add(nsearch);
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
  if (st_.search_query.empty()) {
    VNodePtr cards = build_home_projects();
    cards->key = "new:projects";
    body.add(cards);
  } else {
    // A query narrows the page to matching sessions instead of the full cards:
    // that is the whole point of the box being here.
    VNodePtr results = build_home_search();
    results->key = "new:search";
    body.add(results);
  }
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
  // Section cards rise in sequence (same intent as 项目 cards). The delay is
  // per SECTION here, not per row: a capability list can be long, and a
  // 40ms-per-row cascade would take a second to finish.
  mcp_sec.style("animation-delay", "0ms");
  mcp_sec.style("animation-fill-mode", "backwards");
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
  sk_sec.style("animation-delay", "60ms");
  sk_sec.style("animation-fill-mode", "backwards");
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

VNodePtr UiApp::build_home_search() {
  H wrap = el("div", "search-page");
  H box = el("div", "search-box-wrap");
  H search = comp::search_box(st_.search_query, tr("搜索会话、项目或路径…"),
                              [this](const std::string& v) {
    update([&](AppState& s) { s.search_query = v; });
  });
  search.key("global-search-input");
  box.add(search);
  wrap.add(box);

  std::string q = st_.search_query;
  std::string lower = q;
  for (size_t i = 0; i < lower.size(); ++i) {
    lower[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(lower[i])));
  }
  auto match = [&lower](const std::string& hay) {
    if (lower.empty()) return true;
    std::string h = hay;
    for (size_t i = 0; i < h.size(); ++i) {
      h[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(h[i])));
    }
    return h.find(lower) != std::string::npos;
  };

  H list = el("div", "search-results");
  size_t hits = 0;
  for (size_t i = 0; i < st_.projects.size(); ++i) {
    const ProjectCard& p = st_.projects[i];
    bool proj_match = match(p.name) || match(p.path);
    for (size_t k = 0; k < p.sessions.size(); ++k) {
      const ProjectCard::SessionRow& s = p.sessions[k];
      // A project-name hit lists all of its sessions (searching a project is a
      // normal way to narrow down); otherwise the session title must match.
      if (!proj_match && !match(s.title)) continue;
      ++hits;
      std::string sid = s.sid;
      std::string title = s.title;
      H row = el("div", "search-row");
      // Results cascade in like project cards (capped: a broad query can match
      // dozens, and the last one should not appear a second later).
      int stagger = static_cast<int>(hits - 1) * 30;
      if (stagger > 240) stagger = 240;
      row.style("animation-delay", itos(stagger) + "ms");
      row.style("animation-fill-mode", "backwards");
      row.on(EventKind::kClick, [this, sid, title](const std::string&) {
        open_session(sid, title);
      });
      row.add(dot_for(s));
      H txt = el("div", "search-row-text");
      txt.add(el("div", "search-row-title").text(s.title));
      txt.add(el("div", "search-row-sub dim").text(p.name + "  ·  " + p.path));
      row.add(txt);
      row.add(el("span", "grow"));
      if (s.updated_ms > 0) {
        row.add(el("span", "search-row-time dim").text(rel_time(s.updated_ms)));
      }
      list.add(row);
    }
  }
  if (hits == 0) {
    wrap.add(comp::empty_state(
        q.empty() ? tr("输入关键词搜索所有项目里的会话。")
                  : tr("没有匹配的会话。"),
        "", nullptr));
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
    // Stagger the rise: each card starts a beat after the previous one, so a
    // list of them reads as a cascade instead of one block appearing. Capped so
    // a long list does not take a second to finish.
    card.style("animation-delay", itos(static_cast<int>(i) * 40) + "ms");
    card.style("animation-fill-mode", "backwards");

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
        rrow.add(dot_for(s));
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
      // Right-click a session row: open / rename / delete. The hover buttons
      // already do this, but they only appear on hover - right-click is the
      // conventional path, and with the browser menu suppressed it would
      // otherwise do nothing.
      row.on(EventKind::kContextMenu, [this, sid](const std::string& v) {
        size_t t = v.find('\t');
        int x = std::atoi(v.substr(0, t).c_str());
        int y = t == std::string::npos ? 0 : std::atoi(v.substr(t + 1).c_str());
        update([&](AppState& s2) {
          s2.ctx_open = true;
          s2.ctx_x = x;
          s2.ctx_y = y;
          s2.ctx_kind = "session";
          s2.ctx_id = sid;
        });
      });
      H dot = dot_for(s);
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
  // The page's own entrance comes from build_home (anim::kFade), the same intent
  // every other home sub-page uses. A second animation here overrode it and
  // made this page alone enter with a different motion. The per-section stagger
  // tokens below still drive the bars and rows.

  // ---- left: real scopes (总计 + one per provider) ----
  // The old list was a hardcoded array of three strings, one of which was
  // literally "自定义" - it could not select anything.
  H left = el("div", "usage-scopes");
  H tot = el("div", st_.usage_scope.empty() ? "scope sel" : "scope");
  tot.on(EventKind::kClick, [this](const std::string&) {
    update([&](AppState& s) { s.usage_scope.clear(); });
    load_usage();
    load_balances(false);  // a different scope = a different provider
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
      load_balances(false);  // a different scope = a different provider
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
  // Export: the numbers on this page are a view of the usage log, and the log is
  // what people take into a spreadsheet. The filter is the same one the page is
  // showing (scope + range), so the file matches what is on screen.
  {
    H acts = el("div", "usage-actions");
    acts.add(comp::icon_text_button("file-export", tr("导出 CSV"), [this](const std::string&) {
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
      call_async("usage.exportCsv", Json{{"filter", filter}},
                 [this](const BackendResult& r) {
        if (!r.ok) {
          toast(tr("导出失败：") + r.message);
          return;
        }
        std::string path = jsonutil::get<std::string>(r.data, "path", "");
        toast(path.empty() ? tr("已导出") : (tr("已导出到 ") + path));
      });
    }, "ghost"));
    panel.add(acts);
  }

  // ---- totals ----
  H head = el("div", "usage-head");
  std::string calls = "0";
  std::string cost = "—";
  std::string in_tok = "0";
  std::string out_tok = "0";
  std::string all_tok = "0";
  if (st_.usage_loaded) {
    calls = std::to_string(jsonutil::get_int(st_.usage, "calls", 0));
    int64_t tin = jsonutil::get_int(st_.usage, "totalIn", 0);
    int64_t tout = jsonutil::get_int(st_.usage, "totalOut", 0);
    in_tok = fmt_tokens(tin);
    out_tok = fmt_tokens(tout);
    all_tok = fmt_tokens(tin + tout);
    std::string kind = jsonutil::get<std::string>(st_.usage, "costKind", "");
    double c = jsonutil::get_number(st_.usage, "totalCost", 0.0);
    char buf[64];
    if (kind == "money") {
      std::snprintf(buf, sizeof(buf), "¥%.4f", c);
      cost = buf;
    } else if (kind == "quota") {
      cost = tr("套餐内");
    } else {
      cost = tr("未知");
    }
  }
  head.add(comp::key_value(tr("调用次数"), calls));
  head.add(comp::key_value(tr("总 tokens"), all_tok));
  head.add(comp::key_value(tr("输入 tokens"), in_tok));
  head.add(comp::key_value(tr("输出 tokens"), out_tok));
  head.add(comp::key_value(tr("总成本"), cost));
  panel.add(head);


  // ---- by model: a DONUT (the user asked for 饼/环状图, not bars) ----
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
    // Keep the ring readable: the top 7 models + 其他. A 40-slice ring is noise.
    static const char* kDonutColors[] = {"#4f8cff", "#58c98b", "#e0a44a",
                                         "#b47cff", "#e05d7b", "#3fb6c9",
                                         "#9aa4b2", "#c9b46a"};
    const int kMaxSlices = 8;
    std::vector<std::pair<std::string, int64_t> > top;
    int64_t other = 0;
    for (size_t i = 0; i < rows.size(); ++i) {
      if (static_cast<int>(i) < kMaxSlices - 1) {
        top.push_back(rows[i]);
      } else {
        other += rows[i].second;
      }
    }
    if (other > 0) top.push_back(std::make_pair(tr("其他"), other));

    H wrap = el("div", "donut-wrap");
    H pie = el("div", "donut-pie");
    H ring = el("div", "donut-ring");
    std::string grad = "conic-gradient(";
    int64_t acc = 0;
    for (size_t i = 0; i < top.size(); ++i) {
      double from = total_tok > 0 ? acc * 100.0 / total_tok : 0.0;
      acc += top[i].second;
      double to = total_tok > 0 ? acc * 100.0 / total_tok : 0.0;
      char seg[64];
      std::snprintf(seg, sizeof(seg), "%s %.2f%% %.2f%%%s",
                    kDonutColors[i % 8], from, to,
                    i + 1 == top.size() ? "" : ", ");
      grad += seg;
    }
    grad += ")";
    ring.style("background", grad);
    ring.attr("title", tr("按模型的总 token 占比"));
    pie.add(ring);
    H hole = el("div", "donut-hole");
    hole.add(el("div", "donut-total").text(fmt_tokens(total_tok)));
    hole.add(el("div", "donut-label dim").text(tr("总 tokens")));
    pie.add(hole);
    wrap.add(pie);

    H legend = el("div", "donut-legend");
    for (size_t i = 0; i < top.size(); ++i) {
      int pct = total_tok > 0
                    ? static_cast<int>(top[i].second * 100 / total_tok)
                    : 0;
      H lr = el("div", "donut-item");
      lr.style("animation-delay", itos(30 + static_cast<int>(i) * 25) + "ms");
      H sw = el("span", "donut-swatch");
      sw.style("background", kDonutColors[i % 8]);
      lr.add(sw);
      lr.add(el("span", "donut-name").attr("title", top[i].first).text(top[i].first));
      lr.add(el("span", "donut-val dim").text(itos(pct) + "%"));
      legend.add(lr);
    }
    wrap.add(legend);
    bymodel.add(wrap);
  }
  panel.add(bymodel);

  // ---- table: real byProviderModel rows, else an explicit empty state ----
  // Total tokens (in + out) is what the user watches; it replaces the cache
  // column, which was a per-row detail nobody read in this table.
  H tbl = el("table", "stat-table");
  H thead = el("tr");
  const char* hd[5] = {"模型", "输入", "输出", "总 tokens", "成本"};
  for (int i = 0; i < 5; ++i) thead.add(el("th").text(tr(hd[i])));
  tbl.add(thead);
  bool any = false;
  for (Json::const_iterator it = pm.begin(); it != pm.end(); ++it) {
    const Json& e = it.value();
    H tr = el("tr");
    std::string model = jsonutil::get<std::string>(e, "model", "");
    std::string prov = jsonutil::get<std::string>(e, "provider", "");
    int64_t tin = jsonutil::get_int(e, "in", 0);
    int64_t tout = jsonutil::get_int(e, "out", 0);
    tr.add(el("td").text(prov.empty() ? model : (prov + " / " + model)));
    tr.add(el("td").text(fmt_tokens(tin)));
    tr.add(el("td").text(fmt_tokens(tout)));
    tr.add(el("td").text(fmt_tokens(tin + tout)));
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
  // ---- 余额 / 额度（放在最后：它是"还剩多少"，先看用量再看余额）----
  // Per provider, by its ENDPOINT (the only thing that identifies a vendor
  // reliably). Three states, all honest: the number, 不支持 (no public balance
  // API), or 获取失败 WITH the reason. Asked on entering this page, after every
  // finished turn and when the provider filter changes.
  load_balances(false);
  {
    H bal = el("div", "usage-balances");
    H bh = el("div", "chart-head");
    bh.add(el("div", "chart-title").text(tr("余额 / 额度")));
    bh.add(el("span", "grow"));
    if (st_.balances_loading) {
      bh.add(el("span", "bal-loading dim").text(tr("查询中…")));
    } else {
      bh.add(comp::button(tr("刷新"), [this](const std::string&) {
        load_balances(true);
      }, "ghost"));
    }
    bal.add(bh);
    auto money = [](const std::string& cur, const std::string& v) {
      if (v.empty()) return std::string("—");
      if (cur == "CNY") return "¥" + v;
      if (cur == "USD") return "$" + v;
      return cur.empty() ? v : (cur + " " + v);
    };
    if (st_.balances.empty()) {
      bal.add(el("div", "hint").text(
          st_.balances_loading ? tr("正在查询各供应商的余额…")
                               : tr("还没有供应商。在设置-供应商里添加一家之后这里会显示余额。")));
    }
    for (size_t i = 0; i < st_.balances.size(); ++i) {
      const AppState::BalanceRow& b = st_.balances[i];
      H row = el("div", "bal-row");
      row.style("animation-delay", itos(static_cast<int>(i) * 30) + "ms");
      H nm = el("div", "bal-name");
      nm.add(el("span", "bal-pname").text(b.name.empty() ? b.pid : b.name));
      if (!b.enabled) nm.add(el("span", "bal-off dim").text(tr("已停用")));
      row.add(nm);
      H val = el("div", "bal-val");
      std::string cls = "bal-amount";
      std::string text;
      if (b.status == "ok") {
        text = money(b.currency, b.balance);
        if (!b.used.empty() || !b.limit.empty()) {
          text += "  ";
          if (!b.used.empty()) text += tr("已用 ") + money(b.currency, b.used);
          if (!b.limit.empty()) {
            text += tr(" / 额度 ") + money(b.currency, b.limit);
          }
        }
      } else if (b.status == "unsupported") {
        cls += " dim";
        text = tr("不支持");
        if (!b.note.empty()) text += " · " + tr(b.note.c_str());
      } else {
        cls += " bad";
        // The backend already wrote "获取失败：HTTP 401"; show it verbatim - a
        // silent dash would hide that the key or the endpoint is wrong.
        text = b.note.empty() ? tr("获取失败") : tr(b.note.c_str());
      }
      val.add(el("span", cls).text(text));
      row.add(val);
      if (!b.endpoint.empty()) {
        row.add(el("div", "bal-ep dim").text(b.endpoint));
      }
      bal.add(row);
    }
    panel.add(bal);
  }
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
  hist.key("chat-history");  // scroll position is saved/restored by this key
  H hist_inner = el("div", "history-inner");
  if (st_.messages.empty()) {
    hist_inner.add(comp::empty_state(tr("开始新的对话。输入消息后 Enter 发送。"), "", nullptr));
  }
  // Messages are rendered through a cache keyed by (sid, index). A session can
  // hold hundreds of turns, and this loop runs on EVERY render - several times a
  // second while a reply streams. Re-parsing every markdown block and rebuilding
  // every node each time was the single largest cost in the app, and it also
  // made the diff walk thousands of identical nodes.
  //
  // A message whose revision and signature are unchanged hands back the SAME
  // VNode object, so the framework skips the subtree entirely (see patch_node).
  // Only the message that actually changed is rebuilt - in the common case the
  // streaming one. A solidified turn never changes again, so its cost drops to
  // one map lookup.
  const std::string msg_sid = st_.active_tab == "home" ? std::string() : st_.active_tab;
  std::set<std::string> live_keys;
  // The LAST user message can be edited and re-sent (a do-over of what you asked).
  // Only the last one: editing an older question would orphan every answer after
  // it, which is what 回滚 is for.
  int last_user_index = -1;
  for (size_t i = 0; i < st_.messages.size(); ++i) {
    if (st_.messages[i].role == "user") last_user_index = static_cast<int>(i);
  }
  for (size_t i = 0; i < st_.messages.size(); ++i) {
    const comp::ChatMsg& m = st_.messages[i];
    // Identity: the message's own uid when it has one (loaded turns), else a
    // position-derived fallback for live messages appended this session. Live
    // messages are always at the END, so their position is stable.
    int64_t uid = m.uid != 0 ? m.uid : (1000000000LL + static_cast<int64_t>(i));
    std::string key = comp::chat_message_key(msg_sid, uid);
    live_keys.insert(key);
    const bool can_edit = static_cast<int>(i) == last_user_index;
    // The flag is part of the signature: when a newer message arrives, the one
    // that WAS last must lose its 编辑 action, and a cached node would not.
    uint64_t sig = message_sig(m) ^ (can_edit ? 0x51ed2701ULL : 0ULL);
    std::map<std::string, MsgNode>::iterator hit = msg_nodes_.find(key);
    if (hit != msg_nodes_.end() && hit->second.rev == m.rev && hit->second.sig == sig) {
      hist_inner.add(hit->second.node);
      continue;
    }
    // Handlers address the message by UID, never by the loop index. A cached
    // node keeps the closure it was built with, and prepending an older page
    // shifts every index - so an index-captured handler would toggle (or copy,
    // or fork) whatever message now happens to sit at that position. That is
    // exactly why folding stopped working on older turns after scrolling up.
    H node = comp::chat_message(m, [this, uid](const std::string&) {
      int i = find_message_by_uid(uid);
      if (i >= 0) dom_.copy_text(st_.messages[i].text);
      toast(tr("已复制"));
    },
                               [this, uid](const std::string& v) {
      // The value carries "<blockIndex>\t<action>": a message can have many
      // foldable blocks, and clicking one must not toggle the first.
      size_t tab = v.find('\t');
      int block = 0;
      if (tab != std::string::npos) {
        block = std::atoi(v.substr(0, tab).c_str());
      }
      int i = find_message_by_uid(uid);
      if (i < 0) return;
      update([&](AppState& s) {
        if (i < static_cast<int>(s.messages.size()) && block >= 0 &&
            block < static_cast<int>(s.messages[i].tools.size())) {
          bool open = !s.messages[i].tools[block].expanded;
          s.messages[i].tools[block].expanded = open;
          // The memoised row is keyed on its OWN rev as well as `expanded`: a
          // collapsed row's cached node must never be reused after the expanded
          // body was removed from the DOM, or reopening has nothing to re-add
          // ("展开再折叠后点不开"). Bump rev on every toggle.
          s.messages[i].tools[block].rev = next_msg_rev_++;
          s.messages[i].rev = next_msg_rev_++;  // only this message is rebuilt
          // Opening a tool's JSON is almost always to read its END (a result, an
          // error). Scroll the body to the bottom on the next frame, once the
          // node exists: doing it now would scroll the previous layout.
          if (open) {
            std::string sel = "tb-body:" + std::to_string(uid) + ":" +
                              std::to_string(block);
            dom_.schedule(0, [this, sel]() {
              VNodePtr n = find_by_key(tree_, sel);
              if (n && n->dom.valid()) dom_.scroll_to_bottom(n->dom);
            });
          }        }
      });
    },
                               [this, uid](const std::string& v) {
      // "work" toggles the whole group (reasoning + tools) at once; a bare
      // "\t<action>" would mean the thinking block alone.
      int i = find_message_by_uid(uid);
      if (i < 0) return;
      if (v == "work") {
        update([&](AppState& s) {
          if (i < static_cast<int>(s.messages.size())) {
            comp::ChatMsg& m = s.messages[i];
            // Toggle what is on SCREEN, not the raw flag: while a turn runs the
            // fold is open even when `work_open` is false (streaming implies
            // open), so toggling the flag alone made the click look dead.
            bool shown = m.work_open || (m.streaming && !m.work_touched);
            m.work_open = !shown;
            m.work_touched = true;
            // New fold generation: rows cached for the old state must not be
            // reused (see ChatMsg::work_seq).
            m.work_seq = next_msg_rev_++;
            m.rev = next_msg_rev_++;
          }
        });
        return;
      }
      update([&](AppState& s) {
        if (i < static_cast<int>(s.messages.size())) {
          s.messages[i].thinking_open = !s.messages[i].thinking_open;
          s.messages[i].rev = next_msg_rev_++;
        }
      });
    },
                               [this, uid](const std::string& v) {
      int i = find_message_by_uid(uid);
      if (i >= 0) turn_action(static_cast<size_t>(i), v);
    },
                               [this, uid](const std::string& v) {
      // "<x>\t<y>" from the DOM adapter. The menu content depends on WHAT was
      // clicked, so the message body and its turn id are captured here - the
      // menu itself only knows the state fields.
      int i = find_message_by_uid(uid);
      if (i < 0) return;
      int x = 0, y = 0;
      size_t tab = v.find('\t');
      if (tab != std::string::npos) {
        x = std::atoi(v.substr(0, tab).c_str());
        y = std::atoi(v.substr(tab + 1).c_str());
      }
      comp::ChatMsg m = st_.messages[static_cast<size_t>(i)];
      update([&](AppState& s) {
        s.ctx_open = true;
        s.ctx_x = x;
        s.ctx_y = y;
        s.ctx_kind = "message";
        s.ctx_id = m.turn_id;
        s.ctx_label = m.text;
      });
    },
                               [this](const std::string& href) { open_href(href); },
                               // A subagent tool row's "打开记录": open the
                               // transcript as its own tab.
                               [this](const std::string& subagent_id) {
      open_subagent_tab(subagent_id, tr("子代理 ") + subagent_id);
    },
                               // 编辑并重发 applies to the last user message only.
                               can_edit && m.role == "user");
    node.key(key);
    MsgNode mn;
    mn.node = node.get();
    mn.rev = m.rev;
    mn.sig = sig;
    msg_nodes_[key] = mn;
    hist_inner.add(node);
  }
  // Drop the cached nodes this session no longer uses (a rolled-back or
  // reloaded history). Other sessions keep theirs, so switching back to a tab
  // is also free - but the map is capped so a long-lived window cannot grow it
  // without bound.
  {
    const std::string prefix = "msg:" + msg_sid + ":";
    for (std::map<std::string, MsgNode>::iterator it = msg_nodes_.begin();
         it != msg_nodes_.end();) {
      if (it->first.compare(0, prefix.size(), prefix) == 0 &&
          live_keys.find(it->first) == live_keys.end()) {
        it = msg_nodes_.erase(it);
      } else {
        ++it;
      }
    }
    const size_t kMaxCachedMessages = 4000;
    while (msg_nodes_.size() > kMaxCachedMessages) msg_nodes_.erase(msg_nodes_.begin());
  }
  hist.add(hist_inner);
  center.add(hist);

  // There is NO plan float any more. The plan is not a progress widget you stare
  // at: it is a DOCUMENT (.flashsight/plans/<sid>.md) that the model writes and
  // keeps in step, and the per-round work is the task list (todo_float below).
  // Showing a second step tree beside the checklist duplicated the same progress
  // and was never actionable - the user's verdict was "删掉这个窗口".

  // The model's own checklist (todo_add / todo_check), floating like the plan
  // but on the RIGHT so the two never cover each other. It exists because the
  // backend could already add items while the UI dropped every update into a
  // toast - "todo 可以设置但悬浮窗口不显示".
  if (st_.todo_open) {
    H todo = el("div", "todo-float");
    todo.key("todo-float");
    todo.anim(anim::kPop);
    H thead = el("div", "plan-head");
    thead.add(el("span", "plan-title").text(tr("任务清单")));
    thead.add(el("span", "grow"));
    int tdone = 0;
    for (size_t i = 0; i < st_.todos.size(); ++i) {
      if (st_.todos[i].done) ++tdone;
    }
    thead.add(el("span", "todo-progress dim").text(
        itos(tdone) + "/" + itos(static_cast<int>(st_.todos.size()))));
    thead.add(comp::icon_button("\u2715", tr("收起"), [this](const std::string&) {
      update([&](AppState& s) { s.todo_open = false; });
    }));
    todo.add(thead);
    if (st_.todos.empty()) {
      todo.add(el("div", "plan-empty dim").text(tr("暂无任务（模型会在这里记录本轮步骤）")));
    }
    for (size_t i = 0; i < st_.todos.size(); ++i) {
      const AppState::TodoItem& it = st_.todos[i];
      H row = el("div", it.done ? "plan-step st-done" : "plan-step");
      row.add(el("span", "plan-status").text(it.done ? "\u2713" : "\u25cb"));
      row.add(el("span", "plan-text").text(it.task));
      todo.add(row);
    }
    center.add(todo);
  }
  // ---- composer (embedded in the center column, under the history) ----
  H comp_box = el("div", "composer");
  // Right-click anywhere in the composer (including the textarea) opens the
  // app's own edit menu. The browser's menu is suppressed globally, so without
  // this a right-click in the input did nothing at all.
  comp_box.on(EventKind::kContextMenu, [this](const std::string& v) {
    size_t t = v.find('\t');
    int x = std::atoi(v.substr(0, t).c_str());
    int y = t == std::string::npos ? 0 : std::atoi(v.substr(t + 1).c_str());
    update([&](AppState& s) {
      s.ctx_open = true;
      s.ctx_x = x;
      s.ctx_y = y;
      s.ctx_kind = "composer";
      s.ctx_id.clear();
      s.ctx_label.clear();
    });
  });
  // 编辑并重发 armed: the next send rewrites the transcript, so say it and offer
  // a way out. Without the bar the message would just vanish on send.
  if (!st_.edit_turn_id.empty()) {
    H bar = el("div", "interrupt-bar edit-bar");
    bar.add(el("i", "fa fa-pen"));
    bar.add(el("span", "ib-text").text(tr("正在编辑上一条消息")));
    bar.add(el("span", "ib-reason dim").text(tr("发送后将替换它和之后的回答")));
    bar.add(el("span", "grow"));
    bar.add(comp::button(tr("取消编辑"), [this](const std::string&) {
      update([&](AppState& s) { s.edit_turn_id.clear(); });
    }, "ghost"));
    comp_box.add(bar);
  }
  // Scheduled wake-ups: the same bar slot as the interrupt notice, because both
  // answer the same question - "is this task parked, and what will happen next?"
  // A timer the user set is a promise the UI has to show; a countdown that only
  // exists in the backend is indistinguishable from nothing being scheduled.
  if (!st_.pending_wakes.empty()) {
    for (size_t i = 0; i < st_.pending_wakes.size(); ++i) {
      const PendingWake& w = st_.pending_wakes[i];
      H bar = el("div", "interrupt-bar wake-bar");
      bar.add(el("i", "fa fa-clock"));
      std::string label = w.source == "timer" ? tr("定时唤醒") : tr("待唤醒");
      bar.add(el("span", "ib-text").text(label));
      std::string when;
      if (w.in_ms > 0) {
        long long secs = (w.in_ms + 999) / 1000;
        when = secs >= 60 ? (std::to_string(secs / 60) + tr(" 分钟后"))
                          : (std::to_string(secs) + tr(" 秒后"));
      }
      if (!when.empty()) bar.add(el("span", "ib-reason dim").text(when));
      if (!w.note.empty()) {
        bar.add(el("span", "ib-reason dim").text("· " + w.note));
      }
      bar.add(el("span", "grow"));
      size_t idx = i;
      bar.add(comp::button(tr("取消"), [this, idx](const std::string&) {
        std::string sid = active_sid();
        if (sid.empty()) return;
        call_async("session.cancelWake", Json{{"sid", sid}},
                   [this, idx](const BackendResult& r) {
          if (!r.ok) {
            toast(tr("取消失败：") + r.message);
            return;
          }
          update([&](AppState& s) {
            if (idx < s.pending_wakes.size()) {
              s.pending_wakes.erase(s.pending_wakes.begin() + idx);
            }
          });
        });
      }, "ghost"));
      comp_box.add(bar);
    }
  }
  // Interrupted task: a persistent bar with the action that actually helps.
  // The loop can continue from its saved point, so this is one click instead of
  // retyping the request.
  if (st_.interrupted) {
    H bar = el("div", "interrupt-bar");
    bar.add(el("i", "fa fa-circle-exclamation"));
    bar.add(el("span", "ib-text").text(tr("任务已中断")));
    if (!st_.interrupt_reason.empty()) {
      bar.add(el("span", "ib-reason dim").text(st_.interrupt_reason));
    }
    bar.add(el("span", "grow"));
    bar.add(comp::button(tr("继续"), [this, reason = st_.interrupt_reason](const std::string&) {
      std::string sid = active_sid();
      if (sid.empty()) return;
      // Optimistic: hide the bar NOW. The request runs the continuation turn and
      // only answers when it ENDS, so waiting for the callback kept the bar on
      // screen for the whole turn - clicking again started yet another continue
      // ("点很多次才好"). A failure puts it back with the original reason.
      update([&](AppState& s) {
        s.interrupted = false;
        s.interrupt_reason.clear();
      });
      call_async("session.continue", Json{{"sid", sid}, {"guidance", ""}},
                 [this, reason](const BackendResult& r) {
        if (!r.ok) {
          update([&](AppState& s) {
            s.interrupted = true;
            s.interrupt_reason = reason;
          });
          toast(tr("继续失败：") + r.message);
        }
      });
    }, "primary"));
    bar.add(comp::button(tr("忽略"), [this](const std::string&) {
      update([&](AppState& s) {
        s.interrupted = false;
        s.interrupt_reason.clear();
      });
    }, "ghost"));
    comp_box.add(bar);
  }
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

  // ---- ask: the model is WAITING for an answer ----
  //
  // This takes over the composer area instead of floating over the transcript.
  // Two reasons: (1) a dialog covered the conversation the question is about,
  // (2) the answer is composed here (pick an option, or type one), so it belongs
  // where input normally happens. The user must click 发送 explicitly - an
  // option click only SELECTS it, so a misclick cannot answer the model.
  if (st_.ask_modal && !st_.asks.empty()) {
    if (st_.ask_index < 0) st_.ask_index = 0;
    if (st_.ask_index >= static_cast<int>(st_.asks.size())) {
      st_.ask_index = static_cast<int>(st_.asks.size()) - 1;
    }
    const AppState::AskItem& cur = st_.asks[static_cast<size_t>(st_.ask_index)];
    H panel = el("div", "ask-panel");
    H head = el("div", "ask-head");
    head.add(el("i", "fa fa-circle-question"));
    head.add(el("span", "ask-title").text(tr("模型提问")));
    if (st_.asks.size() > 1) {
      // "1/3" so a multi-question prompt is obviously a sequence, not one long
      // question with a strange title.
      head.add(el("span", "ask-counter").text(
          itos(st_.ask_index + 1) + " / " + itos(static_cast<int>(st_.asks.size()))));
    }
    head.add(el("span", "grow"));
    head.add(comp::icon_button("\u2715", tr("忽略这个问题"), [this](const std::string&) {
      // Dismissing must still ANSWER it: leaving the turn parked forever is
      // worse than an explicit "no answer".
      std::string sid = active_sid();
      int ask_id = st_.asks.empty() ? 0 : st_.asks[0].ask_id;
      update([&](AppState& s) {
        s.ask_modal = false;
        s.asks.clear();
        s.ask_index = 0;
      });
      if (sid.empty() || ask_id <= 0) return;
      call_async("tool.askAnswer",
                 Json{{"sid", sid}, {"askId", ask_id}, {"selection", "(dismissed)"}});
    }));
    panel.add(head);
    panel.add(el("div", "ask-question").text(cur.question));

    // Options are ROWS with a radio mark, not a paragraph of text: the whole
    // complaint was "分辨不出哪些是选项".
    //
    // The custom answer is a PEER OPTION, not a separate field below the list:
    // "pick one of these, or write your own" is one choice, and putting the text
    // box outside the list made it read as a comment box for the question.
    H opts = el("div", "ask-options");
    auto is_custom = [&cur](const std::string& v) {
      if (v.empty()) return false;
      for (auto& o : cur.options) {
        if (o == v) return false;
      }
      return true;
    };
    bool custom_picked = is_custom(cur.answer);
    for (size_t k = 0; k < cur.options.size(); ++k) {
      bool selected = !custom_picked && cur.answer == cur.options[k];
      bool rec = static_cast<int>(k) == cur.recommended;
      H row = el("div", selected ? "ask-option selected" : "ask-option");
      row.add(el("span", "ask-radio").text(selected ? "\u25cf" : "\u25cb"));
      row.add(el("span", "ask-option-text").text(cur.options[k]));
      if (rec) row.add(el("span", "ask-option-tag").text(tr("推荐")));
      size_t idx = k;
      row.on(EventKind::kClick, [this, idx](const std::string&) {
        update([&](AppState& s) {
          if (s.ask_index < static_cast<int>(s.asks.size())) {
            AppState::AskItem& it = s.asks[static_cast<size_t>(s.ask_index)];
            if (idx < it.options.size()) {
              it.answer = it.options[idx];
              it.custom_draft.clear();
            }
          }
        });
      });
      opts.add(row);
    }
    if (cur.options.empty() && !cur.allow_custom) {
      opts.add(el("div", "ask-option-empty").text(tr("（没有可选答案）")));
    }
    if (cur.allow_custom) {
      // The "write your own" row: same shape as the others, and picking it
      // reveals the text field right under it (inside the list).
      H row = el("div", custom_picked ? "ask-option selected custom" : "ask-option custom");
      row.add(el("span", "ask-radio").text(custom_picked ? "\u25cf" : "\u25cb"));
      row.add(el("span", "ask-option-text").text(tr("自定义回答")));
      row.on(EventKind::kClick, [this](const std::string&) {
        update([&](AppState& s) {
          if (s.ask_index >= static_cast<int>(s.asks.size())) return;
          AppState::AskItem& it = s.asks[static_cast<size_t>(s.ask_index)];
          // Selecting the row is what "custom" means; with no text yet the
          // answer stays empty, so 发送 stays disabled until something is typed.
          it.answer = it.custom_draft;
        });
      });
      opts.add(row);
      if (custom_picked || (!cur.custom_draft.empty())) {
        H wrap = el("div", "ask-custom-wrap");
        wrap.add(comp::text_input(
            cur.custom_draft, tr("输入你的回答…"),
            [this](const std::string& v) {
          update([&](AppState& s) {
            if (s.ask_index < static_cast<int>(s.asks.size())) {
              AppState::AskItem& it = s.asks[static_cast<size_t>(s.ask_index)];
              it.custom_draft = v;
              it.answer = v;  // typing IS the answer; an empty box un-answers it
            }
          });
        }));
        opts.add(wrap);
      }
    }
    panel.add(opts);

    H acts = el("div", "ask-actions");
    acts.add(el("span", "grow"));
    if (st_.asks.size() > 1 && st_.ask_index + 1 < static_cast<int>(st_.asks.size())) {
      acts.add(comp::button(tr("下一个"), [this](const std::string&) {
        update([&](AppState& s) { s.ask_index++; });
      }, "ghost"));
    }
    bool ready = !cur.answer.empty();
    H send = comp::button(tr("发送回答"), [this](const std::string&) {
      std::string sid = active_sid();
      if (sid.empty()) return;
      // Send EVERY answered question; unanswered ones stay parked in the panel.
      // The backend resumes only when the whole group has answers.
      std::vector<std::pair<int, std::string>> to_send;
      int next_unanswered = -1;
      for (size_t k = 0; k < st_.asks.size(); ++k) {
        if (st_.asks[k].answer.empty()) {
          if (next_unanswered < 0) next_unanswered = static_cast<int>(k);
          continue;
        }
        to_send.push_back(std::make_pair(st_.asks[k].ask_id, st_.asks[k].answer));
      }
      for (size_t k = 0; k < to_send.size(); ++k) {
        call_async("tool.askAnswer",
                   Json{{"sid", sid}, {"askId", to_send[k].first},
                        {"selection", to_send[k].second}});
      }
      update([&](AppState& s) {
        if (next_unanswered >= 0) {
          s.ask_index = next_unanswered;  // move to the next open question
        } else {
          s.ask_modal = false;
          s.asks.clear();
          s.ask_index = 0;
        }
      });
    }, "primary");
    if (!ready) send.cls("disabled");
    acts.add(send);
    panel.add(acts);
    comp_box.add(panel);
  } else {
  // The placeholder has to match the CURRENT Enter setting: it used to say
  // "Enter sends" even with Ctrl+Enter selected, which is exactly the kind of
  // hard-coded text that makes a working setting look broken.
  //
  // The Enter behaviour also honours the 快捷键 page: if the user rebinds
  // "发送消息" to something containing Ctrl, plain Enter becomes a newline. That
  // page used to be four fields nothing read, which is why the user called the
  // Enter shortcuts 摆设.
  std::string enter_mode = st_.enter_send;
  {
    // A binding that includes a modifier means "plain Enter must NOT send".
    // Left at the default ("Enter") the session setting decides - rebinding
    // "发送消息" back to plain Enter must not silently override a session that
    // was explicitly configured for Ctrl+Enter.
    std::string send_key = normalize_shortcut(
        sv_str(st_.settings_values, "shortcuts.发送消息", "Enter"));
    if (send_key.find("ctrl+") != std::string::npos ||
        send_key.find("meta+") != std::string::npos) {
      enter_mode = "ctrlEnter";
    }
  }
  const char* hint_key = enter_mode == "ctrlEnter"
                             ? "输入消息…（Ctrl+Enter 发送，Enter 换行）"
                             : "输入消息…（Enter 发送，Shift+Enter 换行）";
  H ta = comp::textarea(st_.session_input, tr(hint_key),
                        [this](const std::string& v) {
    st_.session_input = v;  // no re-render while typing (avoid caret jumps)
    // Inline completion trigger. NO re-render here (that would move the caret),
    // so the menu is (re)built only when the TRIGGER STATE actually changes.
    update_complete_from_input(v);
  },
                        [this](const std::string&) { send_message(); });
  ta.key("session-input");  // send_message() clears it directly after sending
  // The adapter reads this to decide whether plain Enter sends (see the
  // keyenter handler): the setting has to reach the DOM or it does nothing.
  ta.attr("data-enter-send", enter_mode);
  // While a completion menu is open, the adapter must preventDefault on Tab and
  // the arrow keys. Otherwise the browser moves focus / scrolls BEFORE the key
  // reaches the app, and the highlighted suggestion can never be accepted.
  if (st_.complete_kind != AppState::CompleteKind::kNone) {
    ta.attr("data-keys-capture", "1");
  }
  // Arrow keys / Tab drive the completion menu. These are node-scoped (the
  // textarea has focus), not document-wide: ↑/↓ must move the selection only
  // while a menu is open, and Tab must insert rather than move focus.
  ta.on(EventKind::kKeyDown, [this](const std::string& key) {
    if (st_.complete_kind == AppState::CompleteKind::kNone) return;
    if (key == "ArrowDown" || key == "ArrowUp") {
      int n = static_cast<int>(complete_row_count());
      if (n <= 0) return;
      int idx = st_.complete_index + (key == "ArrowDown" ? 1 : -1);
      if (idx < 0) idx = n - 1;
      if (idx >= n) idx = 0;
      update([&](AppState& s) { s.complete_index = idx; });
      return;
    }
    if (key == "Escape") {
      update([&](AppState& s) { s.complete_kind = AppState::CompleteKind::kNone; });
      return;
    }
    if (key == "Tab") {
      accept_complete();
    }
  });
  comp_box.add(ta);
  // The completion menu floats ABOVE the textarea (same slot the "/" menu uses),
  // so it never pushes the input around while the user types.
  if (st_.complete_kind != AppState::CompleteKind::kNone) {
    comp_box.add(build_complete_menu());
  }

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
    // The command list comes from the BACKEND (command.list): built-in actions
    // plus the user-callable skills. Nothing here is hard-coded, so adding a
    // command is a backend change only.
    call_async("command.list", Json{{"sid", active_sid()}},
               [this](const BackendResult& r) {
      if (!r.ok) return;
      std::vector<AppState::CommandRow> rows;
      const Json& arr = r.data.contains("commands") ? r.data["commands"] : r.data;
      if (arr.is_array()) {
        for (size_t i = 0; i < arr.size(); ++i) {
          AppState::CommandRow row;
          row.name = jsonutil::get<std::string>(arr[i], "name", "");
          row.title = jsonutil::get<std::string>(arr[i], "title", row.name);
          row.description = jsonutil::get<std::string>(arr[i], "description", "");
          row.action = jsonutil::get<std::string>(arr[i], "action", "");
          row.group = jsonutil::get<std::string>(arr[i], "group", "");
          if (!row.name.empty()) rows.push_back(row);
        }
      }
      update([&](AppState& s) {
        s.command_menu = rows;
        s.skill_menu_open = true;
      });
    });
  }, "ghost"));
  // The plan has NO composer control: it is a document, not a panel.
  // The task list button is ALWAYS here: it is the only progress view, and a
  // control that appears only after the model remembers to add an item is a
  // control the user cannot find ("把 todo 的按钮搞哪去了"). The panel itself
  // says 暂无任务 when the list is empty.
  left_tools.add(comp::icon_text_button("list-check", tr("清单"),
                                        [this](const std::string&) {
    bool open = !st_.todo_open;
    update([&](AppState& s) { s.todo_open = open; });
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
    // No manual "定时/自动唤醒" button: scheduling a wake is the MODEL's action
    // (it calls session.schedule when it decides to retry later). A user-facing
    // button duplicated that, and its only real use - cancelling a pending wake -
    // belongs next to 挂起 and appears only while one is actually pending.
  }
  // (The inline "schedule a wake in N minutes" form is gone with the button.)
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
  // The label carries the PROVIDER too. "deepseek-chat" alone is ambiguous once
  // two providers offer a model with the same id (or the user has several
  // endpoints of one vendor) - and the provider is what actually routes the
  // request. Show it dimmed next to the model name.
  if (st_.model.empty()) {
    model_btn.add(el("span", "mpb-name").text(tr("选择模型")));
  } else {
    // The provider comes from model.allEnabled (the authoritative owner of this
    // model id). Fall back to scanning the picker's list so the label still shows
    // before the availability check has replied.
    std::string prov_label = st_.model_provider_label;
    if (prov_label.empty()) {
      for (size_t i = 0; i < st_.models.size(); ++i) {
        if (st_.models[i].id != st_.model) continue;
        std::string pid = st_.models[i].provider;
        for (size_t k = 0; k < st_.providers.size(); ++k) {
          if (st_.providers[k].id != pid) continue;
          prov_label = st_.providers[k].name.empty() ? st_.providers[k].id
                                                     : st_.providers[k].name;
          break;
        }
        if (prov_label.empty()) prov_label = pid;
        break;
      }
    }
    if (!prov_label.empty()) {
      // Provider is shown only when there is room (CSS hides it in compact mode,
      // under .composer crowded / narrow width). It is context, not the model's
      // identity, so it must not push the model name out of the button.
      model_btn.add(el("span", "mpb-prov").text(prov_label));
    }
    model_btn.add(el("span", "mpb-name").text(st_.model));
  }
  model_btn.add(el("span", "mpb-caret").text("\u25be"));
  if (!st_.model_available) {
    // The remembered model is gone. The box is already empty (refresh cleared
    // it); mark it so the reason is visible at a glance instead of the user
    // wondering why send is dead.
    model_btn.cls("warn");
    model_btn.attr("title", tr("上次使用的模型已不可用，请重新选择"));
  }
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
    lvl.attr("title", st_.context_pct < 0
                          ? tr("上下文水位：还没有用量数据")
                          : (tr("上下文水位 ") + itos(st_.context_pct) + "%"));
    mb.add(lvl);
    // A 2px hairline is not readable as a number. Once the level is known, show
    // it as text too: "-1%" told the user nothing was computed, and 1% looked
    // identical to 0%.
    if (st_.context_pct >= 0) {
      H txt = el("span", "model-ctx-pct");
      txt.text(itos(st_.context_pct) + "%");
      if (st_.context_pct >= 85) txt.cls("hot");
      else if (st_.context_pct >= 60) txt.cls("warm");
      mb.add(txt);
    }
    right_tools.add(mb);
  }
  // 思考档位: ONE control. It used to be an icon button that did nothing plus a
  // native <select> showing the same value next to it, which read as two
  // controls - the first one dead ("思考档位，思考档位（重复！这个按钮按下没反应）).
  // Now the icon IS the control: it opens a list of the model's real levels, and
  // each row carries that level's own icon, so 关/低/中/高 are distinguishable
  // at a glance instead of all sharing one `brain`.
  //
  // The options are EXACTLY what the model's table configures, and the model's
  // own `thinking.default` is pre-selected. There is deliberately no synthetic
  // "默认" row: the levels shown are the real choices the API accepts. The
  // disable/enable ladder is only a FALLBACK for a model whose table carries no
  // levels at all.
  std::vector<std::string> levels = st_.model_thinking_levels;
  if (levels.empty() && !st_.model.empty()) {
    levels.push_back("disable");
    levels.push_back("enable");
  }
  if (!levels.empty()) {
    // Selection: the session's own choice if it is a valid level, otherwise the
    // model's configured default, otherwise the first level. Never "nothing
    // selected", because an empty value means "send no hint" and the control
    // would then misrepresent that as one of the listed options.
    int sel = 0;
    std::string want = st_.thinking.empty() ? st_.model_thinking_default
                                            : st_.thinking;
    for (size_t i = 0; i < levels.size(); ++i) {
      if (levels[i] == want) { sel = static_cast<int>(i); break; }
    }
    const std::string cur = levels[static_cast<size_t>(sel)];
    // The button AND its list live in one wrapper, so the list opens right above
    // the BUTTON. Rendering it as a child of the composer anchored to the
    // composer's edge put it over the input box, a column away from the control
    // that opened it.
    H thwrap = el("div", "think-wrap");
    thwrap.key("think-wrap");
    H th = comp::icon_text_button(thinking_icon(cur), thinking_label(cur),
                                  [this](const std::string&) {
      update([&](AppState& s) { s.thinking_menu_open = !s.thinking_menu_open; });
    }, "ghost");
    th.key("thinking-btn");
    th.cls("thinking-btn");
    th.attr("title", tr("思考档位：") + thinking_label(cur));
    thwrap.add(th);
    if (st_.thinking_menu_open) {
      H dismiss = el("div", "skill-menu-backdrop");
      dismiss.on(EventKind::kClick, [this](const std::string&) {
        update([&](AppState& s) { s.thinking_menu_open = false; });
      });
      thwrap.add(dismiss);
      H pop = el("div", "think-menu");
      pop.key("think-menu");
      for (size_t i = 0; i < levels.size(); ++i) {
        std::string lv = levels[i];
        const bool on = static_cast<int>(i) == sel;
        H it = el("div", on ? "think-menu-item active" : "think-menu-item");
        it.on(EventKind::kClick, [this, lv](const std::string&) {
          std::string sid = active_sid();
          update([&](AppState& s) {
            s.thinking = lv;
            s.thinking_menu_open = false;
          });
          if (!sid.empty()) {
            call_async("session.setThinking", Json{{"sid", sid}, {"thinking", lv}});
          }
        });
        it.add(el("i", std::string("fa fa-") + thinking_icon(lv)));
        it.add(el("span", "think-menu-label").text(thinking_label(lv)));
        if (on) it.add(el("i", "fa fa-check"));
        pop.add(it);
      }
      thwrap.add(pop);
    }
    right_tools.add(thwrap);
  }
  // 挂起 sits right next to 发送. Both read as a WORD when there is room and as
  // an icon when there is not. Their height never changes, so crossing the
  // threshold cannot reflow the row.
  //
  // The button only EXISTS while it can do something: a turn is in flight, or a
  // turn is parked. An always-visible 挂起 on an idle session was the "功能疑似
  // 没做全" - pressing it set a flag no running loop would ever read, and the
  // button then sat there claiming the task was suspended.
  //
  // 挂起 must NOT flip on click. The backend owns the state: the loop may be
  // mid-tool and only park when the current call returns, so an optimistic flip
  // showed "恢复" while the turn was still running - and a second click then
  // "resumed" something that had never stopped. The label is derived from the
  // backend's reported loop state; the click only SENDS the request.
  {
    bool is_suspended = st_.suspended;
    if (st_.turn_running || is_suspended) {
      H suspend = comp::icon_text_button(
          is_suspended ? "play" : "pause",
          is_suspended ? tr("恢复") : tr("挂起"),
          [this](const std::string&) {
        std::string sid = active_sid();
        if (sid.empty()) return;
        bool want_suspend = !st_.suspended;
        // Disable while the request is in flight: a double click otherwise sent
        // suspend+resume and left the UI out of step with the loop.
        update([&](AppState& s) { s.suspend_pending = true; });
        call_async(want_suspend ? "session.suspend" : "session.resume",
                   Json{{"sid", sid}}, [this, sid](const BackendResult& r) {
          update([&](AppState& s) { s.suspend_pending = false; });
          if (!r.ok) {
            toast(tr("操作失败：") + r.message);
            return;
          }
          // State comes from the backend, never from the click.
          refresh_loop_state(sid);
        });
      }, "ghost");
      suspend.key("suspend-btn").cls("text-wide");
      if (st_.suspend_pending) {
        suspend.cls("disabled");
        suspend.attr("title", tr("正在等待后端确认…"));
      } else if (is_suspended) {
        suspend.attr("title", tr("当前已挂起；点击恢复（从停下那一步继续）"));
      } else {
        suspend.attr("title", tr("挂起：当前回合跑完后停下，不再发起下一次调用"));
      }
      // Wake cancellation: a scheduled wake (a retry after a failure) would
      // otherwise fire while the user is looking at a parked task. It appears only
      // while a wake is actually pending - not as a manual "schedule a wake" button,
      // which is the model's job (session.schedule), not a user action.
      if (is_suspended && !st_.pending_wakes.empty()) {
        H cancel_wake = comp::icon_text_button(
            "bell-slash", tr("取消唤醒"), [this](const std::string&) {
          std::string sid = active_sid();
          if (sid.empty()) return;
          call_async("session.cancelWake", Json{{"sid", sid}},
                     [this](const BackendResult& r) {
            toast(r.ok ? tr("已取消唤醒") : (tr("取消失败：") + r.message));
          });
        }, "ghost");
        cancel_wake.key("cancel-wake-btn");
        right_tools.add(cancel_wake);
      }
      right_tools.add(suspend);
    }
  }
  // 发送 / 中止: the same button. While the turn runs it stops it, which is what
  // the user expects from the primary action - a "send" button that does nothing
  // during a turn hides the only way out of a runaway one.
  //
  // 中止 is the HARD stop (session.interrupt): it aborts the reply being
  // streamed and parks it, keeping the transcript. 挂起 next to it is the
  // graceful one - finish this round, then wait.
  if (st_.turn_running) {
    H stop = comp::icon_text_button("stop", tr("中止"), [this](const std::string&) {
      std::string sid = active_sid();
      if (sid.empty()) return;
      update([&](AppState& s) { s.suspend_pending = true; });
      call_async("session.interrupt", Json{{"sid", sid}},
                 [this, sid](const BackendResult& r) {
        update([&](AppState& s) { s.suspend_pending = false; });
        if (!r.ok) toast(tr("取消失败：") + r.message);
        refresh_loop_state(sid);
      });
    }, "danger");
    stop.key("stop-btn").cls("text-wide");
    stop.attr("title", tr("中止本轮（立即停止输出，已产生的输出会保留，可继续）"));
    right_tools.add(stop);
  } else {
    // Sending needs a usable model: a session whose remembered model was disabled
    // (or whose provider was deleted) cannot send, and a click that only produces
    // a backend error is worse than a visibly disabled button.
    bool can_send = !backend_ || (!st_.model.empty() && st_.model_available);
    H send = comp::icon_text_button("paper-plane", tr("发送"),
                                    [this](const std::string&) { send_message(); },
                                    "primary");
    send.key("send-btn").cls("text-wide");
    if (!can_send) {
      send.cls("disabled");
      send.attr("title", st_.model.empty() ? tr("请先选择模型")
                                           : tr("该模型已不可用，请重新选择"));
    }
    right_tools.add(send);
  }
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
    if (st_.command_menu.empty()) {
      pop.add(el("div", "skill-menu-item dim").text(tr("（暂无命令）")));
    }
    for (size_t i = 0; i < st_.command_menu.size(); ++i) {
      const AppState::CommandRow cmd = st_.command_menu[i];
      H it = el("div", "skill-menu-item");
      it.on(EventKind::kClick, [this, cmd](const std::string&) {
        run_command(cmd);
      });
      it.add(el("span", "skill-menu-name").text("/" + cmd.name));
      if (!cmd.title.empty() && cmd.title != cmd.name) {
        it.add(el("span", "skill-menu-title").text(cmd.title));
      }
      if (!cmd.description.empty()) {
        it.add(el("span", "skill-menu-desc dim").text(cmd.description));
      }
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
  }  // end: not showing an ask panel
  center.add(comp_box);

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
  // 侧栏标签也可以拖着排序：顺序是用户的工作区安排，和顶栏标签同一套交互。
  {
    VNodePtr strip = side.get();
    // the tab strip is the first child of the panel
    for (size_t i = 0; i < strip->children.size(); ++i) {
      std::string cls;
      for (size_t k = 0; k < strip->children[i]->attrs.size(); ++k) {
        if (strip->children[i]->attrs[k].key == "class") {
          cls = strip->children[i]->attrs[k].value;
        }
      }
      if (cls.find("panel-tabs") != std::string::npos) {
        strip->children[i]->key = "side-tabs";
        break;
      }
    }
  }

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
    H scontent = el("div", "side-content");
    scontent.add(sbody);
    side.add(scontent);
    return side.get();
  }

  // The body lives inside .side-content (absolute, inset 0) so that switching
  // panels can keep the OLD body in the DOM for one animation while the new one
  // enters - the same two-view transition the pages use. Without the wrapper the
  // two bodies would stack in normal flow and each take half the height.
  H content = el("div", "side-content");
  content.key("side-content");
  H body = el("div", "side-body");
  // 侧栏面板内容按面板 key 化：切换面板时整块重新创建并播放进场动画。原来
  // body 是一个无 key 的稳定节点，切换只是原地换字，看起来像"闪了一下"。
  //
  // The direction is HORIZONTAL, matching where the panel came from: a vertical
  // rise inside a right-hand panel reads as the content jumping up. The exit
  // intent is declared too, so the old body slides out while the new one slides
  // in (a real two-view swap) instead of fading in place under the new one.
  body.key("side-body:" + st_.active_side);
  body.anim(anim::kSlideLeft).anim_key("side:" + st_.active_side);
  body.exit(anim::kExitLeft);
  // Stagger, exactly like the page transition: the incoming panel waits until
  // the outgoing one is ~80% gone. Without the delay both bodies move at once
  // and the switch reads as a flicker rather than a swap.
  //
  // The style is re-applied on EVERY render while the entrance is still running
  // (side_entering_key_). The first version only added it on the render that
  // created the node; the second render (the panel's data landing) then produced
  // a tree WITHOUT the inline delay, and patch_attrs cleared the live node's
  // delay - so the animation restarted with 0s and the panel snapped in.
  // The flag is dropped once the stagger + slide has certainly finished.
  {
    std::string body_key = "side-body:" + st_.active_side;
    if (side_entering_key_ != body_key) {
      side_entering_key_ = body_key;
      dom_.schedule(480, [this, body_key]() {
        if (side_entering_key_ == body_key) side_entering_key_.clear();
      });
    }
    if (!side_entering_key_.empty() && side_entering_key_ == body_key) {
      body.style("animation-delay", ".18s");
      body.style("animation-fill-mode", "backwards");
    }
  }
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
    // Every number here comes from usage.session; a value that has not loaded
    // is shown as a placeholder, never as a plausible-looking constant.
    if (!st_.session_usage_loaded && !st_.active_tab.empty() &&
        st_.active_tab != "home") {
      load_session_usage(st_.active_tab);
    }
    // Context: percentage AND absolute numbers. "42%" of a 1M window and 42% of
    // a 32k window are different situations, and only the absolute numbers
    // answer "is there room for one more file read".
    if (st_.context_pct < 0) {
      body.add(comp::key_value(tr("上下文水位"), "—"));
    } else {
      std::string level = itos(st_.context_pct) + "%";
      if (st_.context_window > 0) {
        level += "  (" + fmt_tokens(st_.context_tokens) + " / " +
                 fmt_tokens(st_.context_window) + ")";
      }
      body.add(comp::key_value(tr("上下文水位"), level));
      body.add(comp::progress(st_.context_pct));
      // No "压缩上下文" button: it only armed a flag that the NEXT turn consumed,
      // so pressing it at 90% changed nothing on screen and looked broken. The
      // water-level threshold in settings does the same thing automatically.
    }
    // Cache: the single biggest lever on cost. -1 means the provider never
    // reported it, which is not the same as "no caching".
    if (!st_.cache_reported) {
      body.add(comp::key_value(tr("缓存命中"), "—"));
    } else {
      std::string hit = itos(st_.cache_hit_pct) + "%";
      if (st_.session_cache_tokens > 0) {
        hit += "  (" + fmt_tokens(st_.session_cache_tokens) + tr(" 命中") + ")";
      }
      body.add(comp::key_value(tr("缓存命中"), hit));
    }
    // This turn vs this session: two different questions ("what did that answer
    // cost" vs "what has this conversation cost").
    if (st_.turn_in >= 0) {
      std::string t = fmt_tokens(st_.turn_in) + " / " + fmt_tokens(st_.turn_out);
      if (st_.turn_cost_kind == "money" && st_.turn_cost_micros >= 0) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "  ¥%.4f",
                      static_cast<double>(st_.turn_cost_micros) / 1000000.0);
        t += buf;
      } else if (st_.turn_cost_kind == "quota") {
        t += "  " + tr("套餐内");
      } else if (!st_.turn_cost_kind.empty() && st_.turn_cost_kind != "unknown") {
        t += "  " + tr("未知");
      }
      body.add(comp::key_value(tr("本轮"), t));
    }
    body.add(comp::key_value(tr("本会话成本"),
                             st_.session_cost.empty() ? tr("—") : st_.session_cost));
    if (st_.session_tokens_in >= 0) {
      body.add(comp::key_value(tr("累计 tokens"),
                               fmt_tokens(st_.session_tokens_in) + " / " +
                                   fmt_tokens(st_.session_tokens_out)));
    } else {
      body.add(comp::key_value(tr("累计 tokens"), "—"));
    }
    if (st_.usage_calls > 0) {
      body.add(comp::key_value(tr("调用次数"), itos(static_cast<int>(st_.usage_calls))));
    }
    // ---- 余额 / 额度 ----
    // The same provider.balances rows the usage page shows, in the sidebar: this
    // is where "how much is left" is usually asked (while working, not while
    // reading a report). Asked when the panel is opened and after every turn.
    load_balances(false);
    body.add(el("div", "side-sep"));
    H bhead = el("div", "side-section");
    bhead.add(el("span").text(tr("余额 / 额度")));
    bhead.add(el("span", "grow"));
    if (!st_.balances_loading) {
      bhead.add(comp::icon_button("rotate", tr("重新查询"), [this](const std::string&) {
        load_balances(true);
      }));
    }
    body.add(bhead);
    auto money = [](const std::string& cur, const std::string& v) {
      if (v.empty()) return std::string("—");
      if (cur == "CNY") return "¥" + v;
      if (cur == "USD") return "$" + v;
      return cur.empty() ? v : (cur + " " + v);
    };
    if (st_.balances.empty()) {
      body.add(el("div", "hint").text(st_.balances_loading ? tr("正在查询…")
                                                           : tr("还没有供应商")));
    }
    for (size_t i = 0; i < st_.balances.size(); ++i) {
      const AppState::BalanceRow& b = st_.balances[i];
      std::string right;
      std::string cls = "sb-bal";
      if (b.status == "ok") {
        right = money(b.currency, b.balance);
      } else if (b.status == "unsupported") {
        right = tr("不支持");
        cls += " dim";
      } else {
        right = b.note.empty() ? tr("获取失败") : tr(b.note.c_str());
        cls += " bad";
      }
      H row = el("div", "sb-bal-row");
      row.add(el("span", "sb-bal-name").text(b.name.empty() ? b.pid : b.name));
      row.add(el("span", cls).text(right));
      if (!b.endpoint.empty()) row.attr("title", b.endpoint);
      body.add(row);
    }
  } else if (kind == "subagent") {
    // One subagent's full transcript, in the SIDEBAR (see open_subagent_tab).
    // The id comes from THIS panel, so two agents can be open side by side.
    std::string sub_id = panel ? panel->subagent_id : std::string();
    AppState::SubagentDetail* det = nullptr;
    {
      std::map<std::string, AppState::SubagentDetail>::iterator it =
          st_.subagent_details.find(sub_id);
      if (it != st_.subagent_details.end()) det = &it->second;
    }
    if (!det || !det->loaded) {
      body.add(comp::empty_state(tr("读取子代理记录…"), "", nullptr));
    } else {
      const AppState::SubagentRow& row = det->row;
      H hd = el("div", "sap-head");
      hd.add(el("span", row.status == "running" ? "sa-status running" : "sa-status done")
                 .text(row.status == "running" ? "●" : "✓"));
      hd.add(el("span", "sa-kind").text(row.kind));
      if (!row.model.empty()) hd.add(comp::badge(row.model));
      hd.add(el("span", "grow"));
      if (row.started_at > 0 && row.ended_at > 0) {
        hd.add(el("span", "dim").text(session_duration(row.started_at, row.ended_at)));
      }
      hd.add(comp::icon_button("rotate", tr("刷新"), [this, sub_id](const std::string&) {
        load_subagent_detail(sub_id);
      }));
      body.add(hd);
      if (!row.task.empty()) {
        H t = el("div", "sap-task");
        t.add(el("span", "sap-label").text(tr("任务")));
        t.add(el("div", "sap-task-text").text(row.task));
        body.add(t);
      }
      if (!row.summary.empty()) {
        H sm = el("div", "sap-summary");
        sm.add(el("span", "sap-label").text(tr("摘要")));
        sm.add(el("div", "sap-summary-text").text(row.summary));
        body.add(sm);
      }
      // 引导 lives HERE, on the detail page: a running agent takes it between
      // tool calls, a finished one gets a follow-up that continues with the same
      // transcript. On the overview card the field was unreachable - the card
      // opens the record on click, so every press in the field bubbled into that
      // navigation.
      {
        bool running = row.status == "running";
        H steer = el("div", "sa-steer");
        H input = comp::text_input(
            det->steer_draft,
            running ? tr("给这个子代理的引导…") : tr("追加任务，让它继续…"),
            [this, sub_id](const std::string& v) {
          std::map<std::string, AppState::SubagentDetail>::iterator it =
              st_.subagent_details.find(sub_id);
          if (it != st_.subagent_details.end()) it->second.steer_draft = v;
        },
            [this, sub_id, running](const std::string&) {
          std::map<std::string, AppState::SubagentDetail>::iterator it =
              st_.subagent_details.find(sub_id);
          if (it == st_.subagent_details.end()) return;
          send_subagent_steer_text(sub_id, it->second.steer_draft, running);
        });
        input.key("sa-detail-steer:" + sub_id);
        steer.add(input);
        steer.add(comp::icon_button("paper-plane",
                                    running ? tr("发送引导") : tr("追加任务"),
                                    [this, sub_id, running](const std::string&) {
          std::map<std::string, AppState::SubagentDetail>::iterator it =
              st_.subagent_details.find(sub_id);
          if (it == st_.subagent_details.end()) return;
          send_subagent_steer_text(sub_id, it->second.steer_draft, running);
        }));
        body.add(steer);
      }
      if (row.transcript.is_array() && row.transcript.size() > 0) {
        body.add(el("div", "sap-label").text(
            tr("对话记录") + " · " + itos(static_cast<int>(row.transcript.size()))));
        for (size_t k = 0; k < row.transcript.size(); ++k) {
          const Json& e = row.transcript[k];
          std::string role = jsonutil::get<std::string>(e, "role", "");
          std::string text = jsonutil::get<std::string>(e, "content",
                              jsonutil::get<std::string>(e, "text", ""));
          // The transcript is shown WHOLE here: this panel exists precisely
          // because the card could not show it. Tool calls are listed whenever
          // the message HAS them - the old code only fell back to the names when
          // the text was empty, so a round that both spoke and called tools
          // looked like it only spoke.
          std::string names;
          if (e.contains("toolCalls") && e["toolCalls"].is_array()) {
            for (size_t c = 0; c < e["toolCalls"].size(); ++c) {
              std::string nm = jsonutil::get<std::string>(e["toolCalls"][c], "name", "");
              if (nm.empty()) continue;
              if (!names.empty()) names += ", ";
              names += nm;
            }
          }
          if (text.empty() && names.empty()) continue;
          H row_h = el("div", "sap-msg " + (role.empty() ? std::string("note") : role));
          row_h.add(el("div", "sap-role").text(role));
          if (!text.empty()) row_h.add(el("pre", "sap-body").text(text));
          if (!names.empty()) {
            row_h.add(el("div", "sap-tools").text(tr("工具调用") + ": " + names));
          }
          body.add(row_h);
        }
      } else {
        body.add(comp::empty_state(tr("这个子代理还没有对话记录。"), "", nullptr));
      }
    }
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
      if (!a.model.empty()) head.add(comp::badge(a.model));
      head.add(el("span", "grow"));
      // Timing: "what is still running, and how long did the finished ones
      // take" is the question this panel exists to answer.
      if (a.status == "running") {
        head.add(el("span", "sa-time dim").text(
            a.started_at > 0 ? (tr("已运行 ") + session_duration(a.started_at, 0))
                             : std::string()));
      } else if (a.ended_at > 0) {
        head.add(el("span", "sa-time dim").text(
            session_duration(a.started_at, a.ended_at) + "  ·  " +
            rel_time(a.ended_at)));
      }
      card.add(head);
      // The card is CLICKABLE and opens the transcript as its own tab. The old
      // design only folded a few lines inside the sidebar, so a finished
      // subagent's history was effectively unreachable - and a transcript is far
      // too long for a card anyway.
      {
        std::string id = a.id;
        std::string title = a.kind.empty() ? a.id : (a.kind + " · " + a.id);
        card.on(EventKind::kClick, [this, id, title](const std::string&) {
          open_subagent_tab(id, title);
        });
        card.attr("title", tr("点击打开完整记录"));
        card.cls("clickable");
      }
      card.add(el("div", "sa-task").text(a.task));
      // Live progress: what the subagent is doing RIGHT NOW. A running subagent
      // used to show only "running" plus a duration, so the panel answered
      // "something is happening" but not "what" - the report was "只能在侧边栏
      // 看到在运行，不能看跑的具体内容".
      if (a.status == "running" && !a.live_text.empty()) {
        H live = el("div", "sa-live");
        live.add(el("span", "sa-live-label").text(tr("正在做")));
        live.add(el("div", "sa-live-text").text(a.live_text));
        card.add(live);
      }
      // Transcript: the subagent's own conversation, folded. It is kept out of
      // the main chat on purpose, but the user must be able to read it.
      if (a.transcript.is_array() && a.transcript.size() > 0) {
        H trow = el("div", "sa-history-toggle");
        std::string id = a.id;
        trow.on(EventKind::kClick, [this, id](const std::string&) {
          update([&](AppState& s) {
            for (size_t k = 0; k < s.subagents.size(); ++k) {
              if (s.subagents[k].id == id) s.subagents[k].open = !s.subagents[k].open;
            }
          });
        });
        trow.add(el("span", "sa-caret").text(a.open ? "\u25be" : "\u25b8"));
        trow.add(el("span").text(
            tr("对话记录") + " · " + itos(static_cast<int>(a.transcript.size()))));
        card.add(trow);
        if (a.open) {
          H log = el("div", "sa-history");
          for (size_t k = 0; k < a.transcript.size(); ++k) {
            const Json& e = a.transcript[k];
            std::string role = jsonutil::get<std::string>(e, "role", "");
            std::string text = jsonutil::get<std::string>(e, "content",
                                jsonutil::get<std::string>(e, "text", ""));
            if (text.empty()) {
              text = jsonutil::get<std::string>(e, "summary", "");
            }
            // A tool call inside the transcript: show WHAT was called. Without
            // this the log skipped every tool row and read like a conversation
            // with the actual work removed.
            if (text.empty() && e.contains("toolCalls") && e["toolCalls"].is_array()) {
              std::string names;
              for (size_t c = 0; c < e["toolCalls"].size(); ++c) {
                std::string nm = jsonutil::get<std::string>(e["toolCalls"][c], "name", "");
                if (nm.empty()) continue;
                if (!names.empty()) names += ", ";
                names += nm;
              }
              if (!names.empty()) {
                text = tr("工具调用") + ": " + names;
                if (role.empty()) role = "tool";
              }
            }
            if (role.empty() && text.empty()) continue;
            H line = el("div", "sa-hline " + (role.empty() ? std::string("note") : role));
            line.add(el("span", "sa-hrole").text(role));
            line.add(el("span", "sa-hbody").text(text));
            log.add(line);
          }
          card.add(log);
        }
      } else if (!a.summary.empty()) {
        card.add(el("div", "sa-summary").text(a.summary));
      }
      // Steering lives on the DETAIL page (this card NAVIGATES on click, so a
      // text field here could not even be clicked into: the click bubbled and
      // opened the record instead). Only 取消 stays here.
      H acts = el("div", "sa-actions");
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
      if (panel && panel->files_truncated) {
        body.add(el("div", "hint").text(tr("目录太大，只列出前 2000 项。")));
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
    // One file's contents, in its own tab. WHAT the file is decides the
    // renderer: an image gets an <img>, a binary says so instead of showing
    // mojibake, text is source, and markdown renders as prose with 源码 one
    // click away.
    const std::string& rel = panel ? panel->path : std::string();
    if (rel.empty()) {
      body.add(comp::empty_state(tr("没有选中的文件。"), "", nullptr));
    } else {
      std::string lang = highlight::lang_for_path(rel);
      bool is_md = lang == "markdown";
      std::string pkind = panel->preview_kind.empty() ? "text" : panel->preview_kind;
      H head = el("div", "files-nav");
      head.add(el("span", "files-cwd").text(folder_basename(rel)));
      head.add(el("span", "grow"));
      // Markdown: 渲染 / 源码. Only the INACTIVE half is a live button - clicking
      // the mode you are already in would be a no-op with a lit face.
      if (is_md && panel->loaded && pkind == "text" && !panel->preview.empty()) {
        bool src = panel->preview_source;
        H seg = el("div", "preview-modes");
        EventFn set_src = [this, panel_id](const std::string& v) {
          bool to_src = v == "1";
          update([&](AppState& s) {
            for (size_t i = 0; i < s.side_tabs.size(); ++i) {
              if (s.side_tabs[i].id == panel_id) s.side_tabs[i].preview_source = to_src;
            }
          });
        };
        H b_render = el("button", src ? "mini-btn" : "mini-btn active");
        b_render.attr("type", "button");
        b_render.text(tr("渲染"));
        if (src) {
          b_render.on(EventKind::kClick,
                      [set_src](const std::string&) { set_src("0"); });
        }
        seg.add(b_render);
        H b_src = el("button", src ? "mini-btn active" : "mini-btn");
        b_src.attr("type", "button");
        b_src.text(tr("源码"));
        if (!src) {
          b_src.on(EventKind::kClick,
                   [set_src](const std::string&) { set_src("1"); });
        }
        seg.add(b_src);
        head.add(seg);
      }
      // Copy the project-relative path: that is the string the model and the
      // terminal both understand ("src/core/a.cpp"), unlike an absolute path.
      head.add(comp::icon_button("copy", tr("复制相对路径"), [this, rel](const std::string&) {
        dom_.copy_text(rel);
        toast(tr("已复制路径"));
      }));
      head.add(comp::button(tr("刷新"), [this, panel_id, rel](const std::string&) {
        preview_file(panel_id, rel);
      }, "ghost"));
      body.add(head);
      if (!panel->loaded) {
        body.add(comp::empty_state(tr("加载中…"), "", nullptr));
      } else if (pkind == "image") {
        // The backend hands over a data URI, so the browser paints it with no
        // extra request (and the wasm build has no server to ask).
        H media = el("div", "file-preview-media");
        H img = el("img", "file-preview-img");
        img.attr("src", panel->preview_uri);
        img.attr("alt", folder_basename(rel));
        media.add(img);
        body.add(media);
      } else if (pkind == "binary") {
        std::string sz = panel->preview_size > 1024 * 1024
                             ? itos(static_cast<int>(panel->preview_size / (1024 * 1024))) + " MB"
                             : (panel->preview_size > 1024
                                    ? itos(static_cast<int>(panel->preview_size / 1024)) + " KB"
                                    : itos(static_cast<int>(panel->preview_size)) + " B");
        body.add(comp::empty_state(tr("二进制文件，没有可显示的文字。") + " (" + sz + ")",
                                   "", nullptr));
      } else if (panel->preview.empty()) {
        body.add(comp::empty_state(tr("（空文件）"), "", nullptr));
      } else if (is_md && !panel->preview_source) {
        // Rendered markdown: links open like they do in an answer, and a code
        // block's copy button copies the block (not the whole document).
        H md = el("div", "file-preview-md article");
        md.key("preview-md");
        std::vector<H> blocks = comp::markdown_blocks(
            panel->preview,
            [this](const std::string& t) {
          dom_.copy_text(t);
          toast(tr("已复制"));
        },
            [this](const std::string& href) { open_href(href); });
        for (size_t i = 0; i < blocks.size(); ++i) md.add(blocks[i]);
        body.add(md);
      } else {
        // Line numbers and content share ONE scroll container, so a number can
        // never drift away from its line.
        H box = el("div", "file-preview");
        H pre = el("pre", "file-preview-body");
        // Guess the language from the FILE NAME, not from a fence label: the
        // preview has no language tag to go on. An unknown extension falls back
        // to plain text, which is the right answer for a config or a log.
        if (!lang.empty()) {
          pre.attr("data-lang", lang);
          pre.html(highlight::to_html_cached(panel->preview, lang));
        } else {
          pre.text(panel->preview);
        }
        int nlines = 0;
        for (size_t k = 0; k < panel->preview.size(); ++k) {
          if (panel->preview[k] == '\n') ++nlines;
        }
        if (nlines == 0 || panel->preview[panel->preview.size() - 1] != '\n') ++nlines;
        H gutter = el("div", "file-preview-lines");
        std::string nums;
        for (int n = 1; n <= nlines; ++n) {
          if (n > 1) nums += "\n";
          nums += itos(n);
        }
        gutter.text(nums);
        box.add(gutter);
        box.add(pre);
        body.add(box);
      }
    }
  } else if (kind == "git") {
    // ---- Git panel ----
    // Three states, and they are genuinely different: not a repository (nothing
    // to do), a repository that failed to read (show git's own message), and a
    // repository with a status. Collapsing them into one "no changes" message
    // is how a broken repo looks healthy.
    if (!panel || !panel->git_loaded) {
      body.add(comp::empty_state(tr("读取 Git 状态…"), "", nullptr));
      if (panel && !panel->git_loaded && !st_.active_pid.empty()) load_git(panel->id);
    } else if (!panel->git_repo) {
      body.add(comp::empty_state(
          panel->git_error.empty()
              ? tr("这个项目不是 Git 仓库。")
              : (tr("无法读取 Git 状态：") + panel->git_error),
          "", nullptr));
    } else {
      // --- header: branch / ahead-behind / operation in progress ---
      H head = el("div", "git-head");
      head.add(el("i", "fa fa-code-branch"));
      if (panel->git_detached) {
        head.add(el("span", "git-branch").text(tr("游离 HEAD")));
      } else {
        head.add(el("span", "git-branch").text(
            panel->git_branch.empty() ? tr("（未知分支）") : panel->git_branch));
      }
      if (panel->git_ahead > 0) {
        head.add(el("span", "git-ahead dim").text("\u2191" + itos(panel->git_ahead)));
      }
      if (panel->git_behind > 0) {
        head.add(el("span", "git-behind dim").text("\u2193" + itos(panel->git_behind)));
      }
      head.add(el("span", "grow"));
      head.add(comp::icon_button("\u21bb", tr("刷新"), [this, panel_id](const std::string&) {
        load_git(panel_id);
        load_git_log(panel_id);
        load_git_branches(panel_id);
      }));
      body.add(head);
      if (!panel->git_operation.empty()) {
        H op = el("div", "git-op");
        op.add(el("span", "git-op-name").text(
            tr("进行中的操作：") + panel->git_operation));
        body.add(op);
      }

      // --- view switch: changes / history / branches ---
      // One tab, three views: all three answer "what is the state of this
      // repository", and three side-panel tabs for that would bury the file list.
      {
        H views = el("div", "git-views");
        const char* labels[3] = {"改动", "历史", "分支"};
        const char* keys[3] = {"changes", "log", "branches"};
        for (int k = 0; k < 3; ++k) {
          std::string key = keys[k];
          bool active = panel->git_view == key ||
                        (k == 0 && panel->git_view != "log" && panel->git_view != "branches");
          H b = el("button", active ? "git-view-btn active" : "git-view-btn");
          b.attr("type", "button");
          b.text(tr(labels[k]));
          b.on(EventKind::kClick, [this, panel_id, key](const std::string&) {
            update([&](AppState& s) {
              for (size_t i = 0; i < s.side_tabs.size(); ++i) {
                if (s.side_tabs[i].id == panel_id) s.side_tabs[i].git_view = key;
              }
            });
            if (key == "log") load_git_log(panel_id);
            if (key == "branches") load_git_branches(panel_id);
          });
          views.add(b);
        }
        body.add(views);
      }

      if (panel->git_view == "log") {
        if (!panel->git_log_loaded) {
          body.add(comp::empty_state(tr("读取提交历史…"), "", nullptr));
          load_git_log(panel_id);
        } else if (panel->git_commits.empty()) {
          body.add(comp::empty_state(tr("还没有提交。"), "", nullptr));
        } else {
          H list = el("div", "git-log");
          for (size_t i = 0; i < panel->git_commits.size(); ++i) {
            const SideTab::GitCommit& c = panel->git_commits[i];
            H row = el("div", "git-log-row");
            row.add(el("span", "git-hash").text(c.short_hash));
            row.add(el("span", "git-subject").text(c.subject));
            row.add(el("span", "grow"));
            row.add(el("span", "git-log-meta dim").text(
                c.author + (c.time > 0 ? (" · " + rel_time(c.time)) : "")));
            list.add(row);
          }
          body.add(list);
        }
      } else if (panel->git_view == "branches") {
        if (!panel->git_branches_loaded) {
          body.add(comp::empty_state(tr("读取分支…"), "", nullptr));
          load_git_branches(panel_id);
        } else if (panel->git_branch_rows.empty()) {
          body.add(comp::empty_state(tr("没有分支。"), "", nullptr));
        } else {
          H list = el("div", "git-branches");
          for (size_t i = 0; i < panel->git_branch_rows.size(); ++i) {
            const SideTab::GitBranch& b = panel->git_branch_rows[i];
            H row = el("div", b.current ? "git-branch-row current" : "git-branch-row");
            row.add(el("span", "git-branch-mark").text(b.current ? "\u2713" : ""));
            row.add(el("span", "git-branch-name").text(b.name));
            list.add(row);
          }
          // Switching branches changes files under the user's feet, so it is
          // deliberately not a click on this list; the CLI is one command away.
          body.add(comp::hint(tr("切换分支请在终端执行 git switch。")));
          body.add(list);
        }
      } else {
      // --- changed files: staged group first, then unstaged ---
      // A single list with per-row checkboxes was the alternative; two groups
      // are clearer because "what a commit would record" is the question this
      // panel exists to answer.
      std::vector<size_t> staged_idx, unstaged_idx;
      for (size_t i = 0; i < panel->git_changes.size(); ++i) {
        if (panel->git_changes[i].staged) staged_idx.push_back(i);
        else unstaged_idx.push_back(i);
      }
      if (panel->git_changes.empty()) {
        body.add(comp::empty_state(tr("工作区干净，没有待提交的改动。"), "", nullptr));
      }
      if (!staged_idx.empty()) {
        H sec = el("div", "git-section");
        sec.add(el("div", "git-section-head").text(
            tr("已暂存") + " · " + itos(static_cast<int>(staged_idx.size()))));
        for (size_t k = 0; k < staged_idx.size(); ++k) {
          const SideTab::GitChange& c = panel->git_changes[staged_idx[k]];
          H row = el("div", c.path == panel->git_diff_path && panel->git_diff_staged
                                 ? "git-row open" : "git-row");
          row.add(el("span", "git-code staged").text(
              std::string(1, c.index == ' ' ? 'M' : c.index)));
          std::string shown = c.path;
          if (!c.orig_path.empty()) shown = c.orig_path + " \u2192 " + c.path;
          row.add(el("span", "git-path").text(shown));
          row.on(EventKind::kClick, [this, panel_id, c](const std::string&) {
            load_git_diff(panel_id, c.path, true);
          });
          H acts = el("div", "git-row-acts");
          std::string path = c.path;
          acts.add(comp::icon_button("\u2212", tr("取消暂存"),
                                     [this, panel_id, path](const std::string&) {
            git_stage_paths(panel_id, std::vector<std::string>(1, path), false);
          }));
          row.add(acts);
          sec.add(row);
        }
        body.add(sec);
      }
      if (!unstaged_idx.empty()) {
        H sec = el("div", "git-section");
        H shead = el("div", "git-section-head");
        shead.add(el("span").text(
            tr("未暂存") + " · " + itos(static_cast<int>(unstaged_idx.size()))));
        shead.add(el("span", "grow"));
        shead.add(comp::button(tr("全部暂存"), [this, panel_id](const std::string&) {
          git_stage_all(panel_id);
        }, "ghost"));
        sec.add(shead);
        for (size_t k = 0; k < unstaged_idx.size(); ++k) {
          const SideTab::GitChange& c = panel->git_changes[unstaged_idx[k]];
          H row = el("div", c.path == panel->git_diff_path && !panel->git_diff_staged
                                 ? "git-row open" : "git-row");
          if (c.conflicted) row.cls("conflict");
          row.add(el("span", c.untracked ? "git-code untracked" : "git-code").text(
              std::string(1, c.untracked ? '?' : (c.work == ' ' ? 'M' : c.work))));
          row.add(el("span", "git-path").text(c.path));
          row.on(EventKind::kClick, [this, panel_id, c](const std::string&) {
            load_git_diff(panel_id, c.path, false);
          });
          H acts = el("div", "git-row-acts");
          std::string path = c.path;
          // Discard is destructive and cannot be undone, so it is a separate,
          // clearly-labelled action rather than a hover icon next to stage.
          if (!c.untracked) {
            acts.add(comp::icon_button("\u21ba", tr("撤销改动"),
                                       [this, panel_id, path](const std::string&) {
              git_discard_paths(panel_id, std::vector<std::string>(1, path));
            }));
          }
          acts.add(comp::icon_button("+", tr("暂存"),
                                     [this, panel_id, path](const std::string&) {
            git_stage_paths(panel_id, std::vector<std::string>(1, path), true);
          }));
          row.add(acts);
          sec.add(row);
        }
        body.add(sec);
      }

      // --- diff of the selected file ---
      if (!panel->git_diff_path.empty()) {
        H dh = el("div", "git-diff-head");
        dh.add(el("span", "git-diff-path").text(panel->git_diff_path));
        dh.add(el("span", "grow"));
        dh.add(el("span", "git-diff-side dim").text(
            panel->git_diff_staged ? tr("已暂存") : tr("工作区")));
        // "Before" view: the committed content of the same file. A diff tells
        // you what changed; reading a change often needs the whole old version.
        {
          std::string p = panel->git_diff_path;
          bool showing = panel->git_show_head;
          dh.add(comp::icon_button(showing ? "\u25c0" : "\u2261",
                                   showing ? tr("只看改动") : tr("对照 HEAD"),
                                   [this, panel_id, p, showing](const std::string&) {
            if (showing) {
              update([&](AppState& s) {
                for (size_t i = 0; i < s.side_tabs.size(); ++i) {
                  if (s.side_tabs[i].id == panel_id) {
                    s.side_tabs[i].git_show_head = false;
                    s.side_tabs[i].git_head_text.clear();
                  }
                }
              });
              return;
            }
            load_git_head(panel_id, p);
          }));
        }
        dh.add(comp::icon_button("\u2715", tr("关闭"), [this, panel_id](const std::string&) {
          update([&](AppState& s) {
            for (size_t i = 0; i < s.side_tabs.size(); ++i) {
              if (s.side_tabs[i].id == panel_id) {
                s.side_tabs[i].git_diff_path.clear();
                s.side_tabs[i].git_diff_text.clear();
                s.side_tabs[i].git_show_head = false;
                s.side_tabs[i].git_head_text.clear();
              }
            }
          });
        }));
        body.add(dh);
        if (panel->git_show_head) {
          if (!panel->git_head_exists) {
            body.add(comp::hint(tr("HEAD 里没有这个文件（新增文件）。")));
          } else {
            H pre = el("pre", "git-head-pre");
            pre.html(highlight::to_html_cached(
                panel->git_head_text, highlight::lang_for_path(panel->git_diff_path)));
            body.add(pre);
          }
        }
        if (!panel->git_diff_error.empty()) {
          body.add(comp::hint(tr("无法读取 diff：") + panel->git_diff_error));
        } else if (panel->git_diff_text.empty()) {
          body.add(comp::hint(tr("（这个文件没有差异）")));
        } else {
          H pre = el("pre", "git-diff");
          pre.html(highlight::to_html_cached(panel->git_diff_text, "diff"));
          body.add(pre);
          if (panel->git_diff_truncated) {
            body.add(comp::hint(tr("diff 过长，只显示前一部分。")));
          }
        }
      }

      // --- commit box ---
      // Only shown when something is staged: committing nothing is an error, and
      // offering the button anyway invites the click that produces it.
      if (!staged_idx.empty()) {
        H cbox = el("div", "git-commit");
        H ta = comp::textarea(panel->git_message, tr("提交信息"),
                              [this, panel_id](const std::string& v) {
          update([&](AppState& s) {
            for (size_t i = 0; i < s.side_tabs.size(); ++i) {
              if (s.side_tabs[i].id == panel_id) s.side_tabs[i].git_message = v;
            }
          });
        },
                              [this, panel_id](const std::string&) {
          std::string msg;
          for (size_t i = 0; i < st_.side_tabs.size(); ++i) {
            if (st_.side_tabs[i].id == panel_id) msg = st_.side_tabs[i].git_message;
          }
          git_commit(panel_id, msg, false);
        });
        ta.key("git-message:" + panel_id);
        cbox.add(ta);
        H acts = el("div", "git-commit-acts");
        acts.add(comp::button(tr("提交"), [this, panel_id](const std::string&) {
          std::string msg;
          for (size_t i = 0; i < st_.side_tabs.size(); ++i) {
            if (st_.side_tabs[i].id == panel_id) msg = st_.side_tabs[i].git_message;
          }
          git_commit(panel_id, msg, false);
        }, "primary"));
        cbox.add(acts);
        body.add(cbox);
      }
      }  // end of the "changes" view
    }
  } else if (kind == "workflow") {
    // ---- 工作流：模型规划 → 用户允许 → 这里看进度 ----
    // This panel is NOT a launcher. A workflow is planned by the model (the
    // `workflow_plan` tool), waits for the user's 允许, and then runs; the panel's
    // job is to show the PLAN before it runs and the PROGRESS while it does.
    if (!st_.workflow_runs_loaded) {
      body.add(comp::empty_state(tr("读取工作流…"), "", nullptr));
      load_workflows(active_sid());
    } else {
      std::vector<AppState::WorkflowRunRow> proposed;
      std::vector<AppState::WorkflowRunRow> active;
      for (size_t i = 0; i < st_.workflow_runs.size(); ++i) {
        if (st_.workflow_runs[i].state == "proposed") proposed.push_back(st_.workflow_runs[i]);
        else active.push_back(st_.workflow_runs[i]);
      }
      if (proposed.empty() && active.empty()) {
        body.add(comp::empty_state(
            tr("还没有工作流。模型规划多步任务时会在这里等你允许。"), "", nullptr));
      }
      // ---- 待允许：the plan, with the two buttons that matter ----
      for (size_t i = 0; i < proposed.size(); ++i) {
        AppState::WorkflowRunRow run = proposed[i];
        H card = el("div", "wf-run proposed");
        H head = el("div", "wf-run-head");
        head.add(el("span", "wf-dot wait"));
        head.add(el("span", "wf-run-name").text(run.name));
        head.add(el("span", "grow"));
        head.add(el("span", "wf-state dim").text(tr("待允许")));
        card.add(head);
        body.add(el("div", "wf-plan-note").text(
            tr("模型规划了 " + itos(static_cast<int>(run.steps.size())) + " 步，允许后开始执行").c_str()));
        for (size_t k = 0; k < run.steps.size(); ++k) {
          H row = el("div", "wf-step pending");
          row.add(el("span", "wf-step-state").text(itos(static_cast<int>(k + 1)) + "."));
          row.add(el("span", "wf-step-name").text(run.steps[k].name));
          card.add(row);
        }
        H acts = el("div", "wf-acts");
        std::string run_id = run.id;
        acts.add(comp::button(tr("允许"), [this, run_id](const std::string&) {
          call_async("workflow.approve", Json{{"runId", run_id}},
                     [this](const BackendResult& r) {
            if (!r.ok) {
              toast(tr("启动失败：") + r.message);
              return;
            }
            toast(tr("已允许，开始执行"));
            load_workflows(active_sid());
          });
        }, "primary"));
        acts.add(comp::button(tr("拒绝"), [this, run_id](const std::string&) {
          call_async("workflow.reject", Json{{"runId", run_id}},
                     [this](const BackendResult& r) {
            if (!r.ok) {
              toast(tr("取消失败：") + r.message);
              return;
            }
            load_workflows(active_sid());
          });
        }, "ghost"));
        card.add(acts);
        body.add(card);
      }
      // ---- 运行中 / 历史：the progress ----
      for (size_t i = 0; i < active.size(); ++i) {
        AppState::WorkflowRunRow run = active[i];
        H card = el("div", "wf-run");
        H head = el("div", "wf-run-head");
        std::string state_cls = run.state == "done" ? "ok"
                                : run.state == "failed" ? "err"
                                : run.state == "rejected" ? "err"
                                                          : "run";
        head.add(el("span", "wf-dot " + state_cls));
        head.add(el("span", "wf-run-name").text(run.name));
        head.add(el("span", "grow"));
        std::string state_text = run.state == "done" ? tr("完成")
                                 : run.state == "failed" ? tr("失败")
                                 : run.state == "rejected" ? tr("已拒绝")
                                                           : tr("运行中");
        head.add(el("span", "wf-state dim").text(state_text));
        card.add(head);
        for (size_t k = 0; k < run.steps.size(); ++k) {
          const AppState::WorkflowStepRow& st = run.steps[k];
          H row = el("div", "wf-step " + st.state);
          row.add(el("span", "wf-step-state").text(
              st.state == "done" ? "\u2713" : st.state == "failed" ? "\u2717"
                                    : st.state == "running" ? "\u25cf" : "\u25cb"));
          row.add(el("span", "wf-step-name").text(st.name));
          if (st.attempts > 0) {
            row.add(el("span", "wf-attempts dim").text(
                tr("第 ") + itos(st.attempts) + tr(" 次")));
          }
          if (!st.error.empty()) row.add(el("span", "wf-error dim").text(st.error));
          H acts = el("div", "wf-step-acts");
          if (st.state == "failed") {
            std::string run_id = run.id, step_id = st.id;
            acts.add(comp::icon_button("\u21bb", tr("重试"), [this, run_id, step_id](const std::string&) {
              call_async("workflow.retryStep", Json{{"runId", run_id}, {"stepId", step_id}},
                         [this](const BackendResult& r) {
                if (!r.ok) toast(tr("重试失败：") + r.message);
                load_workflows(active_sid());
              });
            }));
            acts.add(comp::icon_button("\u21b7", tr("跳过"), [this, run_id, step_id](const std::string&) {
              call_async("workflow.skipStep", Json{{"runId", run_id}, {"stepId", step_id}},
                         [this](const BackendResult& r) {
                if (!r.ok) toast(tr("跳过失败：") + r.message);
                load_workflows(active_sid());
              });
            }));
          }
          if (st.state == "running") {
            std::string run_id = run.id, step_id = st.id;
            acts.add(comp::icon_button("comment-dots", tr("引导"), [this, run_id, step_id](const std::string&) {
              call_async("workflow.steer", Json{{"runId", run_id}, {"stepId", step_id},
                                                {"text", "继续"}},
                         [this](const BackendResult& r) {
                toast(r.ok ? tr("已发送引导") : (tr("引导失败：") + r.message));
              });
            }));
          }
          row.add(acts);
          card.add(row);
        }
        body.add(card);
      }
    }
  } else if (kind == "ops") {
    // systemOps: a live log while the panel is open (refresh_session_ops runs on
    // pump's 2 s tick - it used to be fetched once, when the session opened).
    if (st_.session_ops.empty()) {
      body.add(comp::empty_state(tr("暂无操作记录（systemOps）。"), "", nullptr));
    }
    for (size_t i = st_.session_ops.size(); i-- > 0;) {
      H row = el("div", "op-row");
      row.add(el("span", "op-name").text(st_.session_ops[i].op));
      row.add(el("span", "op-detail dim").text(st_.session_ops[i].detail));
      body.add(row);
    }
  } else if (kind == "calls") {
    // The call log IS the transcript's tool blocks - no separate fetch, and no
    // way for the two to disagree. (`session_calls` was a field nothing ever
    // filled, so this panel said "暂无工具调用记录" even during a busy turn.)
    int shown = 0;
    for (size_t i = st_.messages.size(); i-- > 0;) {
      const comp::ChatMsg& m = st_.messages[i];
      for (size_t k = 0; k < m.tools.size(); ++k) {
        const comp::ToolBlock& tb = m.tools[k];
        if (tb.name.empty()) continue;
        H row = el("div", "call-row");
        std::string cls = "call-ico ";
        cls += tb.running ? "run" : (tb.ok ? "ok" : "err");
        row.add(el("span", cls).text(tb.running ? "\u25cb" : (tb.ok ? "\u2713" : "\u2717")));
        row.add(el("span", "call-name").text(tb.name));
        row.add(el("span", "call-args dim").text(tb.summary));
        body.add(row);
        ++shown;
      }
    }
    if (shown == 0) {
      body.add(comp::empty_state(tr("暂无工具调用记录。"), "", nullptr));
    }
  } else {
    body.add(comp::empty_state(tr("面板内容"), "", nullptr));
  }
  content.add(body);
  side.add(content);
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
    // The BLOCK animates (a fade), not each row: rows are text on a grid, and a
    // per-row translate made a one-line terminal wobble its single line upward.
    lines_box.anim(anim::kFade);
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
        // A terminal screen is always rows x cols: a fresh terminal has ONE line
        // of output and 23 blank ones. Drawing all of them made the surface
        // taller than its box, so a one-line terminal had a scrollbar. Trailing
        // blank rows are not content - drop them, but never the row the cursor
        // is on (the cursor must stay visible even on an empty screen).
        size_t last = lines.size();
        while (last > 0) {
          const Json& runs = lines[last - 1];
          bool blank = true;
          if (runs.is_array()) {
            for (size_t k = 0; k < runs.size(); ++k) {
              if (!jsonutil::get<std::string>(runs[k], "t", "").empty()) {
                blank = false;
                break;
              }
            }
          }
          if (!blank) break;
          if (cursor_visible && static_cast<int>(last - 1) == cursor_line) break;
          --last;
        }
        if (last == 0) last = 1;  // an empty screen still shows its cursor row
        for (size_t y = 0; y < last; ++y) {
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
    // Encoding: a Windows shell emitting GBK is mojibake under UTF-8 decoding.
    // terminal.show carries the current value, so the control reflects reality
    // instead of remembering what was last clicked.
    tb.add(comp::icon_button("language", tr("编码"),
                             [this, tid](const std::string&) {
      call_async("terminal.show", Json{{"tid", tid}},
                 [this, tid](const BackendResult& r) {
        std::string cur = r.ok ? jsonutil::get<std::string>(r.data, "encoding", "") : "";
        // Cycle system-default -> UTF-8 -> GBK. A select inside a tab strip is
        // not usable; three states are cheap to cycle.
        std::string next = cur.empty() ? "UTF-8" : (cur == "UTF-8" ? "GBK" : "");
        call_async("terminal.setEncoding", Json{{"tid", tid}, {"enc", next}},
                   [this, next](const BackendResult& r2) {
          toast(r2.ok ? (tr("编码：") + (next.empty() ? tr("系统默认") : next))
                      : (tr("设置失败：") + r2.message));
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
    // 浏览器控制: removed. Nothing drives a browser yet (no driver, no tools that
    // use one), so every switch on that page was inert.
    {"通用", "computer", "截图"},
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
    {"权限", "webserver", "Web 服务"},
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
  // Project-level "ignore global settings" is a property of the DUMP, not a
  // global key: store it where the files page reads it so the toggle reflects
  // the real state instead of always starting unchecked.
  if (values.is_object() && values.contains("ignoreGlobal") &&
      values["ignoreGlobal"].is_boolean()) {
    st_.settings_values["settings.ignoreGlobal"] =
        Json{{"value", values["ignoreGlobal"].get<bool>()}};
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
  // Compaction: only the water-level threshold is a knob (the keep-turns count
  // and the token budget were misleading controls - see the settings page).
  st_.history_compress_at =
      std::to_string(sv_int(st_.settings_values, "history.compressAtPct", 80));
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
  if (section == "webserver") {
    refresh_server_status();
  }
}

// Execute a command picked from the composer's menu.
//
// "ui:<x>" actions belong to the UI (opening a panel, starting a session) and
// are the ONLY names this function knows. Everything else is a backend action:
// a skill to load, or a session operation. The split matters because the list
// itself comes from the backend - a new command needs no frontend change unless
// it is genuinely a UI action.
void UiApp::run_command(const AppState::CommandRow& cmd) {
  update([&](AppState& s) { s.skill_menu_open = false; });
  const std::string& action = cmd.action;
  if (action.compare(0, 3, "ui:") == 0) {
    std::string what = action.substr(3);
    if (what == "new-session") {
      open_new_session();
      return;
    }
    if (what.compare(0, 6, "panel:") == 0) {
      update([&](AppState& s) { s.side_collapsed = false; });
      open_or_focus_side_tab(what.substr(6));
      return;
    }
    toast(tr("未知命令：") + cmd.name);
    return;
  }
  // Backend action: a skill load or a session operation.
  std::string sid = active_sid();
  call_async("command.run", Json{{"sid", sid}, {"name", cmd.name}},
             [this, cmd](const BackendResult& r) {
    if (!r.ok) {
      toast(tr("命令失败：") + r.message);
      return;
    }
    toast(tr("已执行：") + cmd.name);
  });
}

int UiApp::find_message_by_uid(int64_t uid) const {
  for (size_t i = 0; i < st_.messages.size(); ++i) {
    // Same fallback the render loop uses, so a live message (uid 0) resolves to
    // the same identity it was rendered with.
    int64_t id = st_.messages[i].uid != 0
                     ? st_.messages[i].uid
                     : (1000000000LL + static_cast<int64_t>(i));
    if (id == uid) return static_cast<int>(i);
  }
  return -1;
}

// A per-turn action from the message menu.
//
// "fork" branches the conversation at this turn (the new session keeps the turns
// up to and including it); "rollback" reverts the files this turn and every
// later turn touched and truncates the history there - destructive, so it asks
// first; "forget" only marks the turn as not-for-context, which is reversible
// informationally and needs no confirmation.
void UiApp::turn_action(size_t message_index, const std::string& action) {
  if (message_index >= st_.messages.size()) return;
  const std::string turn_id = st_.messages[message_index].turn_id;
  const std::string sid = st_.active_tab;
  if (sid.empty() || sid == "home") return;
  if (action == "edit") {
    // 编辑并重发 the last user message: put its text back in the composer and
    // ARM the turn. Nothing is trimmed here - the transcript changes only when
    // the edited text is actually sent, so clicking 编辑 and then changing your
    // mind leaves the conversation intact. The answer after it goes too (a
    // question without its answer is not a turn), but the FILE edits it made
    // stay: "let me rephrase" is not "undo my work".
    if (st_.turn_running || st_.suspended) {
      toast(tr("本轮还在进行，等它结束再编辑"));
      return;
    }
    const std::string text = st_.messages[message_index].text;
    update([&](AppState& s) {
      s.session_input = text;
      s.edit_turn_id = turn_id;
    });
    // The textarea's value only follows state when it is NOT focused (that rule is
    // what keeps the caret still while typing), so write it through directly.
    {
      VNodePtr ta = find_by_key(tree_, "session-input");
      if (ta && ta->dom.valid()) {
        dom_.set_attr(ta->dom, "value", text);
        dom_.focus(ta->dom);
      }
    }
    return;
  }
  if (turn_id.empty()) {
    toast(tr("这一轮没有可用的轮次 id"));
    return;
  }
  if (action == "fork") {
    fork_from_turn(sid, turn_id);
    return;
  }
  if (action == "forget") {
    forget_turn(sid, turn_id);
    return;
  }
  if (action == "rollback") {
    st_.confirm_kind = "rollback";
    st_.confirm_id = sid;
    st_.confirm_arg = turn_id;
    st_.confirm_text = tr("回滚到这一轮？此轮及之后的文件改动会被撤销，历史也会被截断。");
    render();
    return;
  }
}

void UiApp::fork_from_turn(const std::string& sid, const std::string& turn_id) {
  call_async("session.fork", Json{{"sid", sid}, {"turnId", turn_id}},
             [this, sid](const BackendResult& r) {
    if (!r.ok) {
      toast(tr("分叉失败：") + r.message);
      return;
    }
    std::string new_sid = jsonutil::get<std::string>(r.data, "sid", "");
    if (new_sid.empty()) {
      toast(tr("分叉失败：后端没有返回新会话"));
      return;
    }
    // Open the branch immediately: forking and then having to find it in the
    // project list would be a strange way to continue a train of thought.
    open_session(new_sid, tr("分叉会话"));
    for (size_t i = 0; i < st_.projects.size(); ++i) load_sessions(i);
  });
}

void UiApp::rollback_to_turn(const std::string& sid, const std::string& turn_id) {
  call_async("session.rollback", Json{{"sid", sid}, {"turnId", turn_id}},
             [this, sid](const BackendResult& r) {
    if (!r.ok) {
      toast(tr("回滚失败：") + r.message);
      return;
    }
    toast(tr("已回滚"));
    // The transcript on disk changed; reload it rather than patching locally.
    load_session(sid);
  });
}

void UiApp::forget_turn(const std::string& sid, const std::string& turn_id) {
  call_async("session.forget", Json{{"sid", sid}, {"scope", turn_id}},
             [this](const BackendResult& r) {
    if (!r.ok) {
      toast(tr("忘记失败：") + r.message);
      return;
    }
    toast(tr("已忘记这一轮（不再进入上下文）"));
  });
}

void UiApp::refresh_loop_state(const std::string& sid) {
  if (sid.empty()) return;
  // Ordered replies only: opening a session fires a refresh, and a turn that
  // ends while that request is in flight leaves a reply saying "running" -
  // applying it AFTER `loop.interrupted` put 中止 back on a stopped turn.
  int seq = ++loop_state_seq_;
  call_async("session.loopState", Json{{"sid", sid}},
             [this, sid, seq](const BackendResult& r) {
    if (!r.ok) return;
    if (seq != loop_state_seq_) return;  // a newer request supersedes this reply
    std::string state = jsonutil::get<std::string>(r.data, "state", "idle");
    bool suspended = state == "suspended";
    // A turn is "in flight" for anything except idle/turnDone - and NOT for
    // "suspended": a parked turn is not running, so the primary button has to be
    // 发送 again (sending it continues the parked task). Counting it as running
    // showed 中止 for a task that had already stopped, next to a 恢复 that did
    // nothing - "挂起功能疑似没做全".
    bool running = state != "idle" && state != "turnDone" && !suspended;
    if (st_.active_tab != sid) {
      SessionView& v = views_.get_or_create(sid);
      v.suspended = suspended;
      v.turn_running = running;
      return;
    }
    update([&](AppState& s) {
      s.suspended = suspended;
      s.turn_running = running;
    });
  });
}

// A session remembers the model it last used, but that model can disappear: the
// user disabled it, or the provider was deleted outright. Sending then fails
// deep in the provider layer with an error that reads like a network problem,
// and the model box keeps showing a model that no longer exists.
//
// So the composer asks which models are usable and, if the remembered one is not
// among them, CLEARS the model box (it must not silently keep a dead name) and
// disables send. Choosing a model again re-enables it.
void UiApp::refresh_model_availability() {
  std::string sid = active_sid();
  call_async("model.allEnabled", Json::object(),
             [this, sid](const BackendResult& r) {
    if (!r.ok) return;   // a failed check must not block sending
    std::vector<std::string> ids;
    // The composer needs the model's CONFIGURED thinking levels, not a guess from
    // its name. `thinking_levels_for()` is a hardcoded name whitelist (o1/claude/
    // gemini/deepseek-r...), so a custom or newly-released model produced an empty
    // list and the 思考强度 control never rendered - "没做思考强度按钮". The model
    // table carries what the user (or the API) actually configured.
    std::vector<std::string> levels;
    std::string level_default;
    std::string provider_of_model;
    if (r.data.contains("models") && r.data["models"].is_array()) {
      const Json& arr = r.data["models"];
      std::string cur_model = sid == st_.active_tab ? st_.model
                                                    : views_.get_or_create(sid).model;
      for (size_t i = 0; i < arr.size(); ++i) {
        std::string id = jsonutil::get<std::string>(arr[i], "id", "");
        if (!id.empty()) ids.push_back(id);
        if (id != cur_model) continue;
        provider_of_model =
            jsonutil::get<std::string>(arr[i], "providerName",
                                       jsonutil::get<std::string>(arr[i], "providerId", ""));
        if (arr[i].contains("thinking") && arr[i]["thinking"].is_object() &&
            arr[i]["thinking"].contains("levels") &&
            arr[i]["thinking"]["levels"].is_array()) {
          const Json& lv = arr[i]["thinking"]["levels"];
          for (size_t k = 0; k < lv.size(); ++k) {
            if (lv[k].is_string()) levels.push_back(lv[k].get<std::string>());
          }
          // The model's own default drives the pre-selection, so the composer
          // shows the level configured in the model settings instead of a
          // meaningless "默认" row.
          level_default =
              jsonutil::get<std::string>(arr[i]["thinking"], "default", "");
        }
      }
    }
    // The reply belongs to whatever session was active when it was asked. If the
    // user switched tabs meanwhile, only the background view is updated.
    bool same_tab = (sid == st_.active_tab) || sid.empty();
    bool gone = false;
    std::string cur = same_tab ? st_.model : views_.get_or_create(sid).model;
    if (!cur.empty()) {
      gone = true;
      for (size_t i = 0; i < ids.size(); ++i) {
        if (ids[i] == cur) { gone = false; break; }
      }
    }
    if (!same_tab) {
      SessionView& v = views_.get_or_create(sid);
      v.available_model_ids = ids;
      v.model_available = !gone;
      if (gone) v.model.clear();
      return;
    }
    update([&](AppState& s) {
      s.available_model_ids = ids;
      s.model_available = !gone;
      s.model_thinking_levels = levels;
      s.model_thinking_default = level_default;
      s.model_provider_label = provider_of_model;
      // Pre-select the model's configured default when this session has not
      // chosen a level yet. Setting it in STATE (not just the <select>) is what
      // makes the choice visible to send_message: the request carries the level
      // shown in the control.
      if (s.thinking.empty() && !level_default.empty()) {
        bool known = false;
        for (size_t i = 0; i < levels.size(); ++i) {
          if (levels[i] == level_default) known = true;
        }
        if (known) s.thinking = level_default;
      }
      if (gone) {
        // Clearing the name is the point: an empty box says "pick one", while a
        // dead name next to a disabled send button says nothing useful.
        s.model.clear();
        s.context_pct = -1;
        toast(tr("上次使用的模型已不可用，请重新选择"));
      }
    });
  });
}

void UiApp::refresh_server_status() {
  call_async("server.status", Json::object(), [this](const BackendResult& r) {
    if (!r.ok) return;
    // The rebind happens on a helper thread (see UiServer::restart), so a status
    // read right after the settings write can still see the OLD listener. Poll
    // again shortly instead of reporting a stale port as "current".
    bool pending = jsonutil::get<bool>(r.data, "rebinding", false);
    update([&](AppState& s) {
      s.server_running = jsonutil::get<bool>(r.data, "running", false);
      s.server_host = jsonutil::get<std::string>(r.data, "host", "");
      s.server_port = static_cast<int>(jsonutil::get_int(r.data, "port", 0));
      s.server_requires_password = jsonutil::get<bool>(r.data, "password", false);
      s.server_error = jsonutil::get<std::string>(r.data, "error", "");
      // A LAN URL is only meaningful when listening on all interfaces. Build it
      // from the machine's real address (server.lanIp, best-effort) so the user
      // can copy it straight into a phone.
      std::string lan_ip = jsonutil::get<std::string>(r.data, "lanIp", "");
      s.server_lan_url = (s.server_host == "0.0.0.0" && !lan_ip.empty())
                             ? ("http://" + lan_ip + ":" + itos(s.server_port) + "/")
                             : "";
    });
    // The rebind happens on a helper thread (see UiServer::restart), so a status
    // read right after the settings write can still see the OLD listener. Re-read
    // until "rebinding" clears; a single fixed delay silently reported the old
    // address as current whenever the swap took longer than the delay.
    if (pending && ++server_status_polls_ < 20) {
      dom_.schedule(250, [this]() { refresh_server_status(); });
    } else {
      server_status_polls_ = 0;
    }
  });
}

void UiApp::refresh_pending_wakes() {
  std::string sid = active_sid();
  if (sid.empty()) return;
  call_async("session.pendingWakes", Json{{"sid", sid}},
             [this, sid](const BackendResult& r) {
    if (!r.ok) return;
    std::vector<PendingWake> list;
    const Json& arr = r.data.contains("wakes") ? r.data["wakes"] : r.data;
    if (arr.is_array()) {
      for (size_t i = 0; i < arr.size(); ++i) {
        PendingWake w;
        w.source = jsonutil::get<std::string>(arr[i], "source", "");
        w.note = jsonutil::get<std::string>(arr[i], "ref", "");
        w.at_ms = jsonutil::get_int(arr[i], "at", 0);
        w.in_ms = jsonutil::get_int(arr[i], "inMs", 0);
        list.push_back(w);
      }
    }
    if (st_.active_tab != sid) return;
    update([&](AppState& s) { s.pending_wakes = list; });
  });
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
            row.kind = jsonutil::get<std::string>(s, "kind", "skill");
            row.enabled = jsonutil::get<bool>(s, "enabled", true);
            row.user = jsonutil::get<bool>(s, "user", false);
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
  // The Web-service page only exists in the Web build: that is the only variant
  // whose UI IS the HTTP server. Webview / WhaleUI open a native window and never
  // start one, so offering "listen address / port / password" there would be a
  // page of switches that cannot do anything.
  const bool has_web_server =
      std::string(dom_.backend_name()) == "wasm" || !backend_;
  for (size_t i = 0; i < kSettingsNavCount; ++i) {
    const SettingsItem& it = kSettingsNav[i];
    if (std::string(it.key) == "webserver" && !has_web_server) continue;
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
      // The provider's brand mark, from its ENDPOINT: a provider created from a
      // template has no template id any more, so the list used to show no logo at
      // all for anything the user added.
      {
        std::string brand = provider_brand_for_endpoint(p.endpoint, p.name);
        H mark = el("span", "pv-item-mark");
        if (!brand.empty()) mark.add(comp::model_icon(brand, p.name, "mi-lg"));
        else mark.text(p.name.empty() ? "?" : p.name.substr(0, 1));
        it.add(mark);
      }
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
      // No "模型" sub-title: other tables in the app have none, and this panel is
      // not a card any more. The two icon actions are the only controls needed.
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
      // The backend caches the language for skill/MCP descriptions. Writing the
      // setting alone left it at the old value until the next restart.
      call_async("thinking.setLang", Json{{"lang", l.empty() ? "auto" : l}});
    }), tr("影响工具/skill 说明的双语选择")));

    body.add(comp::section_title(tr("压缩")));
    // 自动压缩阈值：上下文水位（上一次请求的 prompt tokens / 模型窗口）达到该
    // 百分比就开始压缩较早的轮次。给档位而不是让用户填百分比，因为"多少算满"
    // 本来就是个经验值。键名与后端一致：history.compressAtPct。
    body.add(comp::field(tr("自动压缩阈值"), comp::select(
        std::vector<std::string>{"60%", "70%", "80%", "90%", tr("关闭")},
        [&]() {
          int n = sv_int(st_.settings_values, "history.compressAtPct", 80);
          return n <= 0 ? 4 : n <= 60 ? 0 : n <= 70 ? 1 : n <= 80 ? 2 : 3;
        }(),
        [this](const std::string& v) {
      int idx = std::atoi(v.c_str());
      const int vals[5] = {60, 70, 80, 90, 0};
      set_setting("history.compressAtPct", vals[idx < 0 || idx > 4 ? 2 : idx]);
    }), tr("上下文占用达到该比例时自动压缩较早的轮次")));
    // This page used to carry three more knobs. Only the threshold above is a
    // real control:
    //   * "保留最近轮数" - the number of full turns kept when compaction runs is an
    //     internal detail (a constant now); exposing it asked the user to tune
    //     something they cannot observe.
    //   * "立即压缩当前会话" - it only set a flag consumed by the NEXT turn, so
    //     pressing it changed nothing visible and read as broken.
    //   * "最近轮次" window / "token 硬上限" - removed earlier: both compacted on
    //     every turn regardless of the water level.
    // The water-level threshold is the one automatic trigger and the only knob.

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
        // mcp_import reads a directory of MCP packages; it is the bulk form of
        // add_mcp and does not need the per-entry dialog.
        call_async("mcp.import", Json{{"dir", path}},
                   [this](const BackendResult& r2) {
          if (!r2.ok) {
            toast(tr("导入失败：") + r2.message);
            return;
          }
          int n = static_cast<int>(jsonutil::get_int(r2.data, "added", 0));
          // mcp_import reports the whole registry rather than a count; say what
          // happened without inventing a number the backend did not send.
          if (n > 0) toast(tr("已导入 ") + itos(n) + tr(" 个 MCP"));
          else toast(tr("已导入"));
          load_mcp_tools();
        });
      });
    }, "ghost"));
    body.add(head);
    body.add(comp::search_box(st_.mcp_filter, tr("搜索 MCP"),
                              [this](const std::string& v) {
      update([&](AppState& s) { s.mcp_filter = v; });
    }));
    size_t shown = 0;
    H list = el("div", "mcp-list");
    // Built-in MCP entries (browser-control, lsp) live in the same registry as
    // skills because the load flow is shared, but they are MCP: the model calls
    // them, the user cannot type /lsp. Listing them under Skill was wrong.
    for (size_t i = 0; i < st_.skills.size(); ++i) {
      const AppState::SkillRow& sk = st_.skills[i];
      if (sk.kind != "mcp") continue;
      std::string label = sk.title.empty() ? sk.name : sk.title;
      if (!st_.mcp_filter.empty() &&
          label.find(st_.mcp_filter) == std::string::npos &&
          sk.name.find(st_.mcp_filter) == std::string::npos) {
        continue;
      }
      ++shown;
      H card = el("div", "mcp-card");
      H top = el("div", "mcp-card-top");
      top.add(el("span", "mcp-card-name").text(label));
      top.add(comp::badge(tr("内置")));
      top.add(el("span", "grow"));
      std::string name = sk.name;
      top.add(comp::icon_button("eye", tr("查看内容"), [this, name](const std::string&) {
        call_async("skill.load", Json{{"name", name}},
                   [this, name](const BackendResult& r) {
          toast(r.ok ? (tr("已加载 ") + name) : (tr("加载失败：") + r.message));
        });
      }));
      card.add(top);
      if (!sk.description.empty()) {
        card.add(el("div", "mcp-card-desc").text(sk.description));
      }
      list.add(card);
    }
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
      // A server entry shows its command/url - that is what identifies it. A
      // bare directory is the older "folder import" shape and says so instead of
      // pretending to be a server definition.
      if (!m.server_command.empty() || !m.server_url.empty()) {
        std::string line = !m.server_command.empty() ? m.server_command : m.server_url;
        for (size_t k = 0; k < m.server_args.size(); ++k) line += " " + m.server_args[k];
        card.add(el("div", "mcp-card-path").text(line));
      } else if (!m.dir.empty()) {
        card.add(el("div", "mcp-card-path").text(m.dir));
      }
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
    if (st_.mcp_tools.empty() && shown == 0) {
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
    head.add(comp::button(tr("新建"), [this](const std::string&) {
      // A user skill is just a name + title + body; the body is what the model
      // receives when it loads it. Creating one here avoids hand-editing a
      // markdown file for what is often a two-line instruction.
      update([&](AppState& s) {
        s.skill_new_name.clear();
        s.skill_new_title.clear();
        s.skill_new_body.clear();
        s.skill_creating = true;
      });
    }, "ghost"));
    head.add(comp::button(tr("自动探测"), [this](const std::string&) { detect_skills(); },
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
    if (st_.skill_creating) {
      H form = el("div", "mcp-card");
      form.add(comp::field(tr("名称"), comp::text_input(
          st_.skill_new_name, "my-skill", [this](const std::string& v) {
        st_.skill_new_name = v;  // no re-render while typing
      }), tr("用小写字母和连字符；调用时写 /名称")));
      form.add(comp::field(tr("标题"), comp::text_input(
          st_.skill_new_title, tr("显示名称"), [this](const std::string& v) {
        st_.skill_new_title = v;
      })));
      form.add(comp::field(tr("内容"), comp::textarea(
          st_.skill_new_body, tr("这个 skill 给模型的说明…"),
          [this](const std::string& v) { st_.skill_new_body = v; },
          nullptr)));
      H acts = el("div", "settings-actions");
      acts.add(comp::button(tr("创建"), [this](const std::string&) {
        if (st_.skill_new_name.empty()) {
          toast(tr("名称不能为空"));
          return;
        }
        call_async("skill.create",
                   Json{{"name", st_.skill_new_name},
                        {"title", st_.skill_new_title},
                        {"description", st_.skill_new_body.substr(0, 120)},
                        {"body", st_.skill_new_body}},
                   [this](const BackendResult& r) {
          if (!r.ok) {
            toast(tr("创建失败：") + r.message);
            return;
          }
          update([&](AppState& s) { s.skill_creating = false; });
          load_skills();
        });
      }, "primary"));
      acts.add(comp::button(tr("取消"), [this](const std::string&) {
        update([&](AppState& s) { s.skill_creating = false; });
      }, "ghost"));
      form.add(acts);
      body.add(form);
    }
    body.add(comp::search_box(st_.skill_filter, tr("搜索 Skill"),
                              [this](const std::string& v) {
      update([&](AppState& s) { s.skill_filter = v; });
    }));
    size_t shown = 0;
    H list = el("div", "mcp-list");
    for (size_t i = 0; i < st_.skills.size(); ++i) {
      const AppState::SkillRow& sk = st_.skills[i];
      // Built-in MCP entries are listed on the MCP page, not here: this page is
      // "commands you can type", and /lsp is not one of them.
      if (sk.kind == "mcp") continue;
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
      // Enable/disable: a disabled skill stays visible here (you can turn it back
      // on) but stops being offered to the model and in the / command menu.
      {
        bool enabled = sk.enabled;
        top.add(comp::toggle(enabled, "", [this, name, enabled](const std::string&) {
          call_async("skill.setEnabled", Json{{"name", name}, {"enabled", !enabled}},
                     [this](const BackendResult& r) {
            if (!r.ok) {
              toast(tr("操作失败：") + r.message);
              return;
            }
            load_skills();
          });
        }));
      }
      if (sk.user) {
        top.add(comp::icon_button("trash", tr("删除"), [this, name](const std::string&) {
          call_async("skill.remove", Json{{"name", name}},
                     [this](const BackendResult& r) {
            if (!r.ok) {
              toast(tr("删除失败：") + r.message);
              return;
            }
            toast(tr("已删除"));
            load_skills();
          });
        }));
      }
      card.add(top);
      card.add(el("div", "mcp-card-path").text("/" + sk.name +
                                               (sk.dir.empty() ? "" : ("  ·  " + sk.dir))));
      if (!sk.description.empty()) {
        card.add(el("div", "mcp-card-desc").text(sk.description));
      }
      list.add(card);
    }
    if (shown == 0) {
      body.add(comp::empty_state(
          st_.skills.empty()
              ? tr("暂无 Skill。点「自动探测」扫描常见位置，或用「手动导入」选择目录。")
              : tr("没有匹配的 Skill。"),
          "", nullptr));
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
    // Enter behaviour. It used to be removed because it had no visible effect -
    // the composer always sent on Enter, so the setting was a lie. It is back
    // now that it IS wired: the textarea carries data-enter-send, the DOM
    // adapter's key handler reads it, and the value is pushed to the session
    // (session.setEnterSend) so a reload keeps it.
    body.add(comp::section_title(tr("Enter 行为")));
    {
      std::vector<comp::ChoiceOption> enter;
      comp::ChoiceOption o;
      o.value = "enter";
      o.icon = "arrow-right-to-bracket";
      o.title = tr("Enter 发送");
      o.desc = tr("Shift+Enter 换行");
      enter.push_back(o);
      o.value = "ctrlEnter";
      o.icon = "keyboard";
      o.title = tr("Ctrl+Enter 发送");
      o.desc = tr("Enter 直接换行，适合多行输入");
      enter.push_back(o);
      std::string cur = sv_str(st_.settings_values, "session.default.enterSend", "enter");
      body.add(comp::choice_list(enter, cur == "ctrlEnter" ? "ctrlEnter" : "enter",
                                 [this](const std::string& v) {
        set_setting("session.default.enterSend", v);
        std::string sid = active_sid();
        update([&](AppState& s) { s.enter_send = v; });
        if (!sid.empty()) {
          call_async("session.setEnterSend", Json{{"sid", sid}, {"enterSend", v}});
        }
      }));
    }
  } else if (sec == "webserver") {
    body.add(comp::section_title(tr("Web 服务")));
    body.add(el("div", "field-hint").text(tr(
        "Web 版用这个服务提供界面；Webview / WhaleUI 版有原生窗口，不启动服务。"
        "改动立即生效：端口/地址变化会重新监听（当前页面会断开，请用新地址重开）。"
        "本机访问始终免密，密码只约束远程来源。")));
    // 监听地址：两个明确的选项，而不是让人填 IP。绝大多数人只需要"仅本机"和
    // "局域网/远程"两种；填错 IP 的结果是服务器起不来，而错误信息在远程端看不见。
    {
      std::vector<comp::ChoiceOption> hosts;
      comp::ChoiceOption o;
      o.value = "127.0.0.1";
      o.icon = "desktop";
      o.title = tr("仅本机");
      o.desc = tr("只有这台机器能访问（默认）");
      hosts.push_back(o);
      o.value = "0.0.0.0";
      o.icon = "globe";
      o.title = tr("局域网 / 远程");
      o.desc = tr("监听所有网卡，其他设备可以连进来");
      hosts.push_back(o);
      std::string cur = sv_str(st_.settings_values, "server.host", "127.0.0.1");
      if (cur.empty()) cur = "127.0.0.1";
      body.add(comp::field(tr("监听地址"), comp::choice_list(
          hosts, cur, [this](const std::string& v) {
        set_setting("server.host", v);
        refresh_server_status();
      })));
    }
    // Port and password are typed, so they commit on ENTER (or when focus
    // leaves the field) - never on every keystroke. Committing per character
    // rebinds the listener once per digit ("1", "18", "189"...), each rebind
    // closes the connection that is still carrying the next keystroke, and the
    // last one can land on a port nobody asked for. That is exactly why editing
    // the port appeared to do nothing.
    //
    // The box shows the draft while there is one: the saved value does not change
    // until commit, and re-rendering from it would erase what is being typed.
    std::string port_text = !st_.server_port_draft.empty()
                                ? st_.server_port_draft
                                : std::to_string(sv_int(st_.settings_values, "server.port", 0));
    // Commit the port. Shared by Enter and blur so clicking away applies too.
    // A value that is not a whole number in [0, 65535] is REJECTED and said so -
    // the old code clamped silently, which is where the mysterious 65535 in the
    // saved config came from: a non-numeric string parsed to 0 by atoi, but the
    // "too big" clamp turned a typo into a real (and unusable) port.
    auto commit_port = [this](const std::string& v) {
      std::string s = v;
      // trim ASCII whitespace
      size_t b = s.find_first_not_of(" \t\r\n");
      size_t e = s.find_last_not_of(" \t\r\n");
      s = (b == std::string::npos) ? std::string() : s.substr(b, e - b + 1);
      bool digits = !s.empty();
      for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9') { digits = false; break; }
      }
      int p = 0;
      if (!s.empty()) {
        if (!digits) {
          st_.server_port_draft.clear();
          set_setting("server.port", sv_int(st_.settings_values, "server.port", 0));
          toast(tr("端口要是 0-65535 的数字"));
          render();
          return;
        }
        // strtol so "99999" is caught as out of range instead of wrapping.
        long n = std::strtol(s.c_str(), nullptr, 10);
        if (n < 0 || n > 65535) {
          st_.server_port_draft.clear();
          set_setting("server.port", sv_int(st_.settings_values, "server.port", 0));
          toast(tr("端口要在 0-65535 之间"));
          render();
          return;
        }
        p = static_cast<int>(n);
      }
      st_.server_port_draft.clear();   // the saved value is the source of truth now
      set_setting("server.port", p);
      toast(p > 0 ? (tr("端口已改为 ") + itos(p))
                  : tr("已设为自动（下次启动生效）"));
      refresh_server_status();
    };
    body.add(comp::field(tr("端口"), comp::text_input(
        port_text, "0",
        [this](const std::string& v) {
      // Local echo only: keeps what was typed visible without touching the
      // server. The commit happens on Enter or blur.
      st_.server_port_draft = v;
    },
        commit_port,
        commit_port), tr("0 = 启动时自动选端口；回车或点到别处生效")));
    std::string pw_text = !st_.server_password_draft.empty()
                              ? st_.server_password_draft
                              : sv_str(st_.settings_values, "server.password", "");
    auto commit_pw = [this](const std::string& v) {
      if (v == sv_str(st_.settings_values, "server.password", "")) return;
      st_.server_password_draft.clear();
      set_setting("server.password", v);
      toast(v.empty() ? tr("已关闭访问密码") : tr("访问密码已更新"));
      refresh_server_status();
    };
    body.add(comp::field(tr("访问密码"), comp::text_input(
        pw_text,
        tr("留空 = 不设密码"),
        [this](const std::string& v) {
      st_.server_password_draft = v;
    },
        commit_pw,
        commit_pw), tr("远程连接时需要；本机始终免密；回车或点到别处生效")));
    // 当前生效的地址：用户需要知道该在另一台机器上输入什么。
    if (st_.server_running) {
      H st_row = el("div", "settings-note");
      std::string line = tr("正在监听 ") + st_.server_host + ":" +
                         itos(st_.server_port);
      if (st_.server_requires_password) line += tr("（远程需密码）");
      st_row.text(line);
      body.add(st_row);
      if (st_.server_host == "0.0.0.0") {
        // Show a real, copyable address. The literal "<本机 IP>" placeholder that
        // used to be here could not be typed on a phone.
        std::string lan = !st_.server_lan_url.empty()
                              ? st_.server_lan_url
                              : (tr("http://<本机 IP>:") + itos(st_.server_port) + "/");
        body.add(el("div", "settings-note dim").text(tr("局域网地址：") + lan));
      }
    } else {
      body.add(el("div", "settings-note dim").text(tr("当前没有运行 Web 服务。")));
    }
    // A failed rebind must not look like "the setting did nothing": the address
    // may be taken or the port reserved, and the only place that says so is here.
    if (!st_.server_error.empty()) {
      body.add(el("div", "settings-note err").text(
          tr("重新监听失败：") + st_.server_error + tr("（已保留原监听）")));
    }
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
    // 代理：只有主机 + 端口，留空/0 就是直连。
    //
    // There used to be a three-way choice above these fields (跟随系统 / 自定义 /
    // 关闭), but nothing ever read it: the network layer applies host+port
    // unconditionally, so "关闭" still proxied and "跟随系统" did not follow
    // anything. A control that lies is worse than no control - the two fields are
    // the whole truth, so show them and nothing else.
    body.add(comp::section_title(tr("代理")));
    {
      body.add(comp::field(tr("代理主机"), comp::text_input(
          sv_str(st_.settings_values, "permissions.network.proxyHost", ""), "127.0.0.1",
          [this](const std::string& v) {
        set_setting("permissions.network.proxyHost", v);
      }), "如 127.0.0.1（留空 = 直连）"));
      body.add(comp::field(tr("代理端口"), comp::text_input(
          std::to_string(sv_int(st_.settings_values, "permissions.network.proxyPort", 0)), "",
          [this](const std::string& v) {
        int p = std::atoi(v.c_str());
        set_setting("permissions.network.proxyPort", p > 0 ? p : 0);
      }), tr("如 7890")));
    }

    // ---- 搜索（search_web 工具的后端）----
    // 搜索没有通用协议：每家 API 的请求体与响应都不一样，所以这里只让用户选
    // 一家并填 key/endpoint。没配置时工具会返回"去设置里配"，而不是静默失败。
    body.add(comp::section_title(tr("搜索")));
    {
      std::vector<comp::ChoiceOption> sp;
      comp::ChoiceOption o;
      o.value = "";
      o.icon = "ban";
      o.title = tr("关闭");
      o.desc = tr("search_web 返回\"未配置\"");
      sp.push_back(o);
      o.value = "tavily";
      o.icon = "magnifying-glass";
      o.title = "Tavily";
      o.desc = tr("需要 API key");
      sp.push_back(o);
      o.value = "brave";
      o.icon = "magnifying-glass";
      o.title = "Brave Search";
      o.desc = tr("需要 API key");
      sp.push_back(o);
      o.value = "searxng";
      o.icon = "server";
      o.title = "SearXNG";
      o.desc = tr("自建实例，需要 endpoint");
      sp.push_back(o);
      o.value = "custom";
      o.icon = "plug";
      o.title = tr("自定义");
      o.desc = tr("任意接受 {query,count} 的接口");
      sp.push_back(o);
      std::string provider = sv_str(st_.settings_values, "search.provider", "");
      body.add(comp::choice_list(sp, provider, [this](const std::string& v) {
        set_setting("search.provider", v);
      }));
      if (!provider.empty()) {
        if (provider == "searxng" || provider == "custom") {
          body.add(comp::field(tr("接口地址"), comp::text_input(
              sv_str(st_.settings_values, "search.endpoint", ""),
              provider == "searxng" ? "https://searx.example.org/search"
                                    : "https://example.com/search",
              [this](const std::string& v) { set_setting("search.endpoint", v); }),
              tr("完整 URL（必填）")));
        }
        if (provider != "searxng" && provider != "custom") {
          body.add(comp::field("API key", comp::text_input(
              sv_str(st_.settings_values, "search.apiKey", ""), "…",
              [this](const std::string& v) { set_setting("search.apiKey", v); }),
              tr("保存在本地配置文件")));
        }
        body.add(comp::field(tr("返回条数"), comp::text_input(
            std::to_string(sv_int(st_.settings_values, "search.maxResults", 5)), "5",
            [this](const std::string& v) {
          int n = std::atoi(v.c_str());
          if (n < 1) n = 1;
          if (n > 20) n = 20;
          set_setting("search.maxResults", n);
        }), "1-20"));
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
    // "Reset to defaults" is the counterpart of every setting on this page: a
    // value you cannot unset is a value you are stuck with (and the project-level
    // ignore flag makes the global value invisible, not gone).
    fsave.add(comp::button(tr("恢复默认"), [this](const std::string&) {
      const char* keys[] = {"permissions.files.blacklist",
                            "permissions.files.whitelist",
                            "permissions.files.autoGitInit"};
      for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        call_async("settings.unset", Json{{"key", keys[i]}});
      }
      update([&](AppState& s) {
        s.file_blacklist_text.clear();
        s.file_whitelist_text.clear();
      });
      toast(tr("已恢复默认"));
    }, "ghost"));
    body.add(fsave);
    if (!st_.active_pid.empty()) {
      bool ignore = sv_bool(st_.settings_values, "settings.ignoreGlobal", false);
      body.add(comp::field(tr("忽略全局设置"), comp::toggle(ignore, "",
          [this](const std::string&) {
        bool cur = sv_bool(st_.settings_values, "settings.ignoreGlobal", false);
        std::string pid = st_.active_pid;
        update([&](AppState& s) {
          s.settings_values["settings.ignoreGlobal"] = Json{{"value", !cur}};
        });
        call_async("settings.setIgnoreGlobal", Json{{"pid", pid}, {"ignore", !cur}},
                   [this](const BackendResult& r) {
          toast(r.ok ? tr("已更新") : (tr("更新失败：") + r.message));
        });
      }), tr("本项目只使用项目级设置，忽略全局设置")));
    }
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
    // These bindings are live now (see the global-key handler in render()):
    // 关闭标签页 / 新建标签页 are matched against the document keydown, and
    // 发送消息 / 换行 drive the composer's Enter behaviour. They used to be four
    // text fields nothing read back.
    body.add(comp::hint(tr(
        "关闭/新建标签页在桌面端与浏览器端均生效；发送/换行由输入框的 Enter 行为决定")));
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
  } else if (sec == "computer") {
    // 截图 only. There is no "computer control" (no mouse/keyboard tools yet), and
    // the second toggle here ("允许截图") wrote a key nothing ever read - two
    // switches for one feature, one of them inert. The one that IS read
    // (`computer.enabled`) gates the screenshot tool, so that is the only row.
    body.add(comp::section_title(tr("截图")));
    body.add(comp::field(tr("允许截图"), comp::toggle(
        sv_bool(st_.settings_values, "computer.enabled", true), "",
        [this](const std::string&) {
      bool cur = sv_bool(st_.settings_values, "computer.enabled", true);
      set_setting("computer.enabled", !cur);
    }), tr("关闭后模型的 screenshot 工具会被拒绝")));
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
  // The ASK panel is NOT an overlay: it replaces the composer area (see
  // build_composer). A floating dialog covered the conversation it was asking
  // about, and its options read as plain text - "分辨不出哪些是选项".
  if (st_.ask_modal && st_.asks.empty()) {
    // ---- permission gate: allow / deny ONE tool call ----
    std::vector<H> body;
    body.push_back(el("div").text(st_.ask_text.empty() ? "模型请求确认。" : st_.ask_text));
    layer.add(comp::modal("需要确认", body, "允许",
                          [this](const std::string&) {
    std::string sid = active_sid();
    update([&](AppState& s) { s.ask_modal = false; });
    if (sid.empty()) return;
    call_async("session.approve", Json{{"sid", sid}, {"yes", true}});
  },
                          [this](const std::string&) {
    std::string sid = active_sid();
    update([&](AppState& s) { s.ask_modal = false; });
    if (sid.empty()) return;
    call_async("session.approve", Json{{"sid", sid}, {"yes", false}});
  }));
  }
  // Generic confirm modal (destructive actions). Its own branch so a session
  // delete and a permission prompt can never be confused for each other.
  //
  // The OK label and the action both come from `confirm_kind`: rollback needs to
  // read as "回滚" rather than "删除", and running the WRONG action because the
  // label was generic is exactly the kind of thing a confirmation is supposed to
  // prevent. `confirm_arg` carries the second value (a turnId, a permission
  // level) since several of these need more than an id.
  if (!st_.confirm_kind.empty()) {
    std::string kind = st_.confirm_kind;
    std::string id = st_.confirm_id;
    std::string arg = st_.confirm_arg;
    std::string ok_label = tr("删除");
    if (kind == "rollback") ok_label = tr("回滚");
    else if (kind == "discard-turn") ok_label = tr("撤销");
    else if (kind == "forget") ok_label = tr("忘记");
    std::vector<H> body;
    body.push_back(el("div").text(st_.confirm_text));
    layer.add(comp::modal(tr("请确认"), body, ok_label,
                          [this, kind, id, arg](const std::string&) {
      update([&](AppState& s) {
        s.confirm_kind.clear();
        s.confirm_id.clear();
        s.confirm_arg.clear();
        s.confirm_text.clear();
      });
      if (kind == "session") delete_session(id);
      else if (kind == "rollback") rollback_to_turn(id, arg);
      else if (kind == "forget") forget_turn(id, arg);
    },
                          [this](const std::string&) {
      update([&](AppState& s) {
        s.confirm_kind.clear();
        s.confirm_id.clear();
        s.confirm_arg.clear();
        s.confirm_text.clear();
      });
    }));
  }
  if (!st_.toast.empty()) {
    H t = el("div", "toast");
    t.text(st_.toast);
    layer.add(t);
  }
  // ---- right-click menu ----
  // Positioned at the pointer, unlike the other popups (which anchor to a
  // control): a context menu belongs to the click, not to an element.
  if (st_.ctx_open) {
    H menu = el("div", "ctx-menu");
    menu.key("ctx-menu");
    // Inline placement: the framework has no "absolute at (x,y)" primitive and
    // the menu lives in the overlay layer, which is already fixed-positioned.
    menu.attr("style", "left:" + std::to_string(st_.ctx_x) + "px;top:" +
                           std::to_string(st_.ctx_y) + "px");
    auto item = [&](const std::string& label, const std::string& icon,
                    std::function<void()> fn, bool danger = false) {
      H it = el("div", danger ? "ctx-item danger" : "ctx-item");
      if (!icon.empty()) it.add(el("i", "fa fa-" + icon));
      it.add(el("span").text(label));
      it.on(EventKind::kClick, [this, fn](const std::string&) {
        update([&](AppState& s) { s.ctx_open = false; });
        fn();
      });
      menu.add(it);
    };
    auto sep = [&]() { menu.add(el("div", "ctx-sep")); };

    if (st_.ctx_kind == "message") {
      std::string text = st_.ctx_label;   // the message body, captured on click
      std::string turn_id = st_.ctx_id;
      item(tr("复制"), "copy", [this, text]() {
        dom_.copy_text(text);
        toast(tr("已复制"));
      });
      sep();
      // These three only make sense for a stored assistant turn, and they all
      // address the turn by its stable id (never by position: paging shifts it).
      item(tr("从这里分叉"), "code-branch", [this, turn_id]() {
        std::string sid = active_sid();
        if (sid.empty() || turn_id.empty()) return;
        call_async("session.fork", Json{{"sid", sid}, {"turnId", turn_id}},
                   [this](const BackendResult& r) {
          toast(r.ok ? tr("已分叉") : (tr("分叉失败：") + r.message));
          if (r.ok) load_projects();
        });
      });
      item(tr("回滚到这里"), "rotate-left", [this, turn_id]() {
        std::string sid = active_sid();
        if (sid.empty() || turn_id.empty()) return;
        update([&](AppState& s) {
          s.confirm_kind = "rollback";
          s.confirm_id = sid;
          s.confirm_arg = turn_id;
          s.confirm_text = tr("回滚会撤销这之后的所有轮次（文件改动也会一并还原）。确定？");
        });
      });
      item(tr("忘记这一轮"), "eraser", [this, turn_id]() {
        std::string sid = active_sid();
        if (sid.empty() || turn_id.empty()) return;
        update([&](AppState& s) {
          s.confirm_kind = "forget";
          s.confirm_id = sid;
          s.confirm_arg = turn_id;
          s.confirm_text = tr("忘记后这一轮不再进入上下文（文件不动）。确定？");
        });
      }, true);
    } else if (st_.ctx_kind == "tab") {
      std::string tab = st_.ctx_id;
      item(tr("重命名"), "pen", [this, tab]() { begin_rename_session(tab); });
      item(tr("关闭"), "xmark", [this, tab]() { close_tab(tab); });
    } else if (st_.ctx_kind == "session") {
      std::string sid = st_.ctx_id;
      item(tr("打开"), "arrow-up-right-from-square", [this, sid]() {
        open_session(sid);
      });
      item(tr("重命名"), "pen", [this, sid]() { begin_rename_session(sid); });
      // AI rename: ask the model for a title from the transcript. A separate
      // entry because it is a different action from typing one - and because a
      // bad generated title must be easy to overwrite by hand.
      item(tr("AI 重命名"), "wand-magic-sparkles", [this, sid]() {
        call_async("session.aiTitle", Json{{"sid", sid}},
                   [this](const BackendResult& r) {
          if (!r.ok) {
            toast(tr("生成标题失败：") + r.message);
            return;
          }
          std::string title = jsonutil::get<std::string>(r.data, "title", "");
          if (title.empty()) {
            toast(tr("模型没有给出标题"));
            return;
          }
          toast(tr("已重命名为：") + title);
          load_projects();
          // The tab title comes from the session list, so reloading the projects
          // is enough; the open tab picks the new name up on the next render.
        });
      });
      sep();
      item(tr("删除会话"), "trash", [this, sid]() {
        update([&](AppState& s) {
          s.confirm_kind = "session";
          s.confirm_id = sid;
          s.confirm_text = tr("删除后会话记录不可恢复。确定？");
        });
      }, true);
    } else if (st_.ctx_kind == "composer") {
      // The textarea gets the app's own menu too (the browser's is suppressed
      // globally, so there is no native fallback to fall back to).
      item(tr("剪切"), "scissors", [this]() { dom_.exec_edit("cut"); });
      item(tr("复制"), "copy", [this]() { dom_.exec_edit("copy"); });
      item(tr("粘贴"), "paste", [this]() { dom_.exec_edit("paste"); });
      item(tr("全选"), "arrow-up-from-bracket", [this]() { dom_.exec_edit("selectAll"); });
      sep();
      item(tr("清空输入"), "eraser", [this]() {
        update([&](AppState& s) { s.session_input.clear(); });
      }, true);
    }
    layer.add(menu);
    // No full-screen backdrop: it would swallow the right-click that is supposed
    // to open the NEXT menu. `bind_outside_click` closes the menu on any press
    // outside it while letting the event continue to whatever was clicked, so
    // right-clicking another row closes this menu and opens that row's menu.
    // (The binding itself is made in render(), which looks the node up by key:
    // caching the Node here went stale as soon as the menu was rebuilt.)
  }
  return layer.get();
}

// ===========================================================================
// Actions
// ===========================================================================
void UiApp::send_message() {
  // A message may be attachments only (dropping a file and hitting send).
  if (st_.session_input.empty() && st_.attachments.empty()) return;
  // Same guard as the disabled button: sending without a usable model can only
  // fail in the provider layer, where the error no longer mentions the model.
  // Skipped when there is no backend at all (the local demo path) - that mode has
  // no model to resolve and is used by the UI tests and the standalone preview.
  if (backend_ && (st_.model.empty() || !st_.model_available)) {
    toast(st_.model.empty() ? tr("请先选择模型") : tr("该模型已不可用，请重新选择"));
    return;
  }
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
  Json attachments = Json::array();
  for (size_t i = 0; i < st_.attachments.size(); ++i) attachments.push_back(st_.attachments[i]);
  // 编辑并重发: the send REPLACES the armed turn, so the backend history is cut
  // first and only then is the new text sent. Doing it in this order means a
  // failed trim cannot leave a duplicate question behind.
  const std::string edit_tid = st_.edit_turn_id;
  if (!edit_tid.empty() && backend_ && !sid.empty()) {
    update([&](AppState& s) { s.edit_turn_id.clear(); });
    call_async("session.trimHistory", Json{{"sid", sid}, {"turnId", edit_tid}},
               [this, sid, edit_tid, text, shown, attachments](const BackendResult& r) {
      if (!r.ok) {
        toast(tr("无法替换这一轮：") + r.message);
        return;
      }
      // Mirror the trim locally so the old question and its answer disappear now
      // rather than after the reload that would otherwise be needed.
      int from = -1;
      for (size_t i = 0; i < st_.messages.size(); ++i) {
        if (st_.messages[i].turn_id == edit_tid) {
          from = static_cast<int>(i);
          break;
        }
      }
      if (from >= 0) {
        update([&](AppState& s) {
          int dropped = static_cast<int>(s.messages.size()) - from;
          s.messages.resize(static_cast<size_t>(from));
          if (dropped > 0) s.history_total -= dropped;
        });
      }
      dispatch_message(sid, text, shown, attachments);
    });
    return;
  }
  dispatch_message(sid, text, shown, attachments);
}

void UiApp::dispatch_message(const std::string& sid, const std::string& text,
                             const std::string& shown, const Json& attachments) {
  // echo the user's message immediately (optimistic; the assistant text streams in)
  // A message sent now is newer than everything loaded from disk. Its uid is
  // derived from the absolute history position so it stays distinct from the
  // loaded turns (which use indices 0..history_total-1).
  int64_t next_live_uid = st_.history_total + static_cast<int64_t>(st_.messages.size());
  update([&](AppState& s) {
    comp::ChatMsg m;
    m.role = "user";
    m.text = shown;
    m.time = "刚刚";
    m.rev = next_msg_rev_++;
    m.uid = next_live_uid;
    s.messages.push_back(m);
    s.history_total += 1;  // one more turn now exists on disk
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
      a.rev = next_msg_rev_++;
      s.messages.push_back(a);
    });
    return;
  }
  // 发送时把当前模型一起带上：会话页上换过的模型必须真的用于这次请求，而不是
  // 只改了按钮上的字。
  Json args = Json{{"sid", sid}, {"text", text}, {"attachments", attachments}};
  if (!st_.model.empty()) args["model"] = st_.model;
  if (!st_.thinking.empty()) args["thinking"] = st_.thinking;
  // SHOW THE WAIT IMMEDIATELY. The first token can take seconds (a reasoning
  // model, a cold endpoint); until then the assistant message did not exist, so
  // the screen looked exactly like a dead app. The message's own live timer
  // (spinner + counting seconds) is what shows the wait - no extra sentence.
  update([&](AppState& s) {
    // Sending from the composer means the user is at the END of the transcript;
    // pin the view there so the question and the answer are visible without a
    // manual scroll.
    s.hist_follow = true;
    if (!s.messages.empty() && s.messages.back().role == "assistant" &&
        s.messages.back().streaming) {
      return;
    }
    comp::ChatMsg a;
    a.role = "assistant";
    a.streaming = true;
    a.work_open = true;
    a.rev = next_msg_rev_++;
    a.started_at = now_ms();
    a.uid = s.history_total + static_cast<int64_t>(s.messages.size());
    s.messages.push_back(a);
  });
  follow_history_if_pinned();
  call_async("session.send", args,
             [this](const BackendResult& r) {
    if (!r.ok) toast(tr("发送失败：") + r.message);
  });
}

std::string UiApp::rel_time_for_test(int64_t ms) { return rel_time(ms); }

// Fingerprint of everything that changes a message's rendered output. Sizes and
// flags only - no string comparison, so this is a handful of adds per message.
// A length change catches streamed text, tool results and thinking; the flags
// catch every fold state.
uint64_t UiApp::message_sig(const comp::ChatMsg& m) {
  uint64_t h = 1469598103934665603ULL;
  auto mix = [&h](uint64_t v) {
    h ^= v;
    h *= 1099511628211ULL;
  };
  mix(m.text.size());
  mix(m.thinking.size());
  mix(m.tools.size());
  mix(m.summary.size());
  mix(m.time.size());
  mix(m.interim.size());
  mix(static_cast<uint64_t>(m.work_open));
  mix(static_cast<uint64_t>(m.work_touched));
  mix(static_cast<uint64_t>(m.thinking_open));
  mix(static_cast<uint64_t>(m.streaming));
  for (size_t i = 0; i < m.tools.size(); ++i) {
    mix(m.tools[i].name.size());
    mix(m.tools[i].summary.size());
    mix(m.tools[i].detail.size());
    mix(static_cast<uint64_t>(m.tools[i].expanded));
    mix(static_cast<uint64_t>(m.tools[i].ok));
  }
  // The interim texts are part of the rendered work log: without mixing their
  // length in, a new narration line could land under an unchanged signature and
  // the cached node would be reused, silently hiding it.
  for (size_t i = 0; i < m.interim.size(); ++i) mix(m.interim[i].size());
  // ...and so is WHERE each note sits relative to the calls (see interim_after).
  for (size_t i = 0; i < m.interim_after.size(); ++i) mix(m.interim_after[i] + 1);
  return h;
}

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
  st_.thinking_menu_open = false;
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
    // A brand-new session has no terminals; the drawer loads its list on open.
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
    // The composer's thinking control is built from the model TABLE, so a level
    // ladder edited here has to be re-read immediately. Without this the input row
    // kept showing the old options (or the fallback pair) until the session was
    // reopened - "我设置了模型，输入框那也没变".
    refresh_model_availability();
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
    else refresh_model_availability();  // a disabled model must leave the composer
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
// Owners that can host the shared model picker. The render pass finds the open
// popup by key prefix instead of enumerating owners, because a subagent row's
// owner name is dynamic.
void UiApp::open_model_picker(const std::string& owner, const std::string& target) {
  // The picker lists providers, so make sure they are loaded before it opens
  // (it can be opened from the session page without ever visiting settings).
  if (!st_.providers_loaded) load_providers();
  // Which model is this picker FOR? Opening it on the page of whatever provider
  // was last browsed elsewhere left the current model on a different page - it
  // was there, just not where the eye landed ("不会自动放到选择的那一页").
  std::string focus = target.empty() ? st_.model
                                     : sv_str(st_.settings_values, target, "");
  update([&](AppState& s) {
    s.model_picker_open = true;
    s.model_picker_owner = owner;
    s.model_picker_target = target;
    s.model_picker_query.clear();
    s.model_picker_focus = focus;
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
  // model.allEnabled returns every enabled model across ALL providers, with the
  // provider attached. The picker filters by provider in memory, so one request
  // covers the whole popup (switching providers costs nothing) and the list is
  // exactly what the session may use - the same source the backend resolves
  // against, instead of a per-provider view that could disagree.
  call_async("model.allEnabled", Json::object(),
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
        row.provider = jsonutil::get<std::string>(arr[i], "providerId", "");
        if (!row.id.empty() && row.enabled) rows.push_back(row);
      }
    }
    update([&](AppState& s) {
      if (s.model_picker_provider != provider_id) return;
      s.model_picker_models = rows;
      s.model_picker_loaded = true;
      s.model_picker_status.clear();
      // Open on the page the CURRENT model lives on: the left column is a
      // provider list, and the focus model's provider may not be the one that
      // happened to be selected (a fresh window defaults to providers[0]).
      if (!s.model_picker_focus.empty()) {
        for (size_t i = 0; i < rows.size(); ++i) {
          if (rows[i].id != s.model_picker_focus) continue;
          if (!rows[i].provider.empty() && rows[i].provider != s.model_picker_provider) {
            s.model_picker_provider = rows[i].provider;
          }
          break;
        }
        s.model_picker_focus.clear();
      }
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
    update([&](AppState& s) {
      s.model = model_id;
      s.model_available = true;   // an explicit pick is by definition usable
    });
    if (!sid.empty()) {
      call_async("session.setModel", Json{{"sid", sid}, {"model", model_id}},
                 [this, sid](const BackendResult&) {
        // The provider runtime caches per-session config (model, thinking,
        // retry counters). Changing the model without refreshing it means the
        // next turn can still be routed with the old one.
        call_async("session.refreshConfig", Json{{"sid", sid}});
        // The context window follows the model, so the water level must be
        // recomputed (the old percentage belongs to the old window).
        load_session_usage(sid);
        // A different model is usually a different PROVIDER: the balance is stale.
        load_balances(false);
      });
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
  box.key("model-field:" + owner);
  H btn = el("button", "model-pick-btn");
  btn.attr("type", "button");
  btn.attr("title", tr("选择模型"));
  btn.key("model-btn:" + owner);
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
    VNodePtr pop = build_model_picker(owner, current);
    box.add(pop);
    // Anchored to this field's button, not to the whole window: the placement
    // pass measures and flips it when there is no room above.
    pop->key = "model-picker:" + owner;
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
    // The list holds every enabled model; the left column selects which
    // provider's models are shown.
    if (!m.provider.empty() && !st_.model_picker_provider.empty() &&
        m.provider != st_.model_picker_provider) {
      continue;
    }
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
      // The row carries its own provider (the list is cross-provider), which is
      // also what the backend needs to resolve the model's capabilities.
      std::string ppid = m.provider.empty() ? st_.model_picker_provider : m.provider;
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
// The shortcut table's stored binding for `action`, matched against the key the
// adapter reported (which now includes modifiers, so "Ctrl+W" is expressible).
bool UiApp::shortcut_matches(const std::string& key, const std::string& action,
                             const std::string& def) {
  std::string want = sv_str(st_.settings_values, "shortcuts." + action, def);
  if (want.empty()) want = def;
  return normalize_shortcut(want) == normalize_shortcut(key);
}

void UiApp::detect_mcp() {
  call_async("mcp.detect", Json::object(), [this](const BackendResult& r) {
    if (!r.ok) return;
    size_t n = r.data.contains("scanned") && r.data["scanned"].is_array()
                   ? r.data["scanned"].size() : 0;
    toast(n > 0 ? (tr("探测到 ") + itos(static_cast<int>(n)) + tr(" 个 MCP 配置"))
                : tr("未发现 MCP 服务器配置"));
    load_mcp_tools();
  });
}

// Skill detection. The Skill page used to call detect_mcp, which only knows
// about MCP server configs - so a skill directory was never found, and MCP
// entries ended up listed where skills belong.
void UiApp::detect_skills() {
  call_async("skill.detect", Json::object(), [this](const BackendResult& r) {
    if (!r.ok) return;
    size_t n = r.data.contains("scanned") && r.data["scanned"].is_array()
                   ? r.data["scanned"].size() : 0;
    toast(n > 0 ? (tr("探测到 ") + itos(static_cast<int>(n)) + tr(" 个 Skill 目录"))
                : tr("未发现 Skill 目录"));
    load_skills();
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
    // The session is now "background busy" (a live terminal), and the project's
    // dot says so: a terminal that is merely OPEN prints nothing, so no
    // terminal.output event would have refreshed the row.
    mark_sessions_dirty();
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
    update([&](AppState& s) {
      s.term_grids[tid] = r.data;
      // A REPLAYED terminal (a record from a previous run) has no live output
      // buffer, so the overview's filters matched nothing. Fold the replayed
      // text in once, so searching a restored terminal works like a live one.
      if (jsonutil::get<std::string>(r.data, "replay", "") == "log" &&
          s.term_outputs.find(tid) == s.term_outputs.end()) {
        std::string text;
        if (r.data.contains("lines") && r.data["lines"].is_array()) {
          const Json& lines = r.data["lines"];
          for (size_t i = 0; i < lines.size(); ++i) {
            if (!lines[i].is_array()) continue;
            for (size_t k = 0; k < lines[i].size(); ++k) {
              text += jsonutil::get<std::string>(lines[i][k], "t", "");
            }
            text += "\n";
          }
        }
        if (!text.empty()) s.term_outputs[tid] = text;
      }
    });
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

void UiApp::move_tab(const std::string& from, const std::string& to, bool after) {
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
    // The target is still in the list; its index after the erase is the base.
    // BEFORE = its index, AFTER = one past it. (The old "ti - 1" rule made a
    // drop on the adjacent tab a no-op, so dragging looked broken.)
    ti = -1;
    for (size_t i = 0; i < s.tab_ids.size(); ++i) {
      if (s.tab_ids[i] == to) ti = static_cast<int>(i);
    }
    if (ti < 0) return;
    int insert = after ? ti + 1 : ti;
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
  if (kind == "git" && !st_.active_pid.empty()) load_git(id);
  if (kind == "workflow") load_workflows(active_sid());
  if (kind == "usage") {
    load_session_usage(active_sid());
    // Opening the panel is a deliberate "what is left?" moment.
    load_balances(true);
  }
}
// 侧栏标签拖拽排序：把 from 放到 to 的左半边（before）或右半边（after）。
void UiApp::move_side_tab(const std::string& from, const std::string& to, bool after) {
  if (from.empty() || to.empty() || from == to) return;
  update([&](AppState& s) {
    int fi = -1;
    for (size_t i = 0; i < s.side_tabs.size(); ++i) {
      if (s.side_tabs[i].id == from) fi = static_cast<int>(i);
    }
    if (fi < 0) return;
    SideTab moved = s.side_tabs[fi];
    s.side_tabs.erase(s.side_tabs.begin() + fi);
    int ti = -1;
    for (size_t i = 0; i < s.side_tabs.size(); ++i) {
      if (s.side_tabs[i].id == to) ti = static_cast<int>(i);
    }
    if (ti < 0) return;
    int insert = after ? ti + 1 : ti;
    if (insert < 0) insert = 0;
    if (insert > static_cast<int>(s.side_tabs.size())) {
      insert = static_cast<int>(s.side_tabs.size());
    }
    s.side_tabs.insert(s.side_tabs.begin() + insert, moved);
  });
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
