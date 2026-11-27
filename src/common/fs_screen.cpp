// fs_screen.cpp - screen capture per platform
//
// Windows: Desktop Duplication (DXGI) FIRST, GDI BitBlt as fallback.
//
// Why the order matters: BitBlt on a DWM-composited desktop is the legacy path.
// It works on most machines but returns a completely black frame in several
// real cases - a locked screen, an RDP session that is not attached, a
// protected-content window, some multi-GPU/HDR configurations. Desktop
// Duplication reads the same surface DWM composites, so it sees what the user
// sees. It needs D3D11 and Windows 8+, and it can also fail (no GPU adapter,
// duplication already held by another process), which is exactly why BitBlt
// stays as the fallback rather than being deleted.
//
// macOS: the system `screencapture` CLI (macOS 15 obsoletes the CoreGraphics
// image APIs) decoded into our RGBA buffer through stb.
//
// Linux: X11 XGetImage on the root window. Wayland has no portable capture API
// (it is deliberately restricted to the portal), so the capture reports that
// instead of failing mysteriously.
#include "fs_screen.hpp"

#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#elif defined(__APPLE__)
#include <ApplicationServices/ApplicationServices.h>
#include <CoreGraphics/CoreGraphics.h>
#else
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#if defined(__APPLE__)
#include <fstream>
#include <iterator>
#include <sstream>
#include <unistd.h>
#include <utility>
#endif

namespace fs {
namespace screen {

#if defined(_WIN32)

namespace {

Capture from_bgra(const uint8_t* src, int w, int h, int stride,
                  const std::string& note) {
  Capture out;
  if (w <= 0 || h <= 0 || !src) {
    out.error = "capture produced an empty bitmap";
    return out;
  }
  out.image.width = w;
  out.image.height = h;
  out.image.rgba.assign(static_cast<size_t>(w) * h * 4, 255);
  for (int y = 0; y < h; ++y) {
    const uint8_t* row = src + static_cast<size_t>(y) * stride;
    uint8_t* dst = out.image.rgba.data() + static_cast<size_t>(y) * w * 4;
    for (int x = 0; x < w; ++x) {
      dst[x * 4] = row[x * 4 + 2];
      dst[x * 4 + 1] = row[x * 4 + 1];
      dst[x * 4 + 2] = row[x * 4];
      dst[x * 4 + 3] = 255;  // desktop surfaces are opaque
    }
  }
  out.ok = true;
  out.note = note;
  return out;
}

// Is there any non-black pixel? A capture that "succeeds" but is uniformly
// black is not a usable screenshot: feeding it to a vision model produces a
// confident wrong answer, so it is treated as a failure.
bool has_colour(const image::Image& img) {
  const std::vector<uint8_t>& px = img.rgba;
  for (size_t i = 0; i + 2 < px.size(); i += 4 * 101) {
    if (px[i] || px[i + 1] || px[i + 2]) return true;
  }
  return false;
}

// ---- DXGI Desktop Duplication -------------------------------------------
//
// One COM call chain: create a D3D11 device, get the DXGI output it renders
// to, duplicate that output, grab one frame into a staging texture, map and
// copy. Everything is released on the way out (the RAII helper below), because
// a leaked duplication blocks the NEXT capture for the whole session - a
// failure mode that looks like "screenshots stopped working".
template <typename T>
void release(T*& p) {
  if (p) {
    p->Release();
    p = nullptr;
  }
}

struct ComFreed {
  ~ComFreed() {}
};

// Capture the output that contains (x, y). `region` empty means the whole output.
bool dxgi_capture(int want_x, int want_y, int want_w, int want_h, bool whole,
                  Capture* out) {
  ID3D11Device* device = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  IDXGIOutputDuplication* dup = nullptr;
  IDXGIResource* res = nullptr;
  ID3D11Texture2D* tex = nullptr;
  ID3D11Texture2D* staging = nullptr;
  bool ok = false;
  D3D_FEATURE_LEVEL level;

  HRESULT hr = D3D11CreateDevice(
      nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
      D3D11_SDK_VERSION, &device, &level, &ctx);
  if (FAILED(hr)) {
    // Some VMs only expose WARP; duplication still works through it.
    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                           D3D11_SDK_VERSION, &device, &level, &ctx);
  }
  if (FAILED(hr) || !device) return false;

