// fs_encoding.cpp - subprocess output -> valid UTF-8
#include "fs_encoding.hpp"

#include <cstdint>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace fs {
namespace encoding {

namespace {

bool valid_utf8(const std::string& s, size_t* bad_at) {
  size_t i = 0;
  while (i < s.size()) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    size_t need = 0;
    if (c < 0x80) {
      ++i;
      continue;
    } else if ((c & 0xE0) == 0xC0) {
      need = 1;
    } else if ((c & 0xF0) == 0xE0) {
      need = 2;
    } else if ((c & 0xF8) == 0xF0) {
      need = 3;
    } else {
      if (bad_at) *bad_at = i;
      return false;
    }
    if (i + need >= s.size()) {
      if (bad_at) *bad_at = i;
      return false;
    }
    for (size_t k = 1; k <= need; ++k) {
      if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) {
        if (bad_at) *bad_at = i;
        return false;
      }
    }
    i += need + 1;
  }
  return true;
}

// trailing bytes that could still grow into a valid sequence with more data
// (UTF-8 variant: a partial multi-byte sequence at the end)
size_t incomplete_tail_utf8(const std::string& s) {
  size_t n = s.size();
  if (n == 0) return 0;
  size_t i = n;
  while (i > 0 && (static_cast<unsigned char>(s[i - 1]) & 0xC0) == 0x80) --i;
  if (i == 0) return 0;
  unsigned char c = static_cast<unsigned char>(s[i - 1]);
  size_t need = 0;
  if ((c & 0xE0) == 0xC0) need = 2;
  else if ((c & 0xF0) == 0xE0) need = 3;
  else if ((c & 0xF8) == 0xF0) need = 4;
  else return 0;
  size_t have = n - (i - 1);
  return have < need ? have : 0;
}

// GBK variant: scan pairs from the start; a lead byte at the very end without
// its trail byte is a partial sequence (a Chinese char split across reads).
size_t incomplete_tail_gbk(const std::string& s) {
  size_t i = 0;
  while (i < s.size()) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) {
      ++i;
    } else if (c >= 0x81 && c <= 0xFE) {
      if (i + 1 >= s.size()) return 1;  // lead byte, trail missing
      i += 2;
    } else {
      ++i;  // 0x80 / 0xFF: single invalid byte
    }
  }
  return 0;
}

#if defined(_WIN32)
// OEM code page (cmd.exe) <-> wide <-> UTF-8
std::string codepage_to_utf8(const std::string& bytes, UINT cp) {
  if (bytes.empty()) return std::string();
  int wlen = MultiByteToWideChar(cp, 0, bytes.data(),
                                 static_cast<int>(bytes.size()), nullptr, 0);
  if (wlen <= 0) return sanitize_utf8(bytes);
  std::wstring w(static_cast<size_t>(wlen), L'\0');
  MultiByteToWideChar(cp, 0, bytes.data(), static_cast<int>(bytes.size()), &w[0], wlen);
  int ulen = WideCharToMultiByte(CP_UTF8, 0, w.data(), wlen, nullptr, 0, nullptr, nullptr);
  if (ulen <= 0) return sanitize_utf8(bytes);
  std::string out(static_cast<size_t>(ulen), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), wlen, &out[0], ulen, nullptr, nullptr);
  return out;
}
#endif

}  // namespace

std::string sanitize_utf8(const std::string& bytes) {
  std::string out;
  out.reserve(bytes.size());
  size_t i = 0;
  while (i < bytes.size()) {
    unsigned char c = static_cast<unsigned char>(bytes[i]);
    size_t need = 0;
    if (c < 0x80) {
      out.push_back(static_cast<char>(c));
      ++i;
      continue;
    } else if ((c & 0xE0) == 0xC0) {
      need = 1;
    } else if ((c & 0xF0) == 0xE0) {
      need = 2;
    } else if ((c & 0xF8) == 0xF0) {
      need = 3;
    }
    bool ok = need > 0 && i + need < bytes.size();
    if (ok) {
      for (size_t k = 1; k <= need; ++k) {
        if ((static_cast<unsigned char>(bytes[i + k]) & 0xC0) != 0x80) {
          ok = false;
          break;
        }
      }
    }
    if (ok) {
      out.append(bytes, i, need + 1);
      i += need + 1;
    } else {
      out += "\xEF\xBF\xBD";  // U+FFFD
      ++i;
    }
  }
  return out;
}

