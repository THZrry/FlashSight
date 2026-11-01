// fs_utf8.hpp - UTF-8 safe string helpers
//
// WHY: every tool result, prompt and history entry is dumped to JSON before it
// leaves the process, and nlohmann::json THROWS (type_error 316) on invalid
// UTF-8. A plain `s.substr(0, 4000)` cuts inside a 3-byte CJK character and
// kills the whole turn - which is exactly what happened to the agent prompt.
// Anything that truncates user/tool text must go through here.
#pragma once

#include <cstddef>
#include <string>

namespace fs {
namespace utf8 {

// Length of the character starting at `i` (1..4), or 0 if the bytes at `i` are
// not a well-formed sequence.
size_t char_len(const std::string& s, size_t i);

// Largest length <= max_bytes that ends on a character boundary.
size_t safe_cut(const std::string& s, size_t max_bytes);

// Truncate, appending `suffix` (e.g. "\n... (truncated)") only when cut.
std::string truncate(const std::string& s, size_t max_bytes,
                     const std::string& suffix = "");

// True when the whole string is well-formed UTF-8.
bool valid(const std::string& s);

// Remove ill-formed bytes (defence in depth for text that came from a terminal
// or a file with a legacy encoding).
std::string sanitize(const std::string& s);

}  // namespace utf8
}  // namespace fs
