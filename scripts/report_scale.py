#!/usr/bin/env python3
"""Summarize a scale-track JSONL run.

Scale-track measures peak RSS + wall time per (file, decoder) under a
per-invocation memory cap. The headline metric is **peak RSS by
decoder**, not wall time — wall time on huge JP2s tracks decode work
linearly, but allocation regressions (which Sub-project 5 targets) only
manifest as RSS deltas.

Usage:
  report_scale.py results/scale_TS.jsonl                         # one-run report
  report_scale.py results/scale_BASE.jsonl results/scale_NEW.jsonl  # regression gate
  report_scale.py ... --regression-pct 10                        # tighter gate

Exit codes:
  0 — no regression detected (or one-run mode)
  1 — at least one file's peak RSS grew by more than the threshold
"""
from __future__ import annotations

import argparse
import json
import sys
from collections import defaultdict
from pathlib import Path


def _load(path: str) -> dict:
    """Load a scale-track JSONL stream.

    The first line is type="run" with scale_track_config. Subsequent
    lines are type="result" with scale_track=true plus rss_peak_kb_sampled
    and memory_max_bytes. Tolerates a truncated final line (the bench
    flushes per-row but may die mid-write).
    """
    run = None
    results: list[dict] = []
    with open(path) as f:
        for lineno, line in enumerate(f, 1):
            line = line.strip()
            if not line:
                continue
            try:
                rec = json.loads(line)
            except json.JSONDecodeError as e:
                sys.stderr.write(
                    f"{path}:{lineno}: dropping unparseable line ({e})\n")
                continue
            t = rec.get("type")
            if t == "run":
                run = rec
            elif t == "result":
                results.append(rec)
            # ignore aggregate/correctness
    if run is None:
        sys.exit(f"{path}: missing 'run' header record")
    return {"run": run, "results": results}


def _fmt_bytes(n: int | float) -> str:
    n = float(n)
    for unit in ("B", "KB", "MB", "GB", "TB"):
        if abs(n) < 1024 or unit == "TB":
            return f"{n:.1f}{unit}" if unit != "B" else f"{int(n)}B"
        n /= 1024
    return f"{n:.1f}TB"


def _fmt_seconds(s: float) -> str:
    if s < 1:
        return f"{s*1000:.0f}ms"
    if s < 60:
        return f"{s:.2f}s"
    return f"{s//60:.0f}m{s%60:.1f}s"


# Stable, semantically-meaningful decoder ordering (openjpeg baseline first).
_DECODER_ORDER = ("openjpeg", "openjp2k", "grok")


def _ordered_decoders(seen: set[str]) -> list[str]:
    ordered = [d for d in _DECODER_ORDER if d in seen]
    extra = sorted(seen - set(_DECODER_ORDER))
    return ordered + extra


def _decoder_summary(results: list[dict]) -> None:
    """Per-(file, decoder) table: peak RSS by decoder, wall time, stages, OOM."""
    by_file: dict[str, dict[str, dict]] = defaultdict(dict)
    for r in results:
        by_file[r["file"]][r["decoder"]] = r

    decoders = _ordered_decoders({r["decoder"] for r in results})
    width = 55 + 9 + len(decoders) * 24
    print("=" * width)
    print("Scale-track per-file results")
    print("=" * width)
    header = f"{'file':<55} {'size':>8} "
    for d in decoders:
        header += f" {d+' RSS':>11} {d+' wall':>10}"
    print(header)
    print("-" * width)
    for fp, dmap in sorted(by_file.items(), key=lambda kv: -(next(iter(kv[1].values())).get("bytes", 0))):
        sz = next(iter(dmap.values())).get("bytes", 0)
        short = fp.rsplit("/", 1)[-1][:55]
        row = f"{short:<55} {_fmt_bytes(sz):>8} "
        for d in decoders:
            rec = dmap.get(d)
            if rec is None:
                row += f" {'-':>11} {'-':>10}"
                continue
            if rec.get("oom_killed"):
                row += f" {'OOM':>11} {'OOM':>10}"
                continue
            if rec.get("error"):
                row += f" {'ERR':>11} {'ERR':>10}"
                continue
            rss_kb = rec.get("rss_peak_kb_sampled") or rec.get("rss_peak_kb") or 0
            wall = rec.get("timing_s", {}).get("min", 0)
            row += f" {_fmt_bytes(rss_kb*1024):>11} {_fmt_seconds(wall):>10}"
        print(row)


