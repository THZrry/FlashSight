// fs_hash.cpp - xxh64 (public-domain algorithm by Yann Collet) + file helper
#include "fs_hash.hpp"

#include <cstdio>
#include <fstream>
#include <vector>

namespace fs {
namespace hash {

namespace {

constexpr uint64_t kP1 = 11400714785074694791ULL;
constexpr uint64_t kP2 = 14029467366897019727ULL;
constexpr uint64_t kP3 = 1609587929392839161ULL;
constexpr uint64_t kP4 = 9650029242287828579ULL;
constexpr uint64_t kP5 = 2870177450012600261ULL;

inline uint64_t rotl(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }

inline uint64_t read64(const unsigned char* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
  return v;
}
inline uint32_t read32(const unsigned char* p) {
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(p[i]) << (8 * i);
  return v;
}

inline uint64_t round64(uint64_t acc, uint64_t input) {
  acc += input * kP2;
  acc = rotl(acc, 31);
  acc *= kP1;
  return acc;
}

inline uint64_t merge_round(uint64_t acc, uint64_t val) {
  val = round64(0, val);
  acc ^= val;
  acc = acc * kP1 + kP4;
  return acc;
}

}  // namespace

uint64_t xxh64(const void* data, size_t len, uint64_t seed) {
  const unsigned char* p = static_cast<const unsigned char*>(data);
  const unsigned char* end = p + len;
  uint64_t h64;

  if (len >= 32) {
    const unsigned char* limit = end - 32;
    uint64_t v1 = seed + kP1 + kP2;
    uint64_t v2 = seed + kP2;
    uint64_t v3 = seed;
    uint64_t v4 = seed - kP1;
    do {
      v1 = round64(v1, read64(p)); p += 8;
      v2 = round64(v2, read64(p)); p += 8;
      v3 = round64(v3, read64(p)); p += 8;
      v4 = round64(v4, read64(p)); p += 8;
    } while (p <= limit);
    h64 = rotl(v1, 1) + rotl(v2, 7) + rotl(v3, 12) + rotl(v4, 18);
    h64 = merge_round(h64, v1);
    h64 = merge_round(h64, v2);
    h64 = merge_round(h64, v3);
    h64 = merge_round(h64, v4);
  } else {
    h64 = seed + kP5;
  }
  h64 += static_cast<uint64_t>(len);

  while (p + 8 <= end) {
    uint64_t k1 = round64(0, read64(p));
    h64 ^= k1;
    h64 = rotl(h64, 27) * kP1 + kP4;
    p += 8;
  }
  if (p + 4 <= end) {
    h64 ^= static_cast<uint64_t>(read32(p)) * kP1;
    h64 = rotl(h64, 23) * kP2 + kP3;
    p += 4;
  }
  while (p < end) {
    h64 ^= static_cast<uint64_t>(*p) * kP5;
    h64 = rotl(h64, 11) * kP1;
    ++p;
  }
  h64 ^= h64 >> 33;
  h64 *= kP2;
  h64 ^= h64 >> 29;
  h64 *= kP3;
  h64 ^= h64 >> 32;
  return h64;
}

std::string xxh64_hex(const std::string& s) {
  uint64_t h = xxh64(s.data(), s.size(), 0);
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
  return std::string(buf);
}

std::string file_hex(const std::string& path) {
  std::ifstream ifs(path, std::ios::binary);
  if (!ifs) return std::string();
  // Stream the file instead of loading it: this is called on files we may not
  // want to read into memory at all (a large log, a binary).
  uint64_t h = 0;
  std::vector<char> buf(64 * 1024);
  size_t total = 0;
  // xxh64 is not incremental in this tiny implementation, so accumulate a
  // rolling mix: hash each block and fold it in. Collision resistance is not
  // the goal - "did the bytes change" is.
  while (ifs) {
    ifs.read(buf.data(), static_cast<std::streamsize>(buf.size()));
    std::streamsize n = ifs.gcount();
    if (n <= 0) break;
    uint64_t block = xxh64(buf.data(), static_cast<size_t>(n), total);
    h = (h ^ block) * kP1 + kP4;
    total += static_cast<size_t>(n);
  }
  h ^= total;
  h ^= h >> 33;
  h *= kP2;
  h ^= h >> 29;
  h *= kP3;
  h ^= h >> 32;
  char out[17];
  std::snprintf(out, sizeof(out), "%016llx", static_cast<unsigned long long>(h));
  return std::string(out);
}

std::string short_hex(const std::string& s) {
  return s.size() > 12 ? s.substr(0, 12) : s;
}

}  // namespace hash
}  // namespace fs
