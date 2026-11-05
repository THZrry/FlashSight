// ui_components.cpp - Composers implementation.
#include "ui_components.hpp"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <map>
#include <string>

#include "fs_highlight.hpp"
#include "fs_i18n.hpp"
#include "icon_tables_gen.hpp"

namespace fs {
namespace ui {
namespace comp {

namespace {

// UI strings go through the shared table: Chinese is the key, English is looked
// up. Components are built by the app layer too, so they need the same treatment
// as the page builders (otherwise a button label stays Chinese in an English UI).
std::string tr(const std::string& zh) { return i18n::tr(zh); }

// small helpers -------------------------------------------------------------

std::string itos(int v) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%d", v);
  return std::string(buf);
}

// escape text for HTML-ish inline rendering (we build text nodes, so only
// need to keep raw text; escaping happens naturally through textContent)
bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r'; }

std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && is_space(s[a])) ++a;
  while (b > a && is_space(s[b - 1])) --b;
  return s.substr(a, b - a);
}

namespace {

// A table's separator row: | --- | :---: | ---: |
bool is_table_separator(const std::string& line) {
  std::string t = trim(line);
  if (t.empty() || t[0] != '|') return false;
  bool saw_dash = false;
  for (char c : t) {
    if (c == '-') saw_dash = true;
    else if (c != '|' && c != ':' && c != ' ' && c != '\t') return false;
  }
  return saw_dash;
}

// inline markdown: `code`, **bold**, *italic*, [text](url) -> spans / anchors
//
// `on_link` is bound on each ANCHOR (not delegated from the message): binding a
// click on the whole message made every click look like a link, so any click in
// an answer tried to open something.
void inline_md(H& host, const std::string& text, EventFn on_link) {
  size_t i = 0;
  std::string plain;
  while (i < text.size()) {
    char c = text[i];
    if (c == '`') {
      size_t e = text.find('`', i + 1);
      if (e != std::string::npos) {
        if (!plain.empty()) {
          host.add(txt(plain));
          plain.clear();
        }
        host.add(el("code", "inline").text(text.substr(i + 1, e - i - 1)));
        i = e + 1;
        continue;
      }
    } else if (c == '[') {
      // [label](target). The anchor carries the target as a data attribute and
      // NO href: the UI intercepts the click (a file path opens the side panel
      // preview, an http URL goes to the browser). A real href would make the
      // engine navigate away from the app.
      size_t rb = text.find(']', i + 1);
      if (rb != std::string::npos && rb + 1 < text.size() && text[rb + 1] == '(') {
        size_t rp = text.find(')', rb + 2);
        if (rp != std::string::npos) {
          if (!plain.empty()) {
            host.add(txt(plain));
            plain.clear();
          }
          std::string label = text.substr(i + 1, rb - i - 1);
          std::string target = text.substr(rb + 2, rp - rb - 2);
          H a = el("a", "md-link");
          a.attr("data-href", target);
          a.attr("title", target);
          // Bound HERE, on the anchor. Delegating the click from the message
          // wrapper made every click in the answer look like a link, so a plain
          // click tried to open whatever text happened to be under the pointer.
          if (on_link) {
            std::string t = target;
            EventFn fn = on_link;
            a.on(EventKind::kClick, [fn, t](const std::string&) { fn(t); });
          }
          // The label itself may carry `code` / **bold**; recurse so a link
          // around inline code still renders correctly.
          inline_md(a, label, on_link);
          host.add(a);
          i = rp + 1;
          continue;
        }
      }
    } else if (c == '*' && i + 1 < text.size() && text[i + 1] == '*') {
      size_t e = text.find("**", i + 2);
      if (e != std::string::npos) {
        if (!plain.empty()) {
          host.add(txt(plain));
          plain.clear();
        }
        host.add(el("strong").text(text.substr(i + 2, e - i - 2)));
        i = e + 2;
        continue;
      }
    } else if (c == '*') {
      size_t e = text.find('*', i + 1);
      if (e != std::string::npos && e > i + 1) {
        if (!plain.empty()) {
          host.add(txt(plain));
          plain.clear();
        }
        host.add(el("em").text(text.substr(i + 1, e - i - 1)));
        i = e + 1;
        continue;
      }
    }
    plain += c;
    ++i;
  }
  if (!plain.empty()) host.add(txt(plain));
}

}  // namespace
}  // namespace

// ---------- buttons ----------

H button(const std::string& label, EventFn on_click, const std::string& variant) {
  H b = el("button", variant.empty() ? "btn" : ("btn " + variant));
  b.attr("type", "button");
  b.on(EventKind::kClick, on_click);
  b.text(label);
  return b;
}

// An icon-button's glyph argument is either a literal character ("✕", "+", "↻")
// or a Font Awesome name ("folder-open", "trash", "pen"). Rendering an FA name
// as text is what put the word "folder-open" on a project card, so detect the
// shape: an all-[a-z0-9-] string is an FA name and becomes an <i class="fa ...">.
static bool looks_like_fa_name(const std::string& s) {
  if (s.empty()) return false;
  for (size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
    if (!ok) return false;
  }
  return true;
}

H icon_button(const std::string& glyph, const std::string& tip, EventFn on_click) {
  H b = el("button", "icon-btn");
  b.attr("type", "button");
  b.attr("title", tip);
  b.attr("aria-label", tip);
  b.on(EventKind::kClick, on_click);
  if (looks_like_fa_name(glyph)) {
    H i = el("i", "fa fa-" + glyph);
    i.attr("aria-hidden", "true");
    b.add(i);
  } else {
    b.text(glyph);
  }
  return b;
}

H icon_text_button(const std::string& icon, const std::string& label,
                   EventFn on_click, const std::string& variant) {
  H b = el("button", variant.empty() ? "btn it-btn" : ("btn it-btn " + variant));
  b.attr("type", "button");
  if (!label.empty()) b.attr("aria-label", label);
  // A null handler is allowed: some buttons are driven by a DOM-adapter binding
  // instead (the file picker opens a chooser, which only the browser can do).
  if (on_click) b.on(EventKind::kClick, on_click);
  // <i class="fa fa-<icon>"></i> + <span class="it-label">label</span>; CSS
  // hides the span (and trims the padding) when the toolbar gets narrow.
  H i = el("i", "fa fa-" + icon);
  i.attr("aria-hidden", "true");
  b.add(i);
  if (!label.empty()) b.add(el("span", "it-label").text(label));
  return b;
}

H title_button(const std::string& label, bool active, EventFn on_click) {
  // small title-like nav: no background; active = color + underline
  H b = el("button", active ? "title-btn active" : "title-btn");
  b.attr("type", "button");
  b.on(EventKind::kClick, on_click);
  b.text(label);
  return b;
}

