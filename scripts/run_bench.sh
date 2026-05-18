#!/usr/bin/env bash
# Run jp2k-bench across every .jp2/.j2k file under one or more paths.
# Writes results to stdout; progress to stderr.
#
# Defaults to JSONL output: one self-describing JSON object per line, flushed
# after each (file, decoder, threads) record. A partial run survives any kill
# — parse stdout line-by-line. Set JSONL=0 to fall back to a single JSON doc.
#
# Heavy-file iter override: the LoC scans and Sentinel-2 bands take tens of
# seconds per decode and dominate the run wall clock. Files whose path matches
# HEAVY_PATTERN run with HEAVY_ITERS instead of ITERS.
#
# Example:
#   ./scripts/run_bench.sh --threads 1,8 corpus/ > results.jsonl
#
# Recommended root is `corpus/` (not `corpus/public/`) so the synthetic
# parameter sweep — built by scripts/gen_corpus.sh, one axis varied at a
# time — runs alongside the public buckets. The public conformance suite
# does not vary parameter axes cleanly; the synthetic sweep is what gives
# controlled-variable signal when a regression lands.
#
# Crash-proof example (recommended for the full corpus):
#   nohup ./scripts/run_bench.sh corpus/ \
#         > results/full_corpus_$(date +%Y%m%d_%H%M%S).jsonl \
#         2> results/full_corpus_$(date +%Y%m%d_%H%M%S).log &
#   # detach the shell; the bench keeps running on SIGHUP.

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
JSONL="${JSONL:-1}"
HEAVY_ITERS="${HEAVY_ITERS:-5}"
# Default targets the public corpus buckets outside the openjpeg-data tree.
# Override to disable: HEAVY_PATTERN='' or HEAVY_ITERS=0.
HEAVY_PATTERN="${HEAVY_PATTERN:-(loc-maps|remote-sensing)/}"
EXTRA=()
PATHS=()

INCLUDE_NONREGRESSION=0  # default: exclude */nonregression/* from perf runs

while [ $# -gt 0 ]; do
  case "$1" in
    --iters)   ITERS="$2"; shift 2 ;;
    --warmup)  WARMUP="$2"; shift 2 ;;
    --threads) THREADS="$2"; shift 2 ;;
    --include-nonregression) INCLUDE_NONREGRESSION=1; shift ;;
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
# Always skip dot-directories (e.g. .archived/ in the corpus). The
# nonregression exclude is the upstream input/nonregression/ bucket only
# — *not* baseline/nonregression/, which holds known-good encoder outputs
# (Bretagne_*, etc) that decode fine. Use --include-nonregression to
# bench against the upstream CVE/fuzzer corpus (intended only for the
# --correctness track; run_correctness.sh is the right entry point).
if [ "$INCLUDE_NONREGRESSION" = "0" ]; then
  PRUNE_EXPR=( -type d \( -name '.*' -o -path '*/input/nonregression' \) -prune )
else
  PRUNE_EXPR=( -type d -name '.*' -prune )
fi
for p in "${PATHS[@]}"; do
  if [ -d "$p" ]; then
    while IFS= read -r -d '' f; do FILES+=("$f"); done < \
      <(find -L "$p" \( "${PRUNE_EXPR[@]}" \) -o \
            \( -type f \( -iname '*.jp2' -o -iname '*.j2k' -o -iname '*.jpc' \) -print0 \) | sort -z)
  elif [ -f "$p" ]; then
    FILES+=("$p")
  fi
done

if [ ${#FILES[@]} -eq 0 ]; then
  echo "no .jp2/.j2k files found" >&2
  exit 2
fi

FLAGS=( --iters "$ITERS" --warmup "$WARMUP" --threads "$THREADS" )
if [ "$JSONL" = "1" ]; then
  FLAGS+=( --jsonl )
fi
if [ -n "$HEAVY_PATTERN" ] && [ "$HEAVY_ITERS" -gt 0 ]; then
  FLAGS+=( --heavy-pattern "$HEAVY_PATTERN" --heavy-iters "$HEAVY_ITERS" )
fi

# Provenance: tell the bench HOW we assembled the file list so a future
# bisect can distinguish "more files added" from "real perf change."
# Hand-rolled JSON (no jq dependency); keys mirror what's tunable above.
roots_json=$(printf '"%s",' "${PATHS[@]}" | sed 's/,$//')
SPEC="{\"roots\": [${roots_json}]"
SPEC="${SPEC}, \"find_extensions\": [\".jp2\", \".j2k\", \".jpc\"]"
SPEC="${SPEC}, \"find_follows_symlinks\": true"
if [ "$INCLUDE_NONREGRESSION" = "0" ]; then
  SPEC="${SPEC}, \"exclude_globs\": [\"*/.*/*\", \"*/nonregression/*\"]"
else
  SPEC="${SPEC}, \"exclude_globs\": [\"*/.*/*\"]"
fi
SPEC="${SPEC}, \"include_nonregression\": $([ \"$INCLUDE_NONREGRESSION\" = \"1\" ] && echo true || echo false)"
SPEC="${SPEC}, \"heavy_pattern\": \"${HEAVY_PATTERN}\""
SPEC="${SPEC}, \"heavy_iters\": ${HEAVY_ITERS}"
SPEC="${SPEC}, \"iters\": ${ITERS}"
SPEC="${SPEC}, \"warmup\": ${WARMUP}"
SPEC="${SPEC}}"
FLAGS+=( --corpus-spec "$SPEC" )

echo "[run] $BIN ${FLAGS[*]} ${EXTRA[*]:-} (${#FILES[@]} files)" >&2
"$BIN" "${FLAGS[@]}" "${EXTRA[@]}" "${FILES[@]}"
