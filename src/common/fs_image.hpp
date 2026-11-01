// fs_image.hpp - image codec + resize (stb_image / stb_image_resize2 / stb_image_write)
//
// WHY STB AND NOT A HAND-ROLLED CODEC
// The first version of this file implemented PNG decode (a small inflate) and a
// stored-deflate PNG encoder by hand. It worked for the one case it was tested
// with (a screenshot round trip) and was wrong for everything else:
//   * JPEG/GIF/WebP could not be decoded at all, so a large photo could not be
//     downscaled - the tool had to refuse it;
//   * the encoder produced uncompressed PNGs (a 1600px screenshot was megabytes);
//   * resize was a naive bilinear, which is visibly worse than a proper filter
//     for text-heavy screenshots.
// stb is three single-header public-domain files with no build system, which is
// exactly the dependency shape this project wants (see doc-internal/thirdparty).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fs {
namespace image {

enum class Format { kUnknown, kPng, kJpeg, kGif, kWebp, kBmp, kTga, kPsd, kHdr, kPic };

Format sniff(const std::string& bytes);
const char* mime_of(Format f);
const char* name_of(Format f);

struct Image {
  int width = 0;
  int height = 0;
  std::vector<uint8_t> rgba;  // width*height*4, straight (non-premultiplied)

  bool valid() const {
    return width > 0 && height > 0 &&
           rgba.size() == static_cast<size_t>(width) * height * 4;
  }
};

// Dimensions from the header only (0/0 when unknown). Never decodes pixels.
void dimensions(const std::string& bytes, int* w, int* h);

// Decode any format stb_image supports into RGBA8. `err` explains a failure in
// terms the model can act on (unsupported format, corrupt data, too large).
bool decode(const std::string& bytes, Image* out, std::string* err);

// Encode RGBA8 to PNG (stb_image_write, real zlib-ish compression).
std::string encode_png(const Image& img);

// High-quality resize so that max(w,h) <= max_side. Never upscales.
Image resize_max_side(const Image& img, int max_side);

// base64 (standard alphabet, no line breaks).
std::string base64(const std::string& raw);

// "data:image/png;base64,...."
std::string data_uri(Format f, const std::string& raw);

// Max decoded pixels accepted. A "decompression bomb" is a tiny file that
// decodes to gigabytes; the header tells us before we allocate.
constexpr int64_t kMaxPixels = 80LL * 1000 * 1000;

}  // namespace image
}  // namespace fs
