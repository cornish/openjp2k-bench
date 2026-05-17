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
  std::size_t n = (std::size_t)a.width * a.height * a.channels;
  if (n == 0) return std::numeric_limits<double>::quiet_NaN();
  uint32_t bpp = a.bit_depth <= 8 ? 1u : 2u;
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
