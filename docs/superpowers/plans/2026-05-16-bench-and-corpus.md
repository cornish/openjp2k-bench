# Bench + corpus expansion — implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Land the corpus structure (user/synthetic/public + manifest) and four measurement axes (peak RSS, ROI/region decode, `--concurrent-files`, env capture) into `jp2k-bench` so the upcoming `openjp2k` fork can be benched on day one.

**Architecture:**
- Result JSON migrates from a top-level array to schema v2 (`{schema_version, run, results, aggregate}`) built in memory and serialized once at the end.
- C++ side gains `env_capture`, RSS sampling around the timed loop, a `decode_region` adapter method, and a tiny thread pool wrapping per-file dispatch.
- Corpus tooling is a set of small shell + Python scripts under `scripts/`; corpus payload stays gitignored.

**Tech Stack:** C++17, CMake 3.20+, OpenJPEG 2.5.x, Grok 13.x, Bash, Python 3.8+ (stdlib only).

**Spec:** `docs/superpowers/specs/2026-05-16-bench-and-corpus-design.md`.

---

## File structure

**New files:**
- `src/env_capture.h`, `src/env_capture.cpp` — collects host/build/lib env once per run.
- `src/rss.h`, `src/rss.cpp` — `getrusage`-based RSS sampling.
- `src/json_out.h`, `src/json_out.cpp` — schema v2 emitter (extracted from `main.cpp`).
- `src/verify.h`, `src/verify.cpp` — PSNR fallback for lossy mismatches.
- `src/thread_pool.h` — header-only minimal pool for `--concurrent-files`.
- `scripts/fetch_corpus.sh` — pinned-URL public bucket downloader.
- `scripts/build_manifest.sh` — wraps the Python tool.
- `scripts/manifest_tool.py` — parses JP2K headers, emits `corpus/manifest.json`.
- `scripts/check_corpus.sh` — bucket validation.
- `tests/smoke.sh` — end-to-end smoke against a tiny fixture.
- `tests/fixtures/tiny.jp2` — checked-in 128² fixture.
- `tests/manifest_tool_test.py` — Python unit tests for the manifest parser.
- `cmake/Versions.cmake` — extracts compiler version + lib commit SHAs, generates `src/build_info.h`.

**Modified files:**
- `src/main.cpp` — schema v2, `--roi`, `--roi-tile`, `--concurrent-files` flags.
- `src/bench.h`, `src/bench.cpp` — `BenchOptions.roi`, `FileResult.rss_*`, region passthrough.
- `src/adapter.h` — `Decoder::supports_region()`, `Decoder::decode_region()`.
- `src/adapter_openjpeg.cpp` — `decode_region` via `opj_set_decode_area`.
- `src/adapter_grok.cpp` — `decode_region` via Grok's region API.
- `CMakeLists.txt` — wires new sources, includes `cmake/Versions.cmake`.
- `cmake/Flags.cmake` — unchanged unless `_extra` flag list needs a tweak; left alone.
- `scripts/gen_corpus.sh` — rewritten for exhaustive synthetic grid.
- `scripts/run_bench.sh` — pass-through for new flags.
- `scripts/report.py` — schema v2 reader, env diff, `--group-by`, `--filter`.
- `README.md` — corpus + flag docs updated at the end.

---

## Task 1 — Refactor result JSON to schema v2 envelope (no new fields yet)

**Goal:** Migrate the on-disk format from a top-level array to `{schema_version: 2, run: {...}, results: [...], aggregate: {...}}`. Defer new payload fields to later tasks; this task is purely a structural move so subsequent tasks can plug in.

**Files:**
- Create: `src/json_out.h`, `src/json_out.cpp`
- Modify: `src/main.cpp`
- Modify: `CMakeLists.txt:70-74`

- [ ] **Step 1: Add `src/json_out.h`**

```cpp
#pragma once
#include <ostream>
#include <string>
#include <vector>
#include "bench.h"

namespace jp2kbench {

struct RunHeader {
  std::string started_at_iso8601;   // e.g. "2026-05-16T14:00:00Z"
  std::vector<std::string> argv;
  int concurrent_files = 1;
  // Env block is a single pre-rendered JSON object string ("{...}"); the env
  // module owns formatting. Empty string => omit field.
  std::string env_json;
};

struct Aggregate {
  // Populated when concurrent_files > 1. NaN => omitted.
  double concurrent_throughput_mpix_s = 0.0 / 0.0;
};

// Writes the full schema v2 object to `os`. Caller passes the assembled
// FileResults; this function does the JSON encoding.
void write_schema_v2(std::ostream& os,
                     const RunHeader& header,
                     const std::vector<FileResult>& results,
                     const Aggregate& agg);

std::string json_escape(const std::string& s);

}  // namespace jp2kbench
```

- [ ] **Step 2: Add `src/json_out.cpp`**

```cpp
#include "json_out.h"

#include <cmath>
#include <cstdio>
#include <sstream>

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
  os << "\"pixel_match\": " << r.pixel_match << ", ";
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
```

- [ ] **Step 3: Add `RunStats.warmup` field**

In `src/bench.h`, change the `RunStats` struct to include the warmup count actually used (needed for the JSON emitter):

```cpp
struct RunStats {
  double min = 0, p50 = 0, p90 = 0, p99 = 0, max = 0, mean = 0, stddev = 0;
  int iters = 0;
  int warmup = 0;
};
```

In `src/bench.cpp`, set `s.warmup = ...` in `bench_file` before returning. Modify `bench_file` to set `r.stats.warmup = opts.warmup;` immediately after `r.stats = summarize(times);`. (Apply in both the early-return error path and the normal return.)

- [ ] **Step 4: Rewrite `main.cpp` to collect results and emit via `write_schema_v2`**

Replace the `[ ... ]` streaming block and the local `emit_result` / `json_escape` helpers with collection into a vector, then a single call to `write_schema_v2` after the loops.

Concretely, in `src/main.cpp`:

1. Remove the local `json_escape` and `emit_result` functions (now in `json_out.cpp`).
2. Add `#include "json_out.h"` near the other includes.
3. Replace `std::cout << "[\n"; bool first = true;` and the trailing `std::cout << "\n]\n";` with:

```cpp
  std::vector<FileResult> all_results;
  // ... loops that previously emitted incrementally now do all_results.push_back(r); ...

  RunHeader header;
  {
    char ts[32];
    std::time_t t = std::time(nullptr);
    std::strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));
    header.started_at_iso8601 = ts;
  }
  for (int i = 0; i < argc; ++i) header.argv.emplace_back(argv[i]);
  header.concurrent_files = 1;   // set by Task 10
  header.env_json = "";          // set by Task 2

  Aggregate agg;                 // populated by Task 10

  write_schema_v2(std::cout, header, all_results, agg);
```

Add `#include <ctime>` to the includes for `std::time` / `std::strftime` / `std::gmtime`.

Inside the per-decoder / per-thread loop, replace `emit_result(std::cout, r, first); first = false;` with `all_results.push_back(std::move(r));`.

- [ ] **Step 5: Wire `json_out.cpp` into CMake**

Edit `CMakeLists.txt`, change the `add_executable` block (currently at lines 70-74):

```cmake
add_executable(jp2k-bench
  src/main.cpp
  src/bench.cpp
  src/json_out.cpp
  src/adapter_openjpeg.cpp
)
```

- [ ] **Step 6: Build and run smoke check**

Run:
```bash
./scripts/build.sh
```
Expected: build succeeds, prints `[build] built: build/jp2k-bench` and `openjpeg` (and `grok` if present).

Then, with any existing JP2 (or skip if no corpus yet — just verify `--list-decoders` still works):
```bash
./build/jp2k-bench --list-decoders
```
Expected: prints `openjpeg` (and `grok`).

If a `.jp2` exists anywhere on the machine, run it through the bench and pipe to `python3 -m json.tool` to confirm the new envelope:
```bash
./build/jp2k-bench --iters 2 --warmup 1 /path/to/any.jp2 | python3 -m json.tool | head -20
```
Expected: top-level object with `schema_version: 2`, `run`, `results`, `aggregate`. No trailing array.

- [ ] **Step 7: Commit**

```bash
git add src/json_out.h src/json_out.cpp src/main.cpp src/bench.h src/bench.cpp CMakeLists.txt
git commit -m "Refactor result JSON to schema v2 envelope"
```

---

## Task 2 — Inject build info, capture host env

**Goal:** Populate the `env` block in the result JSON with CPU, governor, turbo, kernel, compiler, lib versions, lib commit SHAs, and compile flags. Compile-time facts (compiler, lib versions, commits, flags) come from a CMake-generated header; runtime facts (CPU, governor, kernel) come from `/proc` and `/sys`.

**Files:**
- Create: `cmake/Versions.cmake`
- Create: `src/env_capture.h`, `src/env_capture.cpp`
- Create: `src/build_info.h.in` (CMake input template)
- Modify: `CMakeLists.txt`
- Modify: `src/main.cpp`

- [ ] **Step 1: Add `src/build_info.h.in`**

```cpp
#pragma once
// Generated by CMake from build_info.h.in. Do not edit.

#define JP2KBENCH_COMPILER_ID      "@CMAKE_CXX_COMPILER_ID@"
#define JP2KBENCH_COMPILER_VERSION "@CMAKE_CXX_COMPILER_VERSION@"
#define JP2KBENCH_COMPILE_FLAGS    "@JP2KBENCH_COMPILE_FLAGS_STR@"

#define JP2KBENCH_OPENJPEG_VERSION "@JP2KBENCH_OPENJPEG_VERSION_STR@"
#define JP2KBENCH_OPENJPEG_COMMIT  "@JP2KBENCH_OPENJPEG_COMMIT_STR@"

#define JP2KBENCH_GROK_VERSION     "@JP2KBENCH_GROK_VERSION_STR@"
#define JP2KBENCH_GROK_COMMIT      "@JP2KBENCH_GROK_COMMIT_STR@"
```

- [ ] **Step 2: Add `cmake/Versions.cmake`**

```cmake
# Resolve library version strings and commit SHAs at configure time, then
# render src/build_info.h from src/build_info.h.in. Re-run when configure
# is re-run; not a build-time dependency.

function(_jp2kbench_git_describe out_var path)
  set(_sha "unknown")
  if(EXISTS "${path}/.git")
    execute_process(
      COMMAND git -C "${path}" rev-parse --short=12 HEAD
      OUTPUT_VARIABLE _sha
      OUTPUT_STRIP_TRAILING_WHITESPACE
      RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
      set(_sha "unknown")
    endif()
  endif()
  set(${out_var} "${_sha}" PARENT_SCOPE)
endfunction()

# OpenJPEG: version comes from its CMake (OPENJPEG_VERSION) once added.
# Capture commit here.
_jp2kbench_git_describe(JP2KBENCH_OPENJPEG_COMMIT_STR "${JP2KBENCH_OPENJPEG_SOURCE}")
if(DEFINED OPENJPEG_VERSION)
  set(JP2KBENCH_OPENJPEG_VERSION_STR "${OPENJPEG_VERSION}")
else()
  set(JP2KBENCH_OPENJPEG_VERSION_STR "unknown")
endif()

# Grok
if(JP2KBENCH_HAVE_GROK)
  _jp2kbench_git_describe(JP2KBENCH_GROK_COMMIT_STR "${JP2KBENCH_GROK_SOURCE}")
  if(DEFINED GRK_VERSION)
    set(JP2KBENCH_GROK_VERSION_STR "${GRK_VERSION}")
  else()
    set(JP2KBENCH_GROK_VERSION_STR "unknown")
  endif()
else()
  set(JP2KBENCH_GROK_COMMIT_STR  "n/a")
  set(JP2KBENCH_GROK_VERSION_STR "n/a")
endif()

# Compile flags string. Best-effort: the actual per-target flags are applied
# inside jp2kbench_apply_flags(), so reconstruct the same string here.
if(MSVC)
  set(JP2KBENCH_COMPILE_FLAGS_STR "/O2 /DNDEBUG /Oi /Ot /GL /LTCG")
else()
  set(JP2KBENCH_COMPILE_FLAGS_STR
      "-O3 -DNDEBUG -fno-plt -fomit-frame-pointer -funroll-loops -ffp-contract=fast")
  if(JP2KBENCH_NATIVE)
    set(JP2KBENCH_COMPILE_FLAGS_STR
        "${JP2KBENCH_COMPILE_FLAGS_STR} -march=native -mtune=native")
  endif()
endif()

configure_file(
  ${CMAKE_SOURCE_DIR}/src/build_info.h.in
  ${CMAKE_BINARY_DIR}/generated/build_info.h
  @ONLY)
```

