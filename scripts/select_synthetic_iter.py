#!/usr/bin/env python3
"""Pick a deterministic stratified subset of corpus/synthetic/ for the
iteration-loop bench.

The full synthetic sweep is ~3360 files and runs for hours. For per-commit
runs we want a ~150-file subset that exercises each parametric axis at
least once but doesn't take 20 minutes to bench. The full sweep stays as
the per-deliverable check.

Restriction: 1024² rasters only. 4096² rasters take 5–10× longer per
decode and push iteration wall time past an hour for ~zero additional
signal at this cadence. If you need 4096² signal during iteration you're
working on a problem where iteration-loop is the wrong tool — use the
full sweep with the bench's --heavy-pattern instead.

Stratification guarantee:
  Every individual axis *level* (e.g. progression=LRCP, decomp=5, etc.)
  appears at least once in the subset, per raster.

Stratification non-guarantee:
  Every (progression × decomp × tile × rate × cblk × ...) cross-product
  cell is NOT covered — that's by design. Cross-axis interactions are
  the deliverable-run's job. If a regression localizes to a specific
  axis level the subset will catch it; if it only manifests on a
  combination, the full sweep will catch it.

Output: a newline-delimited manifest at corpus/synthetic-iter.txt
(default), suitable for `run_bench.sh --include-from`.

Determinism: a fixed seed is used so the same file list is produced
across machines and across runs unless --seed is overridden.
"""
from __future__ import annotations

import argparse
import os
import random
import re
import sys
from collections import defaultdict
from pathlib import Path

# Synthetic filenames look like
#   pLRCP_d5_b64_t1024x1024_lossy_l4_mon_esop_eph.jp2
# encoding: progression, decomp, cblk, tile, rate, layers, mct, sop/eph.
TAG_RE = re.compile(
    r"^p(?P<progression>[A-Z]+)"
    r"_d(?P<decomp>\d+)"
    r"_b(?P<cblk>\d+)"
    r"_t(?P<tile>[^_]+)"
    r"_(?P<rate>lossless|lossy)"
    r"_l(?P<layers>\d+)"
    r"_m(?P<mct>on|off)"
    r"_e(?P<sop_eph>[A-Za-z_]+)"
    r"\.(?P<container>jp2|j2k)$"
)

ITER_RASTERS = ("rgb8_1024", "mono16_1024", "mono12_1024")
AXES = ("progression", "decomp", "cblk", "tile", "rate",
        "layers", "mct", "sop_eph", "container")


def parse_tag(path: str) -> dict | None:
    m = TAG_RE.match(os.path.basename(path))
    if not m:
        return None
    return {**m.groupdict(), "path": path}


def stratified_pick(records: list[dict], target_n: int, seed: int) -> list[dict]:
    """Greedy stratified picker.

    Pass 1: for each (axis, level) pair, ensure at least one record using
    that level is in the chosen set. Tie-broken deterministically by the
    seeded RNG.

    Pass 2: fill the remainder up to target_n with a random sample of
    the unchosen records (also seeded).
    """
    rng = random.Random(seed)
    chosen: set[str] = set()
    # Pass 1: axis-level coverage.
    for axis in AXES:
        levels_seen: set[str] = set()
        for r in records:
            levels_seen.add(r[axis])
        for level in sorted(levels_seen):
            candidates = [r for r in records if r[axis] == level
                          and r["path"] not in chosen]
            if not candidates:
                continue  # already covered by a prior pick
            pick = rng.choice(candidates)
            chosen.add(pick["path"])
    # Pass 2: fill remainder. Deterministic shuffle of the leftovers.
    remaining = [r for r in records if r["path"] not in chosen]
    rng.shuffle(remaining)
    while remaining and len(chosen) < target_n:
        chosen.add(remaining.pop()["path"])
    return [r for r in records if r["path"] in chosen]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", type=Path,
                    default=Path(__file__).resolve().parent.parent / "corpus" / "synthetic",
                    help="synthetic corpus root (default: ../corpus/synthetic/)")
    ap.add_argument("--per-raster", type=int, default=50,
                    help="target subset size per raster type (default 50)")
    ap.add_argument("--out", type=Path,
                    default=Path(__file__).resolve().parent.parent / "corpus" / "synthetic-iter.txt",
                    help="manifest output path")
    ap.add_argument("--seed", type=int, default=20260518,
                    help="RNG seed for deterministic selection (default 20260518)")
    args = ap.parse_args()

    if not args.root.is_dir():
        sys.exit(f"no synthetic corpus at {args.root}")

    total_picked: list[dict] = []
    skipped_rasters: list[str] = []
    for raster in ITER_RASTERS:
        rd = args.root / raster
        if not rd.is_dir():
            skipped_rasters.append(raster)
            continue
        records: list[dict] = []
        for p in sorted(rd.iterdir()):
            if p.is_file():
                rec = parse_tag(str(p))
                if rec:
                    rec["raster"] = raster
                    records.append(rec)
        if not records:
            skipped_rasters.append(raster)
            continue
        picked = stratified_pick(records, args.per_raster, args.seed)
        total_picked.extend(picked)
        print(f"[iter-subset] {raster:<14} pool={len(records):<4}  picked={len(picked)}",
              file=sys.stderr)

    if skipped_rasters:
        print(f"[iter-subset] warning: skipped (empty/missing): "
              f"{', '.join(skipped_rasters)}", file=sys.stderr)
    if not total_picked:
        sys.exit("no files picked — is the corpus generated?")

    # Manifest paths are written relative to the repo root so the output
    # composes with run_bench.sh's PATHS array without env coupling.
    repo_root = Path(__file__).resolve().parent.parent
    with open(args.out, "w") as f:
        f.write("# Stratified iteration-loop subset of corpus/synthetic/.\n")
        f.write(f"# Generated by {os.path.relpath(__file__, repo_root)} "
                f"--seed {args.seed} --per-raster {args.per_raster}.\n")
        f.write("# Guarantee: every individual axis level (progression, decomp,\n")
        f.write("# cblk, tile, rate, layers, mct, sop_eph, container) appears at\n")
        f.write("# least once per raster. Cross-axis interactions are NOT covered\n")
        f.write("# — that's the full-sweep deliverable run's job.\n")
        f.write(f"# Total files: {len(total_picked)}\n")
        for r in sorted(total_picked, key=lambda r: r["path"]):
            rel = os.path.relpath(r["path"], repo_root)
            f.write(f"{rel}\n")
    print(f"[iter-subset] wrote {args.out} ({len(total_picked)} files)",
          file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
