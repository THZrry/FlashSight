// fs_jsonl.hpp - JSONL writer/reader (10-appcore、usage.md §1: append-only, tolerate bad lines)
#pragma once

#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "fs_json.hpp"

namespace fs {

class JsonlWriter {
 public:
  // append-only writer; open lazily on first append (so empty path never touches disk)
  explicit JsonlWriter(const std::string& path) : path_(path) {}
  ~JsonlWriter() { close(); }
  JsonlWriter(const JsonlWriter&) = delete;
  JsonlWriter& operator=(const JsonlWriter&) = delete;
  JsonlWriter(JsonlWriter&&) = delete;
  JsonlWriter& operator=(JsonlWriter&&) = delete;

  bool append(const Json& line, std::string* err = nullptr);
  void close();

 private:
  std::string path_;
  std::unique_ptr<std::ofstream> f_;
  bool ensure_open(std::string* err);
};

// 读全部行（宽容：坏行跳过，计数坏行）
struct JsonlReadResult {
  bool ok = true;
  std::vector<Json> lines;
  uint64_t bad_lines = 0;
};

bool read_jsonl(const std::string& path, std::vector<Json>* out,
                uint64_t* bad_lines = nullptr);

}  // namespace fs