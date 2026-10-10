// fs_jsonl.cpp - JSONL append-only reader/writer (bad-line tolerant)
#include "fs_jsonl.hpp"

#include <cstdint>

#include <ghc/filesystem.hpp>

namespace fs {

bool JsonlWriter::ensure_open(std::string* err) {
  if (f_) return true;
  try {
    ghc::filesystem::path p(path_);
    if (!p.parent_path().empty() && !ghc::filesystem::exists(p.parent_path())) {
      ghc::filesystem::create_directories(p.parent_path());
    }
    f_.reset(new std::ofstream(path_, std::ios::binary | std::ios::app));
    if (!*f_) {
      if (err) *err = "cannot open jsonl: " + path_;
      f_.reset();
      return false;
    }
    return true;
  } catch (...) {
    if (err) *err = "cannot create dirs for: " + path_;
    return false;
  }
}

bool JsonlWriter::append(const Json& line, std::string* err) {
  if (!ensure_open(err)) return false;
  *f_ << line.dump() << '\n';
  f_->flush();
  if (!*f_) {
    if (err) *err = "jsonl append failed: " + path_;
    return false;
  }
  return true;
}

void JsonlWriter::close() {
  if (f_) {
    f_->flush();
    f_.reset();
  }
}

bool read_jsonl(const std::string& path, std::vector<Json>* out,
                uint64_t* bad_lines) {
  if (out) out->clear();
  if (bad_lines) *bad_lines = 0;
  std::ifstream ifs(path, std::ios::binary);
  if (!ifs) return false;
  std::string line;
  uint64_t bad = 0;
  while (std::getline(ifs, line)) {
    if (line.empty()) continue;
    if (!line.empty() && line.back() == '\r') line.pop_back();  // Windows CRLF
    try {
      Json j = Json::parse(line);
      if (out) out->push_back(std::move(j));
    } catch (...) {
      ++bad;
    }
  }
  if (bad_lines) *bad_lines = bad;
  return true;
}

}  // namespace fs