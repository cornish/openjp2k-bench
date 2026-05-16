# Benchmark methodology

Short, explicit notes on how `jp2k-bench` measures decode performance. The
goal is reproducibility — anyone who follows these steps on similar hardware
should get within a few percent of the same numbers.

## What is measured

Only the **decode** call is timed. Everything else (file I/O, allocator
setup, decoder construction) is excluded. The full pipeline:

1. Read the file into a `std::vector<uint8_t>` once, before any timing.
2. Construct the decoder once.
3. Run `--warmup` (default 2) untimed iterations.
4. Run `--iters` (default 20) iterations, each timed with
   `std::chrono::steady_clock`.
5. Report `min`, `p50`, `p90`, `p99`, `max`, `mean`, `stddev` and
   `megapixels_per_sec` computed from `min`.

`min` is the headline number. Decode is a deterministic CPU workload, so
the fastest run is the run least disturbed by the OS — it's the best
estimate of the codec's intrinsic cost. `p50` and `p99` are reported for
sanity (huge gap → noisy environment).

## Thread modes

Two distinct things are conflated by the word "threads"; we measure both:

- **Internal:** the decoder library uses N threads on a single image
  (`opj_codec_set_threads`, `grk_decompress_parameters.numThreads`).
  Controlled by `--threads N`. Default `1`.
- **External:** decode many images concurrently. Not implemented yet;
  expected to land as `--concurrent-files N` once we wire a thread pool
  around `bench_file`. This better represents WSI tile throughput.

When `--threads N1,N2,...` is given, the sweep runs once per value, so a
single invocation can produce e.g. a 1/4/8-thread comparison.

## Cross-decoder verification

For each file, the first listed decoder produces a reference image. Every
subsequent decoder's output is byte-compared. Mismatches are recorded
(`pixel_match: 0` in JSON) and surfaced in the report. This catches:

- Adapters that mis-handle bit depth, signedness, or component order.
- Decoder bugs that produce different pixels.

For lossy files this is a coarse check: openjpeg and grok generally produce
identical pixels (they implement the same standard) but small differences
near rounding boundaries are possible. If verification routinely fails on
lossy files, switch to PSNR vs a ground-truth raster.

## Build flags

Everything is compiled `-O3 -march=native -flto -DNDEBUG`, statically
linked. CMake applies LTO via `CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE`
so the optimizer can inline across the adapter/library boundary. Identical
flags for openjpeg and grok. Override with `--no-native` for a portable
binary.

## Reducing measurement noise

For low-variance numbers, run on a quiet machine and consider:

```sh
# Pin to a performance core, disable SMT noise on a sibling.
taskset -c 2 ./build/jp2k-bench --iters 50 ...

# Lock CPU governor to performance.
sudo cpupower frequency-set -g performance

# Disable turbo if you want stable, comparable absolute numbers across runs.
# (You're trading absolute speed for variance.)
echo 1 | sudo tee /sys/devices/system/cpu/intel_pstate/no_turbo
```

These are documentation hints, not enforced by the bench.

## What is *not* measured (yet)

- **Cold cache:** every run keeps the file in RAM. JP2K files in production
  often come from disk or network and the file-read cost matters.
- **Memory allocations:** decoders that recycle internal buffers across
  decodes look better here than they would in a one-shot CLI.
- **Region-of-interest decode:** WSI workloads frequently decode only one
  tile (a JP2K precinct) at a time. Both openjpeg and grok support this
  via `opj_set_decode_area` / equivalent; not wired up yet.
- **Memory peak:** we don't measure RSS. Add via `getrusage(RUSAGE_SELF)`
  if you need it.

## Comparing against Kakadu

There is no Kakadu adapter — paste Kakadu's published numbers (or your own
licensed runs) into the report as a separate column. To make them
comparable:

- Same image, same compression parameters.
- Same thread count (`--threads N`).
- Wall time of the decode call only (Kakadu's `kdu_expand` has flags to
  print just the decode time).
