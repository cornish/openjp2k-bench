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
//   --decoder NAME       only run named decoder (openjpeg, grok)
//   --no-verify          skip cross-decoder pixel comparison
//   --list-decoders      print available decoders and exit
//
// Output: JSON array on stdout, one entry per (file, decoder, threads) run.
// Progress lines go to stderr so stdout stays clean for piping.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "adapter.h"
#include "bench.h"
#include "env_capture.h"
#include "json_out.h"

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
    else if (a == "--list-decoders") {
      std::cout << "openjpeg\n";
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
    decoders.push_back(make_openjpeg_decoder());
  }
#if JP2KBENCH_HAVE_GROK
  if (only_decoder.empty() || only_decoder == "grok") {
    decoders.push_back(make_grok_decoder());
  }
#endif
  if (decoders.empty()) {
    std::cerr << "no decoders selected (unknown name?)\n"; return 2;
  }

  std::cerr << "decoders:";
  for (auto& d : decoders) std::cerr << " " << d->name() << "(" << d->version() << ")";
  std::cerr << "\n";

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
  for (const auto& path : files) {
    auto blob = read_file(path);
    if (blob.empty()) {
      std::cerr << "skip (empty/unreadable): " << path << "\n";
      continue;
    }

    // Reference image from first decoder, for cross-decoder verification.
    DecodedImage ref_image;
    bool have_ref = false;

    for (auto& d : decoders) {
      // Capture a reference at threads=1 from the first decoder only.
      if (opts.verify && !have_ref && d.get() == decoders.front().get()) {
        std::string err;
        if (d->decode(blob.data(), blob.size(), 1, ref_image, err)) {
          have_ref = true;
        }
      }
      for (int t : opts.thread_counts) {
        std::cerr << path << "  " << d->name() << "  t=" << t << "  ... ";
        auto r = bench_file(*d, path, blob, t, opts,
                            have_ref && d.get() != decoders.front().get()
                                ? &ref_image : nullptr);
        if (!r.error.empty()) {
          std::cerr << "ERR " << r.error << "\n";
        } else {
          std::cerr << "min=" << (r.stats.min * 1e3) << "ms  "
                    << r.megapixels_per_sec << " MP/s\n";
        }
        all_results.push_back(std::move(r));
      }
    }
  }

  RunHeader header;
  {
    char ts[32];
    std::time_t t = std::time(nullptr);
    std::strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));
    header.started_at_iso8601 = ts;
  }
  for (int i = 0; i < argc; ++i) header.argv.emplace_back(argv[i]);
  header.concurrent_files = 1;   // set by Task 10
  header.env_json = capture_env_json();

  Aggregate agg;                 // populated by Task 10

  write_schema_v2(std::cout, header, all_results, agg);
  return 0;
}