// ---------- tabs ----------

H tab_strip(const std::vector<TabSpec>& tabs, const std::string& active_key,
            EventFn on_select, EventFn on_close, EventFn on_add,
            EventFn on_context) {
  H bar = el("div", "tabs");
  // The strip is split in two: a PINNED slot for 主页 and the "+", and a
  // scrolling slot for the session tabs between them.
  //
  // One flat scroll container (the previous shape) meant opening a dozen
  // sessions pushed 主页 off the left edge and there was no way back to it
  // except scrolling - the user cannot reach the home page any more. Pinning it
  // (and the + that creates a tab) keeps the two navigation anchors always
  // visible while only the session tabs scroll.
  H pinned = el("div", "tabs-fixed");
  pinned.key("tabs-fixed");
  H scroll = el("div", "tabs-scroll");
  scroll.key("tabs-scroll");
  bool seen_scroll = false;
  for (size_t i = 0; i < tabs.size(); ++i) {
    const TabSpec& t = tabs[i];
    H tab = el("div", t.key == active_key ? "tab active" : "tab");
    tab.key(t.key);  // stable identity across re-renders
    tab.attr("data-key", t.key);
    std::string k = t.key;
    tab.on(EventKind::kClick, [on_select, k](const std::string&) { on_select(k); });
    // Middle click closes (browser convention); the × remains.
    if (t.closable && on_close) {
      tab.on(EventKind::kAuxClick, [on_close, k](const std::string&) { on_close(k); });
    }
    // Right-click a tab: rename / close. The browser's own menu is suppressed
    // globally, so without this a right-click on a tab did nothing at all.
    // The tab key is prepended because one callback serves every tab and the
    // event value only carries the pointer position ("<key>\t<x>\t<y>").
    if (on_context) {
      tab.on(EventKind::kContextMenu, [on_context, k](const std::string& v) {
        on_context(k + "\t" + v);
      });
    }
    if (t.dirty) tab.add(el("span", "dot"));
    tab.add(el("span", "tab-title").text(t.title));
    if (t.badge >= 0) tab.add(badge(itos(t.badge), "tab-badge"));
    if (t.closable) {
      H x = el("span", "tab-close");
      x.text("\u00d7");  // multiplication sign
      x.on(EventKind::kClick, [on_close, k](const std::string&) { on_close(k); });
      tab.add(x);
    }
    // A tab the app marks non-closable IS the home tab: keep it (and everything
    // up to and including it) outside the scrolling area.
    if (t.closable && !seen_scroll) {
      seen_scroll = true;
    }
    if (seen_scroll) scroll.add(tab);
    else pinned.add(tab);
  }
  H add = el("div", "tab add");
  add.key("tab-add");  // never inherit a closed tab's slot
  add.attr("title", "新建标签页");
  add.on(EventKind::kClick, on_add);
  add.text("+");
  // "+" sits at the right end of the scrolling area, so adding a tab never
  // requires scrolling to find the button.
  scroll.add(add);
  bar.add(pinned);
  bar.add(scroll);
  return bar;
}

H title_tabs(const std::vector<std::string>& items, int active, EventFn on_select) {
  H row = el("div", "title-tabs");
  for (size_t i = 0; i < items.size(); ++i) {
    int idx = static_cast<int>(i);
    row.add(title_button(items[i], idx == active,
                         [on_select, idx](const std::string&) { on_select(itos(idx)); }));
  }
  return row;
}

H panel_tabs(const std::vector<TabSpec>& tabs, const std::string& active_key,
             EventFn on_select, EventFn on_close, EventFn on_add) {
  H bar = el("div", "panel-tabs");
  for (size_t i = 0; i < tabs.size(); ++i) {
    const TabSpec& t = tabs[i];
    H tab = el("div", t.key == active_key ? "ptab active" : "ptab");
    // Same stable identity as the top strip. Without it the diff matches by
    // POSITION, so closing a middle panel shifts every later node one slot left
    // and the handlers travel with the slot - which is how closing one panel
    // ended up firing another's action (the "+" button, in practice).
    tab.key(t.key);
    tab.attr("data-key", t.key);
    std::string k = t.key;
    tab.on(EventKind::kClick, [on_select, k](const std::string&) { on_select(k); });
    // Browser-like: middle click closes the tab. The × stays for discoverability.
    if (t.closable && on_close) {
      tab.on(EventKind::kAuxClick, [on_close, k](const std::string&) { on_close(k); });
    }
    tab.add(el("span", "ptab-title").text(t.title));
    if (t.badge >= 0) tab.add(badge(itos(t.badge), "tab-badge"));
    if (t.closable) {
      H x = el("span", "tab-close");
      x.text("\u00d7");
      x.on(EventKind::kClick, [on_close, k](const std::string&) { on_close(k); });
      tab.add(x);
    }
    bar.add(tab);
  }
  H add = el("div", "ptab add");
  add.key("panel-add");  // never inherit a closed tab's slot
  add.attr("title", "新建面板");
  add.on(EventKind::kClick, on_add);
  add.text("+");
  bar.add(add);
  return bar;
}

// ---------- forms ----------

H field(const std::string& label, const H& control, const std::string& hint) {
  H f = el("div", "field");
  if (!label.empty()) f.add(el("label").text(label));
  H ctl = el("div", "ctl");
  ctl.add(control);
  f.add(ctl);
  if (!hint.empty()) f.add(el("div", "hint").text(hint));
  return f;
}

H text_input(const std::string& value, const std::string& placeholder,
             EventFn on_input, EventFn on_enter, EventFn on_blur) {
  H i = el("input", "text-input");
  i.attr("type", "text");
  i.attr("value", value);
  if (!placeholder.empty()) i.attr("placeholder", placeholder);
  i.on(EventKind::kInput, on_input);
  if (on_enter) i.on(EventKind::kKeyEnter, on_enter);
  if (on_blur) i.on(EventKind::kBlur, on_blur);
  return i;
}

H textarea(const std::string& value, const std::string& placeholder,
           EventFn on_input, EventFn on_enter, EventFn on_keydown) {
  H t = el("textarea", "text-area");
  if (!placeholder.empty()) t.attr("placeholder", placeholder);
  t.on(EventKind::kInput, on_input);
  t.on(EventKind::kKeyEnter, on_enter);
  // Raw key names (ArrowUp / Tab / Escape) for the inline completion menu. The
  // adapter's keyenter binding only reports Enter; this one reports every key.
  if (on_keydown) t.on(EventKind::kKeyDown, on_keydown);
  // value via the `value` attribute (glue.js maps it to the live property).
  // A text child would only set the defaultValue, so clearing after send
  // would not clear what the user sees.
  t.attr("value", value);
  return t;
}