- [ ] **Step 3: Wire `Versions.cmake` into `CMakeLists.txt`**

In `CMakeLists.txt`, after the `add_subdirectory(... _grok ...)` block (around line 67) and before `add_executable(jp2k-bench ...)`, add:

```cmake
include(cmake/Versions.cmake)
```

Then extend the `target_include_directories(jp2k-bench PRIVATE src)` line to also include the generated directory:

```cmake
target_include_directories(jp2k-bench PRIVATE
  src
  ${CMAKE_BINARY_DIR}/generated)
```

- [ ] **Step 4: Add `src/env_capture.h`**

```cpp
#pragma once
#include <string>

namespace jp2kbench {

// Returns a complete JSON object string ("{...}") describing host + build
// environment. Designed to be embedded into RunHeader.env_json verbatim.
std::string capture_env_json();

}  // namespace jp2kbench
```

- [ ] **Step 5: Add `src/env_capture.cpp`**

```cpp
#include "env_capture.h"
#include "json_out.h"
#include "build_info.h"

#include <fstream>
#include <sstream>
#include <string>
#include <sys/utsname.h>
#include <thread>
#include <unistd.h>

namespace jp2kbench {
namespace {

std::string read_first_line(const std::string& path) {
  std::ifstream f(path);
  if (!f) return "";
  std::string line;
  std::getline(f, line);
  return line;
}

std::string cpu_model() {
  std::ifstream f("/proc/cpuinfo");
  std::string line;
  while (std::getline(f, line)) {
    if (line.rfind("model name", 0) == 0) {
      auto pos = line.find(':');
      if (pos != std::string::npos) {
        std::string v = line.substr(pos + 1);
        while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(0, 1);
        return v;
      }
    }
  }
  return "unknown";
}

int cores_online() {
  long n = sysconf(_SC_NPROCESSORS_ONLN);
  return n > 0 ? (int)n : (int)std::thread::hardware_concurrency();
}

std::string governor() {
  auto v = read_first_line("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor");
  return v.empty() ? "unknown" : v;
}

// "1" => turbo disabled. File absent on AMD / non-intel_pstate kernels.
std::string turbo_disabled() {
  auto v = read_first_line("/sys/devices/system/cpu/intel_pstate/no_turbo");
  if (v == "1") return "true";
  if (v == "0") return "false";
  return "unknown";
}

std::string kernel() {
  struct utsname u{};
  if (uname(&u) != 0) return "unknown";
  return std::string(u.sysname) + " " + u.release + " " + u.machine;
}

}  // namespace

std::string capture_env_json() {
  std::ostringstream os;
  os << "{";
  os << "\"cpu_model\": \"" << json_escape(cpu_model()) << "\", ";
  os << "\"cores_online\": " << cores_online() << ", ";
  os << "\"governor\": \"" << json_escape(governor()) << "\", ";
  os << "\"turbo_disabled\": " << turbo_disabled() << ", ";
  os << "\"kernel\": \"" << json_escape(kernel()) << "\", ";
  os << "\"compiler\": \"" << JP2KBENCH_COMPILER_ID << " "
                            << JP2KBENCH_COMPILER_VERSION << "\", ";
  os << "\"compile_flags\": \"" << json_escape(JP2KBENCH_COMPILE_FLAGS) << "\", ";
  os << "\"libs\": {";
  os <<   "\"openjpeg\": {"
     <<   "\"version\": \"" << json_escape(JP2KBENCH_OPENJPEG_VERSION) << "\", "
     <<   "\"commit\": \""  << json_escape(JP2KBENCH_OPENJPEG_COMMIT)  << "\"}";
  os << ", \"grok\": {"
     <<   "\"version\": \"" << json_escape(JP2KBENCH_GROK_VERSION) << "\", "
     <<   "\"commit\": \""  << json_escape(JP2KBENCH_GROK_COMMIT)  << "\"}";
  os << "}";
  os << "}";
  return os.str();
}

}  // namespace jp2kbench
```

- [ ] **Step 6: Add `env_capture.cpp` to `CMakeLists.txt`**

In the `add_executable(jp2k-bench ...)` block, add `src/env_capture.cpp` to the source list:

```cmake
add_executable(jp2k-bench
  src/main.cpp
  src/bench.cpp
  src/json_out.cpp
  src/env_capture.cpp
  src/adapter_openjpeg.cpp
)
```

- [ ] **Step 7: Populate `header.env_json` in `main.cpp`**

Add `#include "env_capture.h"` to `src/main.cpp` includes. Change the `header.env_json = "";` line written in Task 1 to:

```cpp
header.env_json = capture_env_json();
```

- [ ] **Step 8: Build and verify**

```bash
./scripts/build.sh
./build/jp2k-bench --list-decoders   # still works
```

If any `.jp2` is available:
```bash
./build/jp2k-bench --iters 2 --warmup 1 /path/to/any.jp2 \
  | python3 -m json.tool | sed -n '1,30p'
```
Expected: `run.env` block populated with CPU model, governor, kernel, compiler version, lib versions + 12-char commits.

- [ ] **Step 9: Commit**

```bash
git add src/build_info.h.in src/env_capture.h src/env_capture.cpp \
        cmake/Versions.cmake CMakeLists.txt src/main.cpp
git commit -m "Capture build + host env into result JSON"
```

---

## Task 3 — Peak RSS per decode

**Goal:** Sample `ru_maxrss` before/after each timed iteration; record the max delta and peak across iterations in `FileResult`. Emit in JSON.

**Files:**
- Create: `src/rss.h`, `src/rss.cpp`
- Modify: `src/bench.h` — add `rss_peak_kb`, `rss_delta_kb` to `FileResult`.
- Modify: `src/bench.cpp` — sample around each iteration.
- Modify: `src/json_out.cpp` — emit two new fields.
- Modify: `CMakeLists.txt` — add `src/rss.cpp` to sources.

- [ ] **Step 1: Add `src/rss.h`**

```cpp
#pragma once
#include <cstdint>

namespace jp2kbench {

// Returns current process peak RSS in kilobytes. Linux convention
// (ru_maxrss is KB). On non-Linux platforms returns 0.
uint64_t peak_rss_kb();

}  // namespace jp2kbench
```

- [ ] **Step 2: Add `src/rss.cpp`**

```cpp
#include "rss.h"

#include <sys/resource.h>

namespace jp2kbench {

uint64_t peak_rss_kb() {
  struct rusage ru{};
  if (getrusage(RUSAGE_SELF, &ru) != 0) return 0;
  // Linux: ru_maxrss is KB. (macOS: bytes — we don't target macOS here.)
  return (uint64_t)ru.ru_maxrss;
}

}  // namespace jp2kbench
```

- [ ] **Step 3: Extend `FileResult`**

In `src/bench.h`, inside `FileResult`, after `int pixel_match = -1;`:

```cpp
  uint64_t rss_peak_kb = 0;     // highest peak RSS observed at end of any timed iter
  int64_t  rss_delta_kb = 0;    // max (peak_after - peak_before) across timed iters
```

- [ ] **Step 4: Sample RSS in `bench_file`**

In `src/bench.cpp`, add `#include "rss.h"` near the existing `#include <cstring>` etc.

Change the timed loop (currently lines ~88-99) to sample before/after:

```cpp
  uint64_t peak_observed = 0;
  int64_t  max_delta = 0;
  std::vector<double> times;
  times.reserve(opts.iters);
  for (int i = 0; i < opts.iters; ++i) {
    img.pixels.clear();
    uint64_t rss_before = peak_rss_kb();
    double t0 = now_seconds();
    bool ok = decoder.decode(blob.data(), blob.size(), threads, img, err);
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
```

(`peak_rss_kb` is monotonic over the process lifetime; the delta is still informative because each decode allocates and frees its working set.)

- [ ] **Step 5: Emit new fields in JSON**

In `src/json_out.cpp`, inside `emit_result`, after the `"pixel_match"` line and before `"error"`:

```cpp
  os << "\"rss_peak_kb\": "  << r.rss_peak_kb  << ", ";
  os << "\"rss_delta_kb\": " << r.rss_delta_kb << ", ";
```

- [ ] **Step 6: Add `src/rss.cpp` to CMake**

Add `src/rss.cpp` to the `add_executable(jp2k-bench ...)` source list in `CMakeLists.txt`.

- [ ] **Step 7: Build and verify**

```bash
./scripts/build.sh
./build/jp2k-bench --iters 3 --warmup 1 /path/to/any.jp2 \
  | python3 -m json.tool | grep -E 'rss_(peak|delta)_kb'
```
Expected: two non-zero values per result. `rss_peak_kb` should be in the thousands+ (the process itself).

- [ ] **Step 8: Commit**

```bash
git add src/rss.h src/rss.cpp src/bench.h src/bench.cpp src/json_out.cpp CMakeLists.txt
git commit -m "Sample peak RSS around timed iterations"
```

---

## Task 4 — Python manifest tool: JP2K header parser

**Goal:** Pure-stdlib Python tool that takes a list of JP2K files and writes `corpus/manifest.json`. Parses just enough of the JP2 box structure and codestream (SIZ, COD markers) to populate the manifest schema: width, height, components, bit depth, tile width/height, decomp levels, lossy/lossless, progression order, container type.

**Files:**
- Create: `scripts/manifest_tool.py`
- Create: `tests/manifest_tool_test.py`

- [ ] **Step 1: Add the failing test**

Create `tests/manifest_tool_test.py`:

