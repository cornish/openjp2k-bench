# corpus/

Inputs the bench runs against. Layout:

- `user/` — Files you drop in by hand. Not tracked, not generated.
- `synthetic/` — Output of `scripts/gen_corpus.sh` (parametric `opj_compress` grid). Not tracked, regenerate locally. **Keep this in every bench run** — the public conformance suite does not vary parameter axes (pLRCP × pRPCL × d1/d5 × tiled/untiled × lossless/lossy) cleanly, but the synthetic sweep does, and that controlled-variable signal is how regressions get localized.
- `public/` — Files fetched from public sources (conformance / archival / remote-sensing / medical / cinema). **This repo expects `corpus/public/` to be a symlink to `~/GitHub/openjp2k-data/corpus/`**, the sibling repo that owns the curated fetcher and `MANUAL_SOURCES.md` for auth-gated sources.

**Recommended bench root is `corpus/`** (not `corpus/public/`) so synthetic + public buckets both run. `corpus/public/` only is fine for quick sanity but loses the controlled-variable signal.

To set it up:

```sh
git clone https://github.com/cornish/openjp2k-data ~/GitHub/openjp2k-data
~/GitHub/openjp2k-data/fetch_corpus.sh   # downloads what is auto-fetchable
ln -sfn ~/GitHub/openjp2k-data/corpus corpus/public
```

After populating any bucket, regenerate `corpus/manifest.json`:

```sh
./scripts/build_manifest.sh
```

The manifest records width / height / components / bit depth / tile dims / decomp levels / lossy-vs-lossless / progression / sha256 per file, parsed directly from SIZ + COD markers — no external deps. See `scripts/manifest_tool.py`.

## Why files >25 MB were retired

The active corpus tops out at ~15 MB. Larger JP2s (the Sentinel-2 10980² bands and the bigger LoC scans) were moved to `corpus/.archived/` in the sibling repo for two reasons:

1. **Workload fit.** This bench targets the openscope tile-decode pattern: ~1024×1024 regions, each stored as its own jp2. A single 10980² whole-image decode exercises a different memory access pattern (one giant contiguous allocation) that openscope never hits in practice.
2. **OOM safety.** A single decode of the 135 MB `*_TCI.jp2` peaks at ~13.7 GB RSS. With three decoders dlopen'd in one process (per commit `30229a9`) and the usual desktop overhead, this reliably tripped the global OOM killer on 16 GB hosts — taking the entire terminal scope (and the Claude session running in it) down with the bench process.

The archived files remain on disk; they just don't run in the default sweep. Restore them by moving back out of `corpus/.archived/` and re-running `./scripts/build_manifest.sh`.

## Scale track

`corpus/public/scale/` (in the sibling `openjp2k-data` repo) holds files >25 MB compressed — the LoC archival scans and Sentinel-2 10980² bands. These exercise the huge-allocation path that the `openjp2k` Sub-project 5 work targets (TCD `malloc_trim`, IDWT stripe compositing), but they OOM-cascade if run in the main perf path on a 16 GB host (a single TCI decode peaks at ~13.7 GB RSS, and the main bench dlopens all 3 decoders into one process).

The dedicated **scale-track** measures them safely:

```sh
nohup ./scripts/run_scale.sh corpus/public/scale/ \
      > results/scale_$(date +%Y%m%d_%H%M%S).jsonl &
disown
```

`run_scale.sh` invokes the bench once per `(file, decoder)` pair under `systemd-run --scope -p MemoryMax=` (budget tiered by file size: 8/12/16/24 GB). An overrun kills only the bench process — not the terminal scope. Each result row carries `scale_track=true`, `rss_peak_kb_sampled` (from a /proc/self/status poller during decode, more accurate than getrusage on glibc-with-trim), and `memory_max_bytes` (the cap the run was invoked under).

`scripts/report_scale.py` renders a per-file table (RSS + wall time + stage breakdown per decoder) and a cross-decoder RSS ratio. Two-file mode is a regression gate: flag files where peak RSS grew by >10% (configurable).

Scale runs are deliberately at a **lower cadence than perf** — once per `openjp2k` deliverable, not per commit. Wall time on the initial 5-file set is ~25 minutes.

## Perf vs correctness split

The `input/nonregression/` subtree of the upstream OpenJPEG data is a CVE / fuzzer corpus: ~140 files of which ~80 are intentionally malformed (`*.SIGSEGV.*`, `*.SIGFPE.*`, `*.asan.*`, `broken*.jp2`, etc). Decoding them is not the point — gracefully rejecting them is. They previously drowned the perf bench's error counts in noise.

The default `run_bench.sh` invocation now **excludes `*/input/nonregression/*`** from the perf sweep. (`baseline/nonregression/` — encoder reference outputs that decode fine — is kept.) Pass `--include-nonregression` to override.

A separate **correctness track** measures the same files for behavior conformance instead of speed:

```sh
./scripts/run_correctness.sh \
    corpus/public/conformance/openjpeg-data/input/nonregression/ \
  > results/correctness_$(date +%Y%m%d_%H%M%S).jsonl
```

Each record is `type="correctness"` with `outcome ∈ {decoded_ok, cleanly_rejected}` and `expected_outcome ∈ {pass, fail, unknown}` derived from upstream's `BLACKLIST_JPEG2000` + `test_suite.ctest.in` (see `scripts/classify_nonregression.py`). `scripts/report.py` renders a dedicated correctness section: outcome × expected contingency per decoder, cross-decoder disagreements, files contradicting upstream's expected_outcome.
