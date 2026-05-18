# YUV subsampling + nonregression split — implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make bench output usable for performance comparison by (a) handling chroma-subsampled inputs that the unpack/verify path currently rejects, and (b) splitting the upstream regression corpus out of the perf-result stream so error counts mean something.

**Why both in one plan:** Both items shape *what counts as a measurable result*. (2) increases the universe of files the bench can actually time; (4) shrinks the universe to the files we should be timing in the first place. Land them together so the next full-corpus run has a clean denominator.

**Tech stack:** C++17, Bash, Python 3.8+ (stdlib only).

**Touchpoints:**
- `src/verify.h`, `src/verify.cpp` — shape-mismatch tolerance for subsampled planes.
- `src/adapter.h`, `src/adapter_opj.cpp`, `src/adapter_grok.cpp` — unpack path handles non-uniform component dimensions.
- `src/bench.cpp` — reference image comparison handles chroma-subsampled images.
- `src/main.cpp` — new `--correctness` mode and `--include`/`--exclude` glob flags.
- `scripts/run_bench.sh` — default exclude for `*/nonregression/*` in perf mode.
- `scripts/classify_nonregression.py` (new) — parses upstream `BLACKLIST_JPEG2000` from `external/openjpeg/tests/nonregression/CMakeLists.txt`.
- `scripts/report.py` — separate perf-vs-correctness reporting sections.
- `tests/fixtures/` — add subsampled fixtures (`tiny_420.jp2`, `tiny_422.jp2`).
- `tests/smoke.sh`, `tests/yuv_subsampling_test.py` (new) — coverage.

---

## Task 1 — Subsampled-component support in `DecodedImage`

The current `DecodedImage` shape assumes all components share `width × height`. Subsampled JP2 files break that. Extend the type without breaking the existing 4:4:4 hot path.

- [ ] In `src/adapter.h`, add per-component dims to `DecodedImage`: replace the single `width`/`height` with `image_width`/`image_height` (canvas size) plus a `std::vector<ComponentDims>{w, h, dx, dy}` where `dx,dy` are the subsampling factors. Keep the existing fields as accessors for the 4:4:4 case (assert all components match).
- [ ] In `src/adapter.h`, add `bool DecodedImage::is_subsampled() const` returning true if any component's dims differ from the canvas. Document the pixel-buffer layout: planes are concatenated, in component order, each at its own width×height; **no upsampling happens in the adapter** — the bench owns reference comparison and any rendering.
- [ ] Update `src/bench.cpp::FileResult` to record `subsampled_components: [{dx, dy}, ...]` in the result row (null when not subsampled). New field flows through `src/json_out.cpp`.

## Task 2 — Adapter unpack: openjpeg path

- [ ] In `src/adapter_opj.cpp::unpack_opj_image`, drop the "all components same size" assertion. Read each component's `w`, `h`, `dx`, `dy` from the `opj_image_t` component table, copy each plane to its own region of the output buffer, fill `DecodedImage::components`.
- [ ] Verify against `tests/fixtures/tiny_420.jp2` (added in Task 5) — bench decodes without `non-uniform component size` error and the result row has `subsampled_components: [{1,1},{2,2},{2,2}]`.

## Task 3 — Adapter unpack: grok path

- [ ] In `src/adapter_grok.cpp`, mirror Task 2 against Grok's `grk_image` component layout. Confirm Grok also reports per-component `dx`/`dy` (it should — same JPEG 2000 standard).
- [ ] Verify the same fixture decodes via grok with matching `subsampled_components`.

## Task 4 — Reference comparison for subsampled images

The cross-decoder pixel-equality check assumes plane-by-plane `memcmp`. With subsampling, the planes still match plane-by-plane (subsampling is part of the codestream, not a decoder choice), so plane-by-plane comparison still works — but the planes have different sizes per component.

- [ ] In `src/bench.cpp` and `src/verify.cpp`, replace the single `pixels` buffer check with per-component comparison driven by `DecodedImage::components`. PSNR fallback computes per-component PSNR and reports the minimum (worst plane).
- [ ] Two subsampled decoders must agree per-component before `pixel_match=1`. Shape mismatch (different `components` vectors between two decoders) is a hard error, not a PSNR-fallback case.

## Task 5 — Subsampled test fixtures

- [ ] Generate two tiny fixtures: `tests/fixtures/tiny_420.jp2` (128×128, 4:2:0, 3 components) and `tests/fixtures/tiny_422.jp2` (128×128, 4:2:2, 3 components). Use `opj_compress -F` flags from `external/openjpeg`, document the exact invocations in `tests/fixtures/README.md`.
- [ ] Check the fixtures in (small, ~5 KB each).

