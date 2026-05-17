#include "json_out.h"

#include <cmath>
#include <cstdio>

namespace jp2kbench {

std::string json_escape(const std::string& s) {
  std::string o;
  o.reserve(s.size() + 2);
  for (char c : s) {
    switch (c) {
      case '"':  o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n";  break;
      case '\r': o += "\\r";  break;
      case '\t': o += "\\t";  break;
      default:
        if ((unsigned char)c < 0x20) {
          char buf[8]; std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          o += buf;
        } else o += c;
    }
  }
  return o;
}

namespace {

void emit_result(std::ostream& os, const FileResult& r) {
  os << "    {";
  os << "\"file\": \"" << json_escape(r.path) << "\", ";
  os << "\"bytes\": " << r.bytes << ", ";
  os << "\"decoder\": \"" << json_escape(r.decoder) << "\", ";
  os << "\"decoder_version\": \"" << json_escape(r.decoder_version) << "\", ";
  os << "\"threads\": " << r.threads << ", ";
  os << "\"width\": " << r.width << ", ";
  os << "\"height\": " << r.height << ", ";
  os << "\"channels\": " << r.channels << ", ";
  os << "\"bit_depth\": " << r.bit_depth << ", ";
  os << "\"iters\": " << r.stats.iters << ", ";
  os << "\"warmup\": " << r.stats.warmup << ", ";
  os << "\"timing_s\": {";
  os << "\"min\": "    << r.stats.min    << ", ";
  os << "\"p50\": "    << r.stats.p50    << ", ";
  os << "\"p90\": "    << r.stats.p90    << ", ";
  os << "\"p99\": "    << r.stats.p99    << ", ";
  os << "\"max\": "    << r.stats.max    << ", ";
  os << "\"mean\": "   << r.stats.mean   << ", ";
  os << "\"stddev\": " << r.stats.stddev;
  os << "}, ";
  os << "\"megapixels_per_sec\": " << r.megapixels_per_sec << ", ";
  if (r.has_roi) {
    os << "\"roi\": {\"x0\": " << r.roi_x0 << ", \"y0\": " << r.roi_y0
       << ", \"x1\": " << r.roi_x1 << ", \"y1\": " << r.roi_y1 << "}, ";
  } else {
    os << "\"roi\": null, ";
  }
  os << "\"pixel_match\": " << r.pixel_match << ", ";
  if (std::isfinite(r.pixel_psnr_db)) {
    os << "\"pixel_psnr_db\": " << r.pixel_psnr_db << ", ";
  } else {
    os << "\"pixel_psnr_db\": null, ";
  }
  os << "\"rss_peak_kb\": "  << r.rss_peak_kb  << ", ";
  os << "\"rss_delta_kb\": " << r.rss_delta_kb << ", ";
  os << "\"reused_codec\": " << (r.reused_codec ? "true" : "false") << ", ";
  os << "\"error\": \"" << json_escape(r.error) << "\"";
  os << "}";
}

}  // namespace

void write_schema_v2(std::ostream& os,
                     const RunHeader& h,
                     const std::vector<FileResult>& results,
                     const Aggregate& agg) {
  os << "{\n";
  os << "  \"schema_version\": 2,\n";
  os << "  \"run\": {\n";
  os << "    \"started_at\": \"" << json_escape(h.started_at_iso8601) << "\",\n";
  os << "    \"argv\": [";
  for (std::size_t i = 0; i < h.argv.size(); ++i) {
    if (i) os << ", ";
    os << "\"" << json_escape(h.argv[i]) << "\"";
  }
  os << "],\n";
  os << "    \"concurrent_files\": " << h.concurrent_files;
  if (!h.env_json.empty()) {
    os << ",\n    \"env\": " << h.env_json;
  }
  os << "\n  },\n";

  os << "  \"results\": [\n";
  for (std::size_t i = 0; i < results.size(); ++i) {
    if (i) os << ",\n";
    emit_result(os, results[i]);
  }
  os << "\n  ],\n";

  os << "  \"aggregate\": {";
  if (std::isfinite(agg.concurrent_throughput_mpix_s)) {
    os << "\"concurrent_throughput_mpix_s\": " << agg.concurrent_throughput_mpix_s;
  }
  os << "}\n";
  os << "}\n";
}

}  // namespace jp2kbench
