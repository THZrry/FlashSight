// fs_random.hpp - injectable rng (arch.md §6: 随机数可注入，单测固定种子)
#pragma once

#include <cstdint>
#include <string>

namespace fs {

// 随机接口：单测注入固定序列，生产用 XorShiftRandom
class Random {
 public:
  virtual ~Random() = default;
  virtual uint64_t next_u64() = 0;
  virtual std::string hex(size_t nbytes) = 0;  // 用于 sid/tid 等
};

// 生产实现：xorshift64*（无第三方依赖；seed=0 时用真实随机源初始化）
class XorShiftRandom final : public Random {
 public:
  explicit XorShiftRandom(uint64_t seed = 0);
  uint64_t next_u64() override;
  std::string hex(size_t nbytes) override;

 private:
  uint64_t state_;
};

}  // namespace fs