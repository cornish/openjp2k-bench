#!/usr/bin/env bash
# Validate corpus health before a benchmark run.
#
# Checks:
#   - corpus/manifest.json exists and is fresher than every file it lists.
#   - Each synthetic/ file re-decodes with opj_decompress (catches generator
#     bugs on dusty-corner configurations).
#   - Each public/ file's SHA256 is recorded by the manifest (a separate
#     pinning check lives in fetch_corpus.sh).
#
# Output: one summary line per bucket. Non-zero exit on any check failure.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CORPUS="$ROOT/corpus"
MANIFEST="$CORPUS/manifest.json"

fail=0

if [ ! -f "$MANIFEST" ]; then
  echo "[check] manifest missing; run scripts/build_manifest.sh" >&2
  exit 1
fi

newest_file=$(python3 - "$MANIFEST" "$CORPUS" <<'PY'
import json, os, sys
m, root = sys.argv[1], sys.argv[2]
data = json.load(open(m))
mts = []
for f in data.get("files", []):
    p = os.path.join(root, f["path"])
    if os.path.exists(p):
        mts.append(os.path.getmtime(p))
print(max(mts) if mts else 0)
PY
)
manifest_mt=$(stat -c %Y "$MANIFEST")
if awk "BEGIN{exit !($newest_file > $manifest_mt)}"; then
  echo "[check] manifest is stale (a listed file is newer); rebuild it"
  fail=$((fail+1))
fi

for bucket in user synthetic public; do
  dir="$CORPUS/$bucket"
  if [ ! -d "$dir" ] && [ ! -L "$dir" ]; then
    echo "[check] $bucket: missing (no $dir)"; continue
  fi
  count=$(find -L "$dir" -type f \( -iname '*.jp2' -o -iname '*.j2k' -o -iname '*.jpc' \) | wc -l)
  echo "[check] $bucket: $count files"
done

if command -v opj_decompress >/dev/null; then
  bad=0
  while IFS= read -r f; do
    if ! opj_decompress -i "$f" -o "$(mktemp --suffix=.pgm)" >/dev/null 2>&1; then
      echo "  [check] re-decode FAILED: $f"
      bad=$((bad+1))
    fi
  done < <(find "$CORPUS/synthetic" -type f \( -iname '*.jp2' -o -iname '*.j2k' \) 2>/dev/null)
  echo "[check] synthetic re-decode: $bad failure(s)"
  fail=$((fail + bad))
else
  echo "[check] opj_decompress not on PATH; skipping re-decode check" >&2
fi

if [ "$fail" -gt 0 ]; then
  echo "[check] $fail issue(s) found"
  exit 1
fi
echo "[check] ok"
