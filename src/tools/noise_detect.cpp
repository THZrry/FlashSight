// noise_detect.cpp - 写入内容噪声检测实现（addon2.md）
#include "noise_detect.hpp"

#include <cstdint>

namespace fs {

namespace {

// UTF-8 解码一个码点；返回字节数（0=非法/截断）
int utf8_decode(const std::string& s, size_t i, uint32_t* cp) {
  uint8_t c = static_cast<uint8_t>(s[i]);
  if (c < 0x80) { *cp = c; return 1; }
  if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) {
    *cp = ((c & 0x1F) << 6) | (static_cast<uint8_t>(s[i + 1]) & 0x3F);
    return 2;
  }
  if ((c & 0xF0) == 0xE0 && i + 2 < s.size()) {
    *cp = ((c & 0x0F) << 12) | ((static_cast<uint8_t>(s[i + 1]) & 0x3F) << 6) |
          (static_cast<uint8_t>(s[i + 2]) & 0x3F);
    return 3;
  }
  if ((c & 0xF8) == 0xF0 && i + 3 < s.size()) {
    *cp = ((c & 0x07) << 18) | ((static_cast<uint8_t>(s[i + 1]) & 0x3F) << 12) |
          ((static_cast<uint8_t>(s[i + 2]) & 0x3F) << 6) |
          (static_cast<uint8_t>(s[i + 3]) & 0x3F);
    return 4;
  }
  return 0;
}

// 是否"随机异区"（非 CJK/拉丁/数字/常见符号/空白）：addon2 指阿拉伯等罕见 script。
// 只列代码/注释几乎不会出现的 script 区段。
bool is_random_script(uint32_t cp) {
  // 阿拉伯（0600-06FF, 0750-077F, 08A0-08FF, FB50-FDFF, FE70-FEFF）
  if (cp >= 0x0600 && cp <= 0x06FF) return true;
  if (cp >= 0x0750 && cp <= 0x077F) return true;
  if (cp >= 0x08A0 && cp <= 0x08FF) return true;
  if (cp >= 0xFB50 && cp <= 0xFDFF) return true;
  if (cp >= 0xFE70 && cp <= 0xFEFF) return true;
  // 希伯来（0590-05FF, FB1D-FB4F）
  if (cp >= 0x0590 && cp <= 0x05FF) return true;
  if (cp >= 0xFB1D && cp <= 0xFB4F) return true;
  // 泰文（0E00-0E7F）
  if (cp >= 0x0E00 && cp <= 0x0E7F) return true;
  // 高棉（1780-17FF）
  if (cp >= 0x1780 && cp <= 0x17FF) return true;
  // 天城文（0900-097F）
  if (cp >= 0x0900 && cp <= 0x097F) return true;
  // 吉里尔补充/扩展中罕见区（0500-052F 西里尔补充）
  if (cp >= 0x0500 && cp <= 0x052F) return true;
  // 特殊符号区（如"杂项符号"里的罕见项不列，避免误报）
  return false;
}

// 是否是分隔/正常字符（空白/标点/引号/括号/操作符）
bool is_separator(uint32_t cp) {
  return (cp < 0x80) || (cp >= 0x4E00 && cp <= 0x9FFF) ||  // CJK
         (cp >= 0x3400 && cp <= 0x4DBF) ||                 // CJK 扩展 A
         (cp >= 0x3000 && cp <= 0x303F) ||                 // CJK 标点
         (cp >= 0xFF00 && cp <= 0xFFEF) ||                 // 全角
         (cp >= 0x2000 && cp <= 0x206F);                   // 通用标点
}

}  // namespace

NoiseReport detect_noise(const std::string& text, int thresh) {
  NoiseReport rep;
  int suspicious = 0;
  int run = 0;  // 当前异区连续数
  bool in_normal = false;  // 前面出现过正常内容

  size_t i = 0;
  // 引号计数
  int dq = 0, sq = 0, bq = 0;   // " ' `
  while (i < text.size()) {
    uint32_t cp;
    int n = utf8_decode(text, i, &cp);
    if (n == 0) { ++i; continue; }
    if (cp == '"') ++dq;
    else if (cp == '\'') ++sq;
    else if (cp == '`') ++bq;
    if (is_random_script(cp)) {
      ++run;
      // 前置是正常内容才算可疑（避免整段外语注释误报）
      if (in_normal && run <= 3 && suspicious < 100000) {
        ++suspicious;
      }
    } else {
      run = 0;
      if (!is_separator(cp) || cp == '\n' || cp == '\t' || cp == '\r' || cp == ' ') {
        in_normal = true;
      }
    }
    i += static_cast<size_t>(n);
  }
  // 引号不平衡（奇数个）：仅作佐证信号（addon2 判据为"异区字符 + 引号不平衡"），
  // 单独不平衡不触发（英文注释里的撇号 don't 会误报）。
  bool unbalanced = (dq % 2 != 0) || (sq % 2 != 0) || (bq % 2 != 0);

  if (suspicious >= thresh) {
    rep.noisy = true;
    rep.suspicious_chars = suspicious;
    rep.reason = "检测到可疑随机异区字符（如阿拉伯/希伯来等罕见区，连续短串）。"
                 "Suspicious random foreign-script characters detected; please review your content.";
    if (unbalanced) {
      rep.reason += " 且引号数量不平衡. Quotes also unbalanced.";
    }
  }
  return rep;
}

}  // namespace fs