#include "verify.h"

#include <cmath>
#include <cstdint>
#include <limits>

namespace jp2kbench {
namespace {

double sample(const uint8_t* p, uint32_t bit_depth) {
  if (bit_depth <= 8) return (double)p[0];
  return (double)(uint16_t)(p[0] | (p[1] << 8));
}

}  // namespace

double psnr_db(const DecodedImage& a, const DecodedImage& b) {
  if (!a.same_shape(b)) return std::numeric_limits<double>::quiet_NaN();
  // Pixel buffer is planar: total = sum of (cw*ch*bps) per component. For
  // 4:4:4 this equals width*height*channels*bps; for subsampled images
  // the chroma planes are smaller and width*height*channels overcounts.
  // Derive sample count from the buffer instead so the loop bound is
  // correct for both. PSNR is order-invariant — planar vs interleaved
  // doesn't matter as long as both images use the same layout.
  uint32_t bpp = a.bit_depth <= 8 ? 1u : 2u;
  if (a.pixels.size() != b.pixels.size() || a.pixels.empty()) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  std::size_t n = a.pixels.size() / bpp;
  if (n == 0) return std::numeric_limits<double>::quiet_NaN();
  double sse = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    double da = sample(a.pixels.data() + i * bpp, a.bit_depth);
    double db = sample(b.pixels.data() + i * bpp, b.bit_depth);
    double d = da - db;
    sse += d * d;
  }
  if (sse == 0.0) return std::numeric_limits<double>::infinity();
  double mse = sse / (double)n;
  double max_val = (double)((1u << a.bit_depth) - 1u);
  return 20.0 * std::log10(max_val) - 10.0 * std::log10(mse);
}

}  // namespace jp2kbench