  IDXGIDevice* dxgi_dev = nullptr;
  if (FAILED(device->QueryInterface(__uuidof(IDXGIDevice),
                                    reinterpret_cast<void**>(&dxgi_dev)))) {
    release(ctx);
    release(device);
    return false;
  }
  IDXGIAdapter* adapter = nullptr;
  if (FAILED(dxgi_dev->GetAdapter(&adapter))) {
    release(dxgi_dev);
    release(ctx);
    release(device);
    return false;
  }

  // Find the output containing the requested point (the primary one when the
  // caller asked for the whole desktop).
  IDXGIOutput* output = nullptr;
  DXGI_OUTPUT_DESC chosen;
  bool found = false;
  for (UINT i = 0;; ++i) {
    IDXGIOutput* o = nullptr;
    if (adapter->EnumOutputs(i, &o) == DXGI_ERROR_NOT_FOUND) break;
    if (!o) break;
    DXGI_OUTPUT_DESC d;
    if (SUCCEEDED(o->GetDesc(&d)) && d.AttachedToDesktop) {
      bool hit = whole;
      if (!whole) {
        RECT r = d.DesktopCoordinates;
        hit = want_x >= r.left && want_x < r.right && want_y >= r.top &&
              want_y < r.bottom;
      }
      if (hit) {
        output = o;
        chosen = d;
        found = true;
        break;
      }
    }
    o->Release();
  }
  if (!found) {
    release(adapter);
    release(dxgi_dev);
    release(ctx);
    release(device);
    return false;
  }

  IDXGIOutput1* output1 = nullptr;
  if (FAILED(output->QueryInterface(__uuidof(IDXGIOutput1),
                                    reinterpret_cast<void**>(&output1)))) {
    release(output);
    release(adapter);
    release(dxgi_dev);
    release(ctx);
    release(device);
    return false;
  }
  hr = output1->DuplicateOutput(device, &dup);
  release(output1);
  release(output);
  release(adapter);
  release(dxgi_dev);
  if (FAILED(hr) || !dup) {
    release(ctx);
    release(device);
    return false;
  }

  DXGI_OUTDUPL_FRAME_INFO info;
  hr = dup->AcquireNextFrame(200, &info, &res);
  if (FAILED(hr)) {
    release(dup);
    release(ctx);
    release(device);
    return false;
  }
  if (res && SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D),
                                           reinterpret_cast<void**>(&tex)))) {
    D3D11_TEXTURE2D_DESC desc;
    tex->GetDesc(&desc);
    // Region of the output to copy.
    int left = 0, top = 0, w = static_cast<int>(desc.Width),
        h = static_cast<int>(desc.Height);
    if (!whole) {
      left = want_x - chosen.DesktopCoordinates.left;
      top = want_y - chosen.DesktopCoordinates.top;
      w = want_w;
      h = want_h;
      if (left < 0) left = 0;
      if (top < 0) top = 0;
      if (left + w > static_cast<int>(desc.Width)) w = desc.Width - left;
      if (top + h > static_cast<int>(desc.Height)) h = desc.Height - top;
    }
    if (w > 0 && h > 0) {
      D3D11_TEXTURE2D_DESC sd = desc;
      sd.Usage = D3D11_USAGE_STAGING;
      sd.BindFlags = 0;
      sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      sd.MiscFlags = 0;
      sd.ArraySize = 1;
      sd.MipLevels = 1;
      sd.Width = static_cast<UINT>(w);
      sd.Height = static_cast<UINT>(h);
      if (SUCCEEDED(device->CreateTexture2D(&sd, nullptr, &staging)) && staging) {
        D3D11_BOX box;
        box.left = static_cast<UINT>(left);
        box.top = static_cast<UINT>(top);
        box.front = 0;
        box.right = static_cast<UINT>(left + w);
        box.bottom = static_cast<UINT>(top + h);
        box.back = 1;
        ctx->CopySubresourceRegion(staging, 0, 0, 0, 0, tex, 0, &box);
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
          std::string note = "desktop " + std::to_string(w) + "x" + std::to_string(h);
          Capture c = from_bgra(static_cast<const uint8_t*>(mapped.pData), w, h,
                                static_cast<int>(mapped.RowPitch), note);
          ctx->Unmap(staging, 0);
          if (c.ok && has_colour(c.image)) {
            c.backend = "dxgi";
            *out = c;
            ok = true;
          }
        }
      }
    }
  }
  dup->ReleaseFrame();
  release(staging);
  release(tex);
  release(res);
  release(dup);
  release(ctx);
  release(device);
  return ok;
}