H select(const std::vector<std::string>& options, int selected, EventFn on_change) {
  H s = el("select", "select");
  for (size_t i = 0; i < options.size(); ++i) {
    H o = el("option");
    o.attr("value", itos(static_cast<int>(i)));
    if (static_cast<int>(i) == selected) o.attr("selected", "1");
    o.text(options[i]);
    s.add(o);
  }
  s.on(EventKind::kChange, on_change);
  return s;
}

H checkbox(const std::string& label, bool checked, EventFn on_change) {
  H l = el("label", "checkbox");
  H i = el("input");
  i.attr("type", "checkbox");
  i.attr("checked", checked ? "1" : "0");
  i.on(EventKind::kChange, on_change);
  l.add(i);
  l.add(el("span").text(label));
  return l;
}

H toggle(bool on, const std::string& label, EventFn on_change) {
  H w = el("div", "toggle-wrap");
  H t = el("button", on ? "toggle on" : "toggle");
  t.attr("type", "button");
  t.attr("role", "switch");
  t.attr("aria-checked", on ? "true" : "false");
  t.on(EventKind::kClick, [on_change](const std::string&) {
    on_change("toggle");
  });
  t.add(el("span", "knob"));
  w.add(t);
  if (!label.empty()) w.add(el("span", "toggle-label").text(label));
  return w;
}

H search_box(const std::string& value, const std::string& placeholder, EventFn on_input) {
  H w = el("div", "search-box");
  w.add(el("span", "search-ico").text("\u2315"));  // ⌕
  H i = el("input", "search-input");
  i.attr("type", "search");
  i.attr("value", value);
  if (!placeholder.empty()) i.attr("placeholder", placeholder);
  i.on(EventKind::kInput, on_input);
  w.add(i);
  return w;
}

H choice_list(const std::vector<ChoiceOption>& options, const std::string& selected,
              EventFn on_pick) {
  H wrap = el("div", "choice-list");
  for (size_t i = 0; i < options.size(); ++i) {
    const ChoiceOption& o = options[i];
    H row = el("div", o.value == selected ? "choice active" : "choice");
    std::string v = o.value;
    row.on(EventKind::kClick, [on_pick, v](const std::string&) { on_pick(v); });
    if (!o.icon.empty()) row.add(el("i", "fa fa-" + o.icon));
    H body = el("div", "choice-body");
    body.add(el("div", "choice-title").text(o.title));
    if (!o.desc.empty()) body.add(el("div", "choice-desc").text(o.desc));
    row.add(body);
    if (o.value == selected) row.add(el("i", "fa fa-check choice-mark"));
    wrap.add(row);
  }
  return wrap;
}

// ---------- display ----------

H badge(const std::string& text, const std::string& variant) {
  H b = el("span", variant.empty() ? "badge" : ("badge " + variant));
  b.text(text);
  return b;
}

H spinner() { return el("span", "spinner"); }

H progress(int pct) {
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  H p = el("div", "progress");
  H bar = el("div", "progress-bar");
  bar.style("width", itos(pct) + "%");
  p.add(bar);
  return p;
}

H empty_state(const std::string& hint, const std::string& action_label, EventFn on_action) {
  H e = el("div", "empty-state");
  e.add(el("div", "empty-hint").text(hint));
  if (!action_label.empty()) {
    e.add(button(action_label, on_action, "primary"));
  }
  return e;
}

H card(const std::string& cls, const std::vector<H>& children) {
  H c = el("div", cls.empty() ? "card" : ("card " + cls));
  for (size_t i = 0; i < children.size(); ++i) c.add(children[i]);
  return c;
}

H section_title(const std::string& text) {
  return el("h3", "section-title").text(text);
}

H hint(const std::string& text) { return el("div", "hint").text(text); }

H key_value(const std::string& k, const std::string& v) {
  H r = el("div", "kv");
  r.add(el("span", "kv-k").text(k));
  r.add(el("span", "kv-v").text(v));
  return r;
}

// ---------- model / file icons ----------

namespace {

std::string lower_ascii(const std::string& s) {
  std::string r = s;
  for (size_t i = 0; i < r.size(); ++i) {
    if (r[i] >= 'A' && r[i] <= 'Z') r[i] = static_cast<char>(r[i] - 'A' + 'a');
  }
  return r;
}

bool contains(const std::string& hay, const char* needle) {
  return hay.find(needle) != std::string::npos;
}

// Brand by model name. Mirrors fs::model_icon_for (provider layer) so the UI can
// label a model that arrived without an explicit icon; kept here as well because
// the UI also draws models it only knows by name.
const char* brand_for_name(const std::string& name) {
  const std::string m = lower_ascii(name);
  static const char* const kPairs[][2] = {
      {"gpt-", "openai"}, {"o1", "openai"}, {"o3", "openai"}, {"o4", "openai"},
      {"chatgpt", "openai"}, {"davinci", "openai"},
      {"claude", "claude"}, {"anthropic", "anthropic"},
      {"gemini", "gemini"}, {"gemma", "gemma"}, {"palm", "google"},
      {"deepseek", "deepseek"},
      {"qwen", "qwen"}, {"qwq", "qwen"}, {"tongyi", "qwen"}, {"dashscope", "alibaba"},
      {"kimi", "kimi"}, {"moonshot", "moonshot"},
      {"glm", "zhipu"}, {"zhipu", "zhipu"}, {"chatglm", "chatglm"},
      {"llama", "meta"}, {"meta-", "meta"},
      {"mistral", "mistral"}, {"mixtral", "mistral"}, {"codestral", "mistral"},
      {"grok", "grok"}, {"xai", "xai"},
      {"command-r", "cohere"}, {"cohere", "cohere"},
      {"sonar", "perplexity"}, {"perplexity", "perplexity"},
      {"groq", "groq"},
      {"yi-", "yi"},
      {"doubao", "doubao"}, {"seed-", "bytedance"},
      {"ernie", "ernie"}, {"wenxin", "ernie"},
      {"minimax", "minimax"}, {"abab", "minimax"},
      {"hunyuan", "hunyuan"},
      {"step-", "stepfun"}, {"stepfun", "stepfun"},
      {"spark", "spark"}, {"iflytek", "iflytek"},
      {"baichuan", "baichuan"},
      {"internlm", "internlm"},
      {"nemotron", "nvidia"}, {"nvidia", "nvidia"},
      {"nova-", "aws"}, {"titan", "aws"}, {"bedrock", "bedrock"},
      {"azure", "azure"}, {"copilot", "microsoft"},
      {"ollama", "ollama"}, {"lmstudio", "lmstudio"}, {"lm-studio", "lmstudio"},
      {"vllm", "vllm"}, {"opencode", "opencode"},
      {"flux", "flux"}, {"stable-diffusion", "stability"}, {"midjourney", "midjourney"},
      {"suno", "suno"}, {"runway", "runway"},
  };
  for (size_t i = 0; i < sizeof(kPairs) / sizeof(kPairs[0]); ++i) {
    if (contains(m, kPairs[i][0])) return kPairs[i][1];
  }
  return nullptr;
}

}  // namespace

