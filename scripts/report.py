#!/usr/bin/env python3
"""Summarize jp2k-bench JSON results into a comparison table.

Usage:
  report.py results.json                  # print a per-file comparison
  report.py --baseline a.json --compare b.json   # diff two runs
"""

from __future__ import annotations

import argparse
import json
import sys
from collections import defaultdict
from pathlib import Path


def load(path: str) -> list[dict]:
    with open(path) as f:
        return json.load(f)


def key(row: dict) -> tuple:
    return (row["file"], row["decoder"], row["threads"])


def print_table(rows: list[dict]) -> None:
    # Group by (file, threads); columns = decoders.
    by_file: dict[tuple, dict[str, dict]] = defaultdict(dict)
    decoders: list[str] = []
    for r in rows:
        by_file[(r["file"], r["threads"])][r["decoder"]] = r
        if r["decoder"] not in decoders:
            decoders.append(r["decoder"])

    print(f"{'file':<50} {'threads':>7} ", end="")
    for d in decoders:
        print(f"{d+' MP/s':>14} ", end="")
    if len(decoders) >= 2:
        print(f"{'speedup':>9}", end="")
    print()

    for (path, threads), col in sorted(by_file.items()):
        short = Path(path).name
        print(f"{short[:50]:<50} {threads:>7} ", end="")
        mps = {}
        for d in decoders:
            r = col.get(d)
            if r and not r.get("error"):
                mps[d] = r["mpx_per_s"]
                print(f"{r['mpx_per_s']:>14.2f} ", end="")
            else:
                print(f"{'-':>14} ", end="")
        if len(decoders) >= 2:
            a, b = decoders[0], decoders[1]
            if a in mps and b in mps and mps[a] > 0:
                print(f"{mps[b]/mps[a]:>8.2f}x", end="")
        # Flag pixel mismatches.
        for d in decoders[1:]:
            r = col.get(d)
            if r and r.get("pixel_match") == 0:
                print("  PIXEL-MISMATCH", end="")
        print()


def diff(baseline: list[dict], compare: list[dict]) -> None:
    a = {key(r): r for r in baseline}
    b = {key(r): r for r in compare}
    common = sorted(set(a) & set(b))
    print(f"{'file':<50} {'decoder':>10} {'thr':>4} "
          f"{'base MP/s':>11} {'new MP/s':>11} {'delta':>9}")
    for k in common:
        ra, rb = a[k], b[k]
        if ra.get("error") or rb.get("error"):
            continue
        base = ra["mpx_per_s"]; new = rb["mpx_per_s"]
        delta = (new - base) / base * 100 if base > 0 else 0
        path, dec, thr = k
        short = Path(path).name
        print(f"{short[:50]:<50} {dec:>10} {thr:>4} "
              f"{base:>11.2f} {new:>11.2f} {delta:>+8.1f}%")


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("results", nargs="?", help="single results.json")
    p.add_argument("--baseline")
    p.add_argument("--compare")
    args = p.parse_args()

    if args.baseline and args.compare:
        diff(load(args.baseline), load(args.compare))
        return 0
    if not args.results:
        p.print_help(); return 2
    print_table(load(args.results))
    return 0


if __name__ == "__main__":
    sys.exit(main())
