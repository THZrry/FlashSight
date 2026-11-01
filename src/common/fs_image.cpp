// fs_image.cpp - image decode / resize / encode over stb
#include "fs_image.hpp"

#include <algorithm>
#include <cstring>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_GIF
#define STBI_ONLY_BMP
#define STBI_ONLY_TGA
#define STBI_ONLY_PSD
#define STBI_ONLY_HDR
#define STBI_ONLY_PIC
#define STBI_NO_HDR_GAIN
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image_resize2.h"

namespace fs {
namespace image {

Format sniff(const std::string& b) {
  if (b.size() >= 8 && static_cast<uint8_t>(b[0]) == 0x89 && b[1] == 'P' &&
      b[2] == 'N' && b[3] == 'G') {
    return Format::kPng;
  }
  if (b.size() >= 3 && static_cast<uint8_t>(b[0]) == 0xFF &&
      static_cast<uint8_t>(b[1]) == 0xD8) {
    return Format::kJpeg;
  }
  if (b.size() >= 6 && b.compare(0, 4, "GIF8") == 0) return Format::kGif;
  if (b.size() >= 12 && b.compare(0, 4, "RIFF") == 0 && b.compare(8, 4, "WEBP") == 0) {
    return Format::kWebp;
  }
  if (b.size() >= 2 && b[0] == 'B' && b[1] == 'M') return Format::kBmp;
  if (b.size() >= 18 && b.compare(1, 4, "TGA") != 0) {
    // TGA has no magic at offset 0; stb detects it by the footer.
    if (b.size() >= 18) {
      int w = static_cast<uint8_t>(b[12]) | (static_cast<uint8_t>(b[13]) << 8);
      int h = static_cast<uint8_t>(b[14]) | (static_cast<uint8_t>(b[15]) << 8);
      unsigned char depth = static_cast<unsigned char>(b[16]);
      unsigned char type = static_cast<unsigned char>(b[2]);
      if (w > 0 && h > 0 && (depth == 24 || depth == 32) &&
          (type == 2 || type == 10)) {
        return Format::kTga;
      }
    }
  }
  if (b.size() >= 4 && b.compare(0, 4, "8BPS") == 0) return Format::kPsd;
  if (b.size() >= 11 && b.compare(0, 10, "#?RADIANCE") == 0) return Format::kHdr;
  if (b.size() >= 11 && b.compare(0, 10, "#?RGBE") == 0) return Format::kHdr;
  return Format::kUnknown;
}

const char* mime_of(Format f) {
  switch (f) {
    case Format::kPng: return "image/png";
    case Format::kJpeg: return "image/jpeg";
    case Format::kGif: return "image/gif";
    case Format::kWebp: return "image/webp";
    case Format::kBmp: return "image/bmp";
    case Format::kTga: return "image/x-tga";
    case Format::kPsd: return "image/vnd.adobe.photoshop";
    case Format::kHdr: return "image/vnd.radiance";
    case Format::kPic: return "image/x-pict";
    default: return "application/octet-stream";
  }
}

const char* name_of(Format f) {
  switch (f) {
    case Format::kPng: return "png";
    case Format::kJpeg: return "jpeg";
    case Format::kGif: return "gif";
    case Format::kWebp: return "webp";
    case Format::kBmp: return "bmp";
    case Format::kTga: return "tga";
    case Format::kPsd: return "psd";
    case Format::kHdr: return "hdr";
    case Format::kPic: return "pic";
    default: return "unknown";
  }
}

void dimensions(const std::string& b, int* w, int* h) {
  *w = 0;
  *h = 0;
  int comp = 0;
  // stbi_info reads headers only - it does not decode pixels.
  if (stbi_info_from_memory(reinterpret_cast<const stbi_uc*>(b.data()),
                            static_cast<int>(b.size()), w, h, &comp)) {
    return;
  }
  *w = 0;
  *h = 0;
  // WebP has no stb decoder; read its header by hand so an oversized WebP can
  // still be reported with real dimensions instead of "0x0".
  Format f = sniff(b);
  if (f == Format::kWebp && b.size() >= 30) {
    // VP8X: 24-bit little-endian (width-1) at offset 24, same for height.
    if (b.compare(12, 4, "VP8X") == 0) {
      const unsigned char* p = reinterpret_cast<const unsigned char*>(b.data());
      *w = 1 + (p[24] | (p[25] << 8) | (p[26] << 16));
      *h = 1 + (p[27] | (p[28] << 8) | (p[29] << 16));
    } else if (b.compare(12, 4, "VP8 ") == 0 && b.size() >= 30) {
      const unsigned char* p = reinterpret_cast<const unsigned char*>(b.data());
      *w = (p[26] | (p[27] << 8)) & 0x3FFF;
      *h = (p[28] | (p[29] << 8)) & 0x3FFF;
    } else if (b.compare(12, 4, "VP8L") == 0 && b.size() >= 25) {
      const unsigned char* p = reinterpret_cast<const unsigned char*>(b.data());
      uint32_t bits = static_cast<uint32_t>(p[21]) |
                      (static_cast<uint32_t>(p[22]) << 8) |
                      (static_cast<uint32_t>(p[23]) << 16) |
                      (static_cast<uint32_t>(p[24]) << 24);
      *w = static_cast<int>(bits & 0x3FFF) + 1;
      *h = static_cast<int>((bits >> 14) & 0x3FFF) + 1;
    }
  }
}

bool decode(const std::string& b, Image* out, std::string* err) {
  if (!out) return false;
  if (b.empty()) {
    if (err) *err = "empty file";
    return false;
  }
  Format fmt = sniff(b);
  if (fmt == Format::kWebp) {
    if (err) {
      int w = 0, h = 0;
      dimensions(b, &w, &h);
      *err = "WebP is not supported by the decoder (stb has no WebP); the image is " +
             std::to_string(w) + "x" + std::to_string(h) +
             ". Convert it to PNG/JPEG, or ask the user for one.";
    }
    return false;
  }
  // Reject a decompression bomb BEFORE allocating: the header states the size.
  int hw = 0, hh = 0;
  dimensions(b, &hw, &hh);
  if (hw > 0 && hh > 0 &&
      static_cast<int64_t>(hw) * static_cast<int64_t>(hh) > kMaxPixels) {
    if (err) {
      *err = "image is " + std::to_string(hw) + "x" + std::to_string(hh) +
             " = " + std::to_string(static_cast<int64_t>(hw) * hh) +
             " pixels, over the decode limit (" + std::to_string(kMaxPixels) + ")";
    }
    return false;
  }
  int w = 0, h = 0, comp = 0;
  stbi_uc* px = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(b.data()),
                                      static_cast<int>(b.size()), &w, &h, &comp, 4);
  if (!px) {
    if (err) {
      const char* why = stbi_failure_reason();
      *err = std::string("cannot decode image") +
             (why && *why ? std::string(": ") + why : std::string());
    }
    return false;
  }
  out->width = w;
  out->height = h;
  out->rgba.assign(px, px + static_cast<size_t>(w) * h * 4);
  stbi_image_free(px);
  return true;
}

std::string encode_png(const Image& img) {
  if (!img.valid()) return std::string();
  std::string out;
  // stbi_write_png_to_func appends through the callback; using it avoids a
  // temporary buffer plus a copy.
  stbi_write_png_to_func(
      [](void* ctx, void* data, int size) {
        static_cast<std::string*>(ctx)->append(static_cast<const char*>(data),
                                               static_cast<size_t>(size));
      },
      &out, img.width, img.height, 4, img.rgba.data(), img.width * 4);
  return out;
}

Image resize_max_side(const Image& img, int max_side) {
  if (!img.valid() || max_side <= 0) return img;
  int longest = std::max(img.width, img.height);
  if (longest <= max_side) return img;
  double scale = static_cast<double>(max_side) / longest;
  int nw = std::max(1, static_cast<int>(img.width * scale));
  int nh = std::max(1, static_cast<int>(img.height * scale));
  Image out;
  out.width = nw;
  out.height = nh;
  out.rgba.assign(static_cast<size_t>(nw) * nh * 4, 255);
  // sRGB-aware, Mitchell-ish default filter: text stays legible when a 4K
  // screenshot is scaled down, which a naive bilinear did not manage.
  unsigned char* ok = stbir_resize_uint8_srgb(
      img.rgba.data(), img.width, img.height, img.width * 4,
      out.rgba.data(), nw, nh, nw * 4, STBIR_RGBA);
  if (!ok) {
    // Fall back to the original rather than returning a half-filled buffer.
    return img;
  }
  return out;
}

std::string base64(const std::string& raw) {
  static const char* kTable =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((raw.size() + 2) / 3 * 4);
  size_t i = 0;
  while (i + 2 < raw.size()) {
    uint32_t v = (static_cast<uint8_t>(raw[i]) << 16) |
                 (static_cast<uint8_t>(raw[i + 1]) << 8) |
                 static_cast<uint8_t>(raw[i + 2]);
    out.push_back(kTable[(v >> 18) & 0x3F]);
    out.push_back(kTable[(v >> 12) & 0x3F]);
    out.push_back(kTable[(v >> 6) & 0x3F]);
    out.push_back(kTable[v & 0x3F]);
    i += 3;
  }
  if (i < raw.size()) {
    uint32_t v = static_cast<uint8_t>(raw[i]) << 16;
    bool two = (i + 1 < raw.size());
    if (two) v |= static_cast<uint8_t>(raw[i + 1]) << 8;
    out.push_back(kTable[(v >> 18) & 0x3F]);
    out.push_back(kTable[(v >> 12) & 0x3F]);
    out.push_back(two ? kTable[(v >> 6) & 0x3F] : '=');
    out.push_back('=');
  }
  return out;
}

std::string data_uri(Format f, const std::string& raw) {
  return std::string("data:") + mime_of(f) + ";base64," + base64(raw);
}

}  // namespace image
}  // namespace fs