H model_icon(const std::string& icon_key, const std::string& model_id,
             const std::string& extra_cls) {
  std::string key = icon_key;
  if (key.empty()) {
    const char* b = brand_for_name(model_id);
    if (b) key = b;
  }
  std::string cls = "mi";
  if (!extra_cls.empty()) cls += " " + extra_cls;
  if (key.empty()) {
    // No brand match: a neutral dot rather than an empty box, so the row still
    // has a left edge and the list does not look broken.
    cls += " mi-none";
    return el("span", cls);
  }
  cls += " mi-lobe-" + key;
  return el("span", cls);
}

H file_icon(const std::string& name, bool is_dir) {
  using namespace fs::ui::icons;
  std::string cls = "mi";
  if (is_dir) {
    cls += " mi-file-";
    cls += kFolderIcon;
    return el("span", cls);
  }
  const std::string lower = lower_ascii(name);
  // exact filename first: "Dockerfile" and "Makefile" have no extension, and
  // "cargo.toml" should not be matched by the generic "toml" rule if a more
  // specific name rule exists.
  for (size_t i = 0; i < kFileName_count; ++i) {
    if (lower == kFileName[i].key) {
      cls += " mi-file-";
      cls += kFileName[i].icon;
      return el("span", cls);
    }
  }
  // then the extension (last dot wins: "x.test.ts" -> ts)
  size_t dot = lower.rfind('.');
  if (dot != std::string::npos && dot + 1 < lower.size()) {
    const std::string ext = lower.substr(dot + 1);
    for (size_t i = 0; i < kFileExt_count; ++i) {
      if (ext == kFileExt[i].key) {
        cls += " mi-file-";
        cls += kFileExt[i].icon;
        return el("span", cls);
      }
    }
  }
  cls += " mi-file-";
  cls += kFallbackFileIcon;
  return el("span", cls);
}

// ---------- chat ----------

// "1s" / "1m 20s" / "1h 04m" - short enough for a row of metadata. Whole
// seconds only: a tenth of a second in a transcript is noise, not information.
std::string fmt_duration(int64_t ms) {
  if (ms < 0) ms = 0;
  int64_t sec = ms / 1000;
  if (sec < 60) return itos(static_cast<int>(sec)) + "s";
  int64_t min = sec / 60;
  int64_t rem = sec % 60;
  if (min < 60) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%dm %02ds", static_cast<int>(min),
                  static_cast<int>(rem));
    return std::string(buf);
  }
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%dh %02dm", static_cast<int>(min / 60),
                static_cast<int>(min % 60));
  return std::string(buf);
}

// "14:32" in local time: the finish time of a turn, compact.
std::string fmt_clock(int64_t ms) {
  if (ms <= 0) return std::string();
  time_t t = static_cast<time_t>(ms / 1000);
  struct tm* g = localtime(&t);
  if (!g) return std::string();
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%02d:%02d", g->tm_hour, g->tm_min);
  return std::string(buf);
}

// Stable identity for one rendered message (see chat_message_key in the header).
// Derived from `uid`, NOT from the position in the vector: prepending an older
// page shifts every index but leaves every uid alone, so nothing already on
// screen is rebuilt.
std::string chat_message_key(const std::string& sid, int64_t uid) {
  std::string k = "msg:";
  k += sid;
  k += ':';
  k += itos(static_cast<int>(uid));
  return k;
}

