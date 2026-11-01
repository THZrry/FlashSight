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

// Decode arbitrary file bytes into well-formed UTF-8.
//
// A project has files written by every editor on every platform: UTF-8 with or
// without a BOM, UTF-16 (Windows "Unicode" text, Notepad's default for years),
// and the legacy ANSI codepage (GBK on Chinese Windows, which is what an old
// .cpp/.txt edited by a native Chinese tool usually is). Reading those raw and
// pushing them into a tool result killed the whole turn: nlohmann::json throws
// on invalid UTF-8 when the message is dumped, and the exception unwound the
// loop before anything was persisted.
//
// Order: BOM (UTF-8 / UTF-16LE / UTF-16BE) -> valid UTF-8 -> system ANSI
// codepage -> GB18030 (a CJK heuristic for a non-Chinese locale) -> Latin-1.
// The last step never fails, so this always returns valid UTF-8.
// `encoding` (optional) receives a short name of what was used: "utf-8",
// "utf-8-bom", "utf-16le", "utf-16be", "gbk", "gb18030", "latin1".
std::string decode_to_utf8(const std::string& raw, std::string* encoding = nullptr);

}  // namespace utf8
}  // namespace fs
