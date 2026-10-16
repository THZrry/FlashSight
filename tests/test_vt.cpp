// test_vt.cpp - terminal engine (VT parser + screen grid) unit tests.
//
// These drive the parser with the byte sequences real programs emit and assert
// on the resulting grid, so the UI's rendering is built on verified behavior.
#include "test_framework.hpp"

#include "term/fs_vt.hpp"

using namespace fs;
using namespace fs::vt;

namespace {

// grid row as a plain string (trailing blanks trimmed)
std::string row_text(const Screen& s, int y) {
  std::string out;
  for (int x = 0; x < s.cols(); ++x) {
    const Cell& c = s.cell(x, y);
    if (c.flags & kFlagWideTail) continue;
    if (c.ch == 0) continue;
    out.push_back(c.ch < 0x80 ? static_cast<char>(c.ch) : '?');
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

}  // namespace

FS_TEST(vt_plain_text_and_newlines) {
  Screen s(20, 4);
  s.feed("hello\r\nworld");
  EXPECT_EQ(row_text(s, 0), std::string("hello"));
  EXPECT_EQ(row_text(s, 1), std::string("world"));
  EXPECT_EQ(s.cursor_x(), 5);
  EXPECT_EQ(s.cursor_y(), 1);
}

// CR returns to column 0; LF moves down one row and KEEPS the column (they are
// separate operations - that is why programs send CRLF).
FS_TEST(vt_carriage_return_and_line_feed) {
  Screen s(20, 4);
  s.feed("abc\rX");
  EXPECT_EQ(row_text(s, 0), std::string("Xbc"));
  Screen s2(20, 4);
  s2.feed("abc\nY");
  EXPECT_EQ(row_text(s2, 1), std::string("   Y"));
}

// Printing past the last column wraps to the next line.
FS_TEST(vt_autowrap_at_last_column) {
  Screen s(5, 3);
  s.feed("abcdef");
  EXPECT_EQ(row_text(s, 0), std::string("abcde"));
  EXPECT_EQ(row_text(s, 1), std::string("f"));
}

// Filling the bottom row scrolls the screen up.
FS_TEST(vt_scrolls_at_bottom) {
  Screen s(10, 2);
  s.feed("one\r\ntwo\r\nthree");
  EXPECT_EQ(row_text(s, 0), std::string("two"));
  EXPECT_EQ(row_text(s, 1), std::string("three"));
}

FS_TEST(vt_cursor_movement) {
  Screen s(20, 5);
  s.feed("\x1b[3;5H");        // CUP row 3 col 5 (1-based)
  EXPECT_EQ(s.cursor_x(), 4);
  EXPECT_EQ(s.cursor_y(), 2);
  s.feed("X");
  EXPECT_EQ(row_text(s, 2), std::string("    X"));
  s.feed("\x1b[2A");          // up 2
  EXPECT_EQ(s.cursor_y(), 0);
  s.feed("\x1b[3D");          // left 3 (from column 5)
  EXPECT_EQ(s.cursor_x(), 2);
}

FS_TEST(vt_erase_line_and_display) {
  Screen s(10, 3);
  s.feed("abcdefgh");
  s.feed("\x1b[1;3H\x1b[K");  // cursor at col 3, erase to end of line
  EXPECT_EQ(row_text(s, 0), std::string("ab"));
  s.feed("\x1b[2J");
  EXPECT_EQ(row_text(s, 0), std::string(""));
}

// SGR: colors and attributes must land on the cells that follow.
FS_TEST(vt_sgr_colors) {
  Screen s(20, 2);
  s.feed("\x1b[31mR\x1b[0mP");
  EXPECT_EQ(s.cell(0, 0).fg, static_cast<int32_t>(palette256(1)));
  EXPECT_EQ(s.cell(1, 0).fg, kColorDefault);
  s.feed("\x1b[1;44mB");
  EXPECT_TRUE((s.cell(2, 0).attr & kAttrBold) != 0);
  EXPECT_EQ(s.cell(2, 0).bg, static_cast<int32_t>(palette256(4)));
}

FS_TEST(vt_sgr_256_and_truecolor) {
  Screen s(20, 2);
  s.feed("\x1b[38;5;196mX");
  EXPECT_EQ(s.cell(0, 0).fg, static_cast<int32_t>(palette256(196)));
  s.feed("\x1b[38;2;10;20;30mY");
  EXPECT_EQ(s.cell(1, 0).fg, static_cast<int32_t>(0x0A141E));
  // the colon form with an empty colorspace slot
  s.feed("\x1b[38:2::1:2:3mZ");
  EXPECT_EQ(s.cell(2, 0).fg, static_cast<int32_t>(0x010203));
}

FS_TEST(vt_reset_clears_attributes) {
  Screen s(20, 2);
  // "CSI m" with no parameters is a full reset (a shell printing ESC[m relies
  // on it), not a no-op
  s.feed("\x1b[1;31mA\x1b[mB");
  EXPECT_EQ(s.cell(1, 0).fg, kColorDefault);
  EXPECT_EQ(s.cell(1, 0).attr, static_cast<uint8_t>(0));
}

FS_TEST(vt_insert_and_delete_lines) {
  Screen s(10, 4);
  s.feed("a\r\nb\r\nc\r\nd");
  s.feed("\x1b[2;1H\x1b[L");  // insert a blank line at row 2
  EXPECT_EQ(row_text(s, 0), std::string("a"));
  EXPECT_EQ(row_text(s, 1), std::string(""));
  EXPECT_EQ(row_text(s, 2), std::string("b"));
  EXPECT_EQ(row_text(s, 3), std::string("c"));
  s.feed("\x1b[2;1H\x1b[M");  // delete it again
  EXPECT_EQ(row_text(s, 1), std::string("b"));
  EXPECT_EQ(row_text(s, 2), std::string("c"));
}

FS_TEST(vt_delete_and_insert_chars) {
  Screen s(10, 2);
  s.feed("abcdef");
  s.feed("\x1b[1;2H\x1b[2P");  // delete 2 chars at col 2
  EXPECT_EQ(row_text(s, 0), std::string("adef"));
  s.feed("\x1b[1;2H\x1b[2@");  // insert 2 blanks there
  EXPECT_EQ(row_text(s, 0), std::string("a  def"));
}

// DECSTBM confines scrolling: a full-screen program (or a status line) relies
// on this. Deleting a line inside the region must not touch the rows outside it.
FS_TEST(vt_scroll_region) {
  Screen s(10, 4);
  s.feed("keep\r\nr1\r\nr2\r\nr3");
  s.feed("\x1b[2;3r");        // region = rows 2..3 (1-based)
  s.feed("\x1b[2;1H\x1b[M");  // delete one line at the region's top
  EXPECT_EQ(row_text(s, 0), std::string("keep"));  // above the region: intact
  EXPECT_EQ(row_text(s, 1), std::string("r2"));    // pulled up from row 3
  EXPECT_EQ(row_text(s, 2), std::string(""));      // blanked at the bottom
  EXPECT_EQ(row_text(s, 3), std::string("r3"));    // below the region: intact
}

// The alternate screen: a full-screen app switches in and out, and the main
// screen must come back exactly as it was.
FS_TEST(vt_alternate_screen) {
  Screen s(20, 3);
  s.feed("main-content");
  EXPECT_FALSE(s.alt_screen());
  s.feed("\x1b[?1049h");
  EXPECT_TRUE(s.alt_screen());
  EXPECT_EQ(row_text(s, 0), std::string(""));
  s.feed("alt");
  EXPECT_EQ(row_text(s, 0), std::string("alt"));
  s.feed("\x1b[?1049l");
  EXPECT_FALSE(s.alt_screen());
  EXPECT_EQ(row_text(s, 0), std::string("main-content"));
}

FS_TEST(vt_cursor_visibility_and_app_keys) {
  Screen s(10, 3);
  EXPECT_TRUE(s.cursor_visible());
  s.feed("\x1b[?25l");
  EXPECT_FALSE(s.cursor_visible());
  s.feed("\x1b[?25h");
  EXPECT_TRUE(s.cursor_visible());
  EXPECT_FALSE(s.app_cursor_keys());
  s.feed("\x1b[?1h");
  EXPECT_TRUE(s.app_cursor_keys());
}

FS_TEST(vt_osc_sets_title) {
  Screen s(10, 3);
  s.feed("\x1b]0;my title\x07");
  EXPECT_EQ(s.title(), std::string("my title"));
  // the ST form (ESC \) must terminate it too
  s.feed("\x1b]2;other\x1b\\");
  EXPECT_EQ(s.title(), std::string("other"));
}

// DSR: the shell asks for the cursor position and waits for the reply, so the
// terminal must produce it.
FS_TEST(vt_dsr_reply) {
  Screen s(20, 5);
  s.feed("\x1b[2;3H\x1b[6n");
  EXPECT_EQ(s.take_reply(), std::string("\x1b[2;3R"));
  EXPECT_EQ(s.take_reply(), std::string(""));  // drained
  s.feed("\x1b[5n");
  EXPECT_EQ(s.take_reply(), std::string("\x1b[0n"));
}

// Sequences split across reads must be reassembled (chunk boundaries land
// anywhere).
FS_TEST(vt_split_sequences) {
  Screen s(20, 3);
  s.feed("\x1b[3");
  s.feed("1mAB");
  EXPECT_EQ(s.cell(0, 0).fg, static_cast<int32_t>(palette256(1)));
  EXPECT_EQ(row_text(s, 0), std::string("AB"));
  // an escape sequence cut in the middle
  Screen s2(20, 3);
  s2.feed("\x1b");
  s2.feed("[1;5H");
  EXPECT_EQ(s2.cursor_x(), 4);
}

FS_TEST(vt_utf8_and_wide_chars) {
  Screen s(20, 3);
  s.feed("中");  // E4 B8 AD
  EXPECT_EQ(s.cell(0, 0).ch, static_cast<uint32_t>(0x4E2D));
  EXPECT_TRUE((s.cell(0, 0).flags & kFlagWide) != 0);
  EXPECT_TRUE((s.cell(1, 0).flags & kFlagWideTail) != 0);
  EXPECT_EQ(s.cursor_x(), 2);  // a wide char advances two columns
  // a multi-byte character split across two reads
  Screen s2(20, 3);
  s2.feed("\xE4\xB8");
  s2.feed("\xAD");
  EXPECT_EQ(s2.cell(0, 0).ch, static_cast<uint32_t>(0x4E2D));
  // invalid bytes degrade to the replacement character instead of stalling
  Screen s3(20, 3);
  s3.feed("\xFF\xFE");
  EXPECT_EQ(s3.cell(0, 0).ch, static_cast<uint32_t>(0xFFFD));
}

// DEC special graphics: `tree` and ncurses draw boxes with these.
FS_TEST(vt_dec_graphics_charset) {
  Screen s(20, 3);
  s.feed("\x1b(0qqq\x1b(B");
  EXPECT_EQ(s.cell(0, 0).ch, static_cast<uint32_t>(0x2500));  // ─
  s.feed("q");
  EXPECT_EQ(s.cell(3, 0).ch, static_cast<uint32_t>('q'));  // back to ASCII
}

FS_TEST(vt_resize_keeps_content) {
  Screen s(10, 3);
  s.feed("hello\r\nworld");
  s.resize(20, 5);
  EXPECT_EQ(s.cols(), 20);
  EXPECT_EQ(row_text(s, 0), std::string("hello"));
  EXPECT_EQ(row_text(s, 1), std::string("world"));
  s.resize(3, 2);  // shrink: the top-left survives, the cursor stays in range
  EXPECT_EQ(row_text(s, 0), std::string("hel"));
  EXPECT_TRUE(s.cursor_x() < s.cols());
  EXPECT_TRUE(s.cursor_y() < s.rows());
}

// Text dump (used by the "all terminals" overview and copy).
FS_TEST(vt_text_dump_trims_trailing_blanks) {
  Screen s(20, 4);
  s.feed("a\r\n\r\nbb");
  EXPECT_EQ(s.text(), std::string("a\n\nbb"));
}

// Scrolling off the top keeps the line as history: a terminal that cannot show
// you what just went past cannot be scrolled back through.
FS_TEST(vt_scrollback_keeps_lines_that_scrolled_off) {
  Screen s(20, 2);
  EXPECT_EQ(s.scrollback_size(), 0);
  s.feed("one\r\ntwo\r\nthree");
  // "one" scrolled off the 2-row screen into history
  EXPECT_EQ(s.scrollback_size(), 1);
  EXPECT_EQ(s.total_lines(), 3);
  // the combined buffer: line 0 is the oldest history, then the live screen
  std::string l0;
  for (int x = 0; x < s.cols(); ++x) {
    const Cell& c = s.line_cell(0, x);
    if (c.ch && c.ch != ' ') l0.push_back(static_cast<char>(c.ch));
  }
  EXPECT_EQ(l0, std::string("one"));
  std::string l2;
  for (int x = 0; x < s.cols(); ++x) {
    const Cell& c = s.line_cell(2, x);
    if (c.ch && c.ch != ' ') l2.push_back(static_cast<char>(c.ch));
  }
  EXPECT_EQ(l2, std::string("three"));  // last line = bottom of the live screen
}

// History is bounded (a runaway build log must not grow without limit) and a
// scroll REGION below row 0 is a program drawing in a window, not history.
FS_TEST(vt_scrollback_bounds_and_regions) {
  Screen s(10, 3);
  for (int i = 0; i < 20; ++i) s.feed("line\r\n");
  EXPECT_TRUE(s.scrollback_size() > 0);
  EXPECT_TRUE(s.scrollback_size() <= 2000);
  // a region that does not start at row 0 scrolls inside the screen only
  Screen s2(10, 4);
  s2.feed("keep\r\nr1\r\nr2\r\nr3");
  int before = s2.scrollback_size();
  s2.feed("\x1b[2;3r\x1b[2;1H\x1b[M");
  EXPECT_EQ(s2.scrollback_size(), before);
}

// A reset clears history (ESC c starts a fresh terminal).
FS_TEST(vt_scrollback_cleared_on_reset) {
  Screen s(10, 2);
  s.feed("a\r\nb\r\nc");
  EXPECT_TRUE(s.scrollback_size() > 0);
  s.feed("\x1b" "c");
  EXPECT_EQ(s.scrollback_size(), 0);
}

// The alternate screen has no history to scroll back to (it is a full-screen
// program's own canvas).
FS_TEST(vt_alt_screen_has_no_scrollback) {
  Screen s(10, 2);
  s.feed("\x1b[?1049h");
  int before = s.scrollback_size();
  s.feed("x\r\ny\r\nz\r\nw");
  EXPECT_EQ(s.scrollback_size(), before);
}

FS_TEST(vt_escape_reset) {
  Screen s(10, 3);
  s.feed("abc\x1b[31m");
  // NOTE: "\x1b" "c" - a bare "\x1bc" is one out-of-range hex escape, not ESC+'c'
  s.feed("\x1b" "c");  // RIS
  EXPECT_EQ(row_text(s, 0), std::string(""));
  EXPECT_EQ(s.cell(0, 0).fg, kColorDefault);
}
