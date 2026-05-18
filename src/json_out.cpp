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

// Emits the body fields of a FileResult — no enclosing braces, no leading
// indent. Shared between the pretty array form and the JSONL line form.
void emit_result_fields(std::ostream& os, const FileResult& r) {
  os << "\"file\": \"" << json_escape(r.path) << "\", ";
  os << "\"bytes\": " << r.bytes << ", ";
  os << "\"decoder\": \"" << json_escape(r.decoder) << "\", ";
  os << "\"decoder_version\": \"" << json_escape(r.decoder_version) << "\", ";
  os << "\"threads\": " << r.threads << ", ";
  os << "\"width\": " << r.width << ", ";
  os << "\"height\": " << r.height << ", ";
  os << "\"channels\": " << r.channels << ", ";
  os << "\"bit_depth\": " << r.bit_depth << ", ";
  // Subsampling factors per component. null when 4:4:4 or no decode; otherwise
  // an array [{w,h,dx,dy}, ...].
  bool any_sub = false;
  for (const auto& c : r.subsampled_components) if (c.dx != 1 || c.dy != 1) { any_sub = true; break; }
  if (any_sub) {
    os << "\"subsampled_components\": [";
    for (std::size_t i = 0; i < r.subsampled_components.size(); ++i) {
      const auto& c = r.subsampled_components[i];
      if (i) os << ", ";
      os << "{\"w\": " << c.w << ", \"h\": " << c.h
         << ", \"dx\": " << c.dx << ", \"dy\": " << c.dy << "}";
    }
    os << "], ";
  } else {
    os << "\"subsampled_components\": null, ";
  }
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
  if (r.scale_track) {
    os << "\"scale_track\": true, ";
    os << "\"rss_peak_kb_sampled\": " << r.rss_peak_kb_sampled << ", ";
    os << "\"memory_max_bytes\": "    << r.memory_max_bytes    << ", ";
  } else {
    os << "\"scale_track\": false, ";
  }
  os << "\"reused_codec\": " << (r.reused_codec ? "true" : "false") << ", ";
  os << "\"header_only\": "  << (r.header_only  ? "true" : "false") << ", ";
  if (r.profile_stages) {
    os << "\"stages_s\": {"
       << "\"setup\": "    << r.stage_setup_s    << ", "
       << "\"decode\": "   << r.stage_decode_s   << ", "
       << "\"unpack\": "   << r.stage_unpack_s   << ", "
       << "\"teardown\": " << r.stage_teardown_s
       << "}, ";
  } else {
    os << "\"stages_s\": null, ";
  }
  os << "\"error\": \"" << json_escape(r.error) << "\"";
}

void emit_run_fields(std::ostream& os, const RunHeader& h) {
  os << "\"schema_version\": 2, ";
  os << "\"started_at\": \"" << json_escape(h.started_at_iso8601) << "\", ";
  os << "\"argv\": [";
  for (std::size_t i = 0; i < h.argv.size(); ++i) {
    if (i) os << ", ";
    os << "\"" << json_escape(h.argv[i]) << "\"";
  }
  os << "], ";
  os << "\"concurrent_files\": " << h.concurrent_files;
  if (!h.env_json.empty()) {
    os << ", \"env\": " << h.env_json;
  }
  if (!h.corpus_summary_json.empty()) {
    os << ", \"corpus_summary\": " << h.corpus_summary_json;
  }
  if (!h.corpus_spec_json.empty()) {
    os << ", \"corpus_spec\": " << h.corpus_spec_json;
  }
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
  if (!h.corpus_summary_json.empty()) {
    os << ",\n    \"corpus_summary\": " << h.corpus_summary_json;
  }
  if (!h.corpus_spec_json.empty()) {
    os << ",\n    \"corpus_spec\": " << h.corpus_spec_json;
  }
  os << "\n  },\n";

  os << "  \"results\": [\n";
  for (std::size_t i = 0; i < results.size(); ++i) {
    if (i) os << ",\n";
    os << "    {";
    emit_result_fields(os, results[i]);
    os << "}";
  }
  os << "\n  ],\n";

  os << "  \"aggregate\": {";
  if (std::isfinite(agg.concurrent_throughput_mpix_s)) {
    os << "\"concurrent_throughput_mpix_s\": " << agg.concurrent_throughput_mpix_s;
  }
  os << "}\n";
  os << "}\n";
}

void write_jsonl_run(std::ostream& os, const RunHeader& h) {
  os << "{\"type\": \"run\", ";
  emit_run_fields(os, h);
  os << "}\n";
}

void write_jsonl_result(std::ostream& os, const FileResult& r) {
  if (r.is_correctness_row) {
    os << "{\"type\": \"correctness\", ";
    os << "\"file\": \"" << json_escape(r.path) << "\", ";
    os << "\"bytes\": " << r.bytes << ", ";
    os << "\"decoder\": \"" << json_escape(r.decoder) << "\", ";
    os << "\"decoder_version\": \"" << json_escape(r.decoder_version) << "\", ";
    os << "\"threads\": " << r.threads << ", ";
    const char* oc = (r.outcome == CorrectnessOutcome::Decoded)
                         ? "decoded_ok"
                         : "cleanly_rejected";
    os << "\"outcome\": \"" << oc << "\", ";
    if (!r.expected_outcome.empty()) {
      os << "\"expected_outcome\": \""
         << json_escape(r.expected_outcome) << "\", ";
    } else {
      os << "\"expected_outcome\": null, ";
    }
    if (r.outcome == CorrectnessOutcome::Decoded) {
      os << "\"width\": "      << r.width      << ", "
         << "\"height\": "     << r.height     << ", "
         << "\"channels\": "   << r.channels   << ", "
         << "\"bit_depth\": "  << r.bit_depth  << ", ";
    }
    os << "\"error\": \"" << json_escape(r.error) << "\"";
    os << "}\n";
    return;
  }
  os << "{\"type\": \"result\", ";
  emit_result_fields(os, r);
  os << "}\n";
}

void write_jsonl_aggregate(std::ostream& os, const Aggregate& agg) {
  os << "{\"type\": \"aggregate\"";
  if (std::isfinite(agg.concurrent_throughput_mpix_s)) {
    os << ", \"concurrent_throughput_mpix_s\": "
       << agg.concurrent_throughput_mpix_s;
  }
  os << "}\n";
}

}  // namespace jp2kbench
