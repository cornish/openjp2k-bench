#!/usr/bin/env bash
# Build corpus/manifest.json by scanning corpus/{user,synthetic,public}.
#
# Usage:
#   ./scripts/build_manifest.sh
#
# Re-run after adding/removing files in any bucket.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CORPUS="$ROOT/corpus"

mkdir -p "$CORPUS/user" "$CORPUS/synthetic" "$CORPUS/public"

python3 "$ROOT/scripts/manifest_tool.py" \
  --root "$CORPUS" \
  --out  "$CORPUS/manifest.json"
