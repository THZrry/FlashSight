// fs_simhash.cpp - normalized similarity implementation
#include "fs_simhash.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace fs {

namespace {

// 特征权重表：每个 bigram 一个 token
void add_bigram(std::uint64_t* bits, size_t nbits, const std::string& s) {
  // 用 count-min sketch 风格的叠加：每个 bigram 的 hash 在各 bit 位置翻转
  std::uint64_t h = 1469598103934665603ULL;
  if (s.size() == 0) return;
  // 首字符单特征（避免空串）
  for (size_t i = 0; i + 1 < s.size(); ++i) {
    h ^= static_cast<unsigned char>(s[i]);
    h *= 1099511628211ULL;
    h ^= static_cast<unsigned char>(s[i + 1]);
    h *= 1099511628211ULL;
    // 用 h 的高位作为特征：对每个 bit，若该位为 1 则加 1，否则减 1
    for (size_t b = 0; b < nbits; ++b) {
      if ((h >> b) & 1ULL) {
        bits[b]++;
      } else {
        bits[b]--;
      }
    }
  }
}

}  // namespace

std::uint64_t Similarity::simhash64(const std::string& s) {
  std::vector<std::int64_t> bits(64, 0);
  add_bigram(reinterpret_cast<std::uint64_t*>(bits.data()), 64, s);
  std::uint64_t out = 0;
  for (size_t b = 0; b < 64; ++b) {
    if (bits[b] >= 0) out |= (1ULL << b);
  }
  return out;
}

double Similarity::edit_distance_ratio(const std::string& a, const std::string& b) {
  if (a == b) return 1.0;
  size_t la = a.size(), lb = b.size();
  if (la == 0 || lb == 0) return 0.0;
  // DP 求编辑距离（行滚动）
  std::vector<size_t> prev(lb + 1), cur(lb + 1);
  for (size_t j = 0; j <= lb; ++j) prev[j] = j;
  for (size_t i = 1; i <= la; ++i) {
    cur[0] = i;
    for (size_t j = 1; j <= lb; ++j) {
      size_t cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
      cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
    }
    prev.swap(cur);
  }
  size_t maxlen = std::max(la, lb);
  return 1.0 - static_cast<double>(prev[lb]) / static_cast<double>(maxlen);
}

double Similarity::normalized(const std::string& a, const std::string& b) {
  if (a == b) return 1.0;
  std::uint64_t ha = simhash64(a), hb = simhash64(b);
  // 汉明距离归一化：相似度 = 1 - (hamming/64)
  std::uint64_t x = ha ^ hb;
  int hamming = 0;
  while (x) {
    hamming += static_cast<int>(x & 1ULL);
    x >>= 1;
  }
  double sim = 1.0 - static_cast<double>(hamming) / 64.0;
  // 与编辑距离相似度融合（各占一半），减少 SimHash 误判
  double edit = edit_distance_ratio(a, b);
  return (sim + edit) / 2.0;
}

}  // namespace fs