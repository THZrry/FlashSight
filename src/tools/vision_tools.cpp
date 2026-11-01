// vision_tools.cpp - screenshot / read_image
//
// Both work for real now:
//   * screenshot captures the desktop, a region or a window through the
//     cross-platform backend (common/fs_screen: GDI / CoreGraphics / X11).
//   * read_image reads a local image, sniffs the format, decodes PNG when it
//     needs to downscale, and returns a data URI the model can actually see.
//
// WHAT THE MODEL GETS
// The tool returns metadata plus `dataUri`. The agent loop turns that into a
// real image content block in the next request, which is what makes the model
// able to look at it. Returning only a file path would make the tool useless:
// the model cannot open files.
#include "vision_tools.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

#include <ghc/filesystem.hpp>

#include "fs_image.hpp"
#include "fs_path.hpp"
#include "fs_screen.hpp"

namespace fs {

namespace {

constexpr int64_t kMaxSourceBytes = 40LL * 1024 * 1024;  // refuse to even load
constexpr int64_t kMaxPayloadBytes = 4LL * 1024 * 1024;  // API request budget

std::string read_binary(const ghc::filesystem::path& p, bool* ok) {
  std::ifstream ifs(p.string(), std::ios::binary);
  if (!ifs) {
    *ok = false;
    return std::string();
  }
  std::ostringstream ss;
  ss << ifs.rdbuf();
  *ok = true;
  return ss.str();
}

}  // namespace

void register_vision_tools(ToolRegistry* registry) {
  // ----- screenshot --------------------------------------------------------
  {
    ToolDef def;
    def.name = "screenshot";
    def.description =
        "截屏（桌面/区域/窗口）. Capture the screen so you can look at it. "
        "target: screen (default) | region | window. "
        "For region, pass x/y/width/height in virtual-screen coordinates. "
        "Returns an image the model can see.";
    Json params = Json::object();
    params["type"] = "object";
    Json props = Json::object();
    props["target"] = Json{{"type", "string"},
                           {"description", "screen(默认)|region|window"}};
    props["x"] = Json{{"type", "integer"}, {"description", "区域左上角 X"}};
    props["y"] = Json{{"type", "integer"}, {"description", "区域左上角 Y"}};
    props["width"] = Json{{"type", "integer"}, {"description", "区域宽"}};
    props["height"] = Json{{"type", "integer"}, {"description", "区域高"}};
    props["maxSide"] = Json{{"type", "integer"},
                            {"description", "最大边像素（默认1600，缩小以省 token）"}};
    params["properties"] = props;
    def.parameters = params;
    registry->register_tool("screenshot", def,
      [](const ToolContext& ctx, const Json& args) -> FsResult {
        // A user who turned computer control OFF means it: refuse rather than
        // capture anyway. This is a feature switch, not the permission gate
        // (the gate is what produces the approval prompt).
        if (ctx.computer_enabled && !ctx.computer_enabled()) {
          return FsResult::error(
              ErrCode::kPermissionDenied,
              "screen capture is disabled in Settings > General > Computer control");
        }
        std::string target = jsonutil::get<std::string>(args, "target", "screen");
        screen::Capture cap;
        if (target == "region") {
          int x = static_cast<int>(jsonutil::get_int(args, "x", 0));
          int y = static_cast<int>(jsonutil::get_int(args, "y", 0));
          int w = static_cast<int>(jsonutil::get_int(args, "width", 0));
          int h = static_cast<int>(jsonutil::get_int(args, "height", 0));
          if (w <= 0 || h <= 0) {
            return FsResult::error(ErrCode::kInvalidArg,
                                   "region needs width and height > 0");
          }
          cap = screen::capture_region(x, y, w, h);
        } else if (target == "window") {
          cap = screen::capture_window();
        } else {
          cap = screen::capture_desktop();
        }
        if (!cap.ok) {
          return FsResult::error(ErrCode::kNotSupported,
                                 cap.error.empty() ? "screen capture failed" : cap.error);
        }
        int max_side = static_cast<int>(jsonutil::get_int(args, "maxSide", 1600));
        // Floor of 64: below that a screenshot is unreadable, but a caller may
        // legitimately want a tiny thumbnail (tests, token pinching).
        if (max_side < 64) max_side = 64;
        image::Image img = image::resize_max_side(cap.image, max_side);
        std::string png = image::encode_png(img);
        if (static_cast<int64_t>(png.size()) > kMaxPayloadBytes) {
          // Still too big after resizing: shrink harder rather than fail.
          img = image::resize_max_side(cap.image, 800);
          png = image::encode_png(img);
        }
        Json data = Json::object();
        data["target"] = target;
        data["mime"] = "image/png";
        data["width"] = img.width;
        data["height"] = img.height;
        data["bytes"] = static_cast<int64_t>(png.size());
        data["dataUri"] = image::data_uri(image::Format::kPng, png);
        data["note"] = cap.note;
        data["summary"] = cap.note + " -> " + std::to_string(img.width) + "x" +
                          std::to_string(img.height);
        return FsResult::success(data);
      });
  }

  // ----- read_image --------------------------------------------------------
  {
    ToolDef def;
    def.name = "read_image";
    def.description =
        "读取本地图片给模型看（PNG/JPEG/GIF/WebP/BMP）. Load an image so you can see it. "
        "path is relative to the project (or absolute under full permission). "
        "maxSide downscales (default 1600). Oversized files are refused with a "
        "clear message instead of being silently truncated.";
    Json params = Json::object();
    params["type"] = "object";
    Json props = Json::object();
    props["path"] = Json{{"type", "string"}, {"description", "图片路径"}};
    props["maxSide"] = Json{{"type", "integer"}, {"description", "最大边像素（默认1600）"}};
    params["properties"] = props;
    params["required"] = Json::array({"path"});
    def.parameters = params;
    registry->register_tool("read_image", def,
      [](const ToolContext& ctx, const Json& args) -> FsResult {
        std::string rel = jsonutil::get<std::string>(args, "path", "");
        if (rel.empty()) return FsResult::error(ErrCode::kInvalidArg, "empty path");
        ghc::filesystem::path p(rel);
        if (!p.is_absolute()) {
          ghc::filesystem::path base = pathutil::normalize(ctx.cwd);
          p = pathutil::normalize(base / rel);
          if (!pathutil::is_within(p, base) && p != base) {
            return FsResult::error(ErrCode::kInvalidArg,
                                   "path escapes project: " + rel);
          }
        } else if (ctx.permission != "full" && !ctx.approved) {
          return FsResult::deny("approve",
                                "reading an image outside the project needs approval");
        }
        std::error_code ec;
        if (!ghc::filesystem::is_regular_file(p, ec)) {
          return FsResult::error(ErrCode::kNotFound, "file not found: " + rel);
        }
        int64_t size = static_cast<int64_t>(ghc::filesystem::file_size(p, ec));
        if (size > kMaxSourceBytes) {
          return FsResult::error(
              ErrCode::kInvalidArg,
              "image is too large (" + std::to_string(size) + " bytes, limit " +
                  std::to_string(kMaxSourceBytes) + ")");
        }
        bool ok = false;
        std::string raw = read_binary(p, &ok);
        if (!ok) return FsResult::error(ErrCode::kIo, "cannot read: " + rel);
        image::Format fmt = image::sniff(raw);
        if (fmt == image::Format::kUnknown) {
          return FsResult::error(
              ErrCode::kInvalidArg,
              "not a recognised image (png/jpeg/gif/webp/bmp): " + rel);
        }
        int w = 0, h = 0;
        image::dimensions(raw, &w, &h);
        int max_side = static_cast<int>(jsonutil::get_int(args, "maxSide", 1600));
        // Floor of 64: below that a screenshot is unreadable, but a caller may
        // legitimately want a tiny thumbnail (tests, token pinching).
        if (max_side < 64) max_side = 64;

        Json data = Json::object();
        data["path"] = rel;
        data["format"] = image::name_of(fmt);
        data["sourceWidth"] = w;
        data["sourceHeight"] = h;
        data["sourceBytes"] = size;

        bool needs_work = std::max(w, h) > max_side || fmt == image::Format::kBmp;
        if (!needs_work) {
          // Pass the original bytes through: every vision API accepts these and
          // re-encoding a JPEG would only lose quality.
          if (static_cast<int64_t>(raw.size()) > kMaxPayloadBytes) {
            return FsResult::error(
                ErrCode::kInvalidArg,
                "image is " + std::to_string(raw.size()) +
                    " bytes, over the request budget; pass a smaller maxSide or "
                    "downscale it first");
          }
          data["mime"] = image::mime_of(fmt);
          data["width"] = w;
          data["height"] = h;
          data["bytes"] = size;
          data["dataUri"] = image::data_uri(fmt, raw);
          data["summary"] = std::string(image::name_of(fmt)) + " " + std::to_string(w) +
                            "x" + std::to_string(h);
          return FsResult::success(data);
        }

        // Decode + downscale. stb_image covers png/jpeg/gif/bmp/tga/psd/hdr, so
        // a large photo works the same way a screenshot does. (WebP has no stb
        // decoder; decode() reports that explicitly.)
        image::Image img;
        std::string err;
        if (!image::decode(raw, &img, &err)) {
          return FsResult::error(ErrCode::kInvalidArg,
                                 err.empty() ? "cannot decode the image" : err);
        }
        image::Image scaled = image::resize_max_side(img, max_side);
        std::string png = image::encode_png(scaled);
        if (static_cast<int64_t>(png.size()) > kMaxPayloadBytes) {
          return FsResult::error(
              ErrCode::kInvalidArg,
              "image is still " + std::to_string(png.size()) +
                  " bytes after downscaling to " + std::to_string(max_side) +
                  "px; lower maxSide");
        }
        data["mime"] = "image/png";
        data["width"] = scaled.width;
        data["height"] = scaled.height;
        data["bytes"] = static_cast<int64_t>(png.size());
        data["dataUri"] = image::data_uri(image::Format::kPng, png);
        data["resized"] = true;
        data["summary"] = std::string(image::name_of(fmt)) + " " + std::to_string(w) +
                          "x" + std::to_string(h) + " -> " +
                          std::to_string(scaled.width) + "x" +
                          std::to_string(scaled.height);
        return FsResult::success(data);
      });
  }
}

}  // namespace fs
