#!/usr/bin/env bash
# Run jp2k-bench in --correctness mode against the nonregression bucket.
#
# Writes a JSONL stream of type="correctness" records describing whether
# each (file, decoder) pair decoded or was cleanly rejected, tagged with
# expected_outcome from the upstream OpenJPEG test config.
#
# Example:
#   ./scripts/run_correctness.sh corpus/public/conformance/openjpeg-data/input/nonregression \
#     > results/correctness_$(date +%Y%m%d_%H%M%S).jsonl
#
# Crash-proof equivalent:
#   nohup ./scripts/run_correctness.sh corpus/.../nonregression \
#         > results/correctness_${TS}.jsonl 2> results/correctness_${TS}.log &
#   disown

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${BIN:-$ROOT/build/jp2k-bench}"
OPENJPEG_SRC="${OPENJPEG_SRC:-$ROOT/external/openjpeg}"
# Fallback to the local fork tree for the classifier — the cmake list +
# ctest.in files live there too. The classifier doesn't care which fork
# it reads from, the blacklists are upstream-defined.
[ -d "$OPENJPEG_SRC/tests/nonregression" ] || \
  OPENJPEG_SRC="$HOME/GitHub/openjp2k"

if [ ! -x "$BIN" ]; then
  echo "missing $BIN; run scripts/build.sh first" >&2
  exit 1
fi
if [ ! -d "$OPENJPEG_SRC/tests/nonregression" ]; then
  echo "OPENJPEG_SRC=$OPENJPEG_SRC does not contain tests/nonregression/; " \
       "pass via env or run scripts/setup.sh" >&2
  exit 1
fi

if [ $# -eq 0 ]; then
  echo "usage: run_correctness.sh path/to/nonregression [more paths...]" >&2
  exit 2
fi

PATHS=("$@")

# 1. Regenerate the expected-fail list from upstream config. Cached to
#    /tmp; downstream perf runs don't read this so we don't need to
#    persist it in the repo.
CLS="${CLS:-/tmp/jp2kbench_expected_fail.txt}"
"$ROOT/scripts/classify_nonregression.py" \
  --openjpeg-source "$OPENJPEG_SRC" --format txt > "$CLS"
NCLS=$(grep -vcE '^(#|$)' "$CLS")
echo "[correctness] $NCLS expected-fail basenames from $OPENJPEG_SRC" >&2

# 2. Walk the input paths (file or directory). Same find logic as
#    run_bench.sh but without the dot-dir / nonregression exclusions —
#    here we WANT the nonregression files.
FILES=()
for p in "${PATHS[@]}"; do
  if [ -d "$p" ]; then
    while IFS= read -r -d '' f; do FILES+=("$f"); done < \
      <(find -L "$p" -type f \( -iname '*.jp2' -o -iname '*.j2k' -o -iname '*.jpc' \) -print0 | sort -z)
  elif [ -f "$p" ]; then
    FILES+=("$p")
  fi
done
if [ ${#FILES[@]} -eq 0 ]; then
  echo "no .jp2/.j2k files found under: ${PATHS[*]}" >&2
  exit 2
fi

# 3. Provenance: tell the bench this is correctness mode and which
#    classifier was used.
SPEC="{\"mode\": \"correctness\""
SPEC="${SPEC}, \"classifier\": \"$(realpath "$CLS")\""
SPEC="${SPEC}, \"classifier_source\": \"$(realpath "$OPENJPEG_SRC")\""
SPEC="${SPEC}, \"expected_fail_count\": ${NCLS}"
SPEC="${SPEC}, \"input_count\": ${#FILES[@]}}"

echo "[correctness] $BIN --correctness --jsonl (${#FILES[@]} files)" >&2
"$BIN" --correctness --jsonl --classification "$CLS" \
       --corpus-spec "$SPEC" "${FILES[@]}"