// ---- GDI fallback --------------------------------------------------------
Capture gdi_capture_region(int x, int y, int w, int h) {
  Capture out;
  HDC screen = GetDC(nullptr);
  if (!screen) {
    out.error = "GetDC failed";
    return out;
  }
  HDC mem = CreateCompatibleDC(screen);
  HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
  if (!mem || !bmp) {
    if (bmp) DeleteObject(bmp);
    if (mem) DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    out.error = "cannot allocate a capture bitmap";
    return out;
  }
  HGDIOBJ old = SelectObject(mem, bmp);
  BOOL blt = BitBlt(mem, 0, 0, w, h, screen, x, y, SRCCOPY | CAPTUREBLT);
  SelectObject(mem, old);
  if (!blt) {
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    out.error = "BitBlt failed (is the desktop accessible?)";
    return out;
  }
  BITMAPINFO bi;
  std::memset(&bi, 0, sizeof(bi));
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = w;
  bi.bmiHeader.biHeight = -h;  // top-down
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  std::vector<uint8_t> buf(static_cast<size_t>(w) * h * 4);
  int got = GetDIBits(mem, bmp, 0, static_cast<UINT>(h), buf.data(), &bi,
                      DIB_RGB_COLORS);
  DeleteObject(bmp);
  DeleteDC(mem);
  ReleaseDC(nullptr, screen);
  if (got == 0) {
    out.error = "GetDIBits failed";
    return out;
  }
  Capture c = from_bgra(buf.data(), w, h, w * 4,
                        "region " + std::to_string(x) + "," + std::to_string(y) +
                            " " + std::to_string(w) + "x" + std::to_string(h));
  c.backend = "gdi";
  return c;
}

}  // namespace

Capture capture_region(int x, int y, int w, int h) {
  if (w <= 0 || h <= 0) {
    Capture out;
    out.error = "empty region";
    return out;
  }
  Capture out;
  // Desktop Duplication sees the composited surface; GDI is the fallback for
  // the configurations where duplication is unavailable.
  if (dxgi_capture(x, y, w, h, false, &out)) return out;
  out = gdi_capture_region(x, y, w, h);
  if (out.ok && !has_colour(out.image)) {
    out.ok = false;
    out.error =
        "the captured screen is completely black (both Desktop Duplication and "
        "GDI returned no colour): this session has no interactive desktop - a "
        "locked screen, an RDP session that is not attached, or a service context";
  }
  return out;
}

Capture capture_desktop() {
  int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
  int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
  int w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
  int h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
  if (w <= 0 || h <= 0) {
    Capture out;
    out.error = "no desktop metrics (session 0 or headless)";
    return out;
  }
  Capture out;
  if (dxgi_capture(0, 0, w, h, true, &out)) {
    out.note = "desktop " + std::to_string(out.image.width) + "x" +
               std::to_string(out.image.height);
    return out;
  }
  out = gdi_capture_region(x, y, w, h);
  if (out.ok) {
    out.note = "desktop " + std::to_string(w) + "x" + std::to_string(h);
    if (!has_colour(out.image)) {
      out.ok = false;
      out.error =
          "the captured screen is completely black (both Desktop Duplication and "
          "GDI returned no colour): this session has no interactive desktop - a "
          "locked screen, an RDP session that is not attached, or a service context";
    }
  }
  return out;
}

Capture capture_window() {
  Capture out;
  HWND fg = GetForegroundWindow();
  if (!fg) {
    out.error = "no foreground window";
    return out;
  }
  RECT rc;
  if (!GetWindowRect(fg, &rc)) {
    out.error = "GetWindowRect failed";
    return out;
  }
  int w = rc.right - rc.left;
  int h = rc.bottom - rc.top;
  if (w <= 0 || h <= 0) {
    out.error = "foreground window has no area (minimised?)";
    return out;
  }
  // Duplication is per-output and always returns the composited desktop, so the
  // window is cropped out of it; that is also why an occluded window still
  // captures correctly.
  out = capture_region(rc.left, rc.top, w, h);
  if (out.ok) out.note = "window " + std::to_string(w) + "x" + std::to_string(h);
  return out;
}