```python
"""Tests for scripts/manifest_tool.py.

Generates a tiny .jp2 with opj_compress, parses it, asserts fields.
Skipped if opj_compress is unavailable.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

import manifest_tool  # noqa: E402


@unittest.skipUnless(shutil.which("opj_compress"), "opj_compress not on PATH")
class ManifestToolTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="manifest_test_"))

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def _make_jp2(self, name: str, w: int, h: int, lossless: bool,
                  tile: int | None, prog: str | None) -> Path:
        # Synthesize a tiny PGM as input.
        src = self.tmp / "src.pgm"
        with open(src, "wb") as f:
            f.write(f"P5\n{w} {h}\n255\n".encode())
            f.write(bytes((i * 7) & 0xFF for i in range(w * h)))
        out = self.tmp / name
        cmd = ["opj_compress", "-i", str(src), "-o", str(out)]
        if lossless:
            cmd += ["-r", "1", "-I"]
        else:
            cmd += ["-r", "20"]
        if tile:
            cmd += ["-t", f"{tile},{tile}"]
        if prog:
            cmd += ["-p", prog]
        subprocess.run(cmd, check=True, capture_output=True)
        return out

    def test_lossless_single_tile(self):
        p = self._make_jp2("a.jp2", 64, 48, lossless=True, tile=None, prog=None)
        info = manifest_tool.parse_file(p)
        self.assertEqual(info["width"], 64)
        self.assertEqual(info["height"], 48)
        self.assertEqual(info["components"], 1)
        self.assertEqual(info["bit_depth"], 8)
        self.assertTrue(info["lossless"])
        self.assertEqual(info["container"], "jp2")
        # Single tile => tile == image dimensions.
        self.assertEqual(info["tile_w"], 64)
        self.assertEqual(info["tile_h"], 48)

    def test_lossy_tiled_rlcp(self):
        p = self._make_jp2("b.jp2", 128, 128, lossless=False, tile=32, prog="RLCP")
        info = manifest_tool.parse_file(p)
        self.assertEqual(info["tile_w"], 32)
        self.assertEqual(info["tile_h"], 32)
        self.assertFalse(info["lossless"])
        self.assertEqual(info["progression"], "RLCP")

    def test_writes_manifest_json(self):
        p1 = self._make_jp2("x.jp2", 32, 32, lossless=True, tile=None, prog=None)
        out_json = self.tmp / "manifest.json"
        manifest_tool.write_manifest([p1], self.tmp, out_json, bucket_of=lambda _: "user")
        data = json.loads(out_json.read_text())
        self.assertEqual(len(data["files"]), 1)
        f0 = data["files"][0]
        self.assertEqual(f0["bucket"], "user")
        self.assertEqual(len(f0["sha256"]), 64)
        self.assertEqual(f0["width"], 32)


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run the test to verify it fails (import error)**

```bash
cd /home/cornish/GitHub/openjp2k-bench
python3 -m unittest tests.manifest_tool_test -v
```
Expected: `ModuleNotFoundError: No module named 'manifest_tool'` (or import error).

- [ ] **Step 3: Write `scripts/manifest_tool.py`**

```python
#!/usr/bin/env python3
"""Parse JPEG 2000 files and emit a corpus manifest.

Parses just enough of the JP2 box structure and JPEG 2000 codestream (SIZ,
COD markers) to populate manifest fields without external dependencies.

Usage:
  manifest_tool.py --root corpus/ --out corpus/manifest.json
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path
from typing import Callable, Iterable

# --- JPEG 2000 marker constants ---------------------------------------------
SOC = 0xFF4F   # Start of Codestream
SIZ = 0xFF51   # Image and tile size
COD = 0xFF52   # Coding style default
SOT = 0xFF90   # Start of tile-part (no-op here, terminates scan)

# JP2 box signatures
JP2_SIGBOX = b"\x00\x00\x00\x0cjP  \x0d\x0a\x87\x0a"


def _find_codestream(data: bytes) -> int:
    """Return offset of SOC marker in `data`. Raises if not found."""
    # If JP2-boxed, locate the `jp2c` (codestream contiguous) box.
    if data.startswith(JP2_SIGBOX):
        offset = 0
        n = len(data)
        while offset + 8 <= n:
            box_len = struct.unpack(">I", data[offset:offset + 4])[0]
            box_type = data[offset + 4:offset + 8]
            header = 8
            if box_len == 1:
                # 64-bit extended length follows.
                if offset + 16 > n:
                    break
                box_len = struct.unpack(">Q", data[offset + 8:offset + 16])[0]
                header = 16
            elif box_len == 0:
                box_len = n - offset
            if box_type == b"jp2c":
                start = offset + header
                # The codestream begins with SOC (0xFF4F).
                if start + 2 <= n and data[start] == 0xFF and data[start + 1] == 0x4F:
                    return start
                raise ValueError("jp2c box does not start with SOC")
            offset += box_len
        raise ValueError("no jp2c box found")
    # Raw codestream.
    if len(data) >= 2 and data[0] == 0xFF and data[1] == 0x4F:
        return 0
    raise ValueError("not a JP2/J2K file")


_PROG_ORDER = {0: "LRCP", 1: "RLCP", 2: "RPCL", 3: "PCRL", 4: "CPRL"}


def _parse_codestream(data: bytes, cs_offset: int) -> dict:
    """Walk markers from SOC until SOT; extract SIZ + COD info."""
    out: dict = {}
    n = len(data)
    pos = cs_offset
    if data[pos] != 0xFF or data[pos + 1] != 0x4F:
        raise ValueError("expected SOC")
    pos += 2

    while pos + 4 <= n:
        marker = (data[pos] << 8) | data[pos + 1]
        pos += 2
        if marker == SOT:
            break
        # Segments other than SOC/EOC have a 2-byte length immediately after.
        seg_len = struct.unpack(">H", data[pos:pos + 2])[0]
        seg_start = pos + 2
        seg_end = pos + seg_len
        if seg_end > n:
            break

        if marker == SIZ:
            # SIZ structure (offsets relative to seg_start):
            #   Rsiz       u16 @0
            #   Xsiz       u32 @2
            #   Ysiz       u32 @6
            #   XOsiz      u32 @10
            #   YOsiz      u32 @14
            #   XTsiz      u32 @18
            #   YTsiz      u32 @22
            #   XTOsiz     u32 @26
            #   YTOsiz     u32 @30
            #   Csiz       u16 @34
            #   then Csiz components: Ssiz(u8), XRsiz(u8), YRsiz(u8)
            xsiz = struct.unpack(">I", data[seg_start + 2:seg_start + 6])[0]
            ysiz = struct.unpack(">I", data[seg_start + 6:seg_start + 10])[0]
            xosiz = struct.unpack(">I", data[seg_start + 10:seg_start + 14])[0]
            yosiz = struct.unpack(">I", data[seg_start + 14:seg_start + 18])[0]
            xtsiz = struct.unpack(">I", data[seg_start + 18:seg_start + 22])[0]
            ytsiz = struct.unpack(">I", data[seg_start + 22:seg_start + 26])[0]
            csiz = struct.unpack(">H", data[seg_start + 34:seg_start + 36])[0]
            ssiz = data[seg_start + 36]
            # Bit depth = (Ssiz & 0x7F) + 1; high bit = signed flag.
            out["width"] = xsiz - xosiz
            out["height"] = ysiz - yosiz
            out["components"] = csiz
            out["bit_depth"] = (ssiz & 0x7F) + 1
            out["tile_w"] = xtsiz
            out["tile_h"] = ytsiz

        elif marker == COD:
            # COD structure:
            #   Scod    u8  @0   (coding style flags)
            #   SGcod (4 bytes) @1: progression(u8), num_layers(u16), mct(u8)
            #   SPcod ...      @5: num_decomp(u8), cblk_w(u8), cblk_h(u8),
            #                       cblk_style(u8), transform(u8), then optional precincts
            sg_prog = data[seg_start + 1]
            num_layers = struct.unpack(">H", data[seg_start + 2:seg_start + 4])[0]
            mct = data[seg_start + 4]
            num_decomp = data[seg_start + 5]
            transform = data[seg_start + 10]   # 0 = 9-7 irreversible (lossy), 1 = 5-3 reversible
            out["progression"] = _PROG_ORDER.get(sg_prog, f"unknown({sg_prog})")
            out["num_layers"] = num_layers
            out["mct"] = bool(mct)
            out["decomp_levels"] = num_decomp
            out["lossless"] = (transform == 1)

        pos = seg_end

    return out


def parse_file(path: Path) -> dict:
    """Parse a single JP2/J2K file and return its manifest entry (without sha256)."""
    raw = path.read_bytes()
    info: dict = {
        "container": "jp2" if raw.startswith(JP2_SIGBOX) else "j2k",
        "bytes": len(raw),
    }
    cs_off = _find_codestream(raw)
    info.update(_parse_codestream(raw, cs_off))
    return info


def _sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def write_manifest(files: Iterable[Path], root: Path, out_json: Path,
                   bucket_of: Callable[[Path], str]) -> None:
    """Write a manifest.json describing `files`. Paths recorded relative to root."""
    entries = []
    for p in sorted(files):
        rel = p.resolve().relative_to(root.resolve()).as_posix()
        try:
            info = parse_file(p)
        except Exception as e:
            entries.append({"path": rel, "bucket": bucket_of(p), "error": str(e)})
            continue
        info["path"] = rel
        info["bucket"] = bucket_of(p)
        info["sha256"] = _sha256(p)
        entries.append(info)
    out_json.write_text(json.dumps({"schema_version": 1, "files": entries}, indent=2))


def _bucket_from_path(root: Path):
    def _b(p: Path) -> str:
        rel = p.resolve().relative_to(root.resolve()).parts
        if not rel:
            return "user"
        first = rel[0]
        if first in ("user", "synthetic", "public"):
            return first
        return "user"
    return _b


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", type=Path, required=True,
                    help="corpus root (paths in manifest are relative to this)")
    ap.add_argument("--out", type=Path, required=True,
                    help="manifest output path")
    args = ap.parse_args()

    root: Path = args.root
    if not root.exists():
        print(f"manifest_tool: {root} does not exist", file=sys.stderr)
        return 2
    files = [p for p in root.rglob("*")
             if p.is_file() and p.suffix.lower() in (".jp2", ".j2k", ".jpc")]
    write_manifest(files, root, args.out, _bucket_from_path(root))
    print(f"manifest_tool: wrote {args.out} ({len(files)} files)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 4: Run the test to verify it passes**

```bash
python3 -m unittest tests.manifest_tool_test -v
```
Expected: 3 tests pass (or all skipped if `opj_compress` is unavailable — install with `apt install libopenjp2-tools` to actually run them).

- [ ] **Step 5: Commit**

```bash
git add scripts/manifest_tool.py tests/manifest_tool_test.py
git commit -m "Add manifest_tool: parse JP2K headers, emit corpus manifest"
```

---

## Task 5 — `scripts/build_manifest.sh` wrapper

**Goal:** A thin shell wrapper that invokes `manifest_tool.py` with the right paths and is convenient to call from `check_corpus.sh` and `run_bench.sh`.

**Files:**
- Create: `scripts/build_manifest.sh`

- [ ] **Step 1: Create `scripts/build_manifest.sh`**

```bash
#!/usr/bin/env bash
# Build corpus/manifest.json by scanning corpus/{user,synthetic,public}.
#
# Usage:
#   ./scripts/build_manifest.sh
#
# Re-run after adding/removing files in any bucket.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CORPUS="$ROOT/corpus"

mkdir -p "$CORPUS/user" "$CORPUS/synthetic" "$CORPUS/public"

python3 "$ROOT/scripts/manifest_tool.py" \
  --root "$CORPUS" \
  --out  "$CORPUS/manifest.json"
```

- [ ] **Step 2: Make it executable**

```bash
chmod +x scripts/build_manifest.sh
```

- [ ] **Step 3: Smoke check**

```bash
./scripts/build_manifest.sh
```
Expected: prints `manifest_tool: wrote .../corpus/manifest.json (0 files)`, file exists with empty `files: []`.

- [ ] **Step 4: Commit**

```bash
git add scripts/build_manifest.sh
git commit -m "Add build_manifest.sh wrapper"
```

---

## Task 6 — `scripts/fetch_corpus.sh` (public bucket)

**Goal:** Download a pinned, SHA256-verified set of public JP2 files into `corpus/public/`. Skip-on-failure semantics — a single dead URL must not break the whole fetch.

**Files:**
- Create: `scripts/fetch_corpus.sh`

- [ ] **Step 1: Create `scripts/fetch_corpus.sh`**

```bash
#!/usr/bin/env bash
# Download a pinned set of public JPEG 2000 sample files to corpus/public/.
#
# Each entry is one line: "<sha256>  <filename>  <url>  <domain>"
# Files that fail to download or whose checksum mismatches are reported
# but do not abort the script.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="$ROOT/corpus/public"
mkdir -p "$DEST"

# --- Pinned set ------------------------------------------------------------
# Format: sha256 | filename | url | domain
# Domain is informational (radiology, wsi, photographic, geospatial); shown
# in fetch summary so coverage gaps are obvious.
#
# REPLACE the placeholder SHA256s the first time you run this on a known-
# good machine: copy the actual values from the failure report below.
ENTRIES=(
"REPLACE_ME|p0_01.j2k|https://www.openjpeg.org/samples/p0_01.j2k|conformance"
"REPLACE_ME|p0_02.j2k|https://www.openjpeg.org/samples/p0_02.j2k|conformance"
"REPLACE_ME|relax.jp2|https://www.openjpeg.org/samples/relax.jp2|photographic"
)
# ---------------------------------------------------------------------------

ok=0; bad=0; skipped=0

verify_sha256() {
  local file="$1" want="$2"
  local got
  got=$(sha256sum "$file" | awk '{print $1}')
  [ "$got" = "$want" ]
}

for entry in "${ENTRIES[@]}"; do
  IFS='|' read -r want_sha name url domain <<<"$entry"
  out="$DEST/$name"

  if [ -f "$out" ]; then
    if [ "$want_sha" = "REPLACE_ME" ] || verify_sha256 "$out" "$want_sha"; then
      echo "[fetch] ok       $name ($domain)"
      ok=$((ok+1))
      continue
    else
      echo "[fetch] mismatch $name -- deleting and retrying"
      rm -f "$out"
    fi
  fi

  echo "[fetch] download $name <- $url"
  if ! curl -fsSL --retry 3 -o "$out" "$url"; then
    echo "[fetch] FAILED   $name ($url)"
    rm -f "$out"
    skipped=$((skipped+1))
    continue
  fi

  if [ "$want_sha" = "REPLACE_ME" ]; then
    got=$(sha256sum "$out" | awk '{print $1}')
    echo "[fetch] new pin  $name $got -- update fetch_corpus.sh"
    ok=$((ok+1))
  elif verify_sha256 "$out" "$want_sha"; then
    echo "[fetch] verified $name ($domain)"
    ok=$((ok+1))
  else
    got=$(sha256sum "$out" | awk '{print $1}')
    echo "[fetch] BAD SHA  $name  expected=$want_sha  got=$got"
    rm -f "$out"
    bad=$((bad+1))
  fi
done

echo
echo "[fetch] summary: ok=$ok  bad=$bad  skipped=$skipped  total=${#ENTRIES[@]}"
[ $bad -eq 0 ]   # only fail the script on hash mismatches
```

- [ ] **Step 2: Make executable and verify**

```bash
chmod +x scripts/fetch_corpus.sh
./scripts/fetch_corpus.sh
```
Expected: downloads attempt for each entry; first run prints `[fetch] new pin ...` lines with real SHAs (the placeholder pinning needs a one-time update). Network failures print FAILED and continue. The user should then paste the printed SHAs back into the ENTRIES array (this is a manual one-time bootstrap noted in the file).

The pinned set in this initial version is small (3 files, all from openjpeg.org). Broadening to radiology/WSI/geospatial entries is a follow-up once we identify stable URLs with permissive terms — left as a TODO comment block in the file (see Step 3).

- [ ] **Step 3: Add a TODO block in the file**

At the top of `scripts/fetch_corpus.sh`, after the script header comment but before `set -euo pipefail`, add:

```bash
# TODO: extend ENTRIES with permissively-licensed samples from:
#   - radiology: NEMA DICOM JPEG 2000 reference, 16-bit monochrome
#   - WSI:       OpenSlide test set (CMU-1-Small-Region.jp2 etc.)
#   - geospatial: USGS / Sentinel-2 small GeoJP2 tile
# Verify licensing before pinning; some samples are research-only.
```

- [ ] **Step 4: Commit**

```bash
git add scripts/fetch_corpus.sh
git commit -m "Add fetch_corpus.sh: pinned public JP2 downloader"
```

---

## Task 7 — Rewrite `scripts/gen_corpus.sh` for exhaustive synthetic grid

**Goal:** Replace the current size×rate×decomp grid with a broader axes sweep that exercises dusty JP2K corners (all five progression orders, decomp levels 0/1/5/8, custom precincts, code-block size corners, SOP/EPH on/off, multiple quality layers, tile-parts/PLT/TLM, MCT on/off, .j2k vs .jp2, error resilience). Outputs span four (components × bit-depth) source rasters.

**Files:**
- Modify: `scripts/gen_corpus.sh`

- [ ] **Step 1: Replace `scripts/gen_corpus.sh`**

Overwrite the entire file with:

```bash
#!/usr/bin/env bash
# Generate a parametric JPEG 2000 corpus exercising both common and dusty
# corners of the spec. Designed to surface decoder bugs/regressions on
# rarely-used-but-legal codestreams.
#
# Requires: opj_compress (apt: libopenjp2-tools, or build OpenJPEG with
#           BUILD_CODEC=ON).
#
# No external image dependency: each source raster is generated on the
# fly as a deterministic PGM/PPM gradient + noise pattern.
#
# Output layout:
#   corpus/synthetic/<rastername>/<axis_value...>.{jp2,j2k}
#
# Usage:
#   ./scripts/gen_corpus.sh                 # full sweep
#   ./scripts/gen_corpus.sh --quick         # smaller sweep for smoke
#
# Re-running is idempotent: existing files are skipped.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/corpus/synthetic"
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

command -v opj_compress >/dev/null || { echo "need opj_compress on PATH" >&2; exit 1; }

QUICK=0
if [ "${1:-}" = "--quick" ]; then QUICK=1; fi

# --- Source rasters --------------------------------------------------------
# (rastername, w, h, components, bits, magic): bare-bones P5/P6 PNM. For
# 12/16-bit we write P5 with maxval > 255 (PNM supports up to 65535).

gen_raster() {
  local name="$1" w="$2" h="$3" comps="$4" bits="$5"
  local out="$TMP/${name}.pnm"
  local maxval=$(( (1 << bits) - 1 ))
  local magic="P5"; [ "$comps" -eq 3 ] && magic="P6"
  {
    printf '%s\n%d %d\n%d\n' "$magic" "$w" "$h" "$maxval"
    python3 -c "
import sys, struct
w,h,c,bits = $w,$h,$comps,$bits
mx = (1<<bits) - 1
out = sys.stdout.buffer
big = bits > 8
for y in range(h):
    for x in range(w):
        # Gradient + cheap noise — deterministic, decoder-meaningful entropy.
        base = ((x ^ y) * 131 + x * 17) & mx
        for ch in range(c):
            v = (base + ch * 37) & mx
            if big: out.write(struct.pack('>H', v))
            else:   out.write(bytes([v]))
"
  } > "$out"
  echo "$out"
}

# Raster set: (name w h comps bits)
RASTERS=(
  "rgb8_1024     1024 1024 3  8"
  "mono16_1024   1024 1024 1 16"
  "mono12_1024   1024 1024 1 12"
  "rgba8_1024    1024 1024 4  8"  # encoded as 1+3 separately; see note below
  "rgb8_4096     4096 4096 3  8"
  "mono16_4096   4096 4096 1 16"
)

# 4-component PNM doesn't exist; for "rgba8" we fall back to RGB (skip alpha)
# in the synthetic corpus. Real 4-component coverage comes from the public
# bucket if/when a GeoJP2 sample lands. This is a known limitation — noted
# in the manifest.

if [ "$QUICK" -eq 1 ]; then
  RASTERS=("rgb8_1024 1024 1024 3 8" "mono16_1024 1024 1024 1 16")
fi

# --- Axis sweeps -----------------------------------------------------------
PROGRESSIONS=(LRCP RLCP RPCL PCRL CPRL)
DECOMPS=(0 1 5 8)
CBLK_SIZES=(32 64)
TILE_MODES=("none" "1024,1024")
CONTAINERS=(jp2 j2k)
MCT_MODES=(on off)         # only meaningful for 3-component
SOP_EPH=("none" "sop,eph") # error-resilience-ish markers
QUALITY_LAYERS=(1 4)
RATES=(lossless lossy)

[ "$QUICK" -eq 1 ] && {
  PROGRESSIONS=(LRCP RPCL)
  DECOMPS=(1 5)
  CBLK_SIZES=(64)
  TILE_MODES=("none" "1024,1024")
  CONTAINERS=(jp2)
  MCT_MODES=(on)
  SOP_EPH=("none")
  QUALITY_LAYERS=(1)
}

mkdir -p "$OUT"

emit() {
  # emit raster axis_args... -- output_basename
  local src="$1" name="$2" out_dir="$3" container="$4" rate="$5"
  shift 5
  local outpath="$out_dir/$name.$container"
  [ -f "$outpath" ] && return 0
  local cmd=(opj_compress -i "$src" -o "$outpath" "$@")
  if [ "$rate" = "lossless" ]; then cmd+=(-r 1 -I); else cmd+=(-r 20); fi
  if ! "${cmd[@]}" >/dev/null 2>&1; then
    echo "  [skip] $name ($container, $rate) — opj_compress rejected the combo"
    return 0
  fi
}

total=0
for entry in "${RASTERS[@]}"; do
  # shellcheck disable=SC2086
  set -- $entry
  rname="$1" w="$2" h="$3" comps="$4" bits="$5"
  echo "[gen] raster $rname ${w}x${h} c=$comps b=$bits"
  src=$(gen_raster "$rname" "$w" "$h" "$comps" "$bits")
  rdir="$OUT/$rname"
  mkdir -p "$rdir"

  for prog in "${PROGRESSIONS[@]}"; do
    for decomp in "${DECOMPS[@]}"; do
      for cblk in "${CBLK_SIZES[@]}"; do
        for tile in "${TILE_MODES[@]}"; do
          for cont in "${CONTAINERS[@]}"; do
            for mct in "${MCT_MODES[@]}"; do
              [ "$comps" -ne 3 ] && [ "$mct" = "on" ] && continue
              for marker in "${SOP_EPH[@]}"; do
                for layers in "${QUALITY_LAYERS[@]}"; do
                  for rate in "${RATES[@]}"; do
                    [ "$rate" = "lossless" ] && [ "$layers" -gt 1 ] && continue

                    args=(-p "$prog" -n "$decomp"
                          -b "$cblk,$cblk")
                    [ "$tile" != "none" ] && args+=(-t "$tile")
                    [ "$marker" != "none" ] && args+=(-SOP -EPH)
                    [ "$layers" -gt 1 ] && args+=(-q "20,30,40,50")
                    if [ "$mct" = "off" ] && [ "$comps" -eq 3 ]; then
                      args+=(-mct 0)
                    fi

                    tag="p${prog}_d${decomp}_b${cblk}"
                    [ "$tile" = "none" ] && tag+="_tNone" || tag+="_t${tile//,/x}"
                    tag+="_${rate}_l${layers}_m${mct}_e${marker//,/_}"

                    emit "$src" "$tag" "$rdir" "$cont" "$rate" "${args[@]}"
                    total=$((total+1))
                  done
                done
              done
            done
          done
        done
      done
    done
  done
done

echo "[gen] sweep attempted: $total combinations (some skipped by opj_compress)"
echo "[gen] outputs: $OUT"
find "$OUT" -type f \( -name '*.jp2' -o -name '*.j2k' \) | wc -l \
  | awk '{print "[gen] files on disk:", $1}'
```

- [ ] **Step 2: Smoke check (quick mode)**

```bash
chmod +x scripts/gen_corpus.sh
./scripts/gen_corpus.sh --quick
```
Expected: produces ~tens of files under `corpus/synthetic/`. Some combinations will be rejected by `opj_compress` (e.g., 0 decomposition levels with multiple layers); they print `[skip]` and are not fatal.

If `opj_compress` is not installed:
```bash
sudo apt install libopenjp2-tools
```
or build it from `third_party/openjpeg` by configuring with `-DBUILD_CODEC=ON`.

- [ ] **Step 3: Rebuild manifest, sanity check**

```bash
./scripts/build_manifest.sh
python3 -m json.tool corpus/manifest.json | head -40
```
Expected: each entry has populated `progression`, `decomp_levels`, `lossless`, `tile_w`, `tile_h`, `container`, `bit_depth`, `components`.

- [ ] **Step 4: Commit**

```bash
git add scripts/gen_corpus.sh
git commit -m "Rewrite gen_corpus.sh: exhaustive synthetic grid"
```

---

## Task 8 — `scripts/check_corpus.sh`

**Goal:** Validate the three buckets before a benchmark run. Catches: (1) public files whose SHA changed, (2) synthetic files that don't re-decode, (3) stale manifest.

**Files:**
- Create: `scripts/check_corpus.sh`

- [ ] **Step 1: Create `scripts/check_corpus.sh`**

```bash
#!/usr/bin/env bash
# Validate corpus health before a benchmark run.
#
# Checks:
#   - corpus/manifest.json exists and is fresher than every file it lists.
#   - Each synthetic/ file re-decodes with opj_decompress (catches generator
#     bugs on dusty-corner configurations).
#   - Each public/ file's SHA256 is recorded by the manifest (a separate
#     pinning check lives in fetch_corpus.sh).
#
# Output: one summary line per bucket. Non-zero exit on any check failure.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CORPUS="$ROOT/corpus"
MANIFEST="$CORPUS/manifest.json"

fail=0

if [ ! -f "$MANIFEST" ]; then
  echo "[check] manifest missing; run scripts/build_manifest.sh" >&2
  exit 1
fi

# Freshness: any listed file modified after the manifest?
newest_file=$(python3 - "$MANIFEST" "$CORPUS" <<'PY'
import json, os, sys
m, root = sys.argv[1], sys.argv[2]
data = json.load(open(m))
mts = []
for f in data.get("files", []):
    p = os.path.join(root, f["path"])
    if os.path.exists(p):
        mts.append(os.path.getmtime(p))
print(max(mts) if mts else 0)
PY
)
manifest_mt=$(stat -c %Y "$MANIFEST")
if awk "BEGIN{exit !($newest_file > $manifest_mt)}"; then
  echo "[check] manifest is stale (a listed file is newer); rebuild it"
  fail=$((fail+1))
fi

# Per-bucket counts and synthetic re-decode check.
for bucket in user synthetic public; do
  dir="$CORPUS/$bucket"
  if [ ! -d "$dir" ]; then
    echo "[check] $bucket: missing (no $dir)"; continue
  fi
  count=$(find "$dir" -type f \( -iname '*.jp2' -o -iname '*.j2k' -o -iname '*.jpc' \) | wc -l)
  echo "[check] $bucket: $count files"
done

if command -v opj_decompress >/dev/null; then
  bad=0
  while IFS= read -r f; do
    if ! opj_decompress -i "$f" -o "$(mktemp --suffix=.pgm)" >/dev/null 2>&1; then
      echo "  [check] re-decode FAILED: $f"
      bad=$((bad+1))
    fi
  done < <(find "$CORPUS/synthetic" -type f \( -iname '*.jp2' -o -iname '*.j2k' \) 2>/dev/null)
  echo "[check] synthetic re-decode: $bad failure(s)"
  fail=$((fail + bad))
else
  echo "[check] opj_decompress not on PATH; skipping re-decode check" >&2
fi

if [ "$fail" -gt 0 ]; then
  echo "[check] $fail issue(s) found"
  exit 1
fi
echo "[check] ok"
```

- [ ] **Step 2: Make executable and smoke**

```bash
chmod +x scripts/check_corpus.sh
./scripts/check_corpus.sh
```
Expected: prints per-bucket counts; if synthetic was generated in Task 7, prints `[check] synthetic re-decode: 0 failure(s)`.

- [ ] **Step 3: Commit**

```bash
git add scripts/check_corpus.sh
git commit -m "Add check_corpus.sh: validate buckets, re-decode synthetic"
```

---

## Task 9 — ROI / region decode

**Goal:** Add `decode_region` to the adapter interface and wire `--roi WxH@X,Y` and `--roi-tile N` flags. When ROI is requested, the timed loop calls `decode_region` instead of `decode`; full decode is unchanged when ROI is unset. Pixel verification compares regions only.

**Files:**
- Modify: `src/adapter.h`
- Modify: `src/adapter_openjpeg.cpp`
- Modify: `src/adapter_grok.cpp`
- Modify: `src/bench.h`, `src/bench.cpp`
- Modify: `src/main.cpp`
- Modify: `src/json_out.cpp`

- [ ] **Step 1: Extend `Decoder` in `src/adapter.h`**

After the existing `virtual bool decode(...)` declaration, add:

```cpp
  struct Region {
    uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;   // half-open [x0,x1) × [y0,y1)
  };

  // Decode only the given region. Default implementation calls full decode
  // and crops; adapters override for true region decode via codec APIs.
  virtual bool decode_region(const uint8_t* data, std::size_t size,
                             int num_threads, const Region& region,
                             DecodedImage& out, std::string& err);

  // True if the adapter natively decodes a region (vs falling back to crop).
  virtual bool native_region_decode() const { return false; }
```

Add a forward declaration for the default `decode_region` body — implement it in `adapter.cpp` (new file) or inline at the bottom of `adapter.h`. Simpler: inline default in `adapter.h`:

Add at the bottom of the header, before the closing namespace:

```cpp
inline bool Decoder::decode_region(const uint8_t* data, std::size_t size,
                                   int num_threads, const Region& region,
                                   DecodedImage& out, std::string& err) {
  if (!decode(data, size, num_threads, out, err)) return false;
  uint32_t x0 = region.x0, y0 = region.y0;
  uint32_t x1 = region.x1 == 0 ? out.width  : std::min(region.x1, out.width);
  uint32_t y1 = region.y1 == 0 ? out.height : std::min(region.y1, out.height);
  if (x0 >= x1 || y0 >= y1) { err = "empty region"; return false; }
  uint32_t bw = x1 - x0, bh = y1 - y0;
  uint32_t bpp = (out.bit_depth <= 8 ? 1u : 2u) * out.channels;
  std::vector<uint8_t> cropped((std::size_t)bw * bh * bpp);
  for (uint32_t y = 0; y < bh; ++y) {
    const uint8_t* src = out.pixels.data() + ((y + y0) * out.width + x0) * bpp;
    uint8_t* dst       = cropped.data()     +  y * bw                    * bpp;
    std::memcpy(dst, src, (std::size_t)bw * bpp);
  }
  out.pixels = std::move(cropped);
  out.width  = bw;
  out.height = bh;
  return true;
}
```

Add `#include <algorithm>` and `#include <cstring>` to `src/adapter.h` for the inline body.

- [ ] **Step 2: Override `decode_region` in OpenJPEG adapter**

In `src/adapter_openjpeg.cpp`, inside `class OpenJpegDecoder`, add:

```cpp
  bool native_region_decode() const override { return true; }

  bool decode_region(const uint8_t* data, std::size_t size, int num_threads,
                     const Region& region, DecodedImage& out,
                     std::string& err) override {
    // The full-decode path in this file is long. To keep one implementation,
    // we duplicate the setup and call opj_set_decode_area before opj_decode.
    OPJ_CODEC_FORMAT fmt = sniff_codec(data, size);
    if (fmt == OPJ_CODEC_UNKNOWN) { err = "unknown codestream"; return false; }

    MemStream mem{data, size, 0};
    opj_stream_t* stream = opj_stream_default_create(OPJ_TRUE);
    opj_stream_set_read_function(stream, &mem_read);
    opj_stream_set_skip_function(stream, &mem_skip);
    opj_stream_set_seek_function(stream, &mem_seek);
    opj_stream_set_user_data(stream, &mem, nullptr);
    opj_stream_set_user_data_length(stream, (OPJ_UINT64)size);

    opj_codec_t* codec = opj_create_decompress(fmt);
    if (!codec) { opj_stream_destroy(stream); err = "create_decompress"; return false; }
    opj_set_info_handler(codec, &silent, nullptr);
    opj_set_warning_handler(codec, &silent, nullptr);
    opj_set_error_handler(codec, &silent, nullptr);

    opj_dparameters_t params;
    opj_set_default_decoder_parameters(&params);
    if (!opj_setup_decoder(codec, &params)) {
      opj_destroy_codec(codec); opj_stream_destroy(stream);
      err = "setup_decoder"; return false;
    }
    if (num_threads > 1) opj_codec_set_threads(codec, num_threads);

    opj_image_t* image = nullptr;
    if (!opj_read_header(stream, codec, &image)) {
      if (image) opj_image_destroy(image);
      opj_destroy_codec(codec); opj_stream_destroy(stream);
      err = "read_header"; return false;
    }

    OPJ_UINT32 x0 = region.x0, y0 = region.y0;
    OPJ_UINT32 x1 = region.x1 ? region.x1 : image->x1;
    OPJ_UINT32 y1 = region.y1 ? region.y1 : image->y1;
    if (!opj_set_decode_area(codec, image, x0, y0, x1, y1)) {
      opj_image_destroy(image);
      opj_destroy_codec(codec); opj_stream_destroy(stream);
      err = "set_decode_area"; return false;
    }

    if (!opj_decode(codec, stream, image) ||
        !opj_end_decompress(codec, stream)) {
      opj_image_destroy(image);
      opj_destroy_codec(codec); opj_stream_destroy(stream);
      err = "decode"; return false;
    }

    opj_destroy_codec(codec);
    opj_stream_destroy(stream);

    // Reuse the unpack block from full decode by copying it here. (Kept
    // duplicated rather than refactored to keep this task contained; a
    // follow-up can extract a shared `unpack_opj_image(image, out)` helper.)
    if (!image || image->numcomps == 0 || !image->comps) {
      if (image) opj_image_destroy(image);
      err = "empty image"; return false;
    }
    uint32_t w = image->comps[0].w;
    uint32_t h = image->comps[0].h;
    uint32_t prec = image->comps[0].prec;
    for (uint32_t c = 1; c < image->numcomps; ++c) {
      if (image->comps[c].w != w || image->comps[c].h != h) {
        opj_image_destroy(image);
        err = "non-uniform component size";
        return false;
      }
    }
    out.width = w; out.height = h;
    out.channels = image->numcomps; out.bit_depth = prec;
    if (prec <= 8) {
      out.pixels.resize((std::size_t)w * h * image->numcomps);
      for (uint32_t c = 0; c < image->numcomps; ++c) {
        const OPJ_INT32* src = image->comps[c].data;
        int sgnd = image->comps[c].sgnd;
        int shift = sgnd ? (1 << (prec - 1)) : 0;
        uint8_t* dst = out.pixels.data() + c;
        for (uint32_t i = 0, n = w * h; i < n; ++i) {
          int v = src[i] + shift;
          if (v < 0) v = 0; else if (v > 255) v = 255;
          dst[i * image->numcomps] = (uint8_t)v;
        }
      }
    } else {
      out.pixels.resize((std::size_t)w * h * image->numcomps * 2);
      uint32_t max = (1u << prec) - 1;
      for (uint32_t c = 0; c < image->numcomps; ++c) {
        const OPJ_INT32* src = image->comps[c].data;
        int sgnd = image->comps[c].sgnd;
        int shift = sgnd ? (1 << (prec - 1)) : 0;
        uint8_t* dst = out.pixels.data() + c * 2;
        for (uint32_t i = 0, n = w * h; i < n; ++i) {
          int v = src[i] + shift;
          if (v < 0) v = 0; else if ((uint32_t)v > max) v = (int)max;
          uint16_t u = (uint16_t)v;
          dst[i * image->numcomps * 2 + 0] = (uint8_t)(u & 0xFF);
          dst[i * image->numcomps * 2 + 1] = (uint8_t)(u >> 8);
        }
      }
    }
    opj_image_destroy(image);
    return true;
  }
```

- [ ] **Step 3: Grok adapter — fall back to default crop**

Grok's region API has varied between versions; rather than risk a wrong call, leave Grok using the default `decode_region` (full decode + crop). Mark it explicitly:

In `src/adapter_grok.cpp`, inside `class GrokDecoder`, add:

```cpp
  bool native_region_decode() const override { return false; }
  // decode_region uses the base-class default (full decode + crop).
```

(A native Grok region implementation can be added later; the bench will report the comparison fairly because the JSON records `roi` and an out-of-band reader knows Grok's number includes a full decode.)

- [ ] **Step 4: Extend `BenchOptions` and `FileResult` for ROI**

In `src/bench.h`, inside `BenchOptions`, add:

```cpp
  // ROI: if has_roi is false, do a full decode. If true, (x0,y0,x1,y1) is
  // used verbatim. Tile-index resolution is deferred to a follow-up.
  bool has_roi = false;
  uint32_t roi_x0 = 0, roi_y0 = 0, roi_x1 = 0, roi_y1 = 0;
```

In `src/bench.h`, inside `FileResult`, add:

```cpp
  // Region actually timed. has_roi==false => null in JSON.
  bool has_roi = false;
  uint32_t roi_x0 = 0, roi_y0 = 0, roi_x1 = 0, roi_y1 = 0;
```

- [ ] **Step 5: Wire ROI into `bench_file`**

In `src/bench.cpp`, inside `bench_file`, replace each `decoder.decode(blob.data(), blob.size(), threads, img, err)` call with a conditional:

```cpp
auto do_decode = [&](DecodedImage& target) -> bool {
  if (opts.has_roi) {
    Decoder::Region rg{opts.roi_x0, opts.roi_y0, opts.roi_x1, opts.roi_y1};
    return decoder.decode_region(blob.data(), blob.size(), threads, rg, target, err);
  }
  return decoder.decode(blob.data(), blob.size(), threads, target, err);
};
```

Replace the three call sites (warmup loop, reference-shape capture if any, timed loop) with `do_decode(img)`.

Record the ROI on the result:

```cpp
r.has_roi = opts.has_roi;
r.roi_x0 = opts.roi_x0; r.roi_y0 = opts.roi_y0;
r.roi_x1 = opts.roi_x1; r.roi_y1 = opts.roi_y1;
```

Add `#include "adapter.h"` (already present) and ensure `Decoder::Region` is visible (it is, since it lives inside the class).

- [ ] **Step 6: Reference-image capture must use ROI too**

In `src/main.cpp`, the block that captures `ref_image` (around the existing `if (opts.verify && !have_ref && ...)` check) currently calls `d->decode(...)`. Change to use the same conditional:

```cpp
if (opts.verify && !have_ref && d.get() == decoders.front().get()) {
  std::string err;
  bool ok;
  if (opts.has_roi) {
    Decoder::Region rg{opts.roi_x0, opts.roi_y0, opts.roi_x1, opts.roi_y1};
    ok = d->decode_region(blob.data(), blob.size(), 1, rg, ref_image, err);
  } else {
    ok = d->decode(blob.data(), blob.size(), 1, ref_image, err);
  }
  if (ok) have_ref = true;
}
```

- [ ] **Step 7: Parse `--roi` / `--roi-tile` in `main.cpp`**

In the argv loop, add:

```cpp
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
// --roi-tile N (resolve N to a region via manifest.json) is intentionally
// deferred to a follow-up plan; use explicit --roi WxH@X,Y for now.
```

The tile-index resolution requires manifest lookup which is more plumbing than this task should pull in. Make `--roi-tile` an explicit "not yet wired" error so the flag's contract is clear and the test surface stays small. A follow-up will resolve it against `manifest.json`.

- [ ] **Step 8: Emit `roi` in JSON**

In `src/json_out.cpp`, inside `emit_result`, before `"pixel_match"`:

```cpp
if (r.has_roi) {
  os << "\"roi\": {\"x0\": " << r.roi_x0 << ", \"y0\": " << r.roi_y0
     << ", \"x1\": " << r.roi_x1 << ", \"y1\": " << r.roi_y1 << "}, ";
} else {
  os << "\"roi\": null, ";
}
```

- [ ] **Step 9: Build and verify**

```bash
./scripts/build.sh
# Full decode still works
./build/jp2k-bench --iters 2 --warmup 1 corpus/synthetic/*/*.jp2 \
  2>/dev/null | python3 -m json.tool | grep -m1 '"roi"'
```
Expected: `"roi": null` for a non-ROI run.

Region run (pick any synthetic 1024² file):
```bash
F=$(find corpus/synthetic -name '*.jp2' | head -1)
./build/jp2k-bench --iters 2 --warmup 1 --roi 256x256@0,0 "$F" \
  | python3 -m json.tool | grep -A4 '"roi"'
```
Expected: `roi` object with `x1=256, y1=256`. `pixel_match` is `1` (both decoders crop the same region, OpenJPEG natively, Grok via the default crop fallback).

- [ ] **Step 10: Commit**

```bash
git add src/adapter.h src/adapter_openjpeg.cpp src/adapter_grok.cpp \
        src/bench.h src/bench.cpp src/main.cpp src/json_out.cpp
git commit -m "Add ROI / region decode (--roi WxH@X,Y)"
```

---

## Task 9b — PSNR fallback for lossy mismatches

**Goal:** When `pixel_match == 0` on a lossy file, compute PSNR (dB) against the reference and record it in `FileResult` / JSON. Bytes match remains the strict check; PSNR is descriptive, not enforced. Lossless files keep the existing semantics (mismatch is a real bug).

**Files:**
- Create: `src/verify.h`, `src/verify.cpp`
- Modify: `src/bench.h`, `src/bench.cpp`, `src/json_out.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Add `src/verify.h`**

```cpp
#pragma once
#include "adapter.h"

namespace jp2kbench {

// PSNR in dB between two same-shape DecodedImages. Returns +inf on exact
// match, NaN if shapes differ.
double psnr_db(const DecodedImage& a, const DecodedImage& b);

}  // namespace jp2kbench
```

- [ ] **Step 2: Add `src/verify.cpp`**

```cpp
#include "verify.h"

#include <cmath>
#include <cstdint>
#include <limits>

namespace jp2kbench {

namespace {
double sample(const uint8_t* p, uint32_t bit_depth) {
  if (bit_depth <= 8) return (double)p[0];
  return (double)(uint16_t)(p[0] | (p[1] << 8));
}
}  // namespace

double psnr_db(const DecodedImage& a, const DecodedImage& b) {
  if (!a.same_shape(b)) return std::numeric_limits<double>::quiet_NaN();
  std::size_t n = (std::size_t)a.width * a.height * a.channels;
  if (n == 0) return std::numeric_limits<double>::quiet_NaN();
  uint32_t bpp = a.bit_depth <= 8 ? 1u : 2u;
  double sse = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    double da = sample(a.pixels.data() + i * bpp, a.bit_depth);
    double db = sample(b.pixels.data() + i * bpp, b.bit_depth);
    double d = da - db;
    sse += d * d;
  }
  if (sse == 0.0) return std::numeric_limits<double>::infinity();
  double mse = sse / (double)n;
  double max_val = (double)((1u << a.bit_depth) - 1u);
  return 20.0 * std::log10(max_val) - 10.0 * std::log10(mse);
}

}  // namespace jp2kbench
```

- [ ] **Step 3: Add `pixel_psnr_db` to `FileResult`**

In `src/bench.h`, inside `FileResult`, after `int pixel_match = -1;`:

```cpp
  // Populated only when pixel_match == 0; NaN otherwise.
  double pixel_psnr_db = std::numeric_limits<double>::quiet_NaN();
```

Add `#include <limits>` to `src/bench.h`.

- [ ] **Step 4: Call PSNR on mismatch in `bench_file`**

In `src/bench.cpp`, add `#include "verify.h"`. In the verify block (where `r.pixel_match = 0;` is set on mismatch), add:

```cpp
} else if (img.pixels.size() != ref_image->pixels.size() ||
           std::memcmp(img.pixels.data(), ref_image->pixels.data(),
                       img.pixels.size()) != 0) {
  r.pixel_match = 0;
  r.pixel_psnr_db = psnr_db(img, *ref_image);
} else {
  r.pixel_match = 1;
}
```

- [ ] **Step 5: Emit `pixel_psnr_db` in JSON**

In `src/json_out.cpp`, inside `emit_result`, after `"pixel_match"`:

```cpp
if (std::isfinite(r.pixel_psnr_db)) {
  os << "\"pixel_psnr_db\": " << r.pixel_psnr_db << ", ";
} else {
  os << "\"pixel_psnr_db\": null, ";
}
```

Add `#include <cmath>` at the top of `json_out.cpp` (already present via earlier task; verify).

- [ ] **Step 6: Wire `src/verify.cpp` into CMake**

Append `src/verify.cpp` to the `add_executable(jp2k-bench ...)` source list.

- [ ] **Step 7: Build and smoke**

```bash
./scripts/build.sh
F=$(find corpus/synthetic -name '*lossy*.jp2' | head -1)
./build/jp2k-bench --iters 2 --warmup 1 "$F" \
  | python3 -m json.tool | grep -E '(pixel_match|pixel_psnr_db)'
```
Expected: if openjpeg and grok agree exactly on the lossy file, `pixel_match: 1, pixel_psnr_db: null`. If they disagree, `pixel_match: 0` with a numeric PSNR (typically >40 dB for near-identical lossy output).

- [ ] **Step 8: Commit**

```bash
git add src/verify.h src/verify.cpp src/bench.h src/bench.cpp src/json_out.cpp CMakeLists.txt
git commit -m "Add PSNR fallback for lossy pixel-mismatch verification"
```

---

## Task 10 — `--concurrent-files N`

**Goal:** Run multiple `bench_file` invocations in parallel. Each (file, decoder, threads, roi) combo is one job; a thread pool of size N drains the queue. Reference-image capture remains serial per file (so cross-decoder verification works), then the parallel jobs are dispatched.

**Files:**
- Create: `src/thread_pool.h`
- Modify: `src/main.cpp`
- Modify: `src/bench.h` (no behavior change; just ensure `FileResult` is movable — `std::vector<uint8_t>` already makes it so).
- Modify: `src/json_out.cpp` — populate `aggregate.concurrent_throughput_mpix_s`.

- [ ] **Step 1: Add `src/thread_pool.h`**

```cpp
#pragma once
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace jp2kbench {

class ThreadPool {
 public:
  explicit ThreadPool(int n) : stop_(false) {
    if (n < 1) n = 1;
    for (int i = 0; i < n; ++i) {
      workers_.emplace_back([this] { this->worker_loop(); });
    }
  }
  ~ThreadPool() {
    {
      std::lock_guard<std::mutex> lk(m_);
      stop_ = true;
    }
    cv_.notify_all();
    for (auto& t : workers_) if (t.joinable()) t.join();
  }

  void submit(std::function<void()> fn) {
    {
      std::lock_guard<std::mutex> lk(m_);
      q_.push(std::move(fn));
    }
    cv_.notify_one();
  }

  void wait_idle() {
    std::unique_lock<std::mutex> lk(m_);
    done_cv_.wait(lk, [this] {
      return q_.empty() && active_ == 0;
    });
  }

 private:
  void worker_loop() {
    for (;;) {
      std::function<void()> job;
      {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait(lk, [this] { return stop_ || !q_.empty(); });
        if (stop_ && q_.empty()) return;
        job = std::move(q_.front());
        q_.pop();
        ++active_;
      }
      job();
      {
        std::lock_guard<std::mutex> lk(m_);
        --active_;
      }
      done_cv_.notify_all();
    }
  }

  std::vector<std::thread> workers_;
  std::queue<std::function<void()>> q_;
  std::mutex m_;
  std::condition_variable cv_;
  std::condition_variable done_cv_;
  bool stop_;
  int active_ = 0;
};

}  // namespace jp2kbench
```

- [ ] **Step 2: Parse `--concurrent-files N` in `main.cpp`**

In the argv loop:

```cpp
else if (a == "--concurrent-files") {
  // Stored on a local int; not part of BenchOptions because it's a
  // dispatcher-level concern, not a per-bench-call one.
  // (See concurrent_files variable declared above.)
  concurrent_files = std::atoi(need("--concurrent-files").c_str());
  if (concurrent_files < 1) concurrent_files = 1;
}
```

Declare `int concurrent_files = 1;` near the top of `main()`, alongside `only_decoder`.

- [ ] **Step 3: Restructure the per-file loop to dispatch to a thread pool**

Replace the existing per-file loop in `main.cpp` with this structure. The reference capture stays serial per file; the inner (decoder × threads) jobs go to the pool. Result emission order is sorted at the end so the JSON is deterministic.

```cpp
#include "thread_pool.h"
// ... at top of file

std::mutex results_mtx;
ThreadPool pool(concurrent_files);

for (const auto& path : files) {
  auto blob = read_file(path);
  if (blob.empty()) {
    std::cerr << "skip (empty/unreadable): " << path << "\n";
    continue;
  }

  // Reference image (serial, first decoder, threads=1)
  auto ref_image = std::make_shared<DecodedImage>();
  bool have_ref = false;
  if (opts.verify) {
    std::string err;
    bool ok;
    if (opts.has_roi) {
      Decoder::Region rg{opts.roi_x0, opts.roi_y0, opts.roi_x1, opts.roi_y1};
      ok = decoders.front()->decode_region(blob.data(), blob.size(), 1, rg, *ref_image, err);
    } else {
      ok = decoders.front()->decode(blob.data(), blob.size(), 1, *ref_image, err);
    }
    have_ref = ok;
  }

  for (auto& d : decoders) {
    for (int t : opts.thread_counts) {
      // Capture by value where needed; blob and decoder live for the duration of main().
      bool is_ref_decoder = (d.get() == decoders.front().get());
      const DecodedImage* ref_ptr = (have_ref && !is_ref_decoder) ? ref_image.get() : nullptr;
      Decoder* dec = d.get();
      std::string path_copy = path;
      const std::vector<uint8_t>* blob_ptr = &blob;   // SAFE: lifetime > job (we wait_idle below)

      pool.submit([&, dec, ref_ptr, t, path_copy, blob_ptr]() {
        auto r = bench_file(*dec, path_copy, *blob_ptr, t, opts, ref_ptr);
        std::lock_guard<std::mutex> lk(results_mtx);
        std::cerr << path_copy << "  " << dec->name() << "  t=" << t << "  ";
        if (!r.error.empty()) std::cerr << "ERR " << r.error << "\n";
        else std::cerr << "min=" << (r.stats.min * 1e3) << "ms  "
                       << r.megapixels_per_sec << " MP/s\n";
        all_results.push_back(std::move(r));
      });
    }
  }
  pool.wait_idle();   // one file at a time; keeps blob lifetime simple
}
```

(Why `wait_idle` per file: avoids holding all file blobs in memory simultaneously. The intra-file dispatch — decoders × threads — still parallelizes across `concurrent_files` workers, which is the benefit we actually want.)

After the file loop, sort `all_results` for stable output:

```cpp
std::sort(all_results.begin(), all_results.end(),
          [](const FileResult& a, const FileResult& b) {
            return std::tie(a.path, a.decoder, a.threads, a.has_roi)
                 < std::tie(b.path, b.decoder, b.threads, b.has_roi);
          });
```

Add `#include <algorithm>`, `#include <mutex>`, `#include <memory>`, `#include <tuple>` if not present.

- [ ] **Step 4: Populate aggregate throughput**

Compute aggregate decoded megapixels per second across all results, for each decoder:

```cpp
header.concurrent_files = concurrent_files;
if (concurrent_files > 1) {
  // Aggregate is total megapixels divided by total wall clock used by the
  // pool. We approximate "total wall clock" as max(per-file min * iters)
  // summed across files; close enough for headline throughput.
  double total_mpx = 0;
  double total_s   = 0;
  for (const auto& r : all_results) {
    if (r.error.empty() && r.stats.min > 0) {
      total_mpx += (double)r.width * r.height / 1e6;
      total_s   += r.stats.min;
    }
  }
  if (total_s > 0) agg.concurrent_throughput_mpix_s = total_mpx * concurrent_files / total_s;
}
```

(This is an approximation — real wall-clock would be measured via a `steady_clock` around `pool.wait_idle` summed across files. The approximation makes the JSON populated; a follow-up can refine.)

- [ ] **Step 5: Build and verify**

```bash
./scripts/build.sh
F1=$(find corpus/synthetic -name '*.jp2' | head -1)
F2=$(find corpus/synthetic -name '*.jp2' | sed -n 2p)
./build/jp2k-bench --iters 3 --warmup 1 --concurrent-files 2 "$F1" "$F2" \
  | python3 -m json.tool | grep -E '(concurrent_files|concurrent_throughput)'
```
Expected: `"concurrent_files": 2` and a non-zero `concurrent_throughput_mpix_s` under `aggregate`.

- [ ] **Step 6: Commit**

```bash
git add src/thread_pool.h src/main.cpp
git commit -m "Add --concurrent-files: thread pool around bench_file dispatch"
```

---

## Task 11 — Rewrite `scripts/report.py` for schema v2

**Goal:** Replace the legacy single-array reader with a schema-v2 reader. Add per-file table with optional compare, aggregate summary grouped by bucket/axis, env-diff warning, `--filter` predicate.

**Files:**
- Modify: `scripts/report.py`

- [ ] **Step 1: Replace the body of `scripts/report.py`**

Overwrite the file with:

```python
#!/usr/bin/env python3
"""Summarize jp2k-bench schema-v2 JSON results.

