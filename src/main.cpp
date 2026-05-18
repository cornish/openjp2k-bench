// jp2k-bench: side-by-side JPEG 2000 decode benchmark.
//
// Usage:
//   jp2k-bench [options] file.jp2 [file2.jp2 ...]
//
// Options:
//   --iters N            timed iterations per (file, decoder, threads)  [20]
//   --warmup N           untimed warmup iterations                       [2]
//   --threads N[,N...]   thread counts to sweep                          [1]
//                        (note: Grok's pool is process-singleton; sweeping
//                         with grok enabled pins the pool to the first N)
//   --decoder NAME       only run named decoder (openjpeg, openjp2k, grok)
//   --no-verify          skip cross-decoder pixel comparison
//   --roi WxH@X,Y        decode only the given region (e.g. 256x256@0,0)
//   --concurrent-files N parallel bench jobs (per-file blob held once)    [1]
//   --require-clean      exit 3 if any tracked library tree is dirty
//   --reuse-codec        prepare the codec once per file, reuse across iters
//                        (isolates steady-state decode from per-iter setup;
//                         ROI mode is unaffected by this flag)
//   --header-only        time only create+setup+read_header per iter; no
//                        actual decode runs. Diagnostic for how much of a
//                        timing is setup vs. real work. Implies --no-verify.
//   --profile-stages     bracket the per-iter call into setup / decode /
//                        unpack / teardown phases; emit per-stage min times
//                        in result.stages_s so adapter-side format-conversion
//                        cost can be isolated from codec decode cost.
//   --jsonl              stream each result as one JSON line + flush instead
//                        of buffering a single JSON document. Survives kills.
//   --heavy-iters N      override --iters for files matching --heavy-pattern.
//                        Use to cut runtime on a few extremely large files.
//   --heavy-pattern RE   regex (std::regex_search) over the file path. When a
//                        path matches and --heavy-iters > 0, that file's iters
//                        are replaced. Default: empty (no override).
//   --list-decoders      print available decoders and exit
//
// Output: by default, JSON document on stdout; with --jsonl, one self-describing
// JSON object per line ({"type":"run"|"result"|"aggregate", ...}). Progress
// lines always go to stderr so stdout stays clean for piping.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <regex>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include "adapter.h"
#include "bench.h"
#include "build_info.h"
#include "env_capture.h"
#include "json_out.h"
#include "thread_pool.h"

using namespace jp2kbench;

namespace {

std::vector<uint8_t> read_file(const std::string& path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) return {};
  std::streamsize n = f.tellg();
  f.seekg(0);
  std::vector<uint8_t> buf((std::size_t)n);
  if (n > 0) f.read(reinterpret_cast<char*>(buf.data()), n);
  return buf;
}

