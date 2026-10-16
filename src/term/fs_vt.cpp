// fs_vt.cpp - VT/ANSI parser + screen buffer.
//
// The parser is a small state machine over bytes (ground / escape / CSI / OSC /
// charset selection). It is fed arbitrary chunks, so a sequence or a multi-byte
// character may be split across calls; all state lives in the Screen.
#include "fs_vt.hpp"

#include <cstdio>
#include <cstring>

namespace fs {
namespace vt {

namespace {

// How much history a terminal keeps. 2000 lines is what most terminal emulators
// default to and is plenty to scroll back through a build log.
const size_t kScrollbackMax = 2000;

// ANSI colors 0-15: the VS Code dark palette (readable on a dark terminal,
// which is what a terminal panel is regardless of the app theme).
const uint32_t kAnsi16[16] = {
    0x000000, 0xcd3131, 0x0dbc79, 0xe5e510, 0x2472c8, 0xbc3fbc, 0x11a8cd, 0xe5e5e5,
    0x666666, 0xf14c4c, 0x23d18b, 0xf5f543, 0x3b8eea, 0xd670d6, 0x29b8db, 0xe5e5e5,
};

// DEC special graphics: when a program selects this set (ESC ( 0), 0x60-0x7e
// become box-drawing/symbol characters. ncurses and `tree` rely on it.
const uint32_t kGraphics[0x7f - 0x60 + 1] = {
    0x25C6, 0x2592, 0x2409, 0x240C, 0x240D, 0x240A, 0x00B0, 0x00B1,  // ` a b c d e f g
    0x2424, 0x240B, 0x2518, 0x2510, 0x250C, 0x2514, 0x253C, 0x23BA,  // h i j k l m n o
    0x23BB, 0x2500, 0x23BC, 0x23BD, 0x251C, 0x2524, 0x2534, 0x252C,  // p q r s t u v w
    0x2502, 0x2264, 0x2265, 0x03C0, 0x2260, 0x00A3, 0x00B7,          // x y z { | } ~
};

void append_utf8(std::string* out, uint32_t cp) {
  if (cp < 0x80) {
    out->push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

// Split CSI params on both ';' and ':' (the colon form carries sub-parameters,
// e.g. SGR 38:2::r:g:b). An omitted parameter becomes -1 so callers can tell
// "missing" from "explicit zero".
void parse_params(const std::string& s, std::vector<int>* out) {
  out->clear();
  size_t i = 0;
  while (true) {
    size_t e = s.find_first_of(";:", i);
    if (e == std::string::npos) e = s.size();
    std::string tok = s.substr(i, e - i);
    if (tok.empty()) {
      out->push_back(-1);
    } else {
      int v = 0;
      for (size_t k = 0; k < tok.size(); ++k) {
        if (tok[k] < '0' || tok[k] > '9') { v = -1; break; }
        v = v * 10 + (tok[k] - '0');
        if (v > 100000) v = 100000;
      }
      out->push_back(v);
    }
    if (e == s.size()) break;
    i = e + 1;
  }
}

}  // namespace

uint32_t palette256(int idx) {
  if (idx < 0) return kAnsi16[7];
  if (idx < 16) return kAnsi16[idx];
  if (idx < 232) {
    int n = idx - 16;
    int r = n / 36, g = (n / 6) % 6, b = n % 6;
    static const int steps[6] = {0, 95, 135, 175, 215, 255};
    return (static_cast<uint32_t>(steps[r]) << 16) |
           (static_cast<uint32_t>(steps[g]) << 8) |
           static_cast<uint32_t>(steps[b]);
  }
  if (idx < 256) {
    uint32_t v = static_cast<uint32_t>(8 + (idx - 232) * 10);
    return (v << 16) | (v << 8) | v;
  }
  return kAnsi16[7];
}

int char_width(uint32_t cp) {
  if (cp == 0) return 0;
  // combining marks and other zero-width forms: we drop them (a Cell holds one
  // codepoint, so there is nowhere to attach a mark)
  if ((cp >= 0x0300 && cp <= 0x036F) || (cp >= 0x1AB0 && cp <= 0x1AFF) ||
      (cp >= 0x1DC0 && cp <= 0x1DFF) || (cp >= 0x20D0 && cp <= 0x20FF) ||
      (cp >= 0xFE00 && cp <= 0xFE0F) || (cp >= 0xFE20 && cp <= 0xFE2F) ||
      cp == 0x200B || cp == 0x200C || cp == 0x200D || cp == 0xFEFF) {
    return 0;
  }
  // East Asian Wide / Fullwidth ranges (covers CJK, Hangul, fullwidth forms)
  if ((cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0x303E) ||
      (cp >= 0x3041 && cp <= 0x33FF) || (cp >= 0x3400 && cp <= 0x4DBF) ||
      (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0xA000 && cp <= 0xA4CF) ||
      (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
      (cp >= 0xFE30 && cp <= 0xFE6F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
      (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x1F300 && cp <= 0x1F64F) ||
      (cp >= 0x1F900 && cp <= 0x1F9FF) || (cp >= 0x20000 && cp <= 0x3FFFD)) {
    return 2;
  }
  return 1;
}

// ===========================================================================
// key encoding (UI key press -> bytes for the child process)
// ===========================================================================
std::string encode_key(const std::string& key, bool ctrl, bool alt, bool shift,
                       bool app_cursor_keys) {
  if (key.empty()) return std::string();

  // SS3 (ESC O x) vs CSI (ESC [ x): programs that set DECCKM (application cursor
  // keys) expect the former, and sending the wrong one breaks readline/vi.
  const char* ss3 = app_cursor_keys ? "\x1bO" : "\x1b[";
  std::string out;

  if (key == "Enter" || key == "NumpadEnter") {
    out = "\r";
  } else if (key == "Backspace") {
    out = "\x7f";
  } else if (key == "Tab") {
    out = shift ? "\x1b[Z" : "\t";
  } else if (key == "Escape") {
    out = "\x1b";
  } else if (key == "ArrowUp") {
    out = std::string(ss3) + "A";
  } else if (key == "ArrowDown") {
    out = std::string(ss3) + "B";
  } else if (key == "ArrowRight") {
    out = std::string(ss3) + "C";
  } else if (key == "ArrowLeft") {
    out = std::string(ss3) + "D";
  } else if (key == "Home") {
    out = std::string(ss3) + "H";
  } else if (key == "End") {
    out = std::string(ss3) + "F";
  } else if (key == "Insert") {
    out = "\x1b[2~";
  } else if (key == "Delete") {
    out = "\x1b[3~";
  } else if (key == "PageUp") {
    out = "\x1b[5~";
  } else if (key == "PageDown") {
    out = "\x1b[6~";
  } else if (key.size() >= 2 && key[0] == 'F' && key[1] >= '1' && key[1] <= '9') {
    int n = 0;
    for (size_t i = 1; i < key.size(); ++i) {
      if (key[i] < '0' || key[i] > '9') { n = 0; break; }
      n = n * 10 + (key[i] - '0');
    }
    // F1-F4 are SS3 P..S; F5+ are CSI <n>~
    static const char kF14[] = "PQRS";
    if (n >= 1 && n <= 4) {
      out = std::string("\x1bO") + kF14[n - 1];
    } else {
      static const int kCodes[] = {0, 0, 0, 0, 0, 15, 17, 18, 19, 20, 21, 23, 24};
      if (n >= 5 && n <= 12) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "\x1b[%d~", kCodes[n]);
        out = buf;
      }
    }
  } else if (key.size() == 1) {
    unsigned char c = static_cast<unsigned char>(key[0]);
    if (ctrl) {
      // Ctrl+letter -> 0x01..0x1a; the punctuation cases a terminal defines
      if (c >= 'a' && c <= 'z') {
        out = std::string(1, static_cast<char>(c - 'a' + 1));
      } else if (c >= 'A' && c <= 'Z') {
        out = std::string(1, static_cast<char>(c - 'A' + 1));
      } else if (c == ' ') {
        out = std::string(1, '\0');
      } else if (c == '[') {
        out = "\x1b";
      } else if (c == '\\') {
        out = "\x1c";
      } else if (c == ']') {
        out = "\x1d";
      } else if (c == '^') {
        out = "\x1e";
      } else if (c == '_') {
        out = "\x1f";
      } else if (c == '@') {
        out = std::string(1, '\0');
      } else if (c == '?') {
        out = "\x7f";
      } else {
        out = key;
      }
    } else if (c < 0x80) {
      out = key;
    } else {
      out = key;  // already UTF-8 from the DOM
    }
    // Alt is the ESC prefix (that is what a terminal does, and readline reads
    // it as meta).
    if (alt && !out.empty()) out = std::string("\x1b") + out;
    return out;
  } else {
    // named key we do not report (Shift, Control, ...) or a multi-char string
    if (key == "Shift" || key == "Control" || key == "Alt" || key == "Meta" ||
        key == "CapsLock" || key == "NumLock" || key == "ScrollLock" ||
        key == "ContextMenu" || key == "Dead" || key == "Unidentified") {
      return std::string();
    }
    out = key;  // pasted text / IME output arrives as a multi-char key
    if (alt) out = std::string("\x1b") + out;
    return out;
  }

  if (alt) out = std::string("\x1b") + out;
  return out;
}

std::string encode_paste(const std::string& text, bool bracketed) {
  if (text.empty()) return text;
  if (!bracketed) return text;
  // normalize line endings: a terminal sends CR for Enter, and the child's
  // line editor is what turns that into a newline
  std::string body;
  body.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\n' && (i == 0 || text[i - 1] != '\r')) {
      body.push_back('\r');
    } else {
      body.push_back(text[i]);
    }
  }
  return "\x1b[200~" + body + "\x1b[201~";
}

// ===========================================================================
// Screen
// ===========================================================================
Screen::Screen(int cols, int rows) {
  cols_ = cols > 0 ? cols : 80;
  rows_ = rows > 0 ? rows : 24;
  cells_.assign(static_cast<size_t>(cols_) * rows_, Cell());
  scroll_bot_ = rows_ - 1;
}

void Screen::reset() {
  cells_.assign(static_cast<size_t>(cols_) * rows_, Cell());
  scrollback_.clear();
  alt_cells_.clear();
  alt_ = false;
  cur_ = Cursor();
  saved_ = Cursor();
  scroll_top_ = 0;
  scroll_bot_ = rows_ - 1;
  autowrap_ = true;
  cursor_visible_ = true;
  app_cursor_ = false;
  bracketed_paste_ = false;
  state_ = kGround;
  params_.clear();
  inter_.clear();
  ps_.clear();
  priv_ = false;
  osc_.clear();
  utf8_.clear();
  g0_graphics_ = g1_graphics_ = shift_g1_ = false;
  last_ch_ = ' ';
  title_.clear();
}

void Screen::resize(int cols, int rows) {
  if (cols <= 0 || rows <= 0) return;
  if (cols == cols_ && rows == rows_) return;
  std::vector<Cell> neu(static_cast<size_t>(cols) * rows, Cell());
  int cw = cols < cols_ ? cols : cols_;
  int ch = rows < rows_ ? rows : rows_;
  for (int y = 0; y < ch; ++y) {
    for (int x = 0; x < cw; ++x) {
      neu[static_cast<size_t>(y) * cols + x] = cells_[static_cast<size_t>(y) * cols_ + x];
    }
  }
  cells_.swap(neu);
  cols_ = cols;
  rows_ = rows;
  scroll_top_ = 0;
  scroll_bot_ = rows_ - 1;
  if (cur_.x >= cols_) cur_.x = cols_ - 1;
  if (cur_.y >= rows_) cur_.y = rows_ - 1;
  cur_.wrap_pending = false;
}

const Cell& Screen::cell(int x, int y) const {
  static const Cell kBlank;
  const Cell* c = at(x, y);
  return c ? *c : kBlank;
}

const Cell& Screen::line_cell(int line, int x) const {
  static const Cell kBlank;
  int hist = static_cast<int>(scrollback_.size());
  if (x < 0 || x >= cols_) return kBlank;
  if (line < 0 || line >= hist + rows_) return kBlank;
  if (line < hist) {
    const std::vector<Cell>& l = scrollback_[static_cast<size_t>(line)];
    return x < static_cast<int>(l.size()) ? l[static_cast<size_t>(x)] : kBlank;
  }
  return cell(x, line - hist);
}

Cell* Screen::at(int x, int y) {
  if (x < 0 || y < 0 || x >= cols_ || y >= rows_) return nullptr;
  return &cells_[static_cast<size_t>(y) * cols_ + x];
}

const Cell* Screen::at(int x, int y) const {
  if (x < 0 || y < 0 || x >= cols_ || y >= rows_) return nullptr;
  return &cells_[static_cast<size_t>(y) * cols_ + x];
}

void Screen::clear_cells(int x0, int y0, int x1, int y1) {
  if (x0 > x1 || y0 > y1) return;
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 > cols_ - 1) x1 = cols_ - 1;
  if (y1 > rows_ - 1) y1 = rows_ - 1;
  for (int y = y0; y <= y1; ++y) {
    for (int x = x0; x <= x1; ++x) {
      Cell* c = at(x, y);
      if (!c) continue;
      // erase uses the current background (BCE), like a real terminal
      c->ch = ' ';
      c->fg = kColorDefault;
      c->bg = cur_.bg;
      c->attr = 0;
      c->flags = 0;
    }
  }
}

void Screen::scroll_up(int top, int bot, int n) {
  if (top > bot || n <= 0) return;
  if (top < 0) top = 0;
  if (bot > rows_ - 1) bot = rows_ - 1;
  int height = bot - top + 1;
  if (n > height) n = height;
  // Lines leaving the top of the FULL screen become history (a scroll region
  // that starts below row 0 is a program drawing inside a window, so its lines
  // are not history - they are still on screen).
  if (top == 0 && !alt_) {
    for (int y = 0; y < n; ++y) {
      std::vector<Cell> line;
      line.reserve(static_cast<size_t>(cols_));
      for (int x = 0; x < cols_; ++x) line.push_back(*at(x, y));
      scrollback_.push_back(line);
    }
    if (scrollback_.size() > kScrollbackMax) {
      scrollback_.erase(scrollback_.begin(),
                        scrollback_.begin() +
                            static_cast<long>(scrollback_.size() - kScrollbackMax));
    }
  }
  for (int y = top; y <= bot - n; ++y) {
    for (int x = 0; x < cols_; ++x) {
      *at(x, y) = *at(x, y + n);
    }
  }
  clear_cells(0, bot - n + 1, cols_ - 1, bot);
}

void Screen::scroll_down(int top, int bot, int n) {
  if (top > bot || n <= 0) return;
  if (top < 0) top = 0;
  if (bot > rows_ - 1) bot = rows_ - 1;
  int height = bot - top + 1;
  if (n > height) n = height;
  for (int y = bot; y >= top + n; --y) {
    for (int x = 0; x < cols_; ++x) {
      *at(x, y) = *at(x, y - n);
    }
  }
  clear_cells(0, top, cols_ - 1, top + n - 1);
}

void Screen::line_feed() {
  cur_.wrap_pending = false;
  if (cur_.y == scroll_bot_) {
    scroll_up(scroll_top_, scroll_bot_, 1);
  } else if (cur_.y < rows_ - 1) {
    ++cur_.y;
  }
}

void Screen::reverse_index() {
  cur_.wrap_pending = false;
  if (cur_.y == scroll_top_) {
    scroll_down(scroll_top_, scroll_bot_, 1);
  } else if (cur_.y > 0) {
    --cur_.y;
  }
}

void Screen::put(uint32_t cp) {
  int w = char_width(cp);
  if (w == 0) return;
  if (cur_.wrap_pending) {
    cur_.x = 0;
    line_feed();
  }
  if (w == 2 && cur_.x >= cols_ - 1) {
    // a double-width character never straddles the right edge: wrap it whole
    if (!autowrap_) return;
    cur_.x = 0;
    line_feed();
  }
  Cell* c = at(cur_.x, cur_.y);
  if (c) {
    c->ch = cp;
    c->fg = cur_.fg;
    c->bg = cur_.bg;
    c->attr = cur_.attr;
    c->flags = (w == 2) ? kFlagWide : 0;
    if (w == 2) {
      Cell* t = at(cur_.x + 1, cur_.y);
      if (t) {
        t->ch = 0;
        t->fg = cur_.fg;
        t->bg = cur_.bg;
        t->attr = cur_.attr;
        t->flags = kFlagWideTail;
      }
    }
  }
  last_ch_ = static_cast<int>(cp);
  cur_.x += w;
  if (cur_.x >= cols_) {
    cur_.x = cols_ - 1;
    if (autowrap_) cur_.wrap_pending = true;
  }
}

void Screen::exec_control(unsigned char b) {
  switch (b) {
    case 0x07: bell_ = true; break;
    case 0x08:
      cur_.wrap_pending = false;
      if (cur_.x > 0) --cur_.x;
      break;
    case 0x09: {
      cur_.wrap_pending = false;
      int nx = ((cur_.x / 8) + 1) * 8;
      cur_.x = nx < cols_ ? nx : cols_ - 1;
      break;
    }
    case 0x0A:
    case 0x0B:
    case 0x0C:
      line_feed();
      break;
    case 0x0D:
      cur_.x = 0;
      cur_.wrap_pending = false;
      break;
    case 0x0E: shift_g1_ = true; break;
    case 0x0F: shift_g1_ = false; break;
    default: break;
  }
}

void Screen::emit_codepoint(uint32_t cp) {
  bool graphics = shift_g1_ ? g1_graphics_ : g0_graphics_;
  if (graphics && cp >= 0x60 && cp <= 0x7e) cp = kGraphics[cp - 0x60];
  put(cp);
}

void Screen::decode_utf8(unsigned char b) {
  utf8_.push_back(static_cast<char>(b));
  unsigned char c = static_cast<unsigned char>(utf8_[0]);
  size_t need = 1;
  if (c < 0x80) {
    need = 1;
  } else if ((c & 0xE0) == 0xC0) {
    need = 2;
  } else if ((c & 0xF0) == 0xE0) {
    need = 3;
  } else if ((c & 0xF8) == 0xF0) {
    need = 4;
  } else {
    utf8_.clear();
    emit_codepoint(0xFFFD);
    return;
  }
  if (utf8_.size() < need) return;  // wait for the rest of the character
  bool ok = true;
  for (size_t i = 1; i < need; ++i) {
    if ((static_cast<unsigned char>(utf8_[i]) & 0xC0) != 0x80) {
      ok = false;
      break;
    }
  }
  if (!ok) {
    utf8_.erase(0, 1);
    emit_codepoint(0xFFFD);
    return;
  }
  uint32_t cp = 0;
  if (need == 1) {
    cp = c;
  } else if (need == 2) {
    cp = ((c & 0x1Fu) << 6) | (static_cast<unsigned char>(utf8_[1]) & 0x3Fu);
  } else if (need == 3) {
    cp = ((c & 0x0Fu) << 12) |
         ((static_cast<unsigned char>(utf8_[1]) & 0x3Fu) << 6) |
         (static_cast<unsigned char>(utf8_[2]) & 0x3Fu);
  } else {
    cp = ((c & 0x07u) << 18) |
         ((static_cast<unsigned char>(utf8_[1]) & 0x3Fu) << 12) |
         ((static_cast<unsigned char>(utf8_[2]) & 0x3Fu) << 6) |
         (static_cast<unsigned char>(utf8_[3]) & 0x3Fu);
  }
  utf8_.clear();
  emit_codepoint(cp);
}

int Screen::param(size_t i, int def) const {
  if (i >= ps_.size() || ps_[i] < 0) return def;
  return ps_[i];
}

bool Screen::has_param(size_t i) const { return i < ps_.size() && ps_[i] >= 0; }

void Screen::apply_sgr() {
  // "CSI m" with no parameters (or only omitted ones, e.g. "CSI ;m") is a full
  // reset, not a no-op - a shell printing ESC[m expects attributes to clear.
  bool any = false;
  for (size_t i = 0; i < ps_.size(); ++i) {
    if (ps_[i] >= 0) {
      any = true;
      break;
    }
  }
  if (!any) {
    cur_.fg = kColorDefault;
    cur_.bg = kColorDefault;
    cur_.attr = 0;
    return;
  }
  for (size_t i = 0; i < ps_.size(); ++i) {
    int p = ps_[i];
    if (p < 0) continue;
    if (p == 0) {
      cur_.fg = kColorDefault;
      cur_.bg = kColorDefault;
      cur_.attr = 0;
    } else if (p == 1) {
      cur_.attr |= kAttrBold;
    } else if (p == 2) {
      cur_.attr |= kAttrDim;
    } else if (p == 3) {
      cur_.attr |= kAttrItalic;
    } else if (p == 4) {
      cur_.attr |= kAttrUnderline;
    } else if (p == 5 || p == 6) {
      cur_.attr |= kAttrBlink;
    } else if (p == 7) {
      cur_.attr |= kAttrReverse;
    } else if (p == 9) {
      cur_.attr |= kAttrStrike;
    } else if (p == 21 || p == 22) {
      cur_.attr &= static_cast<uint8_t>(~(kAttrBold | kAttrDim));
    } else if (p == 23) {
      cur_.attr &= static_cast<uint8_t>(~kAttrItalic);
    } else if (p == 24) {
      cur_.attr &= static_cast<uint8_t>(~kAttrUnderline);
    } else if (p == 25) {
      cur_.attr &= static_cast<uint8_t>(~kAttrBlink);
    } else if (p == 27) {
      cur_.attr &= static_cast<uint8_t>(~kAttrReverse);
    } else if (p == 29) {
      cur_.attr &= static_cast<uint8_t>(~kAttrStrike);
    } else if (p >= 30 && p <= 37) {
      cur_.fg = static_cast<int32_t>(palette256(p - 30));
    } else if (p == 38 || p == 48) {
      // extended color: 38;5;n (256) or 38;2;r;g;b (truecolor); the colon form
      // may carry an empty colorspace-id slot, which parses to -1
      int32_t col = kColorDefault;
      size_t k = i + 1;
      if (k < ps_.size() && ps_[k] == 5) {
        if (k + 1 < ps_.size()) {
          int idx = ps_[k + 1];
          if (idx >= 0) col = static_cast<int32_t>(palette256(idx));
          i = k + 1;
        }
      } else if (k < ps_.size() && ps_[k] == 2) {
        size_t base = k + 1;
        if (base < ps_.size() && ps_[base] < 0) ++base;  // skip colorspace id
        if (base + 2 < ps_.size()) {
          int r = ps_[base] < 0 ? 0 : ps_[base];
          int g = ps_[base + 1] < 0 ? 0 : ps_[base + 1];
          int b = ps_[base + 2] < 0 ? 0 : ps_[base + 2];
          col = static_cast<int32_t>((r & 0xFF) << 16 | (g & 0xFF) << 8 | (b & 0xFF));
          i = base + 2;
        }
      }
      if (p == 38) cur_.fg = col; else cur_.bg = col;
    } else if (p == 39) {
      cur_.fg = kColorDefault;
    } else if (p >= 40 && p <= 47) {
      cur_.bg = static_cast<int32_t>(palette256(p - 40));
    } else if (p == 49) {
      cur_.bg = kColorDefault;
    } else if (p >= 90 && p <= 97) {
      cur_.fg = static_cast<int32_t>(palette256(p - 90 + 8));
    } else if (p >= 100 && p <= 107) {
      cur_.bg = static_cast<int32_t>(palette256(p - 100 + 8));
    }
  }
}

void Screen::exec_escape(unsigned char b) {
  switch (b) {
    case '[':
      state_ = kCsi;
      params_.clear();
      inter_.clear();
      ps_.clear();
      priv_ = false;
      priv_ch_ = 0;
      return;
    case ']':
      state_ = kOsc;
      osc_.clear();
      return;
    case '(':
      charset_g0_g1_ = false;  // G0
      state_ = kCharset;
      return;
    case ')':
      charset_g0_g1_ = true;  // G1
      state_ = kCharset;
      return;
    case '*':
    case '+':
      state_ = kCharset;  // G2/G3: accept and ignore
      charset_g0_g1_ = true;
      return;
    case '#':
      state_ = kEscHash;
      return;
    case '7':
      saved_ = cur_;
      return;
    case '8':
      cur_ = saved_;
      return;
    case 'D':
      line_feed();
      return;
    case 'E':
      cur_.x = 0;
      line_feed();
      return;
    case 'M':
      reverse_index();
      return;
    case 'c':
      reset();
      return;
    default:
      return;  // keypad modes, tab stops, etc: nothing to do
  }
}

void Screen::exec_osc(const std::string& s) {
  size_t semi = s.find(';');
  std::string code = semi == std::string::npos ? s : s.substr(0, semi);
  std::string body = semi == std::string::npos ? std::string() : s.substr(semi + 1);
  if (code == "0" || code == "1" || code == "2") {
    title_ = body;
  }
  // 8 (hyperlink), 4/10/11 (colors), 133 (shell marks): accepted and ignored
}

void Screen::exec_csi(unsigned char f) {
  parse_params(params_, &ps_);
  int n1 = param(0, 1);
  if (n1 < 1 && f != 'm') n1 = 1;

  switch (f) {
    case 'A': cur_.y -= n1; cur_.wrap_pending = false; break;
    case 'B': cur_.y += n1; cur_.wrap_pending = false; break;
    case 'C': cur_.x += n1; cur_.wrap_pending = false; break;
    case 'D': cur_.x -= n1; cur_.wrap_pending = false; break;
    case 'E': cur_.x = 0; cur_.y += n1; cur_.wrap_pending = false; break;
    case 'F': cur_.x = 0; cur_.y -= n1; cur_.wrap_pending = false; break;
    case 'G':
    case '`': cur_.x = n1 - 1; cur_.wrap_pending = false; break;
    case 'd': cur_.y = n1 - 1; cur_.wrap_pending = false; break;
    case 'H':
    case 'f': {
      int row = param(0, 1) - 1;
      int col = param(1, 1) - 1;
      if (cur_.origin) {
        cur_.y = scroll_top_ + (row < 0 ? 0 : row);
        cur_.x = col < 0 ? 0 : col;
      } else {
        cur_.y = row;
        cur_.x = col;
      }
      cur_.wrap_pending = false;
      break;
    }
    case 'J': {
      int m = param(0, 0);
      if (m == 0) {
        clear_cells(cur_.x, cur_.y, cols_ - 1, cur_.y);
        if (cur_.y + 1 <= rows_ - 1) clear_cells(0, cur_.y + 1, cols_ - 1, rows_ - 1);
      } else if (m == 1) {
        if (cur_.y - 1 >= 0) clear_cells(0, 0, cols_ - 1, cur_.y - 1);
        clear_cells(0, cur_.y, cur_.x, cur_.y);
      } else if (m == 2 || m == 3) {
        clear_cells(0, 0, cols_ - 1, rows_ - 1);
      }
      break;
    }
    case 'K': {
      int m = param(0, 0);
      if (m == 0) {
        clear_cells(cur_.x, cur_.y, cols_ - 1, cur_.y);
      } else if (m == 1) {
        clear_cells(0, cur_.y, cur_.x, cur_.y);
      } else if (m == 2) {
        clear_cells(0, cur_.y, cols_ - 1, cur_.y);
      }
      break;
    }
    case 'L': {  // insert lines at the cursor row (inside the region)
      if (cur_.y >= scroll_top_ && cur_.y <= scroll_bot_) {
        scroll_down(cur_.y, scroll_bot_, n1);
      }
      break;
    }
    case 'M': {  // delete lines
      if (cur_.y >= scroll_top_ && cur_.y <= scroll_bot_) {
        scroll_up(cur_.y, scroll_bot_, n1);
      }
      break;
    }
    case 'P': {  // delete n chars, pull the rest left
      int cnt = n1 > cols_ - cur_.x ? cols_ - cur_.x : n1;
      for (int x = cur_.x; x < cols_ - cnt; ++x) {
        *at(x, cur_.y) = *at(x + cnt, cur_.y);
      }
      clear_cells(cols_ - cnt, cur_.y, cols_ - 1, cur_.y);
      break;
    }
    case '@': {  // insert n blanks, push the rest right
      int cnt = n1 > cols_ - cur_.x ? cols_ - cur_.x : n1;
      for (int x = cols_ - 1; x >= cur_.x + cnt; --x) {
        *at(x, cur_.y) = *at(x - cnt, cur_.y);
      }
      clear_cells(cur_.x, cur_.y, cur_.x + cnt - 1, cur_.y);
      break;
    }
    case 'X': {  // erase n chars in place
      clear_cells(cur_.x, cur_.y, cur_.x + n1 - 1, cur_.y);
      break;
    }
    case 'S': scroll_up(scroll_top_, scroll_bot_, n1); break;
    case 'T': scroll_down(scroll_top_, scroll_bot_, n1); break;
    case 'Z': {
      int x = cur_.x;
      do {
        if (x == 0) break;
        --x;
      } while (x % 8 != 0);
      cur_.x = x;
      break;
    }
    case 'a': cur_.x += n1; break;
    case 'b': {  // REP: repeat the last printed character
      int cnt = n1;
      if (last_ch_ > 0) {
        for (int i = 0; i < cnt; ++i) put(static_cast<uint32_t>(last_ch_));
      }
      break;
    }
    case 'c':
      if (!priv_) reply_ += "\x1b[?6c";  // VT102
      break;
    case 'm': apply_sgr(); break;
    case 'n': {
      int m = param(0, 0);
      if (m == 5) {
        reply_ += "\x1b[0n";
      } else if (m == 6) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "\x1b[%d;%dR", cur_.y + 1, cur_.x + 1);
        reply_ += buf;
      }
      break;
    }
    case 'r': {
      int top = param(0, 1) - 1;
      int bot = param(1, rows_) - 1;
      if (top < 0) top = 0;
      if (bot > rows_ - 1) bot = rows_ - 1;
      if (top < bot) {
        scroll_top_ = top;
        scroll_bot_ = bot;
        cur_.x = 0;
        cur_.y = cur_.origin ? scroll_top_ : 0;
        cur_.wrap_pending = false;
      }
      break;
    }
    case 's': saved_ = cur_; break;
    case 'u': cur_ = saved_; break;
    case 'h':
    case 'l': {
      bool on = (f == 'h');
      if (!priv_) break;
      for (size_t i = 0; i < ps_.size(); ++i) {
        switch (ps_[i]) {
          case 1: app_cursor_ = on; break;
          case 6:
            cur_.origin = on;
            cur_.x = 0;
            cur_.y = on ? scroll_top_ : 0;
            break;
          case 7: autowrap_ = on; break;
          case 25: cursor_visible_ = on; break;
          case 47:
          case 1047:
          case 1049: {
            if (on) {
              if (!alt_) {
                alt_cells_ = cells_;   // keep the main screen
                saved_ = cur_;
                cells_.assign(static_cast<size_t>(cols_) * rows_, Cell());
                alt_ = true;
              }
              if (ps_[i] == 1049) {
                cur_ = Cursor();
                scroll_top_ = 0;
                scroll_bot_ = rows_ - 1;
              }
            } else if (alt_) {
              cells_ = alt_cells_;
              alt_cells_.clear();
              alt_ = false;
              if (ps_[i] == 1049) cur_ = saved_;
            }
            break;
          }
          case 1048:
            if (on) saved_ = cur_; else cur_ = saved_;
            break;
          case 2004: bracketed_paste_ = on; break;
          default: break;  // mouse reporting and friends: not supported
        }
      }
      break;
    }
    case 't': break;  // window manipulation: ignore
    default: break;
  }

  // clamp the cursor into the grid (commands above may push it out)
  if (cur_.x < 0) cur_.x = 0;
  if (cur_.y < 0) cur_.y = 0;
  if (cur_.x > cols_ - 1) cur_.x = cols_ - 1;
  if (cur_.y > rows_ - 1) cur_.y = rows_ - 1;
}

