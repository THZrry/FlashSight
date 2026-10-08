// FlashSight version & build info (common)
#pragma once

#define FS_VERSION_MAJOR 0
#define FS_VERSION_MINOR 1
#define FS_VERSION_PATCH 0
#define FS_VERSION_STRING "0.1.0"

namespace fs {

inline const char* version_string() { return FS_VERSION_STRING; }

}  // namespace fs