H chat_message(const ChatMsg& m, EventFn on_copy, EventFn on_tool_toggle,
               EventFn on_think_toggle, EventFn on_turn_action,
               EventFn on_context, EventFn on_link, EventFn on_open_subagent,
               bool can_edit) {
  H wrap = el("div", "msg " + m.role);
  // NO click handler on the wrapper: a link binds its own handler (see
  // inline_md). Delegating here made every click in an answer look like a link.
  // Right-click belongs to the MESSAGE, not to the browser: the shell opens its
  // own menu with copy / branch / rollback / forget. Bound on the wrapper so it
  // fires anywhere inside the bubble, the answer or the tool blocks.
  if (on_context) {
    wrap.on(EventKind::kContextMenu, on_context);
  }
  if (m.role == "system") {
    // A system line is a MARKER, not content: the auto-continue divider lives
    // here, and it must read as a divider rather than as another chat turn.
    H sys = el("div", "sys");
    sys.add(el("span", "sys-rule"));
    sys.add(el("span", "sys-text").text(m.text));
    sys.add(el("span", "sys-rule"));
    wrap.add(sys);
    return wrap;
  }
  if (m.role == "user") {
    // user input: pure bubble, no name, no extras
    wrap.add(el("div", "bubble").text(m.text));
    H meta = el("div", "meta");
    if (!m.time.empty()) meta.add(el("span", "time").text(m.time));
    // 编辑并重发. A user turn is where a do-over starts, so the action belongs on
    // the bubble - and only on the LAST one, because rewriting an older question
    // would orphan every answer after it (that is what 回滚 is for).
    if (can_edit && on_turn_action) {
      H acts = el("div", "msg-actions");
      H b = el("button", "mini-btn");
      b.attr("type", "button");
      b.text(tr("编辑"));
      b.on(EventKind::kClick, [on_turn_action](const std::string&) {
        on_turn_action("edit");
      });
      acts.add(b);
      meta.add(acts);
    }
    wrap.add(meta);
    return wrap;
  }
  // ---- 工作过程: reasoning + tool calls, folded into ONE group ----
  // The answer comes first in reading order: you look at what the model said,
  // then open the process if you want to know how it got there. A turn with a
  // dozen tool blocks used to be a wall of rows that buried the answer.
  // `interim` counts: a turn whose ONLY visible process is the narration between
  // tool calls still has a work log. Without it the fold was not rendered at all,
  // so "过程说明" had nowhere to appear - which is why the narration looked like it
  // had been dropped again.
  bool has_work = !m.thinking.empty() || !m.tools.empty() || !m.interim.empty();
  if (has_work) {
    int nfail = 0;
    for (size_t i = 0; i < m.tools.size(); ++i) if (!m.tools[i].ok) ++nfail;
    // RUNNING IS OPEN. The flag alone was not enough: a message that is streaming
    // (or a repaint before the shell opened the fold) showed a closed block while
    // work was happening. An explicit click wins for the rest of the turn;
    // turn.done folds it again for good.
    bool work_open = m.work_open || (m.streaming && !m.work_touched);
    H work = el("div", work_open ? "work-block open" : "work-block");
    H whead = el("div", "work-head");
    if (on_think_toggle) {
      whead.on(EventKind::kClick,
               [on_think_toggle](const std::string&) { on_think_toggle("work"); });
    }
    whead.add(el("i", "fa fa-list-check"));
    if (work_open) {
      whead.add(el("span").text(tr("工作过程")));
    } else {
      // Collapsed: one line that says what happened, so the fold is informative
      // instead of just hiding everything behind a caret.
      std::string label = tr("工作过程");
      if (!m.tools.empty()) {
        label += " · " + itos(static_cast<int>(m.tools.size())) + tr(" 步");
      }
      if (!m.interim.empty()) {
        label += " · " + itos(static_cast<int>(m.interim.size())) + tr(" 段过程说明");
      }
      if (nfail > 0) label += tr("（") + itos(nfail) + tr(" 次失败）");
      whead.add(el("span").text(label));
    }
    whead.add(el("span", "grow"));
    // When + how long: the wall-clock facts of this turn.
    if (m.ended_at > 0) {
      bool ok_turn = nfail == 0;
      whead.add(el("span", ok_turn ? "work-done ok" : "work-done fail")
                   .text(ok_turn ? tr("完成") : tr("有失败")));
      if (m.started_at > 0 && m.ended_at >= m.started_at) {
        whead.add(el("span", "work-dur dim").text(
            fmt_duration(m.ended_at - m.started_at)));
      }
      whead.add(el("span", "work-time dim").text(fmt_clock(m.ended_at)));
    }
    whead.add(el("span", "think-caret").text(work_open ? "\u25be" : "\u25b8"));
    work.add(whead);
    if (work_open) {
      H inner = el("div", "work-body");
      // ONE stream in ARRIVAL ORDER. Each note carries the number of tool calls
      // that had already run when it arrived (`interim_after`), so note k is
      // emitted immediately before tool interim_after[k]. Using the NOTE INDEX
      // for that comparison (the old rule) broke the moment one round issued
      // several tool calls: the next round's thought landed above tools that had
      // already run ("新的思考跑到了上一个工具下面").
      size_t interim_at = 0;
      for (size_t i = 0; i < m.tools.size(); ++i) {
        while (interim_at < m.interim.size()) {
          size_t after = interim_at < m.interim_after.size()
                             ? m.interim_after[interim_at]
                             : interim_at;
          if (after > i) break;
          if (!m.interim[interim_at].empty()) {
            inner.add(el("div", "work-note").text(m.interim[interim_at]));
          }
          ++interim_at;
        }
        EventFn toggle = nullptr;
        if (on_tool_toggle) {
          EventFn base = on_tool_toggle;
          std::string idx = itos(static_cast<int>(i));
          toggle = [base, idx](const std::string& v) { base(idx + "\t" + v); };
        }
        // Give each block a key the shell can address (uid + index): the
        // expanded body is scrolled to its end, which needs a stable handle.
        comp::ToolBlock tb = m.tools[i];
        if (tb.key.empty()) tb.key = std::to_string(m.uid) + ":" + itos(static_cast<int>(i));
        // The fold generation: a cached row is only valid while this fold has
        // stayed in one piece (see ChatMsg::work_seq).
        std::string gen = std::to_string(m.uid) + ":" + std::to_string(m.work_seq) +
                          (work_open ? ":o" : ":c");
        inner.add(tool_block_cached(tb, toggle, on_open_subagent, gen));
      }
      // Anything left (a turn that narrated but called no tool).
      for (; interim_at < m.interim.size(); ++interim_at) {
        if (!m.interim[interim_at].empty()) {
          inner.add(el("div", "work-note").text(m.interim[interim_at]));
        }
      }
      // Appended, not grouped: it goes where the stream has reached, and a tool
      // call that arrives later simply lands below it.
      if (!m.thinking.empty()) {
        inner.add(el("div", "work-label").text(tr("思考")));
        inner.add(el("pre", "think-body").text(m.thinking));
      }
      work.add(inner);
    }
    wrap.add(work);
  }
  // The wait is shown by the turn's own live timer (spinner + counting seconds,
  // see the meta row below): the placeholder message exists from the moment the
  // request is sent, so that counter is already running. No extra "waiting"
  // sentence - the user asked for the counter, not a new hint.
  // assistant: article-like, no bubble, no name
  H body = el("div", "article");
  std::vector<H> blocks = markdown_blocks(m.text, on_copy, on_link);
  for (size_t i = 0; i < blocks.size(); ++i) body.add(blocks[i]);
  if (m.streaming) body.add(el("span", "caret"));
  wrap.add(body);
  // A turn that stopped early says so, in place, right under the partial answer.
  // It used to be invisible: the answer just ended, and after a reload the turn
  // was gone entirely (nothing was persisted).
  if (!m.error.empty()) {
    H err = el("div", "turn-error");
    err.add(el("i", "fa fa-triangle-exclamation"));
    err.add(el("span", "te-text").text(tr("本轮中断：") + m.error));
    wrap.add(err);
  }
  if (!m.summary.empty()) wrap.add(turn_summary(m.summary));
  H meta = el("div", "meta");
  // A USER message carries the time it was sent. An ASSISTANT message does NOT
  // reuse the start time: the clock below is the END time, which is the fact that
  // belongs at the end of the answer (the start is visible in the tool rows).
  if (!m.time.empty() && m.role != "assistant") {
    meta.add(el("span", "time").text(m.time));
  }
  // Turn timing, always visible (not hover-only): "how long did this take" is
  // the question you ask while waiting and right after it lands.
  //   * streaming: elapsed so far, refreshed by the UI timer
  //   * finished : wall-clock finish time + total duration
  if (m.streaming && m.started_at > 0 && m.now_ms >= m.started_at) {
    H live = el("span", "turn-live");
    live.add(el("i", "fa fa-circle-notch fa-spin"));
    live.add(el("span").text(fmt_duration(m.now_ms - m.started_at)));
    meta.add(live);
  } else if (!m.streaming && m.ended_at > 0) {
    H done = el("span", "turn-timing");
    if (m.started_at > 0 && m.ended_at >= m.started_at) {
      done.add(el("span", "turn-dur").text(fmt_duration(m.ended_at - m.started_at)));
    }
    done.add(el("span", "turn-clock").text(fmt_clock(m.ended_at)));
    meta.add(done);
  }
  H cp = el("button", "mini-btn");
  cp.attr("type", "button");
  cp.text(tr("复制"));
  if (on_copy) cp.on(EventKind::kClick, on_copy);
  meta.add(cp);
  // Turn-scoped actions. They appear on hover (CSS) because a transcript should
  // read as prose; they act on THIS turn, which is why they live on the message
  // rather than in a global menu.
  if (on_turn_action) {
    H acts = el("div", "msg-actions");
    auto act = [&acts, on_turn_action](const char* label, const char* value) {
      H b = el("button", "mini-btn");
      b.attr("type", "button");
      b.text(tr(label));
      b.on(EventKind::kClick, [on_turn_action, value](const std::string&) {
        on_turn_action(value);
      });
      acts.add(b);
    };
    act("分叉", "fork");
    act("回滚到这里", "rollback");
    act("忘记", "forget");
    // 编辑并重发: a do-over of the LAST question. It lives on the message (not in
    // the composer) because it edits a specific turn, and it is offered only for
    // the newest user message - an older one would orphan every answer after it.
    if (can_edit && m.role == "user") act("编辑", "edit");
    meta.add(acts);
  }
  wrap.add(meta);
  return wrap;
}

