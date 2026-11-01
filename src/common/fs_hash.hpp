// fs_hash.hpp - content hashing for the edit protocol
//
// WHY A HASH AND NOT A TIMESTAMP: the model must be able to say "I am editing
// the file I just read". A mtime is not reliable (a save within the same
// timestamp tick, a checkout, a formatter writing identical bytes), and a full
// copy of the old content costs tokens. A short hash is cheap to print, cheap to
// compare, and detects exactly the failure that matters: the file changed
// between the read and the edit.
//
// xxh64 is implemented here (no dependency): the project already carries no
// hashing library, and xxh64 is ~60 lines with good distribution.
#pragma once

#include <cstdint>
#include <string>

namespace fs {
namespace hash {

// 64-bit hash of a byte buffer.
uint64_t xxh64(const void* data, size_t len, uint64_t seed = 0);

// 16 lowercase hex chars.
std::string xxh64_hex(const std::string& s);

// Hash of the file's current content, "" when the file cannot be read.
std::string file_hex(const std::string& path);

// Short form used in tool output / the edit protocol: first 12 hex chars
// (48 bits). Enough to catch a concurrent edit, short enough to copy by hand.
std::string short_hex(const std::string& s);

}  // namespace hash
}  // namespace fs
