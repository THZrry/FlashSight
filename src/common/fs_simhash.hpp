#ifndef FS_SIMHASH_HPP
#define FS_SIMHASH_HPP

#include <cstdint>
#include <string>

namespace fs {

class Similarity {
 public:
  static double normalized(const std::string& a, const std::string& b);
  static uint64_t simhash64(const std::string& s);
  static double edit_distance_ratio(const std::string& a, const std::string& b);
};

}  // namespace fs

#endif