// ui_path_range.hpp - find the path-looking intervals of a draft, greedily.
//
// WHY THIS EXISTS: path completion used to take the whitespace-delimited token
// around the caret. That is wrong for the way people actually type: the path is
// often glued to prose ("看下./src/ui"), followed by Chinese punctuation
// ("src/ui，然后"), or the caret is in the MIDDLE of it. The token approach then
// either asked the backend about "看下./src/ui" (no such directory) or replaced
// text from the wrong offset, moving the caret to the end of the draft.
//
// The rule the user asked for is positional: scan the whole draft once, greedily
// parse the intervals that can be paths, then HIT-TEST the caret against those
// intervals and use exactly that interval as the replacement range.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace fs {
namespace ui {

struct PathRange {
  size_t start = 0;  // byte offset of the first path character
  size_t end = 0;    // one past the last path character
};

// All maximal path-looking intervals in `text`, left to right. A range always
// contains a path separator ('/' or '\') and never starts inside a URL or on a
// CLI flag. The scan is greedy: once a start is recognised, every character
// that can belong to a path is consumed, so a directory part with spaces-free
// segments stays one interval.
std::vector<PathRange> parse_path_ranges(const std::string& text);

// Index into `ranges` of the interval the caret belongs to, or -1. `caret` is a
// byte offset and may equal a range's end (typing at the end of the path). When
// ranges touch at the caret, the one that ENDS there wins: it is the path the
// user just finished typing.
int path_range_at(const std::vector<PathRange>& ranges, size_t caret);

}  // namespace ui
}  // namespace fs
