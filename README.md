# jp2k-bench

A small, focused benchmark harness for JPEG 2000 *decoding* performance.
Built to drive a fork of OpenJPEG toward Kakadu-class decode throughput.

## Why

OpenJPEG and [Grok](https://github.com/GrokImageCompression/grok) are the two
open-source JPEG 2000 codecs that matter for production use. Grok is
substantially faster than OpenJPEG, but it is licensed AGPL, which makes it
unusable in Apache/MIT projects.

The plan: fork OpenJPEG, port the algorithmic wins that are public knowledge
(MQ decoder, T1 decode inner loops, multithreading, SIMD), and measure
progress on a shared corpus with a shared methodology. Kakadu is the
unattainable upper bound but we treat its published numbers as the target.

## Status

- OpenJPEG adapter: implemented.
- Grok adapter: implemented (compiled only if grok is found via CMake).
- Kakadu adapter: not implemented — paste published numbers into reports.
- Corpus generator: shell helper that drives `opj_compress`.

## Layout

```
jp2k-bench/
  CMakeLists.txt        Top-level build
  cmake/                Helper modules (flags, find scripts)
  src/                  Bench driver + adapters
  scripts/              setup/build/run/report helpers
  docs/                 Methodology + run-log notes
  corpus/               .gitignored; populated locally
  third_party/          .gitignored; populated by scripts/setup.sh
```

## Build

Prerequisites: a C++17 compiler, CMake 3.20+, git, and (for the corpus
generator) ImageMagick if you want to source from PNG/JPEG.

```sh
# Fetch openjpeg and grok at pinned commits into third_party/
./scripts/setup.sh

# Configure + build with -O3 -march=native -flto (Release)
./scripts/build.sh

# Sanity check
./build/jp2k-bench --list-decoders
```

## Run

```sh
# Bench every .jp2/.j2k under corpus/
./scripts/run_bench.sh corpus/ > results.json

# Or directly
./build/jp2k-bench --iters 20 --warmup 2 --threads 1,4,8 corpus/*.jp2 > results.json

# Reduce to a comparison table
./scripts/report.py results.json
```

## Methodology

See [`docs/methodology.md`](docs/methodology.md). Short version:

- File contents are preloaded into RAM; only `decode()` is timed.
- 2 warmup iterations are discarded, then N (default 20) timed iterations.
- Per-file stats: min, p50, p90, p99, mean, stddev.
- Outputs are pixel-compared between decoders for the same file; mismatches
  are surfaced rather than silently averaged.
- Decoders are built with identical flags: `-O3 -march=native -flto -DNDEBUG`,
  static linkage, LTO across adapter + library.

## Adding a fork of OpenJPEG

Drop your fork at `third_party/openjpeg-fork/`, then:

```sh
./scripts/build.sh --openjpeg-source third_party/openjpeg-fork
./build/jp2k-bench --iters 20 corpus/wsi/*.jp2 > fork.json
./scripts/report.py --baseline results.json --compare fork.json
```

## License

This benchmark harness is MIT licensed. Note that linking against Grok pulls
in AGPL; do not distribute the linked artifact under a non-AGPL license.
The OpenJPEG-only build is unencumbered.
