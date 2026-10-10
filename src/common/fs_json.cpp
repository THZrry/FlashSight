// fs_json.cpp - atomic json write (write tmp then rename; never leaves half file)
#include "fs_json.hpp"

#include <fstream>

#include <ghc/filesystem.hpp>

namespace fs {
namespace jsonutil {

// 原子写 JSON：先写临时文件再 rename（写坏不残留半截文件）
bool write_atomic(const std::string& path, const Json& j, std::string* err) {
  ghc::filesystem::path p(path);
  try {
    ghc::filesystem::path parent = p.parent_path();
    if (!parent.empty() && !ghc::filesystem::exists(parent)) {
      ghc::filesystem::create_directories(parent);
    }
    std::string tmp = p.string() + ".tmp";
    {
      std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
      if (!ofs) {
        if (err) *err = "cannot open temp file: " + tmp;
        return false;
      }
      ofs << j.dump(2);
      ofs.flush();
      if (!ofs) {
        if (err) *err = "write failed: " + tmp;
        return false;
      }
    }
    std::error_code ec;
    ghc::filesystem::rename(tmp, path, ec);
    if (ec) {
      // Windows: rename over existing may fail; remove target then retry
      ghc::filesystem::remove(path, ec);
      ghc::filesystem::rename(tmp, path, ec);
      if (ec) {
        if (err) *err = "rename failed: " + ec.message();
        ghc::filesystem::remove(tmp);
        return false;
      }
    }
    return true;
  } catch (const std::exception& e) {
    if (err) *err = std::string("json write error: ") + e.what();
    return false;
  }
}

}  // namespace jsonutil
}  // namespace fs