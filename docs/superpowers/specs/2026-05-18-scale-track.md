# Scale track — design spec

**Date:** 2026-05-18
**Status:** Draft, needs sign-off before implementation.
**Scope:** A separate bench track for files ≥25 MB that measures peak RSS + wall time per (file, decoder) pair, runs under a per-invocation memory cap to prevent OOM cascades, and runs at a much lower cadence than the main perf bench.

## Motivation

The main bench corpus is capped at ≤25 MB compressed inputs. That cap is right for the perf bench because:

- The openscope tile-decode workload (~1024² tiles, each their own jp2) lives well below 25 MB per decode.
- A single decode of a 135 MB Sentinel-2 TCI tile peaks at ~13.7 GB RSS, which on a 16 GB host OOM-kills the entire terminal scope (see `crash_resistant_bench_run.md` memory and `corpus/README.md`'s ">25 MB retirement" section). With three decoders dlopen'd in one process (commit `30229a9`), the per-process budget is ~5 GB before the third decoder's allocations cross the global ceiling.

But the workloads where decode performance *most* matters — large geospatial mosaics, archival LoC scans, WSI tile base levels, large medical — all sit *above* the cap. Sub-project 5 (`openjp2k` allocation work: TCD `malloc_trim`, IDWT stripe compositing) is specifically targeting the regression class that only manifests on big files. Without observability into that class, the bench is blind to the changes the fork is supposed to validate.

The Grok cleanroom research explicitly flagged big-file behavior as a Kakadu/Grok advantage. That advantage — or its absence in the fork — won't show up until we measure files in that size range.

## Constraints

1. **Cannot OOM the host.** The terminal-kill failure mode on 2026-05-17 must not recur. This rules out the main-bench design (three decoders dlopen'd, all sharing the process address space) for >25 MB inputs.
2. **Must give peak RSS, not just wall time.** Allocation regressions are invisible in wall time on a single-shot decode; they show up as RSS deltas. Sub-project 5 targets are specifically about peak RSS.
3. **Wall-clock cost.** Scale runs are slow. A full-corpus scale sweep at ~30 files × 3 decoders × tens of seconds each = tens of minutes. Acceptable for per-deliverable cadence; not acceptable per-commit.
4. **Must be reproducible across machines.** Like the main bench, scale records compile flags, library SHAs, kernel, CPU model.

## Design

### Process model

**Main bench (existing):** one `jp2k-bench` process, three decoders dlopen'd, iterates over the file list, runs each (file, decoder) pair sequentially. RSS budget shared across all three decoders.

**Scale bench (new):** one `jp2k-bench` *invocation* per (file, decoder) pair. The invocation loads exactly one decoder (existing `--decoder NAME` flag), decodes the one file, exits. RSS budget = one decoder's footprint, not 3×. A separate `scripts/run_scale.sh` orchestrates the loop and aggregates results.

This is a 9× wall-time overhead vs. the main bench design *per file* (process startup × 3 dlopens), but on scale files the decode itself takes seconds-to-minutes; the overhead is in the noise.

### Memory cap

Each `jp2k-bench` invocation runs under `systemd-run --scope -p MemoryMax=<budget>G` (see `crash_resistant_bench_run.md`). On overrun, the kernel kills *only* the bench process; the terminal and orchestrator stay alive.

Budget table — `MemoryMax` per file size tier, picked to give the decode ~2× headroom over observed peak from the partial 2026-05-17 run:

| File size (compressed)   | MemoryMax | Notes                                                     |
|--------------------------|-----------|-----------------------------------------------------------|
| 25–50 MB                 | 8 GB      | Bretagne-class. Observed peak ~3 GB for B07-like inputs.  |
| 50–100 MB                | 12 GB     | world-1689 (74 MB) hit ~7 GB across the three-decoder run.|
| 100–200 MB               | 16 GB     | TCI hit ~13.7 GB with three decoders; 1 decoder ≈ 5 GB.   |
| >200 MB                  | host-dependent | Manual, with explicit user opt-in.                  |

The budget is recorded in the result row so a reader can tell whether an OOM means "decoder used >budget" or "MemoryMax too low."

### Iteration model

- `iters=1`, `warmup=0`. A single decode per (file, decoder). The signal is allocation + per-decode wall time, not steady-state.
- No `--concurrent-files`. Scale runs serially. Concurrent scale decodes would have to share the host budget *and* introduce contention noise on real disk and memory bandwidth, which masks the regression class we're trying to see.
- Per-decode `getrusage` peak RSS captured (already in `src/rss.cpp`) **plus** a sampler thread that polls `/proc/self/status` `VmRSS` every 100ms during the decode. The sampler catches the true peak (`getrusage` post-decode only gives the *final* peak across the process lifetime; if peak occurred mid-decode and was freed before exit, `getrusage` understates it on glibc with `MALLOC_TRIM_THRESHOLD_` set).
- Stage timings (`--profile-stages`) recorded by default — setup/decode/unpack/teardown — since allocation regressions often localize to one stage.

### File selection

Scale files live in **`corpus/scale/`** under the sibling `openjp2k-data` repo (alongside the existing `corpus/public/`, `corpus/.archived/`). Not a symlink; same directory tree. Files are added by `fetch_corpus.sh` from the same upstream URLs that produced the retired-from-perf inputs, plus any new large fixtures.

Initial population from existing `corpus/.archived/`:

- `loc-maps/np000040.jp2` (29 MB)
- `loc-maps/world-1689.jp2` (74 MB)
- `remote-sensing/T30UXC_..._B07.jp2` (34 MB)
- `remote-sensing/T30UXC_..._B02.jp2` (106 MB)
- `remote-sensing/T30UXC_..._TCI.jp2` (135 MB)

Selection rule: anything in `corpus/scale/` runs in the scale track. The 25 MB cap on `corpus/public/` is preserved.

### Output

Schema reuses the v2 schema with two additions in the result row:

```jsonc
{
  "type": "result",
  // ... existing fields ...
  "scale_track": true,
  "memory_max_bytes": 17179869184,    // budget the run was capped at
  "rss_peak_kb_sampled": 13700000,    // 100ms-sampler peak (more accurate)
  "rss_peak_kb": 5200000,             // getrusage final peak (existing)
  "oom_killed": false                 // true if the invocation exited via SIGKILL by cgroup OOM
}
```

Results land in `results/scale_<timestamp>.jsonl`. One JSONL line per (file, decoder) invocation, plus a leading `type=run` line with the same provenance fields as the main bench. The orchestrator emits the run line; each per-(file, decoder) invocation emits its own `type=result` line to the same file via `>>` redirection.

If an invocation is OOM-killed, the orchestrator records a synthetic result row with `oom_killed: true`, `rss_peak_kb: memory_max_bytes / 1024`, `error: "oom_killed by cgroup"`. The orchestrator detects this by inspecting the `systemd-run --scope` exit (SIGKILL via OOM → exit 137 + journalctl annotation).

### Reporting

`scripts/report_scale.py` (separate from `report.py`):

- Per-file table: peak RSS by decoder, wall time, stage breakdown, OOM marker.
- Cross-decoder RSS comparison: which decoder uses least memory per file. This is the headline scale-track metric, not wall time.
- Regression detection: given two scale runs, flag files where peak RSS increased >10% in the newer run. This is the "did the IDWT stripe-compositing change regress allocations" gate.

### Cadence + invocation

**Not on every commit.** Scale runs go on the per-deliverable cadence — once per `openjp2k` release candidate or per Sub-project 5 milestone. Wall time per full sweep: ~30 minutes for the 5-file initial set; scales linearly.

Invocation:

```sh
TS=$(date +%Y%m%d_%H%M%S)
nohup ./scripts/run_scale.sh corpus/scale/ \
      > results/scale_${TS}.jsonl \
      2> results/scale_${TS}.log &
disown
```

The orchestrator:

1. Reads `corpus/scale/` for files; sorts by size descending (largest first so OOMs surface early).
2. For each file: determines MemoryMax tier; for each decoder: invokes `systemd-run --scope -p MemoryMax=<tier> -- ./build/jp2k-bench --decoder <NAME> --iters 1 --warmup 0 --profile-stages --scale-track --rss-sample-ms 100 <file>` and appends the result row to stdout.
3. Emits the run header line before the first result.

## Open questions

- **Sampler accuracy on huge decodes.** Polling `/proc/self/status` at 100ms could miss a sub-100ms peak. Drop to 10ms? At 10ms the syscall overhead might perturb the very thing we're measuring. Empirically check on the TCI file before committing to an interval.
- **Should we cap decode threads?** Multi-threaded decode can spike RSS via per-thread tiling buffers. Default to `--threads 1` for scale-track so RSS is comparable across decoders (Grok's pool-singleton complicates --threads sweeps anyway). Allow override via env.
- **Should the orchestrator support arbitrary `corpus/scale/` paths or insist on the `corpus/scale/` subdirectory?** Insisting on the subdir prevents accidentally running a 100 MB file through the main bench by passing the wrong directory. Probably worth it.
- **`oom_killed` detection robustness.** `systemd-run --scope` exit code 137 isn't conclusive; the journalctl annotation is more authoritative but requires permissions. Document fallback: if journalctl access fails, trust exit 137 + assume OOM.

## Non-goals

- Replacing the main perf bench. The scale track measures a different thing at a different cadence.
- Cross-machine comparison of scale results. Like the main bench, scale numbers only compare on the same host.
- Tile-region decode at scale. `--roi` against huge JP2s is an interesting workload but lives in its own track; not this spec.

## Acceptance criteria for the implementation plan derived from this spec

- A scale run on the initial 5-file set completes without OOM-killing the orchestrator's terminal scope.
- The headline RSS comparison renders for each file with at least 2 decoders successfully measured.
- A re-run with a deliberately-injected `malloc()` regression in the `openjp2k` adapter (e.g., extra `std::vector<uint8_t>(file_size)` in the decode path) is detected by `report_scale.py`'s regression gate.
- The bench survives a deliberate budget overrun (e.g., 1 GB MemoryMax on TCI): the orchestrator records `oom_killed: true`, the terminal stays alive, the run continues to the next file.
