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
# Files matching this regex run with HEAVY_ITERS instead of ITERS. The
# default covers (a) the openjpeg-data LoC/remote-sensing buckets when
# someone has restored them (rare now that those live in corpus/scale/),
# and (b) the synthetic 4096² rasters, which decode 5–10× slower than
# 1024² and dominate wall time at default --iters 20.
# Override to disable: HEAVY_PATTERN='' or HEAVY_ITERS=0.
HEAVY_PATTERN="${HEAVY_PATTERN:-(loc-maps|remote-sensing|/(rgb8|mono16)_4096)/}"
EXTRA=()
PATHS=()
INCLUDE_FROM_FILES=()  # --include-from manifests (one path per line)

INCLUDE_NONREGRESSION=0  # default: exclude */nonregression/* from perf runs

while [ $# -gt 0 ]; do
  case "$1" in
    --iters)   ITERS="$2"; shift 2 ;;
    --warmup)  WARMUP="$2"; shift 2 ;;
    --threads) THREADS="$2"; shift 2 ;;
    --include-nonregression) INCLUDE_NONREGRESSION=1; shift ;;
    --include-from) INCLUDE_FROM_FILES+=("$2"); shift 2 ;;
    --roi|--roi-tile|--decoder|--heavy-iters|--heavy-pattern|--concurrent-files|--rss-sample-ms|--memory-max-bytes)
               EXTRA+=("$1" "$2"); shift 2 ;;
    --)        shift; while [ $# -gt 0 ]; do PATHS+=("$1"); shift; done ;;
    -*)        EXTRA+=("$1"); shift ;;
    *)         PATHS+=("$1"); shift ;;
  esac
done

if [ ${#PATHS[@]} -eq 0 ] && [ ${#INCLUDE_FROM_FILES[@]} -eq 0 ]; then
  echo "usage: run_bench.sh [--iters N] [--warmup N] [--threads N[,N...]] \\" >&2
  echo "                    [--include-from MANIFEST] [--include-nonregression] path..." >&2
  exit 2
fi

# --include-from MANIFEST: each non-comment, non-empty line is a path
# relative to the repo root (or absolute). Used by the synthetic
# iteration-loop subset — scripts/select_synthetic_iter.py emits a
# stable manifest of ~150 stratified files.
for mf in "${INCLUDE_FROM_FILES[@]}"; do
  if [ ! -r "$mf" ]; then
    echo "--include-from: cannot read $mf" >&2
    exit 2
  fi
  while IFS= read -r line; do
    line="${line%%#*}"   # strip comment
    line="${line## }"; line="${line%% }"  # trim
    [ -z "$line" ] && continue
    PATHS+=("$line")
  done < "$mf"
done

FILES=()
# Always skip dot-directories (e.g. .archived/ in the corpus). The
# nonregression exclude is the upstream input/nonregression/ bucket only
# — *not* baseline/nonregression/, which holds known-good encoder outputs
# (Bretagne_*, etc) that decode fine. Use --include-nonregression to
# bench against the upstream CVE/fuzzer corpus (intended only for the
# --correctness track; run_correctness.sh is the right entry point).
# Always prune corpus/public/scale/ from perf walks — those files are
# huge (>25 MB) and only safe to bench under the scale-track wrapper
# (scripts/run_scale.sh) which puts each invocation under a per-decoder
# memory cap. Running them through the main bench path with three
# decoders dlopen'd in one process risks OOM-cascading the host.
if [ "$INCLUDE_NONREGRESSION" = "0" ]; then
  PRUNE_EXPR=( -type d \( -name '.*' -o -path '*/input/nonregression' -o -path '*/scale' \) -prune )
else
  PRUNE_EXPR=( -type d \( -name '.*' -o -path '*/scale' \) -prune )
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

FLAGS=( --iters "$ITERS" --warmup "$WARMUP" )
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
  SPEC="${SPEC}, \"exclude_globs\": [\"*/.*/*\", \"*/nonregression/*\", \"*/scale/*\"]"
else
  SPEC="${SPEC}, \"exclude_globs\": [\"*/.*/*\", \"*/scale/*\"]"
fi
SPEC="${SPEC}, \"include_nonregression\": $([ \"$INCLUDE_NONREGRESSION\" = \"1\" ] && echo true || echo false)"
SPEC="${SPEC}, \"heavy_pattern\": \"${HEAVY_PATTERN}\""
SPEC="${SPEC}, \"heavy_iters\": ${HEAVY_ITERS}"
SPEC="${SPEC}, \"iters\": ${ITERS}"
SPEC="${SPEC}, \"warmup\": ${WARMUP}"
if [ ${#INCLUDE_FROM_FILES[@]} -gt 0 ]; then
  imf_json=$(printf '"%s",' "${INCLUDE_FROM_FILES[@]}" | sed 's/,$//')
  SPEC="${SPEC}, \"include_from\": [${imf_json}]"
fi
SPEC="${SPEC}}"
FLAGS+=( --corpus-spec "$SPEC" )

# --threads N1,N2,... runs the bench once per thread count in a fresh
# jp2k-bench process.  Grok's adapter (adapter_grok.cpp) initializes
# its singleton thread pool ONCE at first --threads value and ignores
# subsequent set_threads() calls, so an internal sweep within a single
# process produces wrong grok numbers for all-but-first thread counts.
# Each thread count gets its own process; result rows already carry
# `threads` so downstream analysis is unchanged.  Single-value
# --threads N is identical to the old behavior modulo this loop.
IFS=',' read -ra THREAD_LIST <<< "$THREADS"
for T in "${THREAD_LIST[@]}"; do
  T_trim="${T## }"; T_trim="${T_trim%% }"
  [ -z "$T_trim" ] && continue
  echo "[run] $BIN ${FLAGS[*]} --threads $T_trim ${EXTRA[*]:-} (${#FILES[@]} files)" >&2
  "$BIN" "${FLAGS[@]}" --threads "$T_trim" "${EXTRA[@]}" "${FILES[@]}"
done
