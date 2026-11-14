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
            EventFn on_select, EventFn on_close, EventFn on_add,
            EventFn on_context = nullptr);

// Centered row of title buttons (home sub-navigation).
H title_tabs(const std::vector<std::string>& items, int active, EventFn on_select);

// Small in-panel tabs (side panel header).
H panel_tabs(const std::vector<TabSpec>& tabs, const std::string& active_key,
             EventFn on_select, EventFn on_close, EventFn on_add);

// ---------- forms ----------
H field(const std::string& label, const H& control, const std::string& hint = "");
// `on_enter` (optional) fires on Enter; used by single-line prompt inputs.
// `on_blur` (optional) fires with the field's value when focus leaves it, so a
// typed value also commits when the user clicks away instead of pressing Enter.
H text_input(const std::string& value, const std::string& placeholder,
             EventFn on_input, EventFn on_enter = nullptr,
             EventFn on_blur = nullptr);
H textarea(const std::string& value, const std::string& placeholder,
           EventFn on_input, EventFn on_enter, EventFn on_keydown = nullptr);
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
  std::string detail;    // body shown expanded (mono) - the RESULT
  // What the call ASKED FOR (its arguments), rendered as its own labeled section
  // ABOVE the result. read_file used to show only the returned JSON, so the block
  // never said which file was read ("只有返回json没有输入"). It is a separate
  // field because tool.update overwrites `detail` with the result.
  std::string input;
  bool expanded = false;
  bool ok = true;
  // The body is prose, not a tool payload: never JSON-highlight it just because
  // the first character happens to be `[` or `{` (thinking blocks).
  bool plain = false;
  // FontAwesome icon instead of the ok/err verdict (a thinking row must not show
  // a checkmark: reasoning is not a successful tool call).
  std::string icon;
  // Still executing. The row shows a "running" state and is completed in place
  // when the matching tool.update arrives (a tool that takes seconds otherwise
  // looks like it never started).
  bool running = false;
  // Stable id used to address the expanded body (scrolling it to the bottom).
  std::string key;
  // For a subagent tool call (subagent_spawn / follow_up), the agent it produced.
  // The row then offers "打开记录": the transcript must stay reachable after the
  // agent finished, and the work log is where the user sees the call happened.
  std::string subagent_id;
  // Bumped whenever any field above changes. `tool_block_cached` reuses the
  // rendered VNode while this (and the key) is unchanged: a turn with hundreds
  // of tool calls rebuilt EVERY row's VNode on every streaming delta, which is
  // seconds of allocation per frame - the "UI freezes during a tool-heavy turn".
  uint64_t rev = 0;
};
H tool_block(const ToolBlock& b, EventFn on_toggle,
             EventFn on_open_subagent = nullptr);
// tool_block with a bounded memoisation keyed by (key, rev, sizes, expanded,
// `gen`). The caller MUST keep `b.rev` current for every mutation, and MUST pass
// a `gen` token that changes whenever the surrounding structure can remove and
// re-add these rows (e.g. the work fold opening/closing). Reusing a VNode whose
// element was exit-animated away leaves the diff convinced it is still attached:
// the fold then opens exactly once and every later toggle renders nothing.
H tool_block_cached(const ToolBlock& b, EventFn on_toggle,
                    EventFn on_open_subagent, const std::string& gen);
// Drop every memoised tool row. MUST be called when the page they belong to is
// torn down (tab switch/open): the cached VNodes' elements are gone, and handing
// them back leaves rows with no attached DOM - clicks silently do nothing.
// The same applies to the message-node cache, which the shell prunes per session.
void tool_block_cache_clear();

// Whitespace-only text carries no content. Providers emit reasoning deltas that
// can be just "\n" or spaces; such a segment must not become a visible 思考 row.
bool text_is_blank(const std::string& s);

