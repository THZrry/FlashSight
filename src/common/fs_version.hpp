// FlashSight version & build info (common)
#pragma once

// A release build passes -DFS_VERSION_STRING="<tag>" so the binary reports the
// tag it was cut from. Local builds keep the default below.
#ifndef FS_VERSION_STRING
#define FS_VERSION_STRING "0.1.0"
#endif

#define FS_VERSION_MAJOR 0
#define FS_VERSION_MINOR 1
#define FS_VERSION_PATCH 0

namespace fs {

inline const char* version_string() { return FS_VERSION_STRING; }

}  // namespace fs
