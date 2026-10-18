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
// Icon + label button. The label collapses to icon-only when the container is
// narrow (CSS container query), so toolbars stay usable on compact windows.
// `icon` is a Font Awesome class suffix, e.g. "paperclip" -> .fa-paperclip.
H icon_text_button(const std::string& icon, const std::string& label,
                   EventFn on_click, const std::string& variant = "");
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
// `on_enter` (optional) fires on Enter; used by single-line prompt inputs.
H text_input(const std::string& value, const std::string& placeholder,
             EventFn on_input, EventFn on_enter = nullptr);
H textarea(const std::string& value, const std::string& placeholder,
           EventFn on_input, EventFn on_enter);
H select(const std::vector<std::string>& options, int selected, EventFn on_change);
H checkbox(const std::string& label, bool checked, EventFn on_change);
H toggle(bool on, const std::string& label, EventFn on_change);
H search_box(const std::string& value, const std::string& placeholder, EventFn on_input);

// A labelled choice rendered as selectable rows: icon + title + one-line
// explanation. A plain <select> cannot carry the explanation, and a bare label
// ("permissive") means nothing to someone who has not read the docs - which is
// exactly what the permission and auto-mode settings need to say.
struct ChoiceOption {
  std::string value;   // value handed to on_pick
  std::string icon;    // Font Awesome class suffix ("" = none)
  std::string title;   // short label
  std::string desc;    // one-line explanation ("" = none)
};
H choice_list(const std::vector<ChoiceOption>& options, const std::string& selected,
              EventFn on_pick);

// ---------- display ----------
H badge(const std::string& text, const std::string& variant = "");
H spinner();
H progress(int pct);
H empty_state(const std::string& hint, const std::string& action_label, EventFn on_action);
H card(const std::string& cls, const std::vector<H>& children);
H section_title(const std::string& text);
// small dim explanation line under a section title
H hint(const std::string& text);
H key_value(const std::string& k, const std::string& v);

// ---------- model icons ----------
// Brand mark for a model. `icon_key` is a lobe-icons key (see
// web/icons/lobe/manifest.json); when it is empty the model NAME is matched to
// a brand here, so a model added from an API listing still gets the right mark
// instead of a blank square. Falls back to a generic dot when nothing matches.
H model_icon(const std::string& icon_key, const std::string& model_id,
             const std::string& extra_cls = "");

// File-type icon for a path or a bare filename, matched on the exact name first
// (Dockerfile has no extension) and then on the extension. Falls back to the
// generic file icon.
H file_icon(const std::string& name, bool is_dir);

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
  // Reasoning text, when the provider returned it (or a stored turn carries it).
  // Rendered as a foldable block BEFORE the answer: it is context, not the
  // answer, and it is long enough to bury the answer if shown open.
  std::string thinking;
  bool thinking_open = false;
  std::vector<ToolBlock> tools;  // flow blocks rendered after the text
  // 工作过程: reasoning + tool calls folded into ONE group above the answer.
  // A turn with a dozen tool blocks used to be a wall of rows; the answer is
  // what you read, the process is what you open when curious.
  bool work_open = false;
  // When the turn ended and how long it took (ms since epoch; 0 = unknown).
  int64_t started_at = 0;
  int64_t ended_at = 0;
  std::string summary;           // turn summary (not folded, no border)
};

// One chat message. user -> right bubble; assistant -> article, no bubble;
// system -> centered dim line. Actions appear on hover (CSS).
H chat_message(const ChatMsg& m, EventFn on_copy, EventFn on_tool_toggle,
               EventFn on_think_toggle = nullptr);

// Turn summary: NOT folded, no border (per user feedback).
H turn_summary(const std::string& text);

// Simple markdown-ish article rendering: fenced code, headings, lists,
// paragraphs, inline `code` / **bold** / *italic*. `on_copy_code` (optional)
// receives a code block's text when its copy button is pressed.
std::vector<H> markdown_blocks(const std::string& text,
                               EventFn on_copy_code = nullptr);

// ---------- overlays ----------
H modal(const std::string& title, const std::vector<H>& body,
        const std::string& ok_label, EventFn on_ok, EventFn on_close);
H menu(const std::vector<std::string>& items, EventFn on_pick);

}  // namespace comp
}  // namespace ui
}  // namespace fs
