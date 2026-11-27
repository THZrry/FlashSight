// fs_screen.hpp - cross-platform screen capture
//
// WHY A SEPARATE BACKEND
// The tool layer must not contain #ifdef'd OS calls, and a screenshot has to
// work on every platform the app ships for. Each platform gets its own
// implementation behind this interface:
//
//   Windows : Desktop Duplication (DXGI) first - the modern path that sees the
//             DWM-composited desktop, multi-GPU and HDR surfaces; falls back to
//             GDI BitBlt when duplication is unavailable (RDP, no GPU, older
//             Windows). BitBlt alone returns a black frame on a composited
//             desktop, which is why the newer API is tried first.
//   macOS   : system `screencapture` CLI (CoreGraphics image APIs are
//             unavailable in the macOS 15 SDK)
//   Linux   : X11 XGetImage on the root window (X11 only; Wayland has no
//             portable capture API and says so)
//
// The output is always RGBA8, so the caller never branches.
#pragma once

#include <string>

#include "fs_image.hpp"

namespace fs {
namespace screen {

struct Capture {
  bool ok = false;
  std::string error;
  image::Image image;
  // Human-readable description of what was captured ("desktop 1920x1080").
  std::string note;
  // Which backend produced it: "dxgi" | "gdi" | "coregraphics" | "x11".
  // Reported because "which one ran" is the first question when a capture looks
  // wrong, and the answer was previously unknowable.
  std::string backend;
};

// Capture the whole desktop.
Capture capture_desktop();

// Capture one rectangle of the desktop (virtual-screen coordinates).
Capture capture_region(int x, int y, int w, int h);

// Capture the frontmost window. Not implemented everywhere; `error` says why.
Capture capture_window();

// Platform support report for the UI/settings and for a clear tool error.
struct Support {
  bool desktop = false;
  bool region = false;
  bool window = false;
  std::string note;      // e.g. "X11 required" or "Wayland is not supported"
  std::string backend;   // the backend that would be used
};
Support support();

}  // namespace screen
}  // namespace fs
