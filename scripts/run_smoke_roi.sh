#!/usr/bin/env bash
# SP3.2b ROI smoke variant: same 90-file synthetic-iter manifest as
# run_smoke.sh, but with a fixed center-50% region (512x512@256,256 on
# the 1024^2 rasters). Used by the SP3.2b decision gate.
#
# Usage: ./scripts/run_smoke_roi.sh [extra args forwarded to run_bench.sh]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MANIFEST="${ROOT}/corpus/synthetic-iter.txt"
if [ ! -r "$MANIFEST" ]; then
  echo "missing $MANIFEST; regenerate via scripts/select_synthetic_iter.py" >&2
  exit 1
fi
exec "$ROOT/scripts/run_bench.sh" --include-from "$MANIFEST" \
     --roi 512x512@256,256 "$@"