std::vector<int> parse_int_list(const std::string& s) {
  std::vector<int> out;
  std::stringstream ss(s);
  std::string tok;
  while (std::getline(ss, tok, ',')) {
    if (!tok.empty()) out.push_back(std::atoi(tok.c_str()));
  }
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  BenchOptions opts;
  std::string only_decoder;
  std::vector<std::string> files;
  int concurrent_files = 1;
  bool require_clean = false;
  bool jsonl = false;
  int heavy_iters = 0;
  std::string heavy_pattern;

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto need = [&](const char* name) -> std::string {
      if (i + 1 >= argc) { std::cerr << name << " needs a value\n"; std::exit(2); }
      return argv[++i];
    };
    if (a == "--iters") opts.iters = std::atoi(need("--iters").c_str());
    else if (a == "--warmup") opts.warmup = std::atoi(need("--warmup").c_str());
    else if (a == "--threads") opts.thread_counts = parse_int_list(need("--threads"));
    else if (a == "--decoder") only_decoder = need("--decoder");
    else if (a == "--no-verify") opts.verify = false;
    else if (a == "--roi") {
      // Format: WxH@X,Y  e.g. 1024x1024@0,0
      std::string v = need("--roi");
      uint32_t w=0,h=0,x=0,y=0;
      if (std::sscanf(v.c_str(), "%ux%u@%u,%u", &w, &h, &x, &y) != 4) {
        std::cerr << "--roi expects WxH@X,Y\n"; return 2;
      }
      opts.has_roi = true;
      opts.roi_x0 = x; opts.roi_y0 = y;
      opts.roi_x1 = x + w; opts.roi_y1 = y + h;
    }
    else if (a == "--roi-tile") {
      need("--roi-tile");  // consume the arg
      std::cerr << "--roi-tile not yet implemented; use --roi WxH@X,Y\n";
      return 2;
    }
    else if (a == "--concurrent-files") {
      concurrent_files = std::atoi(need("--concurrent-files").c_str());
      if (concurrent_files < 1) concurrent_files = 1;
    }
    else if (a == "--require-clean") {
      require_clean = true;
    }
    else if (a == "--reuse-codec") {
      opts.reuse_codec = true;
    }
    else if (a == "--header-only") {
      opts.header_only = true;
      opts.verify = false;  // no pixels to compare
    }
    else if (a == "--profile-stages") {
      opts.profile_stages = true;
    }
    else if (a == "--jsonl") {
      jsonl = true;
    }
    else if (a == "--heavy-iters") {
      heavy_iters = std::atoi(need("--heavy-iters").c_str());
    }
    else if (a == "--heavy-pattern") {
      heavy_pattern = need("--heavy-pattern");
    }
    else if (a == "--list-decoders") {
      std::cout << "openjpeg\n";
      std::cout << "openjp2k\n";
#if JP2KBENCH_HAVE_GROK
      std::cout << "grok\n";
#endif
      return 0;
    }
    else if (a == "-h" || a == "--help") {
      std::cout << "see header of main.cpp for options\n";
      return 0;
    }
    else if (!a.empty() && a[0] == '-') {
      std::cerr << "unknown option: " << a << "\n"; return 2;
    }
    else files.push_back(a);
  }

  if (files.empty()) {
    std::cerr << "no input files. try --help\n"; return 2;
  }
  if (opts.thread_counts.empty()) opts.thread_counts = {1};

  std::vector<std::unique_ptr<Decoder>> decoders;
  if (only_decoder.empty() || only_decoder == "openjpeg") {
    if (auto d = make_openjpeg_decoder()) decoders.push_back(std::move(d));
  }
  if (only_decoder.empty() || only_decoder == "openjp2k") {
    if (auto d = make_openjp2k_decoder()) decoders.push_back(std::move(d));
  }
#if JP2KBENCH_HAVE_GROK
  if (only_decoder.empty() || only_decoder == "grok") {
    decoders.push_back(make_grok_decoder());
  }
