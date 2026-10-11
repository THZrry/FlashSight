// overfit.hpp - overfit detector (agent-loop.md 5, default off)
#pragma once

#include <string>
#include <vector>

namespace fs {

// OverfitDetector: fires truncate.hint when recent outputs are too similar.
// Backend never truncates; only the hint event is emitted.
class OverfitDetector {
 public:
  struct Options {
    Options() : enabled(false), threshold(0.9), window(3), repeat(3) {}
    bool enabled;
    double threshold;
    int window;
    int repeat;
  };

  explicit OverfitDetector(Options opts = Options()) : opts_(opts) {}

  void set_enabled(bool e) { opts_.enabled = e; }
  bool enabled() const { return opts_.enabled; }

  // push one output; returns true when hint should fire
  bool push(const std::string& text);

  void clear() { recent_.clear(); }

 private:
  Options opts_;
  std::vector<std::string> recent_;
};

}  // namespace fs