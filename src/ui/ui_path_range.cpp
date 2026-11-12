// ui_path_range.cpp - greedy path-interval scan (see the header for the why).
#include "ui_path_range.hpp"

#include <cctype>
#include <cstdint>

namespace fs {
namespace ui {
namespace {

size_t u8_len(unsigned char c) {
  if (c < 0x80) return 1;
  if ((c & 0xE0) == 0xC0) return 2;
  if ((c & 0xF0) == 0xE0) return 3;
  if ((c & 0xF8) == 0xF0) return 4;
  return 1;  // ill-formed: consume one byte, never loop forever
}

uint32_t cp_at(const std::string& s, size_t i, size_t* len) {
  unsigned char c = static_cast<unsigned char>(s[i]);
  size_t n = u8_len(c);
  if (i + n > s.size()) n = 1;
  *len = n;
  if (n == 1) return c;
  uint32_t cp = c & (n == 2 ? 0x1F : n == 3 ? 0x0F : 0x07);
  for (size_t k = 1; k < n; ++k) {
    cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
  }
  return cp;
}

bool ascii_space(unsigned char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' ||
         c == '\v';
}

// Sentence punctuation and brackets. They end a path; Chinese punctuation is
// the common case because the draft is usually Chinese prose.
bool cjk_delim(uint32_t cp) {
  switch (cp) {
    case 0x3000:  // ideographic space
    case 0x3001:  // 、
    case 0x3002:  // 。
    case 0x3008:  // 〈
    case 0x3009:  // 〉
    case 0x300A:  // 《
    case 0x300B:  // 》
    case 0x300C:  // 「
    case 0x300D:  // 」
    case 0x300E:  // 『
    case 0x300F:  // 』
    case 0x3010:  // 【
    case 0x3011:  // 】
    case 0x3014:  // 〔
    case 0x3015:  // 〕
    case 0xFF01:  // ！
    case 0xFF08:  // （
    case 0xFF09:  // ）
    case 0xFF0C:  // ，
    case 0xFF1A:  // ：
    case 0xFF1B:  // ；
    case 0xFF1F:  // ？
    case 0x201C:  // “
    case 0x201D:  // ”
    case 0x2018:  // ‘
    case 0x2019:  // ’
    case 0x2026:  // …
    case 0x2014:  // —
      return true;
    default: return false;
  }
}

bool ascii_delim(unsigned char c) {
  switch (c) {
    case ',': case ';': case '!': case '?': case '\'': case '"':
    case '(': case ')': case '[': case ']': case '{': case '}':
    case '<': case '>': case '|': case '*': case '&': case '#':
    case '=': case '%': case '`':
      return true;
    default: return false;
  }
}

bool is_sep(char c) { return c == '/' || c == '\\'; }

bool ascii_path_char(unsigned char c) {
  return std::isalnum(c) != 0 || c == '_' || c == '-' || c == '.' ||
         c == '/' || c == '\\' || c == '~' || c == '+' || c == '@';
}

// One character of a path. `at` is the offset: the one legal colon is the drive
// colon (C:\ / C:/), which may sit inside a run that started in CJK prose
// ("看C:/x"); refine_start() moves the range start back to the drive letter.
bool path_char_at(const std::string& s, size_t at, size_t* len, char* ascii_c) {
  unsigned char c = static_cast<unsigned char>(s[at]);
  *ascii_c = 0;
  if (c < 0x80) {
    *len = 1;
    *ascii_c = static_cast<char>(c);
    if (ascii_space(c) || ascii_delim(c)) return false;
    if (c == ':') {
      // A drive colon: <letter>:/ or <letter>:\ . It may sit inside a run that
      // began in CJK prose ("看C:/x"), so this is not tied to `start + 1`;
      // refine_start() then moves the range start to the drive letter.
      bool drive = at > 0 && at + 1 < s.size() &&
                   std::isalpha(static_cast<unsigned char>(s[at - 1])) != 0 &&
                   is_sep(s[at + 1]);
      return drive;
    }
    return ascii_path_char(c);
  }
  size_t n = 0;
  uint32_t cp = cp_at(s, at, &n);
  *len = n;
  return !cjk_delim(cp);
}

bool boundary_before(const std::string& s, size_t i) {
  if (i == 0) return true;
  size_t n = u8_len(static_cast<unsigned char>(s[i - 1]));
  // walk back to the START of the previous codepoint
  size_t j = i - 1;
  while (j > 0 && (static_cast<unsigned char>(s[j]) & 0xC0) == 0x80) --j;
  (void)n;
  size_t len = 0;
  uint32_t cp = cp_at(s, j, &len);
  if (j + len != i) return false;  // mid-codepoint: not a boundary
  if (cp < 0x80) {
    unsigned char c = static_cast<unsigned char>(cp);
    return ascii_space(c) || ascii_delim(c) || c == ':' || c == ';';
  }
  return cjk_delim(cp);
}

bool prev_is_non_ascii(const std::string& s, size_t i) {
  if (i == 0) return false;
  size_t j = i - 1;
  while (j > 0 && (static_cast<unsigned char>(s[j]) & 0xC0) == 0x80) --j;
  return static_cast<unsigned char>(s[j]) >= 0x80;
}

bool range_has_sep(const std::string& s, size_t a, size_t b) {
  for (size_t i = a; i < b; ++i) {
    if (is_sep(s[i])) return true;
  }
  return false;
}

bool prefix_ci(const std::string& s, size_t a, const char* p) {
  size_t n = 0;
  while (p[n]) ++n;
  if (a + n > s.size()) return false;
  for (size_t i = 0; i < n; ++i) {
    char x = s[a + i], y = p[i];
    if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
    if (x != y) return false;
  }
  return true;
}

bool is_url_like(const std::string& s, size_t a, size_t b) {
  if (prefix_ci(s, a, "http://") || prefix_ci(s, a, "https://") ||
      prefix_ci(s, a, "ftp://") || prefix_ci(s, a, "file:")) {
    return true;
  }
  for (size_t i = a; i + 2 < b; ++i) {
    if (s[i] == ':' && s[i + 1] == '/' && s[i + 2] == '/') return true;
  }
  return false;
}

// "看下src/ui": the greedy run starts at 看 because the previous character is a
// boundary and CJK is legal in a path. The actual path is the ASCII tail of the
// first segment. Move the start there - a pure-CJK folder ("文档/项目") has no
// such tail and keeps its start.
size_t refine_start(const std::string& s, size_t start, size_t end) {
  size_t seg = start;
  while (seg < end && !is_sep(s[seg])) ++seg;
  // An EXPLICIT path marker can begin a new path inside a run that started in
  // CJK prose: "看下./src/ui" is one greedy run from 看, but the path starts at
  // "./". Only markers in the FIRST segment count, and only when the prefix
  // before them contains actual non-ASCII prose - "dir/./x" keeps its start.
  for (size_t m = start + 1; m < seg; ++m) {
    bool dot = s[m] == '.' && m + 1 < end && is_sep(s[m + 1]);
    bool home = s[m] == '~' && m + 1 < end && is_sep(s[m + 1]);
    bool drive = std::isalpha(static_cast<unsigned char>(s[m])) != 0 &&
                 m + 2 < end && s[m + 1] == ':' && is_sep(s[m + 2]);
    if (!dot && !home && !drive) continue;
    bool prefix_has_non_ascii = false;
    for (size_t k = start; k < m; ++k) {
      if (static_cast<unsigned char>(s[k]) >= 0x80) prefix_has_non_ascii = true;
      if (is_sep(s[k])) return start;  // a real path segment before the marker
    }
    if (prefix_has_non_ascii) return m;
  }
  size_t ascii_tail = seg;
  while (ascii_tail > start) {
    size_t j = ascii_tail - 1;
    while (j > start && (static_cast<unsigned char>(s[j]) & 0xC0) == 0x80) --j;
    if (static_cast<unsigned char>(s[j]) < 0x80) {
      ascii_tail = j;
    } else {
      break;
    }
  }
  if (ascii_tail > start && ascii_tail < seg &&
      seg - ascii_tail >= 2 &&
      std::isalpha(static_cast<unsigned char>(s[ascii_tail])) != 0) {
    return ascii_tail;
  }
  return start;
}

}  // namespace

std::vector<PathRange> parse_path_ranges(const std::string& text) {
  std::vector<PathRange> out;
  const size_t n = text.size();
  size_t i = 0;
  while (i < n) {
    unsigned char c = static_cast<unsigned char>(text[i]);
    size_t clen = u8_len(c);
    if (clen == 1 && ascii_space(c)) {
      ++i;
      continue;
    }
    if (clen == 1 && ascii_delim(c)) {
      ++i;
      continue;
    }
    bool boundary = boundary_before(text, i);
    bool explicit_path =
        (c == '.' && i + 1 < n && is_sep(text[i + 1])) ||
        (c == '~' && i + 1 < n && is_sep(text[i + 1])) ||
        (is_sep(static_cast<char>(c)) && c != '\\') ||
        (c == '\\' && i + 1 < n && text[i + 1] == '\\') ||
        (std::isalpha(c) != 0 && i + 2 < n && text[i + 1] == ':' &&
         is_sep(text[i + 2]));
    bool glued_start = !boundary && c < 0x80 && ascii_path_char(c) &&
                       prev_is_non_ascii(text, i);
    if (!boundary && !explicit_path && !glued_start) {
      ++i;
      continue;
    }
    // Consume greedily.
    size_t j = i;
    while (j < n) {
      size_t len = 1;
      char ac = 0;
      if (!path_char_at(text, j, &len, &ac)) break;
      if (len == 0) break;
      j += len;
    }
    if (j <= i) {
      ++i;
      continue;
    }
    size_t start = refine_start(text, i, j);
    if (!range_has_sep(text, start, j) || is_url_like(text, start, j) ||
        (start < j && text[start] == '-')) {
      i = j;
      while (i < n && !ascii_space(static_cast<unsigned char>(text[i])) &&
             !ascii_delim(static_cast<unsigned char>(text[i]))) {
        ++i;
      }
      continue;
    }
    PathRange r;
    r.start = start;
    r.end = j;
    out.push_back(r);
    i = j;
  }
  return out;
}

int path_range_at(const std::vector<PathRange>& ranges, size_t caret) {
  int best = -1;
  for (size_t i = 0; i < ranges.size(); ++i) {
    const PathRange& r = ranges[i];
    if (caret < r.start || caret > r.end) continue;
    // A placeholder caret at the END of a path belongs to that path; prefer the
    // range that ends here over one that starts here (they cannot overlap, but
    // the rule keeps the intent explicit).
    if (best < 0 || (caret == r.end && caret != ranges[static_cast<size_t>(best)].end)) {
      best = static_cast<int>(i);
    }
  }
  return best;
}

}  // namespace ui
}  // namespace fs