#endif
  if (decoders.empty()) {
    std::cerr << "no decoders available (load failure or unknown name?)\n"; return 2;
  }

  std::cerr << "decoders:";
  for (auto& d : decoders) std::cerr << " " << d->name() << "(" << d->version() << ")";
  std::cerr << "\n";

  // --require-clean enforces spec §3.6: refuse merge-gate runs from a
  // working tree with uncommitted changes. The describe string from
  // cmake/Versions.cmake appends "-dirty" when this is the case.
  if (require_clean) {
    std::string oj  = JP2KBENCH_OPENJPEG_COMMIT;
    std::string ok2 = JP2KBENCH_OPENJP2K_COMMIT;
    std::string gk  = JP2KBENCH_GROK_COMMIT;
    auto dirty = [](const std::string& s) {
      return s.find("-dirty") != std::string::npos;
    };
    if (dirty(oj) || dirty(ok2) || dirty(gk)) {
      std::cerr << "--require-clean: refusing to run, dirty source tree:";
      if (dirty(oj))  std::cerr << " openjpeg=" << oj;
      if (dirty(ok2)) std::cerr << " openjp2k=" << ok2;
      if (dirty(gk))  std::cerr << " grok=" << gk;
      std::cerr << "\n";
      return 3;
    }
  }

  // Grok's thread pool is a process-singleton (grk_initialize) and only the
  // first --threads value seen pins the pool size; per-decode num_threads on
  // grk_decompress_parameters is not read by Grok v20. Sweeping multiple
  // thread counts against Grok in one process therefore reports the first
  // value's numbers for every t. See src/adapter_grok.cpp top-of-file note.
  if (opts.thread_counts.size() > 1) {
    for (auto& d : decoders) {
      if (d->name() == "grok") {
        std::cerr << "warning: --threads sweep with grok in one process gives "
                     "misleading numbers; grok's pool is pinned to the first "
                     "value (" << opts.thread_counts.front()
                  << "). Run one process per --threads value for fair Grok "
                     "scaling measurements.\n";
        break;
      }
    }
  }

  std::vector<FileResult> all_results;
  std::mutex results_mtx;
  ThreadPool pool(concurrent_files);

  // In --jsonl mode emit the run header up front so a kill mid-corpus still
  // leaves a parseable prefix on disk.
  RunHeader header;
  {
    char ts[32];
    std::time_t t = std::time(nullptr);
    std::strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));
    header.started_at_iso8601 = ts;
  }
  for (int i = 0; i < argc; ++i) header.argv.emplace_back(argv[i]);
  header.concurrent_files = concurrent_files;
  header.env_json = capture_env_json();

  std::regex heavy_re;
  bool heavy_re_ok = false;
  if (!heavy_pattern.empty() && heavy_iters > 0) {
    try {
      heavy_re = std::regex(heavy_pattern);
      heavy_re_ok = true;
    } catch (const std::regex_error& e) {
      std::cerr << "--heavy-pattern: bad regex: " << e.what() << "\n";
      return 2;
    }
  }

  if (jsonl) {
    write_jsonl_run(std::cout, header);
    std::cout.flush();
  }

  for (const auto& path : files) {
    auto blob = read_file(path);
    if (blob.empty()) {
      std::cerr << "skip (empty/unreadable): " << path << "\n";
      continue;
    }

    // Reference image (serial, first decoder, threads=1).
    auto ref_image = std::make_shared<DecodedImage>();
    bool have_ref = false;
    if (opts.verify && !opts.header_only) {
      std::string err;
      bool ok;
      if (opts.has_roi) {
        Decoder::Region rg{opts.roi_x0, opts.roi_y0, opts.roi_x1, opts.roi_y1};
        ok = decoders.front()->decode_region(blob.data(), blob.size(), 1, rg,
                                             *ref_image, err);
      } else {
        ok = decoders.front()->decode(blob.data(), blob.size(), 1,
                                      *ref_image, err);
      }
      have_ref = ok;
    }

    // Per-file iter override: heavy files (e.g. multi-hundred-MP scans) can
    // dominate the run; --heavy-iters caps them. The override applies only
    // to opts.iters; warmup, threads, verify, etc. are untouched.
    BenchOptions opts_for_file = opts;
    if (heavy_re_ok && std::regex_search(path, heavy_re)) {
      opts_for_file.iters = heavy_iters;
      std::cerr << "[heavy] " << path << "  iters=" << heavy_iters << "\n";
    }

    for (auto& d : decoders) {
      for (int t : opts_for_file.thread_counts) {
        bool is_ref_decoder = (d.get() == decoders.front().get());
        const DecodedImage* ref_ptr =
            (have_ref && !is_ref_decoder) ? ref_image.get() : nullptr;
        Decoder* dec = d.get();
        std::string path_copy = path;
        // blob is captured by reference. Safe: wait_idle() below joins all
        // jobs for this file before blob goes out of scope.
        pool.submit([&, dec, ref_ptr, t, path_copy]() {
          auto r = bench_file(*dec, path_copy, blob, t, opts_for_file, ref_ptr);
          std::lock_guard<std::mutex> lk(results_mtx);
          std::cerr << path_copy << "  " << dec->name() << "  t=" << t << "  ";
          if (!r.error.empty()) std::cerr << "ERR " << r.error << "\n";
          else std::cerr << "min=" << (r.stats.min * 1e3) << "ms  "
                         << r.megapixels_per_sec << " MP/s\n";
          if (jsonl) {
            write_jsonl_result(std::cout, r);
            std::cout.flush();
          }
          all_results.push_back(std::move(r));
        });
      }
    }
    pool.wait_idle();   // bound blob lifetime; one file in memory at a time
  }

  // In --jsonl mode the per-result lines are already on disk in stream
  // order; re-sorting now would require buffering everything in memory and
  // is also redundant — readers can sort post-hoc. The full-JSON path still
  // wants a deterministic order for the array form.
  if (!jsonl) {
    std::sort(all_results.begin(), all_results.end(),
              [](const FileResult& a, const FileResult& b) {
                return std::tie(a.path, a.decoder, a.threads, a.has_roi)
                     < std::tie(b.path, b.decoder, b.threads, b.has_roi);
              });
  }

  Aggregate agg;
  if (concurrent_files > 1) {
    // Approximate aggregate throughput as total megapixels scaled by the
    // concurrency factor over summed per-job min decode time. Real wall-clock
    // around pool.wait_idle would be more precise; this is good enough for
    // a headline number until we wire that in.
    double total_mpx = 0;
    double total_s   = 0;
    for (const auto& r : all_results) {
      if (r.error.empty() && r.stats.min > 0) {
        total_mpx += (double)r.width * r.height / 1e6;
        total_s   += r.stats.min;
      }
    }
    if (total_s > 0) {
      agg.concurrent_throughput_mpix_s =
          total_mpx * concurrent_files / total_s;
    }
  }

  if (jsonl) {
    write_jsonl_aggregate(std::cout, agg);
    std::cout.flush();
  } else {
    write_schema_v2(std::cout, header, all_results, agg);
  }
  return 0;
}
