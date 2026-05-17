#pragma once
#include <ostream>
#include <string>
#include <vector>
#include "bench.h"

namespace jp2kbench {

struct RunHeader {
  std::string started_at_iso8601;
  std::vector<std::string> argv;
  int concurrent_files = 1;
  // Env block is a single pre-rendered JSON object string ("{...}"); the env
  // module owns formatting. Empty => omit field.
  std::string env_json;
};

struct Aggregate {
  // Populated when concurrent_files > 1. NaN => omitted.
  double concurrent_throughput_mpix_s = 0.0 / 0.0;
};

void write_schema_v2(std::ostream& os,
                     const RunHeader& header,
                     const std::vector<FileResult>& results,
                     const Aggregate& agg);

std::string json_escape(const std::string& s);

}  // namespace jp2kbench