## Task 6 — Smoke + unit coverage

- [ ] Extend `tests/smoke.sh` with a `--- yuv-subsampling` section that decodes `tiny_420.jp2` and `tiny_422.jp2` and asserts both adapters produce matching `subsampled_components` and `pixel_match=1`.
- [ ] Add `tests/yuv_subsampling_test.py` exercising the manifest parser on the new fixtures (sanity-check that `manifest_tool.py` reports the per-component dims correctly).

## Task 7 — Upstream blacklist parser

- [ ] Add `scripts/classify_nonregression.py`. Parses `set(BLACKLIST_JPEG2000_TMP ...)` and `set(BLACKLIST_JPEG2000 ...)` from `external/openjpeg/tests/nonregression/CMakeLists.txt`, emits a JSON list of expected-fail filenames to stdout. Also parses any per-file overrides in `test_suite.ctest.in` and `*.ctest.in` files (the additional 38 files our earlier ad-hoc classifier missed).
- [ ] Cache result to `corpus/.nonregression_classification.json` so the bench wrapper doesn't reparse on every run. Regeneration trigger: timestamp of source CMakeLists.txt.

## Task 8 — `--correctness` mode in the bench

The bench currently runs decode + verify + record timing for every input. In correctness mode it runs decode-or-clean-rejection + records outcome class, no timing.

- [ ] In `src/main.cpp`, add `--correctness` flag. Disables `--warmup`/`--iters` knobs (forces warmup=0, iters=1, no timing recorded), enables a result-row `expected_outcome` field set from the classification file passed via `--classification path/to/classification.json`.
- [ ] In `src/bench.cpp`, the timing loop becomes a single attempt; the result row carries `outcome ∈ {decoded_ok, cleanly_rejected, crashed, garbage_output}` rather than timing stats.
- [ ] Emit a different result-row `type`: `correctness` instead of `result`. The JSONL stream stays parseable; downstream tools can filter on `type`.

## Task 9 — Wrapper: default exclude for nonregression in perf mode

- [ ] In `scripts/run_bench.sh`, add a default exclude glob `*/nonregression/*` that's applied unless `--include-nonregression` is passed. Goes into the `corpus_spec` JSON the wrapper already builds (per the 1+3+6 triage), so the run header records that nonregression was excluded.
- [ ] Add `scripts/run_correctness.sh` as a sibling that runs `--correctness` against the nonregression bucket with the classifier output. Emits a `correctness/` results subdir.

## Task 10 — Report split

- [ ] In `scripts/report.py`, partition input records by `type`. The "perf" section (existing comparison table, geomean speedups) only consumes `type=result` records. New "correctness" section consumes `type=correctness` records and reports per-decoder outcome class counts + lists files that disagreed with `expected_outcome`.
- [ ] Two-file mode (`report.py perf.jsonl correctness.jsonl`) merges both into one report with separate sections; one-file mode auto-detects.

## Task 11 — Documentation

- [ ] Update `corpus/README.md` to explain the perf/correctness split and that nonregression is excluded from perf by default.
- [ ] Update `docs/methodology.md` to document subsampled-component handling and the correctness track.
- [ ] Update the `crash_resistant_bench_run.md` memory to mention `--include-nonregression` if a future user wants to force-include it.

## Task 12 — Validation run

- [ ] Re-run the full corpus with the new defaults. Expected change: 252 → ~80 files in the perf JSONL (nonregression excluded). Compare openjp2k vs openjpeg geomean — should sharpen from 0.9765× toward whatever the real signal is once the noise from broken inputs and unmeasurable YUV files is gone.
- [ ] Re-run `--correctness` against the nonregression bucket. Expected: all three decoders cleanly reject the 53 (or ~91 with the deeper classifier) blacklist files, decode the rest, no unexpected outcomes.
- [ ] Compare medical-bucket error counts before/after. The 66 generic warmup: decode rows from the 2026-05-17 run should drop substantially once YUV unpack works — the unknown remainder is the real medical-bucket signal.

---

## Acceptance criteria

- A full-corpus perf run emits no `non-uniform component size` errors.
- The perf JSONL has no `*/nonregression/*` paths by default; including them requires `--include-nonregression`.
- A correctness run on the nonregression bucket produces a report with: per-decoder outcome counts, list of files where outcome ≠ expected, no timing numbers.
- `report.py` renders perf and correctness sections cleanly from one or two JSONL inputs.
- The smoke test covers a 4:2:0 fixture end-to-end through both adapters.
