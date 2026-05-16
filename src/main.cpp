// jp2k-bench: side-by-side JPEG 2000 decode benchmark.
//
// Usage:
//   jp2k-bench [options] file.jp2 [file2.jp2 ...]
//
// Options:
//   --iters N            timed iterations per (file, decoder, threads)  [20]
//   --warmup N           untimed warmup iterations                       [2]
//   --threads N[,N...]   thread counts to sweep                          [1]
//   --decoder NAME       only run named decoder (openjpeg, grok)
//   --no-verify          skip cross-decoder pixel comparison
//   --list-decoders      print available decoders and exit
//
// Output: JSON array on stdout, one entry per (file, decoder, threads) run.
// Progress lines go to stderr so stdout stays clean for piping.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "adapter.h"
#include "bench.h"

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

std::string json_escape(const std::string& s) {
  std::string o;
  o.reserve(s.size() + 2);
  for (char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if ((unsigned char)c < 0x20) {
          char buf[8]; std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          o += buf;
        } else o += c;
    }
  }
  return o;
}

void emit_result(std::ostream& os, const FileResult& r, bool first) {
  if (!first) os << ",\n";
  os << "  {";
  os << "\"file\": \"" << json_escape(r.path) << "\", ";
  os << "\"bytes\": " << r.bytes << ", ";
  os << "\"decoder\": \"" << json_escape(r.decoder) << "\", ";
  os << "\"version\": \"" << json_escape(r.decoder_version) << "\", ";
  os << "\"threads\": " << r.threads << ", ";
  os << "\"width\": " << r.width << ", ";
  os << "\"height\": " << r.height << ", ";
  os << "\"channels\": " << r.channels << ", ";
  os << "\"bit_depth\": " << r.bit_depth << ", ";
  os << "\"iters\": " << r.stats.iters << ", ";
  os << "\"min_s\": " << r.stats.min << ", ";
  os << "\"p50_s\": " << r.stats.p50 << ", ";
  os << "\"p90_s\": " << r.stats.p90 << ", ";
  os << "\"p99_s\": " << r.stats.p99 << ", ";
  os << "\"mean_s\": " << r.stats.mean << ", ";
  os << "\"stddev_s\": " << r.stats.stddev << ", ";
  os << "\"mpx_per_s\": " << r.megapixels_per_sec << ", ";
  os << "\"pixel_match\": " << r.pixel_match << ", ";
  os << "\"error\": \"" << json_escape(r.error) << "\"";
  os << "}";
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

  std::cout << "[\n";
  bool first = true;
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
        emit_result(std::cout, r, first);
        first = false;
      }
    }
  }
  std::cout << "\n]\n";
  return 0;
}
