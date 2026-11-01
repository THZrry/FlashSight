// fs_utf8.cpp - UTF-8 helpers (see header)
#include "fs_utf8.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <string>

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

namespace {

void append_cp(std::string* out, unsigned int cp) {
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

// UTF-16 (either endianness, with or without a BOM being present) -> UTF-8.
std::string from_utf16(const std::string& raw, bool big_endian, size_t skip) {
  std::string out;
  out.reserve(raw.size());
  size_t i = skip;
  for (; i + 1 < raw.size(); i += 2) {
    unsigned int u = big_endian
        ? (static_cast<unsigned char>(raw[i]) << 8) | static_cast<unsigned char>(raw[i + 1])
        : (static_cast<unsigned char>(raw[i + 1]) << 8) | static_cast<unsigned char>(raw[i]);
    if (u >= 0xD800 && u <= 0xDBFF && i + 3 < raw.size()) {
      unsigned int lo = big_endian
          ? (static_cast<unsigned char>(raw[i + 2]) << 8) | static_cast<unsigned char>(raw[i + 3])
          : (static_cast<unsigned char>(raw[i + 3]) << 8) | static_cast<unsigned char>(raw[i + 2]);
      if (lo >= 0xDC00 && lo <= 0xDFFF) {
        unsigned int cp = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
        append_cp(&out, cp);
        i += 2;
        continue;
      }
    }
    append_cp(&out, (u >= 0xD800 && u <= 0xDFFF) ? 0xFFFD : u);
  }
  return out;
}

// Latin-1 is a total mapping: every byte becomes U+00xx. It shows the bytes
// (mojibake for a multi-byte encoding) instead of deleting them, and the result
// is always valid UTF-8 - which is what keeps the turn alive.
std::string from_latin1(const std::string& raw) {
  std::string out;
  out.reserve(raw.size() + raw.size() / 2);
  for (size_t i = 0; i < raw.size(); ++i) {
    append_cp(&out, static_cast<unsigned char>(raw[i]));
  }
  return out;
}

#ifdef _WIN32
std::wstring ansi_to_wide(const std::string& raw, unsigned int codepage) {
  if (raw.empty()) return std::wstring();
  int n = ::MultiByteToWideChar(codepage, MB_ERR_INVALID_CHARS, raw.data(),
                                static_cast<int>(raw.size()), nullptr, 0);
  if (n <= 0) return std::wstring();
  std::wstring w(static_cast<size_t>(n), L'\0');
  if (::MultiByteToWideChar(codepage, MB_ERR_INVALID_CHARS, raw.data(),
                            static_cast<int>(raw.size()), &w[0], n) <= 0) {
    return std::wstring();
  }
  return w;
}

std::string wide_to_utf8(const std::wstring& w) {
  if (w.empty()) return std::string();
  int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                nullptr, 0, nullptr, nullptr);
  if (n <= 0) return std::string();
  std::string out(static_cast<size_t>(n), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &out[0], n,
                        nullptr, nullptr);
  return out;
}

// How "Chinese" does this wide string look? Used to prefer GB18030 over the
// system ANSI codepage on a non-Chinese Windows (a GBK file opened on an English
// machine would otherwise decode to accented Latin nonsense).
int cjk_score(const std::wstring& w) {
  int score = 0;
  for (size_t i = 0; i < w.size(); ++i) {
    unsigned int c = static_cast<unsigned int>(w[i]);
    if ((c >= 0x4E00 && c <= 0x9FFF) || (c >= 0x3400 && c <= 0x4DBF)) ++score;
  }
  return score;
}
#endif  // _WIN32

}  // namespace

std::string decode_to_utf8(const std::string& raw, std::string* encoding) {
  std::string enc = "utf-8";
  size_t skip = 0;
  bool utf16_le = false, utf16_be = false;
  if (raw.size() >= 3 && static_cast<unsigned char>(raw[0]) == 0xEF &&
      static_cast<unsigned char>(raw[1]) == 0xBB &&
      static_cast<unsigned char>(raw[2]) == 0xBF) {
    skip = 3;
    enc = "utf-8-bom";
  } else if (raw.size() >= 2 && static_cast<unsigned char>(raw[0]) == 0xFF &&
             static_cast<unsigned char>(raw[1]) == 0xFE) {
    skip = 2;
    utf16_le = true;
    enc = "utf-16le";
  } else if (raw.size() >= 2 && static_cast<unsigned char>(raw[0]) == 0xFE &&
             static_cast<unsigned char>(raw[1]) == 0xFF) {
    skip = 2;
    utf16_be = true;
    enc = "utf-16be";
  }
  if (utf16_le || utf16_be) {
    if (encoding) *encoding = enc;
    return sanitize(from_utf16(raw, utf16_be, skip));
  }
  std::string body = skip ? raw.substr(skip) : raw;
  // A UTF-16 file WITHOUT a BOM is common enough (concatenated output). ASCII
  // text in UTF-16 has NULs on ONE side of every pair and almost never on the
  // other - that parity is the tell. A single stray NUL (a 5-byte binary like
  // "ab\0cd") must NOT match: it used to be "detected" as UTF-16 and decoded into
  // garbage, which defeated the binary guard.
  if (body.size() >= 16) {
    size_t probe = body.size() < 512 ? body.size() : 512;
    probe &= ~static_cast<size_t>(1);
    size_t even_nul = 0, odd_nul = 0, pairs = probe / 2;
    for (size_t i = 0; i + 1 < probe; i += 2) {
      if (body[i] == '\0') ++even_nul;
      if (body[i + 1] == '\0') ++odd_nul;
    }
    if (pairs >= 8 &&
        (even_nul >= pairs * 3 / 4 || odd_nul >= pairs * 3 / 4)) {
      bool be = even_nul > odd_nul;
      if (encoding) *encoding = be ? "utf-16be" : "utf-16le";
      return sanitize(from_utf16(body, be, 0));
    }
  }
  if (valid(body)) {
    if (encoding) *encoding = enc;
    return skip ? body : raw;
  }
#ifdef _WIN32
  // The file is NOT valid UTF-8: it is text in some 8-bit encoding. Prefer the
  // system ANSI codepage (GBK on a Chinese Windows); on a non-Chinese system,
  // take GB18030 instead when it produces a clearly more Chinese reading.
  std::wstring w = ansi_to_wide(body, CP_ACP);
  std::wstring gb;
  bool gb_ok = false;
  if (w.empty()) {
    gb = ansi_to_wide(body, 54936);  // GB18030
    gb_ok = !gb.empty();
    if (gb_ok) w = gb;
  } else if (::GetACP() != 936) {
    gb = ansi_to_wide(body, 54936);
    gb_ok = !gb.empty();
    if (gb_ok && cjk_score(gb) >= 8 && cjk_score(gb) > cjk_score(w) * 2) w = gb;
  }
  if (!w.empty()) {
    bool used_gb = gb_ok && w == gb;
    if (encoding) *encoding = used_gb ? "gb18030" : "gbk";
    return sanitize(wide_to_utf8(w));
  }
#endif
  // Never fail: byte-for-byte Latin-1 always yields well-formed UTF-8.
  if (encoding) *encoding = "latin1";
  return from_latin1(body);
}

}  // namespace utf8
}  // namespace fs
