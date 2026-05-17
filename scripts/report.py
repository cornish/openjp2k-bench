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
import csv
import json
import math
import sys
from collections import defaultdict
from pathlib import Path
from statistics import median
from typing import Callable


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
    p = corpus_root / "manifest.json"
    if not p.exists():
        return {}
    data = json.loads(p.read_text())
    return {f["path"]: f for f in data.get("files", []) if "path" in f}


def _annotate(results: list[dict], manifest: dict[str, dict]) -> None:
    for r in results:
        rel = Path(r["file"])
        for k in manifest:
            if str(rel).endswith(k):
                r.update({f"_m_{kk}": vv for kk, vv in manifest[k].items()
                          if kk not in ("path", "sha256")})
                r["_bucket"] = manifest[k].get("bucket", _bucket_of(str(rel)))
                break
        else:
            r["_bucket"] = _bucket_of(str(rel))


def _parse_filter(expr: str) -> Callable[[dict], bool]:
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


def _row_key(r: dict) -> tuple:
    """Identity tuple for pairing baseline and compare runs.
    Same file + decoder + threads + roi-or-not + ROI dims."""
    roi = r.get("roi")
    roi_key = (roi["x0"], roi["y0"], roi["x1"], roi["y1"]) if roi else None
    return (r["file"], r["decoder"], r["threads"], roi_key)


# Tab-separated dump of result rows; flat schema for downstream perf-log
# tooling that doesn't want to walk the JSON tree.
_TSV_COLUMNS = [
    "file", "decoder", "decoder_version", "threads", "has_roi",
    "roi_x0", "roi_y0", "roi_x1", "roi_y1",
    "width", "height", "channels", "bit_depth",
    "iters", "warmup",
    "t_min_s", "t_p50_s", "t_p90_s", "t_p99_s", "t_max_s",
    "t_mean_s", "t_stddev_s",
    "megapixels_per_sec",
    "pixel_match", "pixel_psnr_db",
    "rss_peak_kb", "rss_delta_kb",
    "reused_codec",
    "error",
]


def _write_tsv(path: Path, results: list[dict]) -> None:
    with open(path, "w", newline="") as f:
        w = csv.writer(f, delimiter="\t", lineterminator="\n")
        w.writerow(_TSV_COLUMNS)
        for r in results:
            roi = r.get("roi") or {}
            ts = r.get("timing_s") or {}
            w.writerow([
                r.get("file", ""),
                r.get("decoder", ""),
                r.get("decoder_version", ""),
                r.get("threads", ""),
                "1" if r.get("roi") else "0",
                roi.get("x0", ""), roi.get("y0", ""),
                roi.get("x1", ""), roi.get("y1", ""),
                r.get("width", ""), r.get("height", ""),
                r.get("channels", ""), r.get("bit_depth", ""),
                r.get("iters", ""), r.get("warmup", ""),
                ts.get("min", ""), ts.get("p50", ""),
                ts.get("p90", ""), ts.get("p99", ""),
                ts.get("max", ""),
                ts.get("mean", ""), ts.get("stddev", ""),
                r.get("megapixels_per_sec", ""),
                r.get("pixel_match", ""),
                r.get("pixel_psnr_db") if r.get("pixel_psnr_db") is not None else "",
                r.get("rss_peak_kb", ""),
                r.get("rss_delta_kb", ""),
                "1" if r.get("reused_codec") else "0",
                r.get("error", ""),
            ])


def _normal_sf(z: float) -> float:
    """Upper-tail survival function of the standard normal. erfc-based;
    stdlib-only, no scipy. ~1e-7 accurate, fine for our gate."""
    return 0.5 * math.erfc(z / math.sqrt(2.0))


def _wilcoxon_signed_rank(deltas: list[float]) -> tuple[float, float, int]:
    """Two-sided Wilcoxon signed-rank test on paired deltas.

    Returns (W_plus, p_value, n_effective). Zero deltas are dropped per
    Pratt's recommendation; ties get average ranks. Uses the normal
    approximation with continuity correction (valid for n >= ~10).
    """
    nz = [d for d in deltas if d != 0.0]
    n = len(nz)
    if n == 0:
        return (0.0, 1.0, 0)
    abs_d = sorted(((abs(d), 1 if d > 0 else -1) for d in nz), key=lambda x: x[0])
    # Average-rank assignment over ties on |d|.
    ranks = [0.0] * n
    i = 0
    while i < n:
        j = i
        while j + 1 < n and abs_d[j + 1][0] == abs_d[i][0]:
            j += 1
        avg = (i + j) / 2.0 + 1.0  # ranks are 1-based
        for k in range(i, j + 1):
            ranks[k] = avg
        i = j + 1
    w_plus = sum(r for r, (_, s) in zip(ranks, abs_d) if s > 0)
    # Tie-correction adjustment to the variance.
    tie_term = 0.0
    i = 0
    while i < n:
        j = i
        while j + 1 < n and abs_d[j + 1][0] == abs_d[i][0]:
            j += 1
        t = j - i + 1
        if t > 1:
            tie_term += (t ** 3 - t)
        i = j + 1
    mean = n * (n + 1) / 4.0
    var = n * (n + 1) * (2 * n + 1) / 24.0 - tie_term / 48.0
    if var <= 0:
        return (w_plus, 1.0, n)
    # Continuity correction.
    if w_plus > mean:
        z = (w_plus - 0.5 - mean) / math.sqrt(var)
    elif w_plus < mean:
        z = (w_plus + 0.5 - mean) / math.sqrt(var)
    else:
        z = 0.0
    p = 2.0 * _normal_sf(abs(z))
    return (w_plus, min(1.0, p), n)


