#pragma once
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "adapter.h"

namespace jp2kbench {

struct RunStats {
  // All times in seconds.
  double min = 0, p50 = 0, p90 = 0, p99 = 0, max = 0, mean = 0, stddev = 0;
  int iters = 0;
  int warmup = 0;
};

struct FileResult {
  std::string path;
  std::size_t bytes = 0;
  std::string decoder;
  std::string decoder_version;
  int threads = 0;
  uint32_t width = 0, height = 0, channels = 0, bit_depth = 0;
  RunStats stats;
  // Pixels-per-second computed from min decode time.
  double megapixels_per_sec = 0;
  // If a previous decoder produced output for this file, did pixels match?
  // -1 = no prior, 0 = mismatch, 1 = match.
  int pixel_match = -1;
  uint64_t rss_peak_kb = 0;     // highest peak RSS observed at end of any timed iter
  int64_t  rss_delta_kb = 0;    // max (peak_after - peak_before) across timed iters
  // Region actually timed; has_roi=false => null in JSON.
  bool has_roi = false;
  uint32_t roi_x0 = 0, roi_y0 = 0, roi_x1 = 0, roi_y1 = 0;
  std::string error;
};

struct BenchOptions {
  int warmup = 2;
  int iters = 20;
  std::vector<int> thread_counts = {1};
  // If true, verify pixels match across decoders for the same file.
  bool verify = true;
  // ROI: if has_roi is false, do a full decode. If true, [x0,x1) x [y0,y1)
  // is passed verbatim to decode_region.
  bool has_roi = false;
  uint32_t roi_x0 = 0, roi_y0 = 0, roi_x1 = 0, roi_y1 = 0;
};

RunStats summarize(std::vector<double>& times_seconds);

// Bench a single file with one decoder. ref_image is non-null if a previous
// decoder already produced a reference image to compare against.
FileResult bench_file(Decoder& decoder, const std::string& path,
                      const std::vector<uint8_t>& blob, int threads,
                      const BenchOptions& opts,
                      const DecodedImage* ref_image);

}  // namespace jp2kbench
