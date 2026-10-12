// ui_components.cpp - Composers implementation.
#include "ui_components.hpp"

#include <cstdio>

namespace fs {
namespace ui {
namespace comp {

namespace {

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

// inline markdown: `code`, **bold**, *italic* -> spans
void inline_md(H& host, const std::string& text) {
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

// ---------- buttons ----------

H button(const std::string& label, EventFn on_click, const std::string& variant) {
  H b = el("button", variant.empty() ? "btn" : ("btn " + variant));
  b.attr("type", "button");
  b.on(EventKind::kClick, on_click);
  b.text(label);
  return b;
}

H icon_button(const std::string& glyph, const std::string& tip, EventFn on_click) {
  H b = el("button", "icon-btn");
  b.attr("type", "button");
  b.attr("title", tip);
  b.on(EventKind::kClick, on_click);
  b.text(glyph);
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
            EventFn on_select, EventFn on_close, EventFn on_add) {
  H bar = el("div", "tabs");
  for (size_t i = 0; i < tabs.size(); ++i) {
    const TabSpec& t = tabs[i];
    H tab = el("div", t.key == active_key ? "tab active" : "tab");
    tab.key(t.key);  // stable identity across re-renders
    tab.attr("data-key", t.key);
    std::string k = t.key;
    tab.on(EventKind::kClick, [on_select, k](const std::string&) { on_select(k); });
    if (t.dirty) tab.add(el("span", "dot"));
    tab.add(el("span", "tab-title").text(t.title));
    if (t.badge >= 0) tab.add(badge(itos(t.badge), "tab-badge"));
    if (t.closable) {
      H x = el("span", "tab-close");
      x.text("\u00d7");  // multiplication sign
      x.on(EventKind::kClick, [on_close, k](const std::string&) { on_close(k); });
      tab.add(x);
    }
    bar.add(tab);
  }
  H add = el("div", "tab add");
  add.attr("title", "新建标签页");
  add.on(EventKind::kClick, on_add);
  add.text("+");
  bar.add(add);
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
    std::string k = t.key;
    tab.on(EventKind::kClick, [on_select, k](const std::string&) { on_select(k); });
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

H text_input(const std::string& value, const std::string& placeholder, EventFn on_input) {
  H i = el("input", "text-input");
  i.attr("type", "text");
  i.attr("value", value);
  if (!placeholder.empty()) i.attr("placeholder", placeholder);
  i.on(EventKind::kInput, on_input);
  return i;
}

H textarea(const std::string& value, const std::string& placeholder,
           EventFn on_input, EventFn on_enter) {
  H t = el("textarea", "text-area");
  if (!placeholder.empty()) t.attr("placeholder", placeholder);
  t.on(EventKind::kInput, on_input);
  t.on(EventKind::kKeyEnter, on_enter);
  // value via text child so patching updates it
  t.text(value);
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

H key_value(const std::string& k, const std::string& v) {
  H r = el("div", "kv");
  r.add(el("span", "kv-k").text(k));
  r.add(el("span", "kv-v").text(v));
  return r;
}

// ---------- chat ----------

H chat_message(const ChatMsg& m, EventFn on_copy, EventFn on_tool_toggle) {
  H wrap = el("div", "msg " + m.role);
  if (m.role == "system") {
    wrap.add(el("div", "sys").text(m.text));
    return wrap;
  }
  if (m.role == "user") {
    // user input: pure bubble, no name, no extras
    wrap.add(el("div", "bubble").text(m.text));
    H meta = el("div", "meta");
    if (!m.time.empty()) meta.add(el("span", "time").text(m.time));
    wrap.add(meta);
    return wrap;
  }
  // assistant: article-like, no bubble, no name
  H body = el("div", "article");
  std::vector<H> blocks = markdown_blocks(m.text);
  for (size_t i = 0; i < blocks.size(); ++i) body.add(blocks[i]);
  if (m.streaming) body.add(el("span", "caret"));
  wrap.add(body);
  // flow blocks (tool calls etc.) fold, summary does not
  for (size_t i = 0; i < m.tools.size(); ++i) {
    wrap.add(tool_block(m.tools[i], on_tool_toggle));
  }
  if (!m.summary.empty()) wrap.add(turn_summary(m.summary));
  H meta = el("div", "meta");
  if (!m.time.empty()) meta.add(el("span", "time").text(m.time));
  H cp = el("button", "mini-btn");
  cp.attr("type", "button");
  cp.text("复制");
  cp.on(EventKind::kClick, on_copy);
  meta.add(cp);
  wrap.add(meta);
  return wrap;
}

H tool_block(const ToolBlock& b, EventFn on_toggle) {
  H d = el("div", b.expanded ? "tool-block open" : "tool-block");
  H head = el("div", "tb-head");
  head.on(EventKind::kClick, [on_toggle](const std::string&) { on_toggle("toggle"); });
  head.add(el("span", b.ok ? "tb-ico ok" : "tb-ico err").text(b.ok ? "\u2713" : "\u2717"));
  head.add(el("span", "tb-name").text(b.name));
  head.add(el("span", "tb-sum").text(b.summary));
  head.add(el("span", "tb-caret").text(b.expanded ? "\u25be" : "\u25b8"));
  d.add(head);
  if (b.expanded && !b.detail.empty()) {
    H pre = el("pre", "tb-body");
    pre.text(b.detail);
    d.add(pre);
  }
  return d;
}

H turn_summary(const std::string& text) {
  // not folded, no border
  H s = el("div", "turn-summary");
  s.add(el("span", "ts-label").text("本轮要点"));
  s.add(el("div", "ts-text").text(text));
  return s;
}

// ---------- markdown-ish ----------

std::vector<H> markdown_blocks(const std::string& text) {
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
      H pre = el("pre", "code-block");
      if (!lang.empty()) pre.attr("data-lang", lang);
      pre.text(code);
      out.push_back(pre);
      continue;
    }
    // heading
    if (!ln.empty() && ln[0] == '#') {
      size_t n = 0;
      while (n < ln.size() && ln[n] == '#') ++n;
      if (n <= 6 && n < ln.size() && ln[n] == ' ') {
        std::string lvl = "h" + itos(static_cast<int>(n));
        H h = el(lvl, "md-h");
        inline_md(h, trim(ln.substr(n + 1)));
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
        inline_md(li, trim(lines[i].substr(2)));
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
        inline_md(li, trim(lines[i].substr(2)));
        ol.add(li);
        ++i;
      }
      out.push_back(ol);
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
    inline_md(p, para);
    out.push_back(p);
  }
  return out;
}

// ---------- overlays ----------

H modal(const std::string& title, const std::vector<H>& body,
        const std::string& ok_label, EventFn on_ok, EventFn on_close) {
  H back = el("div", "modal-back");
  back.on(EventKind::kClick, [on_close](const std::string&) { on_close("backdrop"); });
  H box = el("div", "modal-box");
  box.on(EventKind::kClick, [](const std::string&) {});  // swallow
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
