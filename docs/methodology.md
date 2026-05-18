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

## What `--profile-stages` measures, and what to read into it

`--profile-stages` brackets each timed iteration into four sub-times:
`setup`, `decode`, `unpack`, and `teardown`. They are not all the codec
doing the same work — knowing what each adapter does in `unpack`
specifically is necessary to avoid attributing adapter-side asymmetry
to the codec.

- **OpenJPEG (`src/adapter_openjpeg.cpp:unpack_opj_image`):** the codec
  always hands back planar `OPJ_INT32*` per-component buffers, regardless
  of the file's bit depth (`prec`). The adapter walks each component
  with a single typed inner loop, applies the sign-shift, clamps to the
  output range, and writes interleaved channel-major into `out.pixels`.
  Output is `uint8_t` when `prec ≤ 8`, otherwise little-endian `uint16_t`.

- **Grok (`src/adapter_grok.cpp:unpack_grok_image`):** v20 hands back a
  planar buffer whose storage type **varies per component**
  (`GRK_INT_8` / `_16` / `_32`, exposed as `grk_image_comp::data_type`).
  The adapter dispatches once per component on `data_type` and runs a
  typed templated inner loop, then applies the same sign-shift / clamp /
  interleave / endian-pack contract as the OpenJPEG path.

Implications for reading the breakdown:

- Equal `unpack` times between adapters mean equal adapter work. They do
  not mean the codecs handed back equivalent representations.
- A faster Grok `unpack` on small 8-bit files is not a Grok-codec win;
  it is a consequence of Grok returning 1-byte storage (so one-byte
  loads in the adapter) versus OpenJPEG's always-int32 (four-byte loads).
- A faster OpenJPEG `unpack` on high-bit-depth content is similarly an
  adapter-side property; both adapters do `int32 → u16` for `prec > 8`
  but the type-dispatching overhead lives only on the Grok side and only
  costs measurable time when there are many small components.
- Neither adapter does color conversion, ICC application, or alpha
  compositing. `unpack` is the minimum useful transform — channel
  interleave + clamp + sign-shift — and nothing else. If you change
  the unpack to do more (e.g. premultiply for a viewer), the headline
  numbers will shift and the codec comparison gets noisier; do that in
  a downstream consumer instead.

Setup, decode, and teardown are codec-internal and are valid for
codec-to-codec comparison. Unpack is adapter-internal and is **not**.
When citing a per-stage delta, name which stage and which adapter pair.

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

## Chroma-subsampled inputs

The pixel buffer is **planar** (each component's plane stored
contiguously, planes concatenated in component order). Files with
chroma subsampling (4:2:0, 4:2:2, 4:1:1) have smaller chroma planes —
`DecodedImage::components[c].{w,h,dx,dy}` carries the per-component
dimensions and subsampling factors. The result row's
`subsampled_components` field is null for 4:4:4 inputs and an array of
`{w,h,dx,dy}` objects otherwise.

The cross-decoder pixel-equality check compares the planar buffers
byte-for-byte; PSNR fallback computes over the same sample sequence.
ROI/region decode on subsampled inputs is **not** supported in the
default crop helper (chroma cropping requires `dx`/`dy` boundary
alignment); adapters with native region APIs must override `decode_region`
to handle it.

## Correctness track

A separate `--correctness` mode runs one decode attempt per file with no
warmup/iters, recording `outcome ∈ {decoded_ok, cleanly_rejected}` and
`expected_outcome ∈ {pass, fail, unknown}` from a classifier file. Used
to measure decoder robustness against the upstream OpenJPEG CVE/fuzzer
corpus (`input/nonregression/`), which the perf path excludes by
default. Records carry `type="correctness"` in the JSONL stream so
`report.py` can render them in a dedicated section. See
`corpus/README.md` and `scripts/run_correctness.sh`.