Support support() {
  Support s;
  s.desktop = true;
  s.region = true;
  s.window = true;
  s.backend = "dxgi+gdi";
  s.note = "Desktop Duplication (DXGI) with GDI fallback";
  return s;
}

#elif defined(__APPLE__)

namespace {

// macOS 15 (Sequoia) marks CGWindowListCreateImage/CGDisplayCreateImage as
// UNAVAILABLE in the SDK, so the old CoreGraphics path no longer compiles with
// the Xcode 16 toolchain. `screencapture` is a system binary that still uses
// the supported capture path (and the same Screen Recording permission prompt),
// so use it and decode the PNG with stb. It keeps this file free of ObjC while
// working from macOS 12 through 15+.
Capture capture_with_screencapture(const std::string& args,
                                   const std::string& note) {
  Capture out;
  char tmpl[] = "/tmp/flashsight_capXXXXXX";
  int fd = mkstemp(tmpl);
  if (fd < 0) {
    out.error = "cannot create a temporary capture path";
    return out;
  }
  ::close(fd);
  std::string path(tmpl);
  std::remove(path.c_str());  // let screencapture create it

  std::string cmd = "screencapture -x -t png ";
  if (!args.empty()) cmd += args + " ";
  cmd += "\"" + path + "\" 2>/dev/null";
  int rc = std::system(cmd.c_str());
  if (rc != 0) {
    std::remove(path.c_str());
    out.error = "screencapture failed (screen recording permission?)";
    return out;
  }
  std::ifstream in(path, std::ios::binary);
  std::string bytes((std::istreambuf_iterator<char>(in)),
                    std::istreambuf_iterator<char>());
  in.close();
  std::remove(path.c_str());
  if (bytes.empty()) {
    out.error = "screencapture produced no image";
    return out;
  }
  image::Image img;
  std::string err;
  if (!image::decode(bytes, &img, &err)) {
    out.error = "cannot decode screencapture output: " + err;
    return out;
  }
  out.image = std::move(img);
  out.ok = true;
  out.note = note;
  out.backend = "screencapture";
  return out;
}

}  // namespace

Capture capture_region(int x, int y, int w, int h) {
  if (w <= 0 || h <= 0) {
    Capture out;
    out.error = "region width/height must be positive";
    return out;
  }
  std::ostringstream opts;
  opts << "-R" << x << "," << y << "," << w << "," << h;
  return capture_with_screencapture(
      opts.str(), "region " + std::to_string(w) + "x" + std::to_string(h));
}

Capture capture_desktop() {
  return capture_with_screencapture("", "desktop");
}

Capture capture_window() {
  // The frontmost on-screen normal window. CGWindowListCopyWindowInfo is not
  // obsoleted (only CreateImage is), so the window id is still available.
  CFArrayRef list = CGWindowListCopyWindowInfo(
      kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements,
      kCGNullWindowID);
  if (!list) {
    Capture out;
    out.error = "cannot enumerate windows (screen recording permission?)";
    return out;
  }
  CGWindowID target = 0;
  CFIndex n = CFArrayGetCount(list);
  for (CFIndex i = 0; i < n; ++i) {
    CFDictionaryRef d =
        static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(list, i));
    CFNumberRef layer = static_cast<CFNumberRef>(
        CFDictionaryGetValue(d, kCGWindowLayer));
    int lv = -1;
    if (layer) CFNumberGetValue(layer, kCFNumberIntType, &lv);
    if (lv != 0) continue;
    CFNumberRef wid = static_cast<CFNumberRef>(
        CFDictionaryGetValue(d, kCGWindowNumber));
    if (wid) CFNumberGetValue(wid, kCGWindowIDCFNumberType, &target);
    if (target) break;
  }
  CFRelease(list);
  if (!target) {
    Capture out;
    out.error = "no normal window on screen";
    return out;
  }
  return capture_with_screencapture("-l" + std::to_string(target) + " -o",
                                    "window");
}

Support support() {
  Support s;
  s.desktop = true;
  s.region = true;
  s.window = true;
  s.backend = "screencapture";
  s.note = "macOS screencapture (needs Screen Recording permission)";
  return s;
}

#else  // Linux / X11