void Screen::feed(const std::string& bytes) {
  for (size_t i = 0; i < bytes.size(); ++i) {
    unsigned char b = static_cast<unsigned char>(bytes[i]);
    switch (state_) {
      case kGround:
        if (b == 0x1b) {
          state_ = kEsc;
        } else if (b < 0x20) {
          exec_control(b);
        } else if (b == 0x7f) {
          // DEL: ignored
        } else {
          decode_utf8(b);
        }
        break;

      case kEsc:
        state_ = kGround;
        exec_escape(b);
        break;

      case kCsi:
        if (b >= 0x40 && b <= 0x7e) {
          state_ = kGround;
          exec_csi(b);
        } else if (b >= 0x30 && b <= 0x3f) {
          if (params_.empty() && (b == '?' || b == '<' || b == '=' || b == '>')) {
            priv_ = true;
            priv_ch_ = static_cast<char>(b);
          } else {
            params_.push_back(static_cast<char>(b));
          }
        } else if (b >= 0x20 && b <= 0x2f) {
          inter_.push_back(static_cast<char>(b));
        } else if (b == 0x1b) {
          state_ = kEsc;
        } else if (b < 0x20) {
          exec_control(b);  // a control inside a CSI is executed immediately
        }
        break;

      case kOsc:
        if (b == 0x07) {
          state_ = kGround;
          exec_osc(osc_);
        } else if (b == 0x1b) {
          state_ = kOscEsc;
        } else if (b >= 0x20) {
          osc_.push_back(static_cast<char>(b));
        }
        break;

      case kOscEsc:
        state_ = kGround;
        if (b == '\\') exec_osc(osc_);  // ST terminates the string
        break;

      case kCharset:
        state_ = kGround;
        if (charset_g0_g1_) {
          g1_graphics_ = (b == '0');
        } else {
          g0_graphics_ = (b == '0');
        }
        break;

      case kEscHash:
        state_ = kGround;
        if (b == '8') {  // DECALN: fill the screen with 'E' (alignment test)
          for (int y = 0; y < rows_; ++y) {
            for (int x = 0; x < cols_; ++x) {
              Cell* c = at(x, y);
              c->ch = 'E';
              c->fg = kColorDefault;
              c->bg = kColorDefault;
              c->attr = 0;
              c->flags = 0;
            }
          }
        }
        break;
    }
  }
}

std::string Screen::take_reply() {
  std::string r;
  r.swap(reply_);
  return r;
}

std::string Screen::text() const {
  std::string out;
  for (int y = 0; y < rows_; ++y) {
    int last = -1;
    for (int x = 0; x < cols_; ++x) {
      const Cell* c = at(x, y);
      if (c && c->ch != ' ' && c->ch != 0) last = x;
    }
    std::string line;
    for (int x = 0; x <= last; ++x) {
      const Cell* c = at(x, y);
      if (!c) continue;
      if (c->flags & kFlagWideTail) continue;
      if (c->ch == 0) continue;
      append_utf8(&line, c->ch);
    }
    out += line;
    if (y != rows_ - 1) out += "\n";
  }
  // drop trailing empty lines so the overview is compact
  while (!out.empty() && (out[out.size() - 1] == '\n')) out.erase(out.size() - 1);
  return out;
}

}  // namespace vt
}  // namespace fs