def _print_gate(base_results: list[dict], cmp_results: list[dict],
                alpha: float, regression_threshold_pct: float) -> int:
    """§2.7 merge-gate decision. Returns process exit code: 0 pass, 1 fail.

    Pass criteria:
      - No paired row regresses by more than `regression_threshold_pct` percent
        on min decode time (a hard floor that ignores significance).
      - The Wilcoxon signed-rank test on (compare - baseline) min decode time
        either shows no significant change (p > alpha) or a significant
        improvement (signed sum negative => compare faster).
    """
    cmp_idx = {_row_key(r): r for r in cmp_results}
    deltas: list[float] = []          # (cmp_min - base_min) seconds
    pct_deltas: list[tuple[str, float]] = []  # (label, pct)
    for b in base_results:
        if b.get("error"):
            continue
        c = cmp_idx.get(_row_key(b))
        if not c or c.get("error"):
            continue
        bm = b["timing_s"]["min"]
        cm = c["timing_s"]["min"]
        if bm <= 0:
            continue
        deltas.append(cm - bm)
        pct = (cm - bm) / bm * 100.0
        label = f"{Path(b['file']).name[:32]} {b['decoder']} t={b['threads']}"
        pct_deltas.append((label, pct))

    if not deltas:
        print("gate: no paired rows; nothing to compare", file=sys.stderr)
        return 1

    w, p, n = _wilcoxon_signed_rank(deltas)
    mean_d = sum(deltas) / len(deltas)
    median_d = median(deltas)
    median_pct = median(p for _, p in pct_deltas)
    worst = max(pct_deltas, key=lambda x: x[1])

    print()
    print("-- §2.7 merge gate --")
    print(f"paired rows           : {len(deltas)}")
    print(f"median Δmin           : {median_d*1e3:+.3f} ms ({median_pct:+.2f}%)")
    print(f"mean Δmin             : {mean_d*1e3:+.3f} ms")
    print(f"worst-case regression : {worst[1]:+.2f}%  ({worst[0]})")
    print(f"Wilcoxon W+           : {w:.1f}")
    print(f"signed-rank p-value   : {p:.4g}  (n_eff={n})")

    verdict_lines: list[str] = []
    failed = False
    if worst[1] > regression_threshold_pct:
        verdict_lines.append(
            f"FAIL: worst-case regression {worst[1]:+.2f}% exceeds "
            f"threshold {regression_threshold_pct:+.2f}%")
        failed = True
    if p < alpha and median_d > 0:
        verdict_lines.append(
            f"FAIL: significant slowdown (p={p:.4g} < α={alpha}, "
            f"median Δ={median_d*1e3:+.3f} ms)")
        failed = True
    if not failed:
        if p < alpha and median_d < 0:
            verdict_lines.append(
                f"PASS: significant speedup (p={p:.4g}, median Δ={median_d*1e3:+.3f} ms)")
        else:
            verdict_lines.append(
                f"PASS: no significant regression (p={p:.4g})")
    for line in verdict_lines:
        print(line)
    return 1 if failed else 0


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
    ap.add_argument("--tsv", type=Path, default=None,
                    help="also write a flat TSV of baseline results to this path")
    ap.add_argument("--gate", action="store_true",
                    help="enable §2.7 merge-gate mode: print signed-rank verdict "
                         "and return non-zero exit on regression "
                         "(requires baseline + compare)")
    ap.add_argument("--alpha", type=float, default=0.05,
                    help="significance threshold for the gate's signed-rank test")
    ap.add_argument("--regression-threshold-pct", type=float, default=5.0,
                    help="hard ceiling on per-row regression in %% min-time; "
                         "any row over this fails the gate regardless of p-value")
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

    if args.tsv:
        _write_tsv(args.tsv, base["results"])
        print(f"[tsv] wrote {args.tsv}", file=sys.stderr)

    if args.gate:
        if not cmp:
            print("--gate requires both baseline and compare arguments", file=sys.stderr)
            return 2
        return _print_gate(base["results"], cmp["results"],
                           args.alpha, args.regression_threshold_pct)
    return 0


if __name__ == "__main__":
    sys.exit(main())