H tool_block(const ToolBlock& b, EventFn on_toggle, EventFn on_open_subagent) {
  std::string cls = "tool-block";
  if (b.expanded) cls += " open";
  if (b.running) cls += " running";
  H d = el("div", cls);
  H head = el("div", "tb-head");
  // A null handler must not kill the header: blocks without a toggle still have
  // to render (and the caret must not vanish).
  if (on_toggle) {
    head.on(EventKind::kClick, [on_toggle](const std::string&) { on_toggle("toggle"); });
  }
  if (b.running) {
    // No verdict yet: a spinner says "working", a check/cross would lie.
    head.add(el("span", "tb-ico run").add(spinner()));
  } else {
    head.add(el("span", b.ok ? "tb-ico ok" : "tb-ico err").text(b.ok ? "\u2713" : "\u2717"));
  }
  head.add(el("span", "tb-name").text(b.name));
  head.add(el("span", "tb-sum").text(b.summary));
  // A subagent call gets an explicit "open record" affordance. The transcript is
  // NOT in this block (it lives in the subagent panel), and without a link here
  // the work log showed that a subagent ran but gave no way to see what it did -
  // so once it finished the history was unreachable.
  if (!b.subagent_id.empty() && on_open_subagent) {
    EventFn open = on_open_subagent;
    std::string sid = b.subagent_id;
    H btn = el("span", "tb-open");
    btn.text(tr("打开记录"));
    // The header toggles the fold; this button has its OWN click handler, and
    // the adapter stops the event at the innermost handler - so pressing it does
    // not also toggle the fold. No per-control marker needed (framework rule).
    btn.attr("title", tr("在新标签页打开这个子代理的完整记录"));
    btn.on(EventKind::kClick, [open, sid](const std::string&) { open(sid); });
    head.add(btn);
  }
  head.add(el("span", "tb-caret").text(b.expanded ? "\u25be" : "\u25b8"));
  d.add(head);
  // "{}" is not an input: a tool with no arguments (or a model that sent an empty
  // object) would otherwise get a labelled section showing two braces. The
  // detail is what matters there.
  bool has_input = !b.input.empty();
  if (has_input) {
    bool braces_only = true;
    for (size_t i = 0; i < b.input.size(); ++i) {
      char c = b.input[i];
      if (c != '{' && c != '}' && c != '[' && c != ']' && c != ' ' && c != '\n' &&
          c != '\r' && c != '\t') {
        braces_only = false;
        break;
      }
    }
    if (braces_only) has_input = false;
  }
  if (b.expanded && (has_input || !b.detail.empty())) {
    // 输入 first, 返回 second: a tool row answers "what did it do" before "what
    // came back". The input used to be thrown away (tool.update replaced it), so
    // a read_file block showed a wall of JSON without saying which file.
    //
    // Labels only when there is something to tell apart: a block that has just
    // the result (an older stored turn, or a tool with no arguments) is not
    // labelled "返回" - a single section explains itself.
    const bool both = has_input && !b.detail.empty();
    const highlight::LangSpec* spec = highlight::find_lang("json");
    auto body = [&](const std::string& label, const std::string& text_str,
                    const std::string& key) {
      if (!label.empty()) d.add(el("div", "tb-label").text(label));
      H pre = el("pre", "tb-body");
      // Keyed so the shell can scroll it to the bottom after opening: the useful
      // end of a tool result is its last lines.
      pre.key(key);
      if (spec && std::strcmp(spec->id, "plain") != 0 && !text_str.empty() &&
          (text_str[0] == '{' || text_str[0] == '[')) {
        pre.html(highlight::to_html_cached(text_str, "json"));
      } else {
        pre.text(text_str);
      }
      d.add(pre);
    };
    if (has_input) {
      body(both || b.detail.empty() ? tr("输入") : std::string(), b.input,
           "tb-input:" + b.key);
    }
    if (!b.detail.empty()) {
      body(both ? tr("返回") : std::string(), b.detail, "tb-body:" + b.key);
    }
  }
  return d;
}

// Memoised tool_block.
//
// WHY: a turn can produce hundreds of tool calls, and chat_message is rebuilt
// whenever the message rev changes - which happens on EVERY streaming delta and
// every tool event. Rebuilding all N tool rows each time is O(N) string copies
// per event and O(N^2) over the turn: a 600-call turn spent seconds per frame
// allocating rows nobody changed. The resource log caught it (tools=634 in 22
// messages). This cache hands back the SAME VNode for an unchanged row, so the
// per-frame cost is O(1) per untouched row and the patch reuses the DOM nodes.
//
// Bounded like the message cache: a long-lived window must not grow it forever.
namespace {
// Memo for tool_block_cached. File-scoped (not function-static) so a page switch
// can DROP it: a cached VNode is only valid while its DOM stays attached, and a
// tab switch detaches every node - reusing them leaves rows whose handlers can
// never fire ("切页面后工具调用点不开").
std::map<std::string, H> g_tool_block_cache;
std::deque<std::string> g_tool_block_order;
}  // namespace

void tool_block_cache_clear() {
  g_tool_block_cache.clear();
  g_tool_block_order.clear();
}

