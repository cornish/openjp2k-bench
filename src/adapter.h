#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace jp2kbench {

struct DecodedImage {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t channels = 0;
  uint32_t bit_depth = 0;
  // Interleaved, channel-major within a pixel. 8bpc => one byte per sample;
  // >8bpc => two bytes per sample (host-endian, right-aligned).
  std::vector<uint8_t> pixels;

  // Equality is exact for lossless and unhelpful for lossy. Callers that
  // need lossy comparison should compute PSNR against a ground-truth image.
  bool same_shape(const DecodedImage& o) const {
    return width == o.width && height == o.height &&
           channels == o.channels && bit_depth == o.bit_depth;
  }
};

class Decoder {
 public:
  virtual ~Decoder() = default;
  virtual const char* name() const = 0;
  virtual std::string version() const = 0;
  // num_threads <= 1 means single-threaded. Decoders that can't honor the
  // request should still produce correct output and report what they used.
  virtual bool decode(const uint8_t* data, std::size_t size, int num_threads,
                      DecodedImage& out, std::string& err) = 0;

  struct Region {
    uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;   // half-open [x0,x1) × [y0,y1)
  };

  // Decode only the given region. Default = full decode + crop; adapters
  // override to call native region APIs.
  virtual bool decode_region(const uint8_t* data, std::size_t size,
                             int num_threads, const Region& region,
                             DecodedImage& out, std::string& err);

  virtual bool native_region_decode() const { return false; }
};

inline bool Decoder::decode_region(const uint8_t* data, std::size_t size,
                                   int num_threads, const Region& region,
                                   DecodedImage& out, std::string& err) {
  if (!decode(data, size, num_threads, out, err)) return false;
  uint32_t x0 = region.x0, y0 = region.y0;
  uint32_t x1 = region.x1 == 0 ? out.width  : std::min(region.x1, out.width);
  uint32_t y1 = region.y1 == 0 ? out.height : std::min(region.y1, out.height);
  if (x0 >= x1 || y0 >= y1) { err = "empty region"; return false; }
  uint32_t bw = x1 - x0, bh = y1 - y0;
  uint32_t bpp = (out.bit_depth <= 8 ? 1u : 2u) * out.channels;
  std::vector<uint8_t> cropped((std::size_t)bw * bh * bpp);
  for (uint32_t y = 0; y < bh; ++y) {
    const uint8_t* src = out.pixels.data() + ((y + y0) * out.width + x0) * bpp;
    uint8_t* dst       = cropped.data()     +  y * bw                    * bpp;
    std::memcpy(dst, src, (std::size_t)bw * bpp);
  }
  out.pixels = std::move(cropped);
  out.width  = bw;
  out.height = bh;
  return true;
}

std::unique_ptr<Decoder> make_openjpeg_decoder();

#if JP2KBENCH_HAVE_GROK
std::unique_ptr<Decoder> make_grok_decoder();
#endif

}  // namespace jp2kbench
