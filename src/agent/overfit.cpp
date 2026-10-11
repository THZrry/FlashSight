// overfit.cpp - OverfitDetector implementation
#include "overfit.hpp"

#include "fs_simhash.hpp"

namespace fs {

bool OverfitDetector::push(const std::string& text) {
  if (!opts_.enabled) return false;
  recent_.push_back(text);
  while ((int)recent_.size() > opts_.window) recent_.erase(recent_.begin());
  if ((int)recent_.size() < opts_.repeat) return false;
  // 检查最近 repeat 次是否两两相似度>阈值
  int n = static_cast<int>(recent_.size());
  for (int i = n - opts_.repeat; i < n; ++i) {
    for (int j = i + 1; j < n; ++j) {
      double sim = Similarity::normalized(recent_[i], recent_[j]);
      if (sim < opts_.threshold) return false;
    }
  }
  return true;
}

}  // namespace fs