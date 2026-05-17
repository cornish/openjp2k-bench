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
  cmake/                Helper modules (flags, version capture)
  src/                  Bench driver + adapters
  scripts/              setup / build / corpus / run / report helpers
  docs/                 Methodology + design specs
  tests/                Smoke test + checked-in fixture
  corpus/               .gitignored; populated locally
    user/                 your real-world JP2 drop zone
    synthetic/            produced by gen_corpus.sh
    public/               symlink to openjp2k-data/corpus
    manifest.json         produced by build_manifest.sh
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

## Corpus

Three buckets under `corpus/`, all gitignored. `public/` is a symlink to
the sibling [openjp2k-data](https://github.com/cornish/openjp2k-data) repo,
which owns the curated public corpus (conformance / archival / remote-
sensing / medical / cinema) and `MANUAL_SOURCES.md` for auth-gated paths.

```sh
# 1. Drop real-world files into corpus/user/ (no tooling needed).
cp ~/wsi-samples/*.jp2 corpus/user/

# 2. Generate the synthetic axis (exercises both common and dusty corners
#    of the JP2K spec — five progression orders, 0/1/5/8 decomposition
#    levels, code-block size corners, SOP/EPH markers, etc.).
./scripts/gen_corpus.sh                 # full sweep
./scripts/gen_corpus.sh --quick         # smaller sweep for smoke

# 3. Point corpus/public at the sibling repo.
git clone https://github.com/cornish/openjp2k-data ~/GitHub/openjp2k-data
~/GitHub/openjp2k-data/fetch_corpus.sh
ln -sfn ~/GitHub/openjp2k-data/corpus corpus/public

# 4. Build / refresh the manifest after any of the above. The manifest
#    parses SIZ + COD markers directly (pure stdlib Python) and records
#    width, height, components, bit depth, tile dims, decomp levels,
#    progression, MCT, lossless/lossy, container, sha256.
./scripts/build_manifest.sh

# 5. Sanity-check before a benchmark run (re-decodes every synthetic file
#    with opj_decompress to catch generator bugs on dusty combos).
./scripts/check_corpus.sh
```

## Run

```sh
# Bench every .jp2/.j2k under corpus/
./scripts/run_bench.sh corpus/ > results.json

# Internal threads sweep
./build/jp2k-bench --iters 20 --threads 1,4,8 corpus/synthetic/*/*.jp2 > results.json

# External concurrency (run N bench jobs in parallel; bound by one
# file blob in memory at a time)
./build/jp2k-bench --concurrent-files 8 corpus/synthetic/*/*.jp2 > results.json

# Region decode (1024x1024 region starting at 0,0). Honors native
# region paths on adapters that support them (OpenJPEG yes, Grok
# falls back to full-decode + crop).
./build/jp2k-bench --roi 1024x1024@0,0 corpus/synthetic/*/pLRCP_*.jp2 > roi.json

# Reduce to a comparison table (group by corpus bucket by default).
./scripts/report.py results.json
./scripts/report.py baseline.json compare.json   # adds Δmin% / Δmpix% columns
./scripts/report.py results.json --group-by progression
./scripts/report.py results.json --filter bit_depth=16,lossless=true
```

Note on `--threads` + Grok: Grok's worker pool is a process-singleton
(initialized once via `grk_initialize`). A multi-value `--threads`
sweep with Grok enabled in one process therefore pins the pool to the
first value seen and the rest of the rows are misleading. The bench
emits a one-line warning when this combination is requested; for fair
Grok scaling numbers, run one process per `--threads` value.

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
