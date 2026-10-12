// noise_detect.hpp - 写入内容噪声检测（addon2.md）
// 当写入内容出现随机 unicode 其它区字符（如阿拉伯字符）且两侧引号不平衡时，
// 提示模型重检查。检测规则（addon2 判据）：
//   - 非 BMP 区字符 / 异区短串（<=3 字符的异区连续出现，如 2 个阿拉伯字符）
//   - 引号数量不平衡（"" '' 等成对引号不成对）
#pragma once

#include <string>

namespace fs {

// 噪声检测结果
struct NoiseReport {
  bool noisy = false;         // 是否判定为噪声
  std::string reason;         // 判据描述（中英双语，给模型看）
  int suspicious_chars = 0;   // 可疑字符数
};

// 检测一段文本是否为写入噪声；thresh 为可疑字符触发阈值（默认 2）
NoiseReport detect_noise(const std::string& text, int thresh = 2);

}  // namespace fs