struct ChatMsg {
  // One reasoning segment. `after` is the number of tool calls already executed
  // when it started, so the work log can interleave thinking and tools in
  // ARRIVAL order (notes <96>). `open` is user/auto fold state; reasoning is
  // auto-folded when the segment ends.
  struct ThinkBlock {
    std::string text;
    size_t after = 0;
    bool open = true;
    bool streaming = false;
    uint64_t rev = 0;
  };
  std::string role;      // "user" | "assistant" | "system"
  std::string text;
  std::string time;      // small, hover-revealed
  bool streaming = false;
  // Reasoning text, when the provider returned it (or a stored turn carries it).
  // Rendered as a foldable block BEFORE the answer: it is context, not the
  // answer, and it is long enough to bury the answer if shown open.
  std::string thinking;
  bool thinking_open = false;
  // Reasoning segments in arrival order; rendered in the same stream as tools
  // and interim notes. Empty => legacy `thinking` (one block at the end).
  std::vector<ThinkBlock> thoughts;
  std::vector<ToolBlock> tools;  // flow blocks rendered after the text
  // Tool rows trimmed at LOAD time (a stored turn can hold thousands after a
  // long auto run). The full record is still on disk; the work log says how many
  // earlier rows are omitted. Only the unrendered prefix is dropped, never a DOM
  // state of a visible row.
  int tools_omitted = 0;
  // Between-tools narration of the turn, in order. It is NOT the answer - it is
  // the model thinking out loud while it works - so it belongs in the work log and
  // used to be dropped entirely (only the final round's text was kept).
  std::vector<std::string> interim;
  // Guidance the user injected while this turn was running. It is the USER's
  // voice, not model narration: rendered in its own block in the work log, never
  // appended to the answer or mixed with `interim`. `guidance_after` is parallel:
  // the number of tool calls already executed when it was injected, so the block
  // lands AFTER those tools instead of at the top of the turn.
  std::vector<std::string> guidance;
  std::vector<size_t> guidance_after;
  // Human-facing events about this turn (auto-continue reason, why it broke, a
  // model switch). Rendered as plain notes in the work log, after the tools.
  // They are NOT the answer and NOT model narration.
  std::vector<std::string> notices;
  // How many tool calls had already run when each note arrived (parallel to
  // `interim`). The work stream places note k immediately before tool
  // interim_after[k]; a round with several tool calls made the old "note index
  // == tool index" rule put a later thought above earlier tools. Empty (old
  // stored turns) falls back to the index.
  std::vector<size_t> interim_after;
  // Live text from a complete-step SUBAGENT while it works. It is process, not
  // the answer: rendered as a non-emphasis note inside the work log and never
  // appended to `text` (which is the main model's answer). Cleared when the
  // subagent reports; the full record stays in the subagent panel.
  std::string work_live;
  std::string work_live_id;
  // 工作过程: reasoning + narration + tool calls, ONE continuous stream above the
  // answer - no separate "工具调用" section (splitting them made the same turn read
  // as two unrelated things), and OPEN while the turn runs so you can watch the
  // work happen. It folds when the turn ends (the answer is what you reread; the
  // process is one click away), and a finished turn loaded from disk starts
  // folded.
  bool work_open = false;
  // Set by the user's own click on the fold. While a turn is RUNNING the log is
  // open regardless of `work_open`, which would otherwise re-open a fold the user
  // had just collapsed on the next repaint; after a click that decision wins
  // until the turn ends.
  bool work_touched = false;
  // Bumped on every fold TRANSITION (open→closed or closed→open). The tool rows
  // are memoised; a cached row may not be reused after the fold was closed,
  // because closing removes the row elements and a reused VNode would make the
  // diff believe they were still attached ("工作过程只能点开一次").
  uint64_t work_seq = 0;
  // When the turn ended and how long it took (ms since epoch; 0 = unknown).
  int64_t started_at = 0;
  int64_t ended_at = 0;
  // "Now" for the live elapsed counter of a streaming turn. The shell stamps it
  // on every render while anything streams; 0 means "do not show a live timer".
  int64_t now_ms = 0;
  std::string summary;           // turn summary (not folded, no border)
  // Project-relative path to this session's plan document. The work-log header
  // offers a direct "规划" link when set (no need to find the file elsewhere).
  std::string plan_path;
  // Why a turn stopped early (provider error, tool crash). A failed turn still
  // has its partial answer and work log; this line says what broke instead of
  // leaving a permanently "streaming" bubble or a silent truncation.
  std::string error;
  // Bumped by whoever mutates this message. The chat view memoises one rendered
  // VNode per message and reuses it while this number is unchanged: a session
  // can hold hundreds of turns, and re-parsing every markdown block and
  // rebuilding every node four times a second (while a reply streams) is pure
  // waste. A solidified turn keeps its rev forever and is never rebuilt again.
  uint64_t rev = 0;
  // Stable identity, independent of position in the vector. Loaded turns use
  // their ABSOLUTE index in the stored session, so prepending an older page
  // does not change the identity of anything already on screen (an index-based
  // key would shift every message and rebuild the whole transcript on every
  // page load). 0 = fall back to the position.
  int64_t uid = 0;
  // The stored turn's id. fork / rollback / forget all address a turn by this,
  // so it must survive the trip from disk to the message menu.
  std::string turn_id;
};

// One chat message. user -> right bubble; assistant -> article, no bubble;
// system -> centered dim line. Actions appear on hover (CSS).
// `on_turn_action` receives "fork" | "rollback" | "forget" for THIS turn; it is
// how a message becomes a navigation point ("branch from here").
// `on_context` receives "<x>\t<y>" on right-click; the shell uses it to open its
// own context menu (the browser's is suppressed by the DOM adapters).
// `on_link` receives the href of a clicked markdown link. The anchor has no
// real href (the engine would navigate the app window away); the shell decides
// what to do - open the browser, or a project file in the side panel.
H chat_message(const ChatMsg& m, EventFn on_copy, EventFn on_tool_toggle,
               EventFn on_think_toggle = nullptr, EventFn on_turn_action = nullptr,
               EventFn on_context = nullptr, EventFn on_link = nullptr,
               EventFn on_open_subagent = nullptr,
               // Offer 编辑并重发 on this message (the caller passes true only for
               // the newest user turn).
               bool can_edit = false);

// Stable identity for one rendered message: a keyed VNode is what lets the
// framework match (and therefore skip) a subtree across renders. `uid` must be
// stable across reordering (loaded turns use their absolute index), so
// prepending older history does not invalidate the cache for newer messages.
std::string chat_message_key(const std::string& sid, int64_t uid);

// Turn summary: NOT folded, no border (per user feedback).
H turn_summary(const std::string& text);

// Simple markdown-ish article rendering: fenced code, headings, lists, tables,
// blockquotes, rules, paragraphs, inline `code` / **bold** / *italic* / links.
// `on_copy_code` (optional) receives a code block's text when its copy button is
// pressed. `on_link` (optional) receives a link's TARGET when that anchor is
// clicked - bound per anchor, so a click anywhere else in the message can never
// be mistaken for a link.
std::vector<H> markdown_blocks(const std::string& text,
                               EventFn on_copy_code = nullptr,
                               EventFn on_link = nullptr);

// ---------- overlays ----------
H modal(const std::string& title, const std::vector<H>& body,
        const std::string& ok_label, EventFn on_ok, EventFn on_close);
H menu(const std::vector<std::string>& items, EventFn on_pick);

}  // namespace comp
}  // namespace ui
}  // namespace fs
