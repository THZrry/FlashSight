// fs_json.hpp - JSON helpers (nlohmann/json)
#pragma once

#include <string>

// The vendored dependency is nlohmann's single header at
// 3rdparty/json/json.hpp (scripts/fetch_3rdparty.py). Do not include
// "nlohmann/json.hpp": that path only existed on machines where someone had
// created a shim by hand, so a clean checkout failed with "nlohmann/json.hpp:
// No such file or directory".
#include "json.hpp"

namespace fs {

using Json = nlohmann::json;

namespace jsonutil {

// 安全取值：key 缺失或类型不符返回缺省
template <typename T>
T get(const Json& j, const std::string& key, const T& def) {
  try {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return def;
    return it->get<T>();
  } catch (...) {
    return def;
  }
}

// 安全取值：int64 自动转换
inline int64_t get_int(const Json& j, const std::string& key, int64_t def) {
  try {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return def;
    if (it->is_number()) return it->get<int64_t>();
    return def;
  } catch (...) {
    return def;
  }
}

// 安全取值：double
inline double get_number(const Json& j, const std::string& key, double def) {
  try {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return def;
    if (it->is_number()) return it->get<double>();
    return def;
  } catch (...) {
    return def;
  }
}

}  // namespace jsonutil

// Serialize JSON without ever throwing on bad bytes.
//
// nlohmann::json::dump() throws type_error.316 the moment a string holds invalid
// UTF-8 - and a tool result, a terminal line or a model delta CAN hold exactly
// that (a GBK file read before decoding existed, a terminal writing CP936, a
// broken provider chunk). An exception thrown while building a message unwound
// the whole turn and nothing was persisted, so every dump of untrusted content
// goes through here. Invalid sequences become U+FFFD instead of losing the turn.
inline std::string dump_safe(const Json& j, int indent = -1) {
  return j.dump(indent, ' ', false, Json::error_handler_t::replace);
}

// 原子写 JSON：先写临时文件再 rename（写坏不残留半截文件），实现见 fs_json.cpp
namespace jsonutil {
bool write_atomic(const std::string& path, const Json& j, std::string* err);
}  // namespace jsonutil

}  // namespace fs