#pragma once
#include <chrono>
#include <cstdint>
#include <limits>
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

// Correctness-mode outcome class. Reflects what the bench observed for a
// single decode attempt — not whether the result was *expected*. The
// classifier file separately specifies expected_outcome; report.py
// cross-references to flag mismatches.
enum class CorrectnessOutcome {
  Decoded,           // decoder returned success
  CleanlyRejected,   // decoder returned an error string without crashing
  // (Crashed / GarbageOutput require orchestrator-side detection — the
  //  bench process exits before we'd emit such a row.)
};

struct FileResult {
  std::string path;
  std::size_t bytes = 0;
  std::string decoder;
  std::string decoder_version;
  int threads = 0;
  uint32_t width = 0, height = 0, channels = 0, bit_depth = 0;
  // Per-component {dx,dy} subsampling factors. Empty when the image is
  // 4:4:4 or no decode was completed. Emitted as JSON null in the 4:4:4
  // case, as an array of {dx,dy} objects when subsampled.
  std::vector<ComponentDims> subsampled_components;
  // Correctness-mode fields. Set only when BenchOptions.correctness is
  // true; otherwise the JSON omits them.
  bool is_correctness_row = false;
  CorrectnessOutcome outcome = CorrectnessOutcome::Decoded;
  std::string expected_outcome;  // "pass" | "fail" | "unknown" (from classifier)
  RunStats stats;
  // Pixels-per-second computed from min decode time.
  double megapixels_per_sec = 0;
  // If a previous decoder produced output for this file, did pixels match?
  // -1 = no prior, 0 = mismatch, 1 = match.
  int pixel_match = -1;
  // Populated only when pixel_match == 0; NaN otherwise.
  double pixel_psnr_db = std::numeric_limits<double>::quiet_NaN();
  uint64_t rss_peak_kb = 0;     // highest peak RSS observed at end of any timed iter
  int64_t  rss_delta_kb = 0;    // max (peak_after - peak_before) across timed iters
  // Scale-track: peak RSS sampled by a /proc/self/status poller during decode.
  // Catches mid-decode peaks that getrusage() understates when glibc trims
  // the heap before the process exits. Zero in non-scale-track runs.
  uint64_t rss_peak_kb_sampled = 0;
  // Scale-track provenance: the cgroup memory cap the run was invoked under
  // (set by run_scale.sh via systemd-run --scope -p MemoryMax=). Zero when
  // the bench was not run under such a wrapper.
  uint64_t memory_max_bytes = 0;
  // Scale-track sweep flag; orchestrator sets via --scale-track to mark
  // result rows for downstream report_scale.py partitioning.
  bool scale_track = false;
  // Region actually timed; has_roi=false => null in JSON.
  bool has_roi = false;
  uint32_t roi_x0 = 0, roi_y0 = 0, roi_x1 = 0, roi_y1 = 0;
  // True iff prepare() actually hoisted nontrivial work for this row.
  bool reused_codec = false;
  // True iff this row is a header-only timing pass (no real decode).
  bool header_only = false;
  // True iff stage breakdown was captured; values are mins across iters.
  bool profile_stages = false;
  double stage_setup_s    = 0.0;
  double stage_decode_s   = 0.0;
  double stage_unpack_s   = 0.0;
  double stage_teardown_s = 0.0;
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
  // If true, prepare() the codec once and reuse for all iters. Isolates the
  // hot decode path from per-iter codec setup cost.
  bool reuse_codec = false;
  // If true, the timed loop calls Decoder::header_only() — measures the
  // create+setup+read_header cost in isolation, without invoking the
  // wavelet/entropy decode. Diagnostic for "how much of single-tile
  // timing is setup vs. real work?".
  bool header_only = false;
  // If true, the timed loop calls Decoder::decode_with_stages() and the
  // result row reports the minimum across iterations for each stage.
  bool profile_stages = false;
  // Correctness-mode: skip the timed loop; do exactly one decode attempt;
  // emit a correctness row (type="correctness") with outcome class +
  // expected_outcome rather than a timing row. Pairs with main.cpp's
  // --correctness flag.
  bool correctness = false;
  // Scale-track mode: orchestrator-controlled per-(file, decoder)
  // invocations under systemd-run --scope -p MemoryMax= caps. Enables
  // /proc/self/status RSS sampling around the timed iter (mid-decode
  // peak that getrusage misses on glibc-with-trim). Sets a flag on the
  // result row so report_scale.py can partition. Sampler period in ms.
  bool scale_track = false;
  int rss_sample_ms = 100;
  // The MemoryMax= cap the orchestrator invoked us under, in bytes.
  // Echoed into the result row for downstream regression-gate context.
  uint64_t memory_max_bytes = 0;
};

RunStats summarize(std::vector<double>& times_seconds);

// Bench a single file with one decoder. ref_image is non-null if a previous
// decoder already produced a reference image to compare against.
FileResult bench_file(Decoder& decoder, const std::string& path,
                      const std::vector<uint8_t>& blob, int threads,
                      const BenchOptions& opts,
                      const DecodedImage* ref_image);

}  // namespace jp2kbench
