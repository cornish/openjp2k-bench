#include "bench.h"
#include "rss.h"
#include "verify.h"

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

  r.has_roi = opts.has_roi;
  r.roi_x0 = opts.roi_x0; r.roi_y0 = opts.roi_y0;
  r.roi_x1 = opts.roi_x1; r.roi_y1 = opts.roi_y1;
  r.header_only = opts.header_only;

  // --reuse-codec: hoist whatever per-iter setup the adapter can hoist out
  // of the timed region. ROI mode goes through decode_region which the
  // prepared-decode handle doesn't currently support; reuse_codec + ROI
  // falls back to one-shot per iter (recorded on the row).
  std::unique_ptr<PreparedDecode> prepared;
  bool actually_reused = false;
  if (opts.reuse_codec && !opts.has_roi) {
    std::string perr;
    prepared = decoder.prepare(blob.data(), blob.size(), perr);
    if (!prepared) {
      r.error = "prepare: " + perr;
      r.stats.warmup = opts.warmup;
      return r;
    }
    actually_reused = decoder.supports_codec_reuse();
  }
  r.reused_codec = actually_reused;

  auto do_decode = [&](DecodedImage& target) -> bool {
    if (opts.header_only) {
      // No pixels produced; verification path is skipped below.
      return decoder.header_only(blob.data(), blob.size(), threads, err);
    }
    if (opts.has_roi) {
      Decoder::Region rg{opts.roi_x0, opts.roi_y0, opts.roi_x1, opts.roi_y1};
      return decoder.decode_region(blob.data(), blob.size(), threads, rg, target, err);
    }
    if (prepared) {
      return prepared->decode(threads, target, err);
    }
    return decoder.decode(blob.data(), blob.size(), threads, target, err);
  };

  // Warmup. Discard timings; warm allocator, branch predictor, code cache.
  for (int i = 0; i < opts.warmup; ++i) {
    img.pixels.clear();
    if (!do_decode(img)) {
      r.error = "warmup: " + err;
      r.stats.warmup = opts.warmup;
      return r;
    }
  }

  r.width = img.width;
  r.height = img.height;
  r.channels = img.channels;
  r.bit_depth = img.bit_depth;

  if (ref_image && !opts.header_only) {
    if (!img.same_shape(*ref_image)) {
      r.pixel_match = 0;
      r.error = "shape mismatch vs reference";
    } else if (img.pixels.size() != ref_image->pixels.size() ||
               std::memcmp(img.pixels.data(), ref_image->pixels.data(),
                           img.pixels.size()) != 0) {
      r.pixel_match = 0;
      r.pixel_psnr_db = psnr_db(img, *ref_image);
    } else {
      r.pixel_match = 1;
    }
  }

  uint64_t peak_observed = 0;
  int64_t  max_delta = 0;
  std::vector<double> times;
  times.reserve(opts.iters);
  for (int i = 0; i < opts.iters; ++i) {
    img.pixels.clear();
    uint64_t rss_before = peak_rss_kb();
    double t0 = now_seconds();
    bool ok = do_decode(img);
    double t1 = now_seconds();
    uint64_t rss_after = peak_rss_kb();
    if (!ok) {
      r.error = "iter: " + err;
      r.stats = summarize(times);
      r.stats.warmup = opts.warmup;
      return r;
    }
    times.push_back(t1 - t0);
    if (rss_after > peak_observed) peak_observed = rss_after;
    int64_t delta = (int64_t)rss_after - (int64_t)rss_before;
    if (delta > max_delta) max_delta = delta;
  }
  r.stats = summarize(times);
  r.stats.warmup = opts.warmup;
  r.rss_peak_kb  = peak_observed;
  r.rss_delta_kb = max_delta;
  if (r.stats.min > 0) {
    double mpx = (double)img.width * (double)img.height / 1.0e6;
    r.megapixels_per_sec = mpx / r.stats.min;
  }
  return r;
}

}  // namespace jp2kbench
