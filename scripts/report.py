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