H tool_block_cached(const ToolBlock& b, EventFn on_toggle,
                    EventFn on_open_subagent, const std::string& gen) {
  std::map<std::string, H>& cache = g_tool_block_cache;
  std::deque<std::string>& order = g_tool_block_order;
  const size_t kMaxCachedBlocks = 4000;
  // `gen` is part of the identity: a cached VNode may only be reused while the
  // surrounding rows are CONTINUOUSLY attached. When the work fold closes, the
  // rows are exit-animated and removed; handing the same VNode back on reopen
  // made the diff think it was still in the tree, so the fold opened once and
  // never again ("工作过程只能点开一次").
  std::string key = gen + "#" + b.key + "#" + std::to_string(b.rev) + "#" +
                    std::to_string(b.name.size()) + ":" +
                    std::to_string(b.summary.size()) + ":" +
                    std::to_string(b.detail.size()) + ":" +
                    std::to_string(b.input.size()) + ":" +
                    std::to_string(b.subagent_id.size()) + "#" +
                    (b.expanded ? "1" : "0") + (b.running ? "r" : "") +
                    (b.ok ? "" : "x");
  std::map<std::string, H>::iterator it = cache.find(key);
  if (it != cache.end()) return it->second;
  H node = tool_block(b, on_toggle, on_open_subagent);
  // A row is addressed by a stable key; without one the cache would collide
  // (fall back to an uncached build).
  if (b.key.empty()) return node;
  // Key the OUTER node too: keyed matching is what makes handing the same
  // VNode back safe (the diff pairs it with itself and skips the subtree).
  node.key("tb:" + gen + ":" + b.key);
  cache.insert(std::make_pair(key, node));
  order.push_back(key);
  while (order.size() > kMaxCachedBlocks) {
    cache.erase(order.front());
    order.pop_front();
  }
  return node;
}

H turn_summary(const std::string& text) {
  // not folded, no border
  H s = el("div", "turn-summary");
  s.add(el("span", "ts-label").text(tr("本轮要点")));
  s.add(el("div", "ts-text").text(text));
  return s;
}

// ---------- markdown-ish ----------

std::vector<H> markdown_blocks(const std::string& text, EventFn on_copy_code,
                               EventFn on_link) {
  std::vector<H> out;
  std::vector<std::string> lines;
  {
    std::string cur;
    for (size_t i = 0; i < text.size(); ++i) {
      if (text[i] == '\n') {
        lines.push_back(cur);
        cur.clear();
      } else {
        cur += text[i];
      }
    }
    lines.push_back(cur);
  }

  size_t i = 0;
  while (i < lines.size()) {
    const std::string& ln = lines[i];
    // fenced code
    if (ln.size() >= 3 && ln.compare(0, 3, "```") == 0) {
      std::string lang = trim(ln.substr(3));
      std::string code;
      ++i;
      while (i < lines.size() &&
             !(lines[i].size() >= 3 && lines[i].compare(0, 3, "```") == 0)) {
        code += lines[i];
        code += "\n";
        ++i;
      }
      if (i < lines.size()) ++i;  // closing fence
      // A code block is a small artefact of its own: it has a language label,
      // line numbers and a copy button. Without them you cannot tell what you
      // are reading, cannot refer to "line 12", and cannot get the code out
      // without selecting it by hand.
      H box = el("div", "code-wrap");
      H chead = el("div", "code-head");
      chead.add(el("span", "code-lang").text(lang.empty() ? std::string("text") : lang));
      chead.add(el("span", "grow"));
      if (on_copy_code) {
        std::string body = code;
        H cp = el("button", "mini-btn code-copy");
        cp.attr("type", "button");
        cp.text(tr("复制"));
        cp.on(EventKind::kClick,
              [on_copy_code, body](const std::string&) { on_copy_code(body); });
        chead.add(cp);
      }
      box.add(chead);
      H body = el("div", "code-body");
      // Line numbers are generated here, not in CSS counters: the <pre> holds
      // the text verbatim (a counter would number wrapped visual lines).
      int nlines = 0;
      for (size_t k = 0; k < code.size(); ++k) {
        if (code[k] == '\n') ++nlines;
      }
      if (nlines == 0 || code.empty() || code[code.size() - 1] != '\n') ++nlines;
      H gutter = el("div", "code-lines");
      std::string nums;
      for (int n = 1; n <= nlines; ++n) {
        if (n > 1) nums += "\n";
        nums += itos(n);
      }
      gutter.text(nums);
      body.add(gutter);
      H pre = el("pre", "code-block");
      if (!lang.empty()) pre.attr("data-lang", lang);
      // Highlighting is a single HTML string written with innerHTML, not a tree
      // of spans: a 200-line block is thousands of nodes otherwise, and this
      // component is rebuilt on every render while a turn streams. The
      // ui.codeHighlight setting is applied in CSS (see the .hl-* rules), so
      // turning it off needs no re-render.
      const highlight::LangSpec* spec = highlight::find_lang(lang);
      if (spec && std::strcmp(spec->id, "plain") != 0) {
        pre.html(highlight::to_html_cached(code, lang));
      } else {
        pre.text(code);
      }
      body.add(pre);
      box.add(body);
      out.push_back(box);
      continue;
    }
    // heading
    if (!ln.empty() && ln[0] == '#') {
      size_t n = 0;
      while (n < ln.size() && ln[n] == '#') ++n;
      if (n <= 6 && n < ln.size() && ln[n] == ' ') {
        std::string lvl = "h" + itos(static_cast<int>(n));
        H h = el(lvl, "md-h");
        inline_md(h, trim(ln.substr(n + 1)), on_link);
        out.push_back(h);
        ++i;
        continue;
      }
    }
    // list (group consecutive items)
    if (ln.size() >= 2 && (ln.compare(0, 2, "- ") == 0 || ln.compare(0, 2, "* ") == 0)) {
      H ul = el("ul", "md-list");
      while (i < lines.size() && lines[i].size() >= 2 &&
             (lines[i].compare(0, 2, "- ") == 0 || lines[i].compare(0, 2, "* ") == 0)) {
        H li = el("li");
        inline_md(li, trim(lines[i].substr(2)), on_link);
        ul.add(li);
        ++i;
      }
      out.push_back(ul);
      continue;
    }
    // ordered list
    if (ln.size() >= 3 && ln[0] >= '0' && ln[0] <= '9' && ln[1] == '.' ) {
      H ol = el("ol", "md-list");
      while (i < lines.size() && lines[i].size() >= 3 && lines[i][0] >= '0' &&
             lines[i][0] <= '9' && lines[i][1] == '.') {
        H li = el("li");
        inline_md(li, trim(lines[i].substr(2)), on_link);
        ol.add(li);
        ++i;
      }
      out.push_back(ol);
      continue;
    }
    // blockquote / callout. A leading "[!TIP]" / "[!WARNING]" / "[!NOTE]" /
    // "[!IMPORTANT]" / "[!CAUTION]" makes it a labelled callout with an icon;
    // anything else is a plain quote. Nested quotes flatten to one level.
    if (!ln.empty() && ln[0] == '>') {
      std::vector<std::string> body;
      while (i < lines.size() && !lines[i].empty() && lines[i][0] == '>') {
        std::string b = lines[i].substr(1);
        if (!b.empty() && b[0] == ' ') b.erase(0, 1);
        body.push_back(b);
        ++i;
      }
      // Callout kind from the first line.
      std::string kind, label, icon;
      if (!body.empty() && body[0].size() > 3 && body[0][0] == '[') {
        size_t close = body[0].find(']');
        if (close != std::string::npos) {
          std::string tag = body[0].substr(1, close - 1);
          std::string up;
          for (char ch : tag) up += static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
          if (up == "TIP") { kind = "tip"; label = tr("提示"); icon = "lightbulb"; }
          else if (up == "WARNING") { kind = "warn"; label = tr("警告"); icon = "triangle-exclamation"; }
          else if (up == "NOTE") { kind = "note"; label = tr("注意"); icon = "circle-info"; }
          else if (up == "IMPORTANT") { kind = "important"; label = tr("重要"); icon = "circle-exclamation"; }
          else if (up == "CAUTION") { kind = "caution"; label = tr("小心"); icon = "ban"; }
          if (!kind.empty()) {
            std::string rest = body[0].substr(close + 1);
            body[0] = trim(rest);
            if (body[0].empty()) body.erase(body.begin());
          }
        }
      }
      std::string cls = "md-quote";
      if (!kind.empty()) cls += " callout callout-" + kind;
      H q = el("blockquote", cls);
      if (!kind.empty()) {
        // The header line (icon + label) is what makes a TIP look different from
        // a quote; the body stays muted either way.
        H head = el("div", "callout-head");
        head.add(el("i", "fa fa-" + icon));
        head.add(el("span", "callout-label").text(label));
        q.add(head);
      }
      for (size_t k = 0; k < body.size(); ++k) {
        H p = el("p", "md-p");
        inline_md(p, body[k], on_link);
        q.add(p);
      }
      out.push_back(q);
      continue;
    }
    // horizontal rule
    {
      std::string t = trim(ln);
      bool rule = t.size() >= 3;
      if (rule) {
        char c0 = t[0];
        if (c0 != '-' && c0 != '*' && c0 != '_') rule = false;
        for (char ch : t) {
          if (ch != c0) { rule = false; break; }
        }
      }
      if (rule) {
        out.push_back(el("hr", "md-hr"));
        ++i;
        continue;
      }
    }
    // table: a header row followed by a |---|---| separator
    if (!ln.empty() && ln[0] == '|' && i + 1 < lines.size() &&
        is_table_separator(lines[i + 1])) {
      auto split_row = [](const std::string& row) {
        std::vector<std::string> cells;
        std::string cur;
        std::string r = trim(row);
        if (!r.empty() && r[0] == '|') r.erase(0, 1);
        if (!r.empty() && r[r.size() - 1] == '|') r.erase(r.size() - 1);
        for (size_t k = 0; k < r.size(); ++k) {
          if (r[k] == '|' && (k == 0 || r[k - 1] != '\\')) {
            cells.push_back(trim(cur));
            cur.clear();
          } else {
            cur += r[k];
          }
        }
        cells.push_back(trim(cur));
        return cells;
      };
      std::vector<std::string> header = split_row(ln);
      i += 2;  // header + separator
      H table = el("table", "md-table");
      H thead = el("thead");
      H hrow = el("tr");
      for (size_t k = 0; k < header.size(); ++k) {
        H th = el("th");
        inline_md(th, header[k], on_link);
        hrow.add(th);
      }
      thead.add(hrow);
      table.add(thead);
      H tbody = el("tbody");
      while (i < lines.size() && !lines[i].empty() && lines[i][0] == '|') {
        std::vector<std::string> cells = split_row(lines[i]);
        H tr = el("tr");
        for (size_t k = 0; k < header.size(); ++k) {
          H td = el("td");
          inline_md(td, k < cells.size() ? cells[k] : std::string(), on_link);
          tr.add(td);
        }
        tbody.add(tr);
        ++i;
      }
      table.add(tbody);
      // The wrapper is the horizontal scroll container: a wide table scrolls on
      // its own instead of widening the transcript (which used to move the whole
      // answer column sideways).
      H twrap = el("div", "md-table-wrap");
      twrap.add(table);
      out.push_back(twrap);
      continue;
    }
    // blank
    if (trim(ln).empty()) {
      ++i;
      continue;
    }
    // paragraph: gather until blank / block start
    std::string para;
    while (i < lines.size()) {
      const std::string& l2 = lines[i];
      if (trim(l2).empty()) break;
      if (l2.size() >= 3 && l2.compare(0, 3, "```") == 0) break;
      if (!l2.empty() && l2[0] == '#') break;
      if (l2.size() >= 2 && (l2.compare(0, 2, "- ") == 0 || l2.compare(0, 2, "* ") == 0)) break;
      if (!para.empty()) para += "\n";
      para += l2;
      ++i;
    }
    H p = el("p", "md-p");
    inline_md(p, para, on_link);
    out.push_back(p);
  }
  return out;
}

