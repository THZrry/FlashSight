// fs_vt.hpp - terminal engine: VT/ANSI parser + screen (cell) buffer.
//
// A terminal is not a text stream, it is a *grid of cells*. Rendering the raw
// byte stream (what we did before) leaks escape sequences into the output and
// makes any full-screen program unreadable. This module turns the byte stream
// into a screen the UI can draw: characters, colors, attributes, a cursor.
//
// Scope: the sequences real shells and TUI programs actually emit - C0 controls,
// CSI (cursor/erase/insert/delete/scroll/SGR/modes), OSC (title), DEC special
// graphics (the box-drawing set ncurses switches to), UTF-8 with wide (CJK)
// characters, the alternate screen, and DSR replies (the shell asks where the
// cursor is and waits for an answer, so replies are collected for the caller to
// write back to the process).
//
// Deliberately dependency-free and allocation-light: it compiles into both the
// native and the wasm build, and the UI rebuilds the visible grid on every
// render.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fs {
namespace vt {

// -1 = "use the theme default" (so light/dark can pick their own base colors)
const int32_t kColorDefault = -1;

enum CellAttr {
  kAttrBold = 1,
  kAttrDim = 2,
  kAttrItalic = 4,
  kAttrUnderline = 8,
  kAttrBlink = 16,
  kAttrReverse = 32,
  kAttrStrike = 64,
};

enum CellFlag {
  kFlagWide = 1,      // lead cell of a double-width character
  kFlagWideTail = 2,  // its (skipped) second column
};

struct Cell {
  uint32_t ch = ' ';
  int32_t fg = kColorDefault;
  int32_t bg = kColorDefault;
  uint8_t attr = 0;
  uint8_t flags = 0;
  bool empty() const {
    return ch == ' ' && fg == kColorDefault && bg == kColorDefault && attr == 0;
  }
};

// xterm-256 palette entry -> 0xRRGGBB (indices 0-15 are the ANSI colors).
uint32_t palette256(int idx);

// Display width of a codepoint: 2 for CJK/fullwidth, 1 for normal, 0 for
// combining marks (which we drop - a Cell holds one codepoint).
int char_width(uint32_t cp);

// Encode one key press as the bytes a terminal must send the child process.
//
// This is terminal knowledge, not UI knowledge: Ctrl+C is not the character 'c'
// (it is 0x03), Shift+Tab is not Tab, and the arrow keys switch between CSI and
// SS3 depending on the application-cursor-keys mode the program set. The UI
// reports which key was pressed plus its modifiers; this turns that into the
// wire format. Returns an empty string for keys a terminal does not report.
//
// `key` uses DOM KeyboardEvent.key names ("a", "Enter", "ArrowUp", "F5", ...).
std::string encode_key(const std::string& key, bool ctrl, bool alt, bool shift,
                       bool app_cursor_keys);

// Wrap pasted text: with bracketed paste enabled the child expects it between
// ESC[200~ and ESC[201~ (that is how a shell distinguishes a paste from typing,
// so it can skip auto-indent and not run the text as commands).
std::string encode_paste(const std::string& text, bool bracketed);

class Screen {
 public:
  Screen(int cols = 100, int rows = 30);

  // Wipe the grid, reset attributes and cursor (ESC c / fresh terminal).
  void reset();
  // Resize keeping the top-left of the content (terminals reflow, we don't).
  void resize(int cols, int rows);

  // Feed a chunk of the process's output. Chunks may split escape sequences and
  // multi-byte characters; parser state carries over between calls.
  void feed(const std::string& bytes);

  int cols() const { return cols_; }
  int rows() const { return rows_; }
  const Cell& cell(int x, int y) const;
  int cursor_x() const { return cur_.x; }
  int cursor_y() const { return cur_.y; }
  bool cursor_visible() const { return cursor_visible_; }
  bool alt_screen() const { return alt_; }
  // Application cursor keys (DECCKM): arrows must be sent as SS3 (ESC O A).
  bool app_cursor_keys() const { return app_cursor_; }
  bool bracketed_paste() const { return bracketed_paste_; }
  const std::string& title() const { return title_; }
  bool bell() const { return bell_; }
  void clear_bell() { bell_ = false; }

  // Bytes the terminal owes the process (DSR replies). The caller writes them
  // back to the process; a shell that never gets its reply can hang.
  std::string take_reply();

  // The grid as plain text (trailing blanks trimmed, one line per row). Used
  // for the "all terminals" overview and for copy.
  std::string text() const;

 private:
  struct Cursor {
    int x = 0, y = 0;
    int32_t fg = kColorDefault, bg = kColorDefault;
    uint8_t attr = 0;
    bool wrap_pending = false;
    bool origin = false;
  };

  // ---- grid helpers ----
  Cell* at(int x, int y);
  const Cell* at(int x, int y) const;
  void clear_cells(int x0, int y0, int x1, int y1);
  void scroll_up(int top, int bot, int n);
  void scroll_down(int top, int bot, int n);
  void line_feed();
  void reverse_index();
  void put(uint32_t cp);

  // ---- parser ----
  enum State { kGround, kEsc, kCsi, kOsc, kOscEsc, kCharset, kEscHash };
  void exec_control(unsigned char b);
  void exec_escape(unsigned char b);
  void exec_csi(unsigned char final_byte);
  void exec_osc(const std::string& s);
  void apply_sgr();
  void decode_utf8(unsigned char b);
  void emit_codepoint(uint32_t cp);
  int param(size_t i, int def) const;
  bool has_param(size_t i) const;

  int cols_, rows_;
  std::vector<Cell> cells_;
  Cursor cur_;
  Cursor saved_;

  // alternate screen (vim/less/top): full second buffer + cursor
  std::vector<Cell> alt_cells_;
  Cursor alt_cur_;
  bool alt_ = false;

  int scroll_top_ = 0, scroll_bot_ = 0;  // DECSTBM region (inclusive)
  bool autowrap_ = true;
  bool cursor_visible_ = true;
  bool app_cursor_ = false;
  bool bracketed_paste_ = false;
  int last_ch_ = ' ';  // for CSI b (REP)

  State state_ = kGround;
  std::string params_;   // raw CSI parameter bytes
  std::string inter_;    // CSI intermediate bytes
  std::vector<int> ps_;  // parsed params (-1 = omitted)
  bool priv_ = false;    // CSI private prefix ('?', '<', '=', '>')
  char priv_ch_ = 0;
  std::string osc_;      // OSC payload
  bool charset_g0_g1_ = false;  // which set ESC ( / ESC ) selects
  bool g0_graphics_ = false;    // G0 = DEC special graphics
  bool g1_graphics_ = false;
  bool shift_g1_ = false;       // SO/SI (rarely used)

  std::string utf8_;     // pending multi-byte input
  std::string reply_;    // DSR answers
  std::string title_;
  bool bell_ = false;
};

}  // namespace vt
}  // namespace fs