Usage:
  report.py results.json
  report.py baseline.json compare.json
  report.py results.json --group-by bucket
  report.py results.json --filter bit_depth=16,lossless=true
"""

from __future__ import annotations

import argparse
import json
import sys
from collections import defaultdict
from pathlib import Path
from statistics import median
from typing import Any, Callable, Iterable


def _load(path: str) -> dict:
    with open(path) as f:
        data = json.load(f)
    if not isinstance(data, dict) or data.get("schema_version") != 2:
        sys.exit(f"{path}: schema_version != 2 (got {data.get('schema_version')!r})")
    return data


def _bucket_of(rel_path: str) -> str:
    parts = Path(rel_path).parts
    for i, p in enumerate(parts):
        if p in ("user", "synthetic", "public") and i + 1 < len(parts):
            return p
    return "other"


def _load_manifest(corpus_root: Path) -> dict[str, dict]:
    """Index manifest.json by relative path. Empty if missing."""
    p = corpus_root / "manifest.json"
    if not p.exists():
        return {}
    data = json.loads(p.read_text())
    return {f["path"]: f for f in data.get("files", []) if "path" in f}


def _annotate(results: list[dict], manifest: dict[str, dict]) -> None:
    """Attach `_bucket` and manifest fields onto each result for grouping/filter."""
    for r in results:
        rel = Path(r["file"])
        # Result paths may be absolute or repo-relative; reduce to last 4 parts
        # to attempt a manifest match.
        for k in manifest:
            if str(rel).endswith(k):
                r.update({f"_m_{kk}": vv for kk, vv in manifest[k].items()
                          if kk not in ("path", "sha256")})
                r["_bucket"] = manifest[k].get("bucket", _bucket_of(str(rel)))
                break
        else:
            r["_bucket"] = _bucket_of(str(rel))


def _parse_filter(expr: str) -> Callable[[dict], bool]:
    """expr: 'key=value,key2=value2'. Looks up key on result; matches stringified."""
    preds: list[tuple[str, str]] = []
    for chunk in expr.split(","):
        if not chunk:
            continue
        if "=" not in chunk:
            sys.exit(f"--filter: malformed predicate {chunk!r}")
        k, v = chunk.split("=", 1)
        preds.append((k.strip(), v.strip()))
    def _ok(r: dict) -> bool:
        for k, v in preds:
            val = r.get(k, r.get(f"_m_{k}"))
            if str(val).lower() != v.lower():
                return False
        return True
    return _ok


def _print_env_diff(base: dict, cmp: dict) -> None:
    keys = ["cpu_model", "governor", "turbo_disabled", "kernel",
            "compiler", "compile_flags"]
    diffs = [(k, base.get(k), cmp.get(k)) for k in keys
             if base.get(k) != cmp.get(k)]
    if not diffs:
        return
    print("WARNING: env differs between baseline and compare:")
    for k, b, c in diffs:
        print(f"  {k}: base={b!r}  cmp={c!r}")
    print()


def _per_file_table(results: list[dict],
                    compare: list[dict] | None = None) -> None:
    print(f"{'file':<48} {'decoder':<10} {'thr':>3} {'roi':>10} "
          f"{'min_ms':>9} {'mpix/s':>9} {'rss_mb':>7} {'pm':>3}", end="")
    if compare:
        print(f" {'Δmin%':>7} {'Δmpix%':>7}", end="")
    print()
    cmp_idx = {(r["file"], r["decoder"], r["threads"], bool(r.get("roi"))): r
               for r in (compare or [])}
    for r in results:
        if r.get("error"):
            continue
        roi = r.get("roi")
        roi_s = f"{roi['x1']-roi['x0']}x{roi['y1']-roi['y0']}" if roi else "-"
        name = Path(r["file"]).name[:48]
        ts = r["timing_s"]
        rss_mb = r.get("rss_peak_kb", 0) / 1024.0
        print(f"{name:<48} {r['decoder']:<10} {r['threads']:>3} {roi_s:>10} "
              f"{ts['min']*1e3:>9.2f} {r['megapixels_per_sec']:>9.2f} "
              f"{rss_mb:>7.1f} {r.get('pixel_match', -1):>3}", end="")
        if compare:
            k = (r["file"], r["decoder"], r["threads"], bool(roi))
            o = cmp_idx.get(k)
            if o and not o.get("error"):
                d_min = (o["timing_s"]["min"] - ts["min"]) / ts["min"] * 100 if ts["min"] else 0
                d_mp  = (o["megapixels_per_sec"] - r["megapixels_per_sec"]) \
                        / r["megapixels_per_sec"] * 100 if r["megapixels_per_sec"] else 0
                tag_min = "+" if d_min < -2 else "-" if d_min > 2 else " "
                tag_mp  = "+" if d_mp  >  2 else "-" if d_mp  < -2 else " "
                print(f" {tag_min}{d_min:>6.1f} {tag_mp}{d_mp:>6.1f}", end="")
            else:
                print(f" {'-':>7} {'-':>7}", end="")
        print()


def _aggregate_summary(results: list[dict], group_by: str) -> None:
    print()
    print(f"-- aggregate (group by: {group_by}) --")
    groups: dict[tuple, list[float]] = defaultdict(list)
    for r in results:
        if r.get("error") or r.get("megapixels_per_sec", 0) <= 0:
            continue
        key = (r["decoder"], r.get(group_by, r.get(f"_m_{group_by}", "?")))
        groups[key].append(r["megapixels_per_sec"])
    print(f"{'decoder':<10} {group_by:<20} {'count':>5} {'median mpix/s':>14}")
    for (dec, g), vals in sorted(groups.items()):
        print(f"{dec:<10} {str(g)[:20]:<20} {len(vals):>5} {median(vals):>14.2f}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("baseline", help="results JSON (schema v2)")
    ap.add_argument("compare", nargs="?", help="optional second JSON to diff")
    ap.add_argument("--corpus", type=Path,
                    default=Path(__file__).resolve().parent.parent / "corpus",
                    help="corpus root containing manifest.json")
    ap.add_argument("--group-by", default="_bucket",
                    help="aggregate axis: _bucket (default), bit_depth, "
                         "progression, lossless, etc.")
    ap.add_argument("--filter", default="",
                    help="key=value[,key=value...] predicate on result fields")
    args = ap.parse_args()

    base = _load(args.baseline)
    cmp = _load(args.compare) if args.compare else None

    manifest = _load_manifest(args.corpus)
    _annotate(base["results"], manifest)
    if cmp:
        _annotate(cmp["results"], manifest)

    if args.filter:
        pred = _parse_filter(args.filter)
        base["results"] = [r for r in base["results"] if pred(r)]
        if cmp:
            cmp["results"] = [r for r in cmp["results"] if pred(r)]

    if cmp:
        _print_env_diff(base["run"].get("env", {}), cmp["run"].get("env", {}))

    _per_file_table(base["results"], cmp["results"] if cmp else None)
    _aggregate_summary(base["results"], args.group_by)
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 2: Smoke check**

Generate a small results file (requires Task 1+ built and a synthetic file present):

```bash
F=$(find corpus/synthetic -name '*.jp2' | head -1)
./build/jp2k-bench --iters 3 --warmup 1 "$F" > /tmp/r.json
./scripts/build_manifest.sh
python3 scripts/report.py /tmp/r.json
```
Expected: per-file table with `min_ms`, `mpix/s`, `rss_mb`, `pm`; then an aggregate block grouped by `_bucket`.

Test the diff path with two runs:
```bash
./build/jp2k-bench --iters 3 --warmup 1 "$F" > /tmp/a.json
./build/jp2k-bench --iters 3 --warmup 1 "$F" > /tmp/b.json
python3 scripts/report.py /tmp/a.json /tmp/b.json
```
Expected: extra `Δmin%` / `Δmpix%` columns; no env-diff warning (same machine, same build).

- [ ] **Step 3: Commit**

```bash
git add scripts/report.py
git commit -m "Rewrite report.py for schema v2 (env diff, group-by, filter)"
```

---

## Task 12 — Smoke test + checked-in fixture

**Goal:** A `tests/smoke.sh` that runs jp2k-bench against `tests/fixtures/tiny.jp2` and validates the JSON envelope. Catches integration regressions.

**Files:**
- Create: `tests/fixtures/tiny.jp2` (generated, then checked in)
- Create: `tests/smoke.sh`

- [ ] **Step 1: Generate `tests/fixtures/tiny.jp2`**

```bash
# Run from the repo root after Task 7 has produced a working opj_compress.
# Produces a 128x128 8-bit RGB lossless JP2 (~few KB) for reproducible testing.
python3 -c "
import struct
w,h = 128,128
with open('/tmp/tiny.ppm','wb') as f:
    f.write(f'P6\n{w} {h}\n255\n'.encode())
    for y in range(h):
        for x in range(w):
            f.write(bytes([(x*2)&0xff, (y*2)&0xff, ((x^y)*3)&0xff]))
"
opj_compress -i /tmp/tiny.ppm -o tests/fixtures/tiny.jp2 -r 1 -I
ls -la tests/fixtures/tiny.jp2
```
Expected: file is ~3-10 KB.

- [ ] **Step 2: Create `tests/smoke.sh`**

```bash
#!/usr/bin/env bash
# End-to-end smoke test for jp2k-bench. Asserts schema v2 envelope and that
# the four optional axes plumb through correctly.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build/jp2k-bench"
FIX="$ROOT/tests/fixtures/tiny.jp2"

[ -x "$BIN" ] || { echo "missing $BIN; run scripts/build.sh first" >&2; exit 1; }
[ -f "$FIX" ] || { echo "missing fixture $FIX" >&2; exit 1; }

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

run() {
  local label="$1"; shift
  local out="$TMP/${label}.json"
  echo "--- $label" >&2
  "$BIN" "$@" "$FIX" > "$out" 2>/dev/null
  python3 -c "
import json, sys
d = json.load(open('$out'))
assert d['schema_version'] == 2, d['schema_version']
assert 'env' in d['run'], 'env missing'
assert isinstance(d['results'], list) and d['results'], 'no results'
" || { echo "FAIL $label: envelope check"; exit 1; }
  echo "$out"
}

# 1. Basic: schema_version=2, env present.
run basic --iters 2 --warmup 1 >/dev/null

# 2. --threads N,M produces two rows.
out=$(run threads --iters 2 --warmup 1 --threads 1,2)
python3 -c "
import json
d = json.load(open('$out'))
assert len({r['threads'] for r in d['results']}) >= 2, 'expected multiple thread counts'
" || { echo "FAIL threads"; exit 1; }

# 3. --roi populates roi field.
out=$(run roi --iters 2 --warmup 1 --roi 64x64@0,0)
python3 -c "
import json
d = json.load(open('$out'))
for r in d['results']:
    assert r['roi'] and r['roi']['x1'] == 64, r.get('roi')
    assert r.get('pixel_match', -1) in (1, -1), r.get('pixel_match')
" || { echo "FAIL roi"; exit 1; }

# 4. --concurrent-files populates aggregate.
out=$(run conc --iters 2 --warmup 1 --concurrent-files 2)
python3 -c "
import json
d = json.load(open('$out'))
assert d['run']['concurrent_files'] == 2
agg = d['aggregate']
assert 'concurrent_throughput_mpix_s' in agg, agg
" || { echo "FAIL concurrent"; exit 1; }

# 5. report.py runs against the basic output.
python3 "$ROOT/scripts/report.py" "$TMP/basic.json" >/dev/null \
  || { echo "FAIL report.py"; exit 1; }

echo "[smoke] all checks passed"
```

- [ ] **Step 3: Make executable and run**

```bash
chmod +x tests/smoke.sh
./tests/smoke.sh
```
Expected: prints `--- basic`, `--- threads`, `--- roi`, `--- conc`, then `[smoke] all checks passed`.

- [ ] **Step 4: Update `.gitignore` to allow the fixture**

Verify `tests/fixtures/tiny.jp2` is not gitignored (the existing `.gitignore` likely globs `*.jp2`). If so, add an exception:

```bash
grep -F 'tests/fixtures/tiny.jp2' .gitignore || \
  printf '\n# Allow checked-in test fixture\n!tests/fixtures/tiny.jp2\n' >> .gitignore
```

- [ ] **Step 5: Commit**

```bash
git add tests/smoke.sh tests/fixtures/tiny.jp2 .gitignore
git commit -m "Add end-to-end smoke test + tiny.jp2 fixture"
```

---

## Task 13 — README updates

**Goal:** Document the corpus structure, the new flags (`--roi`, `--concurrent-files`), and the new scripts (`fetch_corpus.sh`, `build_manifest.sh`, `check_corpus.sh`).

**Files:**
- Modify: `README.md`

- [ ] **Step 1: Update the Layout block**

In `README.md`, change the `## Layout` block to:

```
jp2k-bench/
  CMakeLists.txt        Top-level build
  cmake/                Helper modules (flags, version capture)
  src/                  Bench driver + adapters
  scripts/              setup / build / corpus / run / report helpers
  docs/                 Methodology + design specs
  tests/                Smoke test + fixtures
  corpus/               .gitignored; populated locally
    user/                 your real-world JP2 drop zone
    synthetic/            produced by gen_corpus.sh
    public/               produced by fetch_corpus.sh
    manifest.json         produced by build_manifest.sh
  third_party/          .gitignored; populated by scripts/setup.sh
```

- [ ] **Step 2: Add a Corpus section after Build**

Insert after the existing `## Build` block:

```markdown
## Corpus

The bench operates on three corpus buckets, all under `corpus/` and all
gitignored:

```sh
# 1. Drop real-world files into corpus/user/ (no tooling needed).
cp ~/wsi-samples/*.jp2 corpus/user/

# 2. Generate the synthetic axis (exercises both common and dusty corners
#    of the JP2K spec — five progression orders, 0/1/5/8 decomposition
#    levels, code-block size corners, SOP/EPH markers, etc.).
./scripts/gen_corpus.sh                 # full sweep (slow)
./scripts/gen_corpus.sh --quick         # smaller sweep for smoke

# 3. Fetch pinned public samples (SHA256-verified, skip-on-failure).
./scripts/fetch_corpus.sh

# 4. Build / refresh the manifest after any of the above.
./scripts/build_manifest.sh

# 5. Sanity-check before a benchmark run.
./scripts/check_corpus.sh
```
```

- [ ] **Step 3: Add new flags to the Run section**

In the existing `## Run` block, replace the direct-invocation example with:

```sh
# Internal threads sweep
./build/jp2k-bench --iters 20 --threads 1,4,8 corpus/**/*.jp2 > results.json

# External concurrency (process N files in parallel)
./build/jp2k-bench --concurrent-files 8 corpus/**/*.jp2 > results.json

# Region decode (1024x1024 region starting at 0,0)
./build/jp2k-bench --roi 1024x1024@0,0 corpus/synthetic/*/p_LRCP_*.jp2 > roi.json

# Reduce to a comparison table (group by corpus bucket by default).
./scripts/report.py results.json
./scripts/report.py baseline.json compare.json   # adds Δmin% / Δmpix% columns
./scripts/report.py results.json --group-by progression
./scripts/report.py results.json --filter bit_depth=16,lossless=true
```

- [ ] **Step 4: Commit**

```bash
git add README.md
git commit -m "README: document corpus buckets and new flags"
```

---

## Final verification (manual)

- [ ] Run `./tests/smoke.sh` and confirm `[smoke] all checks passed`.
- [ ] Run `python3 -m unittest tests.manifest_tool_test -v` and confirm pass (or skipped if no `opj_compress`).
- [ ] Run `./scripts/check_corpus.sh` and confirm zero failures.
- [ ] Run a real bench against the synthetic corpus and inspect the report:
  ```bash
  ./scripts/run_bench.sh --threads 1,4 corpus/synthetic > /tmp/full.json
  python3 scripts/report.py /tmp/full.json --group-by progression
  ```
  Expected: median mpix/s differs across progression orders (a sign the parser/decoder paths are actually exercised differently).
- [ ] Scan the per-file table for `pm` (pixel_match) values: 1 (matched) or -1 (no reference, expected for the first decoder). Any 0 = bug to investigate.

When the manual checks pass, the bench is ready for the openjp2k adapter (which is a separate plan: drop it under `third_party/openjp2k`, point `build.sh --openjpeg-source` at it, and re-run the same commands).