namespace {

Capture from_ximage(XImage* img, const std::string& note) {
  Capture out;
  if (!img) {
    out.error = "XGetImage failed (no display, or Wayland without XWayland)";
    return out;
  }
  out.image.width = img->width;
  out.image.height = img->height;
  out.image.rgba.assign(static_cast<size_t>(img->width) * img->height * 4, 255);
  for (int y = 0; y < img->height; ++y) {
    for (int x = 0; x < img->width; ++x) {
      unsigned long pixel = XGetPixel(img, x, y);
      uint8_t* d = &out.image.rgba[(static_cast<size_t>(y) * img->width + x) * 4];
      d[0] = static_cast<uint8_t>((pixel >> 16) & 0xFF);
      d[1] = static_cast<uint8_t>((pixel >> 8) & 0xFF);
      d[2] = static_cast<uint8_t>(pixel & 0xFF);
      d[3] = 255;
    }
  }
  XDestroyImage(img);
  out.ok = true;
  out.note = note;
  out.backend = "x11";
  return out;
}

}  // namespace

Capture capture_region(int x, int y, int w, int h) {
  Capture out;
  Display* dpy = XOpenDisplay(nullptr);
  if (!dpy) {
    out.error = "cannot open the X display (Wayland is not supported; run under X11 "
                "or XWayland)";
    return out;
  }
  Window root = DefaultRootWindow(dpy);
  XImage* img = XGetImage(dpy, root, x, y, static_cast<unsigned>(w),
                          static_cast<unsigned>(h), AllPlanes, ZPixmap);
  XCloseDisplay(dpy);
  return from_ximage(img, "region " + std::to_string(w) + "x" + std::to_string(h));
}

Capture capture_desktop() {
  Capture out;
  Display* dpy = XOpenDisplay(nullptr);
  if (!dpy) {
    out.error = "cannot open the X display (Wayland is not supported; run under X11 "
                "or XWayland)";
    return out;
  }
  Window root = DefaultRootWindow(dpy);
  int w = DisplayWidth(dpy, DefaultScreen(dpy));
  int h = DisplayHeight(dpy, DefaultScreen(dpy));
  XImage* img = XGetImage(dpy, root, 0, 0, static_cast<unsigned>(w),
                          static_cast<unsigned>(h), AllPlanes, ZPixmap);
  XCloseDisplay(dpy);
  return from_ximage(img, "desktop " + std::to_string(w) + "x" + std::to_string(h));
}

Capture capture_window() {
  Capture out;
  Display* dpy = XOpenDisplay(nullptr);
  if (!dpy) {
    out.error = "cannot open the X display (Wayland is not supported)";
    return out;
  }
  Window root = DefaultRootWindow(dpy);
  Window focused = root;
  int revert = 0;
  XGetInputFocus(dpy, &focused, &revert);
  if (focused == root || focused == PointerRoot || focused == None) {
    XCloseDisplay(dpy);
    out.error = "no focused window";
    return out;
  }
  // Walk up to the top-level window (the one whose parent is root).
  Window cur = focused;
  for (int depth = 0; depth < 16; ++depth) {
    Window rt, parent;
    Window* children = nullptr;
    unsigned n = 0;
    if (!XQueryTree(dpy, cur, &rt, &parent, &children, &n)) break;
    if (children) XFree(children);
    if (parent == root || parent == 0) break;
    cur = parent;
  }
  XWindowAttributes attr;
  if (!XGetWindowAttributes(dpy, cur, &attr)) {
    XCloseDisplay(dpy);
    out.error = "cannot read window attributes";
    return out;
  }
  XImage* img = XGetImage(dpy, cur, 0, 0, static_cast<unsigned>(attr.width),
                          static_cast<unsigned>(attr.height), AllPlanes, ZPixmap);
  XCloseDisplay(dpy);
  return from_ximage(img, "window " + std::to_string(attr.width) + "x" +
                             std::to_string(attr.height));
}

Support support() {
  Support s;
  Display* dpy = XOpenDisplay(nullptr);
  if (!dpy) {
    s.note = "no X display (Wayland is not supported; XWayland works)";
    return s;
  }
  XCloseDisplay(dpy);
  s.desktop = true;
  s.region = true;
  s.window = true;
  s.backend = "x11";
  s.note = "X11 XGetImage";
  return s;
}

#endif

}  // namespace screen
}  // namespace fs