// ---------- overlays ----------

H modal(const std::string& title, const std::vector<H>& body,
        const std::string& ok_label, EventFn on_ok, EventFn on_close) {
  H back = el("div", "modal-back");
  // Keyed + an explicit exit intent: the framework only animates a node OUT if
  // it can identify it across renders, and a modal that vanishes instantly
  // (while everything else fades) reads as a glitch.
  back.key("modal:" + title);
  back.anim(anim::kPop);
  back.exit(anim::kExitFade);
  // Losing focus dismisses: a press anywhere outside the box closes it. This is
  // the BACKDROP handler, and it is why the box must swallow clicks - a click on
  // the box bubbles up to here otherwise and closes the dialog the user is
  // filling in. The shell ALSO binds bind_outside_click on this node (see
  // UiApp::render): the backdrop only covers the viewport of the element that
  // rendered it, while a dialog opened from a tag page must still close when the
  // press lands on the tag strip above it.
  back.on(EventKind::kClick, [on_close](const std::string&) { on_close("backdrop"); });
  H box = el("div", "modal-box");
  box.on(EventKind::kClick, [](const std::string&) { /* swallow inside clicks */ });
  box.add(el("div", "modal-title").text(title));
  H content = el("div", "modal-body");
  for (size_t i = 0; i < body.size(); ++i) content.add(body[i]);
  box.add(content);
  H foot = el("div", "modal-foot");
  foot.add(button("取消", on_close));
  if (!ok_label.empty()) foot.add(button(ok_label, on_ok, "primary"));
  box.add(foot);
  back.add(box);
  return back;
}

H menu(const std::vector<std::string>& items, EventFn on_pick) {
  H m = el("div", "menu");
  for (size_t i = 0; i < items.size(); ++i) {
    H it = el("div", "menu-item");
    std::string label = items[i];
    it.on(EventKind::kClick, [on_pick, label](const std::string&) { on_pick(label); });
    it.text(items[i]);
    m.add(it);
  }
  return m;
}

}  // namespace comp
}  // namespace ui
}  // namespace fs