def _cross_decoder_rss(results: list[dict]) -> None:
    """Headline metric: which decoder used the least RSS on each file."""
    by_file: dict[str, dict[str, int]] = defaultdict(dict)
    for r in results:
        if r.get("oom_killed") or r.get("error"):
            continue
        kb = r.get("rss_peak_kb_sampled") or r.get("rss_peak_kb") or 0
        if kb:
            by_file[r["file"]][r["decoder"]] = kb
    print()
    print("=" * 72)
    print("Cross-decoder peak-RSS ratios (lower is better)")
    print("=" * 72)
    decoders = _ordered_decoders({d for dm in by_file.values() for d in dm})
    if len(decoders) < 2:
        print("(need at least 2 decoders' results to compare)")
        return
    base = decoders[0]
    print(f"{'file':<55} ", end="")
    for d in decoders[1:]:
        print(f"{d+'/'+base:>15}", end="")
    print()
    print("-" * 72)
    for fp, dm in sorted(by_file.items()):
        short = fp.rsplit("/", 1)[-1][:55]
        if base not in dm:
            continue
        bv = dm[base]
        print(f"{short:<55} ", end="")
        for d in decoders[1:]:
            if d not in dm:
                print(f"{'-':>15}", end="")
            else:
                print(f"{dm[d]/bv:>15.3f}", end="")
        print()


def _regression_gate(base: list[dict], cmp: list[dict], pct: float) -> int:
    """Two-run mode: flag files where peak RSS grew by >pct%.

    Returns process exit code: 0 if all within budget, 1 if any regressed.
    """
    base_idx = {(r["file"], r["decoder"]): r for r in base
                if not r.get("oom_killed") and not r.get("error")}
    cmp_idx = {(r["file"], r["decoder"]): r for r in cmp
               if not r.get("oom_killed") and not r.get("error")}
    print()
    print("=" * 72)
    print(f"RSS regression gate (threshold: peak grew > {pct:.1f}%)")
    print("=" * 72)
    regressed = []
    for key, b in base_idx.items():
        c = cmp_idx.get(key)
        if c is None:
            continue
        b_kb = b.get("rss_peak_kb_sampled") or b.get("rss_peak_kb") or 0
        c_kb = c.get("rss_peak_kb_sampled") or c.get("rss_peak_kb") or 0
        if b_kb == 0:
            continue
        delta = (c_kb - b_kb) / b_kb * 100.0
        if delta > pct:
            regressed.append((key, b_kb, c_kb, delta))
    if not regressed:
        print("OK — no files exceeded threshold.")
        return 0
    print(f"REGRESSION — {len(regressed)} (file, decoder) pairs grew above threshold:")
    print(f"  {'file':<45} {'decoder':<10} {'base':>10} {'new':>10} {'delta':>8}")
    for (fp, dec), b_kb, c_kb, delta in sorted(regressed, key=lambda x: -x[3]):
        short = fp.rsplit("/", 1)[-1][:45]
        print(f"  {short:<45} {dec:<10} "
              f"{_fmt_bytes(b_kb*1024):>10} {_fmt_bytes(c_kb*1024):>10} "
              f"{delta:>+7.1f}%")
    return 1


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("baseline", help="scale-track JSONL (one or two-run mode)")
    ap.add_argument("compare", nargs="?",
                    help="optional second JSONL — enables regression gate")
    ap.add_argument("--regression-pct", type=float, default=10.0,
                    help="threshold for the RSS regression gate (default 10%%)")
    args = ap.parse_args()

    base = _load(args.baseline)
    print(f"Baseline: {args.baseline}")
    print(f"  started_at:    {base['run'].get('started_at')}")
    cfg = base["run"].get("scale_track_config", {})
    if cfg:
        print(f"  decoders:      {', '.join(cfg.get('decoders', []))}")
        print(f"  rss_sample_ms: {cfg.get('rss_sample_ms')}")
        print(f"  input files:   {cfg.get('input_count')} "
              f"(total {_fmt_bytes(cfg.get('input_total_bytes', 0))})")

    _decoder_summary(base["results"])
    _cross_decoder_rss(base["results"])

    if args.compare:
        cmp = _load(args.compare)
        print()
        print(f"Compare: {args.compare}")
        print(f"  started_at:    {cmp['run'].get('started_at')}")
        return _regression_gate(base["results"], cmp["results"],
                                args.regression_pct)
    return 0


if __name__ == "__main__":
    sys.exit(main())
