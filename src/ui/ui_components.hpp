// ui_components.hpp - Composers (rules/addon3.md "架构/Composers")
//
// Reusable UI pieces built purely from the VNode builder. Everything here is
// a function returning H, so components nest and compose freely and the app
// stays declarative. No component owns DOM state; state lives in the app's
// State<> containers and flows in as arguments.
#pragma once

#include <string>
#include <vector>

#include "ui_vdom.hpp"

namespace fs {
namespace ui {
namespace comp {

// ---------- buttons ----------
// variant: "" (default) | "primary" | "ghost" | "danger" | "link"
H button(const std::string& label, EventFn on_click, const std::string& variant = "");
H icon_button(const std::string& glyph, const std::string& tip, EventFn on_click);
// title-like small button (探索/项目/用量统计): color + underline only, no bg
H title_button(const std::string& label, bool active, EventFn on_click);

// ---------- tabs ----------
struct TabSpec {
  std::string key;
  std::string title;
  bool closable = false;
  bool dirty = false;   // shows a dot before the title
  int badge = -1;       // >=0 shows a count badge
};

// Full tab strip: tabs + "+" button. Active tab gets the raised look.
H tab_strip(const std::vector<TabSpec>& tabs, const std::string& active_key,
            EventFn on_select, EventFn on_close, EventFn on_add);

// Centered row of title buttons (home sub-navigation).
H title_tabs(const std::vector<std::string>& items, int active, EventFn on_select);

// Small in-panel tabs (side panel header).
H panel_tabs(const std::vector<TabSpec>& tabs, const std::string& active_key,
             EventFn on_select, EventFn on_close, EventFn on_add);

// ---------- forms ----------
H field(const std::string& label, const H& control, const std::string& hint = "");
H text_input(const std::string& value, const std::string& placeholder, EventFn on_input);
H textarea(const std::string& value, const std::string& placeholder,
           EventFn on_input, EventFn on_enter);
H select(const std::vector<std::string>& options, int selected, EventFn on_change);
H checkbox(const std::string& label, bool checked, EventFn on_change);
H toggle(bool on, const std::string& label, EventFn on_change);
H search_box(const std::string& value, const std::string& placeholder, EventFn on_input);

// ---------- display ----------
H badge(const std::string& text, const std::string& variant = "");
H spinner();
H progress(int pct);
H empty_state(const std::string& hint, const std::string& action_label, EventFn on_action);
H card(const std::string& cls, const std::vector<H>& children);
H section_title(const std::string& text);
H key_value(const std::string& k, const std::string& v);

// ---------- chat ----------

// Foldable tool/flow block (details-like). `summary` line always visible.
struct ToolBlock {
  std::string name;
  std::string summary;   // one-line description shown collapsed
  std::string detail;    // body shown expanded (mono)
  bool expanded = false;
  bool ok = true;
};
H tool_block(const ToolBlock& b, EventFn on_toggle);

struct ChatMsg {
  std::string role;      // "user" | "assistant" | "system"
  std::string text;
  std::string time;      // small, hover-revealed
  bool streaming = false;
  std::vector<ToolBlock> tools;  // flow blocks rendered after the text
  std::string summary;           // turn summary (not folded, no border)
};

// One chat message. user -> right bubble; assistant -> article, no bubble;
// system -> centered dim line. Actions appear on hover (CSS).
H chat_message(const ChatMsg& m, EventFn on_copy, EventFn on_tool_toggle);

// Turn summary: NOT folded, no border (per user feedback).
H turn_summary(const std::string& text);

// Simple markdown-ish article rendering: fenced code, headings, lists,
// paragraphs, inline `code` / **bold** / *italic*.
std::vector<H> markdown_blocks(const std::string& text);

// ---------- overlays ----------
H modal(const std::string& title, const std::vector<H>& body,
        const std::string& ok_label, EventFn on_ok, EventFn on_close);
H menu(const std::vector<std::string>& items, EventFn on_pick);

}  // namespace comp
}  // namespace ui
}  // namespace fs