std::string decode_output(const std::string& bytes, const std::string& enc,
                          std::string* pending) {
  std::string buf;
  if (pending && !pending->empty()) {
    buf = *pending;
    pending->clear();
  }
  buf += bytes;
  if (buf.empty()) return std::string();

  const bool want_utf8 = (enc == "UTF-8" || enc == "utf-8" || enc == "utf8");
  const bool want_gbk = (enc == "GBK" || enc == "gbk" || enc == "cp936");

  // 1) 末尾是"半个 UTF-8 序列"且前半段完全合法：等下一块数据（跨 read 边界
  //    的汉字不能被切成两个替换符）。显式 GBK 时不走这条路。
  size_t tail_utf8 = incomplete_tail_utf8(buf);
  if (tail_utf8 > 0 && !want_gbk) {
    std::string head = buf.substr(0, buf.size() - tail_utf8);
    if (valid_utf8(head, nullptr)) {
      if (pending) *pending = buf.substr(buf.size() - tail_utf8);
      return head;  // head 本身已是合法 UTF-8
    }
  }

  // 2) 合法 UTF-8 原样通过（bash / 已设 UTF-8 的 pwsh；ASCII 也在这里）
  if (want_utf8 || (!want_gbk && valid_utf8(buf, nullptr))) {
    return sanitize_utf8(buf);
  }

#if defined(_WIN32)
  // 3) 按代码页解码（GBK 显式，或系统默认的 OEM 代码页）。GBK 的汉字是双字节，
  //    尾部只剩一个 lead byte 时同样留待下一块。
  size_t tail_gbk = incomplete_tail_gbk(buf);
  if (tail_gbk > 0) {
    if (pending) *pending = buf.substr(buf.size() - tail_gbk);
    buf.resize(buf.size() - tail_gbk);
  }
  return codepage_to_utf8(buf, want_gbk ? 936 : GetOEMCP());
#else
  return sanitize_utf8(buf);
#endif
}

std::string utf8_to_codepage(const std::string& text, unsigned cp) {
#if defined(_WIN32)
  if (cp == 0 || text.empty()) return text;
  int wlen = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                 static_cast<int>(text.size()), nullptr, 0);
  if (wlen <= 0) return text;
  std::wstring w(static_cast<size_t>(wlen), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                      &w[0], wlen);
  BOOL used = FALSE;
  char def = '?';
  int blen = WideCharToMultiByte(cp, 0, w.data(), wlen, nullptr, 0, &def, &used);
  if (blen <= 0) return text;
  std::string out(static_cast<size_t>(blen), '\0');
  WideCharToMultiByte(cp, 0, w.data(), wlen, &out[0], blen, &def, &used);
  return out;
#else
  (void)cp;
  return text;
#endif
}

std::string encode_input(const std::string& text, const std::string& enc) {
  if (text.empty()) return text;
  if (enc == "UTF-8" || enc == "utf-8" || enc == "utf8") return text;
#if defined(_WIN32)
  if (enc == "GBK" || enc == "gbk" || enc == "cp936") {
    return utf8_to_codepage(text, 936);
  }
  // 系统默认：cmd.exe 等按 OEM 代码页读管道输入
  bool ascii = true;
  for (size_t i = 0; i < text.size(); ++i) {
    if (static_cast<unsigned char>(text[i]) >= 0x80) {
      ascii = false;
      break;
    }
  }
  if (ascii) return text;  // ASCII 在任何代码页下都一样
  return utf8_to_codepage(text, GetOEMCP());
#else
  (void)enc;
  return text;
#endif
}

}  // namespace encoding
}  // namespace fs
