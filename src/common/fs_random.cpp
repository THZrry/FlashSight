// fs_random.cpp - xorshift64* impl; seed 0 = entropy from time+address
#include "fs_random.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>

namespace fs {

namespace {

uint64_t splitmix64(uint64_t& x) {
  uint64_t z = (x += 0x9e3779b97f4a7c15ULL);
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  return z ^ (z >> 31);
}

const char kHex[] = "0123456789abcdef";

}  // namespace

XorShiftRandom::XorShiftRandom(uint64_t seed) {
  if (seed == 0) {
    auto now = std::chrono::system_clock::now().time_since_epoch().count();
    uintptr_t addr = reinterpret_cast<uintptr_t>(&state_);
    seed = static_cast<uint64_t>(now) ^ (static_cast<uint64_t>(addr) << 32) ^
           static_cast<uint64_t>(addr);
  }
  state_ = splitmix64(seed);
}

uint64_t XorShiftRandom::next_u64() {
  uint64_t x = state_;
  x ^= x >> 12;
  x ^= x << 25;
  x ^= x >> 27;
  state_ = x;
  return x * 0x2545F4914F6CDD1DULL;
}

std::string XorShiftRandom::hex(size_t nbytes) {
  std::string out;
  out.reserve(nbytes * 2);
  while (out.size() < nbytes * 2) {
    uint64_t v = next_u64();
    for (int i = 0; i < 16 && out.size() < nbytes * 2; ++i) {
      out.push_back(kHex[(v >> (i * 4)) & 0xf]);
    }
  }
  return out;
}

}  // namespace fs