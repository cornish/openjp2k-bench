#!/usr/bin/env bash
# Run jp2k-bench across every .jp2/.j2k file under one or more paths.
# Writes JSON to stdout; progress to stderr.
#
# Example:
#   ./scripts/run_bench.sh --threads 1,8 corpus/ > results.json

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${BIN:-$ROOT/build/jp2k-bench}"

if [ ! -x "$BIN" ]; then
  echo "missing $BIN; run scripts/build.sh first" >&2
  exit 1
fi

ITERS="${ITERS:-20}"
WARMUP="${WARMUP:-2}"
THREADS="${THREADS:-1}"
EXTRA=()
PATHS=()

while [ $# -gt 0 ]; do
  case "$1" in
    --iters)   ITERS="$2"; shift 2 ;;
    --warmup)  WARMUP="$2"; shift 2 ;;
    --threads) THREADS="$2"; shift 2 ;;
    --)        shift; while [ $# -gt 0 ]; do PATHS+=("$1"); shift; done ;;
    -*)        EXTRA+=("$1"); shift ;;
    *)         PATHS+=("$1"); shift ;;
  esac
done

if [ ${#PATHS[@]} -eq 0 ]; then
  echo "usage: run_bench.sh [--iters N] [--warmup N] [--threads N[,N...]] path..." >&2
  exit 2
fi

FILES=()
for p in "${PATHS[@]}"; do
  if [ -d "$p" ]; then
    while IFS= read -r -d '' f; do FILES+=("$f"); done < \
      <(find "$p" -type f \( -iname '*.jp2' -o -iname '*.j2k' -o -iname '*.jpc' \) -print0 | sort -z)
  elif [ -f "$p" ]; then
    FILES+=("$p")
  fi
done

if [ ${#FILES[@]} -eq 0 ]; then
  echo "no .jp2/.j2k files found" >&2
  exit 2
fi

echo "[run] $BIN --iters $ITERS --warmup $WARMUP --threads $THREADS ${EXTRA[*]:-} (${#FILES[@]} files)" >&2
"$BIN" --iters "$ITERS" --warmup "$WARMUP" --threads "$THREADS" "${EXTRA[@]}" "${FILES[@]}"
