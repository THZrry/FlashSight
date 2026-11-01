// fs_utf8.cpp - UTF-8 helpers (see header)
#include "fs_utf8.hpp"

namespace fs {
namespace utf8 {

size_t char_len(const std::string& s, size_t i) {
  if (i >= s.size()) return 0;
  unsigned char c = static_cast<unsigned char>(s[i]);
  size_t extra;
  if (c < 0x80) return 1;
  else if ((c & 0xE0) == 0xC0) extra = 1;
  else if ((c & 0xF0) == 0xE0) extra = 2;
  else if ((c & 0xF8) == 0xF0) extra = 3;
  else return 0;
  if (i + extra >= s.size()) return 0;
  for (size_t k = 1; k <= extra; ++k) {
    if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return 0;
  }
  return extra + 1;
}

size_t safe_cut(const std::string& s, size_t max_bytes) {
  if (max_bytes >= s.size()) return s.size();
  size_t i = 0;
  while (i < max_bytes) {
    size_t n = char_len(s, i);
    if (n == 0) {  // ill-formed: stop before it
      return i;
    }
    if (i + n > max_bytes) return i;  // would split a character
    i += n;
  }
  return i;
}

std::string truncate(const std::string& s, size_t max_bytes,
                     const std::string& suffix) {
  if (s.size() <= max_bytes) return s;
  return s.substr(0, safe_cut(s, max_bytes)) + suffix;
}

bool valid(const std::string& s) {
  size_t i = 0;
  while (i < s.size()) {
    size_t n = char_len(s, i);
    if (n == 0) return false;
    i += n;
  }
  return true;
}

std::string sanitize(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  size_t i = 0;
  while (i < s.size()) {
    size_t n = char_len(s, i);
    if (n == 0) {
      ++i;
      continue;
    }
    out.append(s, i, n);
    i += n;
  }
  return out;
}

}  // namespace utf8
}  // namespace fs
