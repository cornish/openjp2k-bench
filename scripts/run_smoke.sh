#!/usr/bin/env bash
# Fast smoke / inner-loop perf bench.
#
# Runs jp2k-bench against the synthetic-iter subset only (~150 files,
# 1024² rasters with each parametric axis level hit at least once). No
# public corpus. Use for "did this commit move the needle?" feedback
# during inner-loop dev.
#
# Wall time on the dev host: ~9 minutes (90 files × 3 decoders × iters=20).
# Smoke is a tripwire, not a measurement: p90 intra-row timing stddev
# (~3% on this host) is wider than the openjp2k-vs-openjpeg geomean gap
# (~1%). Use the iteration or deliverable tier for an actual perf call.
#
# When you want broader coverage, escalate:
#   - Iteration / pre-commit (~50 min, 401 files):
#       ./scripts/run_bench.sh --include-from corpus/synthetic-iter.txt corpus/public/
#   - Deliverable / full sweep (hours):
#       ./scripts/run_bench.sh corpus/
#
# Output: results/smoke_<timestamp>.jsonl by default. Pipe elsewhere if
# you want a different destination. Pass-through args go to run_bench.sh
# (e.g. --threads 1,8, --iters 5 for an even faster smoke).

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MANIFEST="${ROOT}/corpus/synthetic-iter.txt"

if [ ! -r "$MANIFEST" ]; then
  echo "missing $MANIFEST; regenerate via scripts/select_synthetic_iter.py" >&2
  exit 1
fi

exec "$ROOT/scripts/run_bench.sh" --include-from "$MANIFEST" "$@"
