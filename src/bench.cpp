#include "bench.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace jp2kbench {

namespace {
double now_seconds() {
  using clk = std::chrono::steady_clock;
  static const auto t0 = clk::now();
  return std::chrono::duration<double>(clk::now() - t0).count();
}

double percentile(const std::vector<double>& sorted, double p) {
  if (sorted.empty()) return 0;
  double idx = p * (sorted.size() - 1);
  std::size_t lo = (std::size_t)idx;
  std::size_t hi = std::min(lo + 1, sorted.size() - 1);
  double frac = idx - (double)lo;
  return sorted[lo] * (1 - frac) + sorted[hi] * frac;
}
}  // namespace

RunStats summarize(std::vector<double>& times) {
  RunStats s;
  s.iters = (int)times.size();
  if (times.empty()) return s;
  std::sort(times.begin(), times.end());
  s.min = times.front();
  s.max = times.back();
  s.p50 = percentile(times, 0.5);
  s.p90 = percentile(times, 0.9);
  s.p99 = percentile(times, 0.99);
  double sum = 0;
  for (double t : times) sum += t;
  s.mean = sum / times.size();
  double var = 0;
  for (double t : times) var += (t - s.mean) * (t - s.mean);
  s.stddev = std::sqrt(var / times.size());
  return s;
}

FileResult bench_file(Decoder& decoder, const std::string& path,
                      const std::vector<uint8_t>& blob, int threads,
                      const BenchOptions& opts,
                      const DecodedImage* ref_image) {
  FileResult r;
  r.path = path;
  r.bytes = blob.size();
  r.decoder = decoder.name();
  r.decoder_version = decoder.version();
  r.threads = threads;

  DecodedImage img;
  std::string err;

  // Warmup. Discard timings; warm allocator, branch predictor, code cache.
  for (int i = 0; i < opts.warmup; ++i) {
    img.pixels.clear();
    if (!decoder.decode(blob.data(), blob.size(), threads, img, err)) {
      r.error = "warmup: " + err;
      r.stats.warmup = opts.warmup;
      return r;
    }
  }

  r.width = img.width;
  r.height = img.height;
  r.channels = img.channels;
  r.bit_depth = img.bit_depth;

  if (ref_image) {
    if (!img.same_shape(*ref_image)) {
      r.pixel_match = 0;
      r.error = "shape mismatch vs reference";
    } else if (img.pixels.size() != ref_image->pixels.size() ||
               std::memcmp(img.pixels.data(), ref_image->pixels.data(),
                           img.pixels.size()) != 0) {
      r.pixel_match = 0;
    } else {
      r.pixel_match = 1;
    }
  }

  std::vector<double> times;
  times.reserve(opts.iters);
  for (int i = 0; i < opts.iters; ++i) {
    img.pixels.clear();
    double t0 = now_seconds();
    bool ok = decoder.decode(blob.data(), blob.size(), threads, img, err);
    double t1 = now_seconds();
    if (!ok) {
      r.error = "iter: " + err;
      r.stats = summarize(times);
      r.stats.warmup = opts.warmup;
      return r;
    }
    times.push_back(t1 - t0);
  }
  r.stats = summarize(times);
  r.stats.warmup = opts.warmup;
  if (r.stats.min > 0) {
    double mpx = (double)img.width * (double)img.height / 1.0e6;
    r.megapixels_per_sec = mpx / r.stats.min;
  }
  return r;
}

}  // namespace jp2kbench
