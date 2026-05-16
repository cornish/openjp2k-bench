#!/usr/bin/env bash
# Fetch openjpeg and grok at pinned commits into third_party/.
# Idempotent: re-running updates pinned refs but skips clean clones.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TP="$ROOT/third_party"
mkdir -p "$TP"

# --- Pinned refs ------------------------------------------------------------
# Update these as upstream moves. Capture the version next to the SHA so it's
# easy to bump deliberately.
OPENJPEG_URL="https://github.com/uclouvain/openjpeg.git"
OPENJPEG_REF="v2.5.3"

GROK_URL="https://github.com/GrokImageCompression/grok.git"
GROK_REF="v13.0.0"   # adjust if API changes break adapter_grok.cpp
# ---------------------------------------------------------------------------

clone_or_update() {
  local url="$1" ref="$2" dest="$3"
  if [ -d "$dest/.git" ]; then
    echo "[setup] $dest exists; fetching $ref"
    git -C "$dest" fetch --tags --depth 1 origin "$ref" || git -C "$dest" fetch --tags origin
    git -C "$dest" checkout --detach "$ref"
  else
    echo "[setup] cloning $url@$ref -> $dest"
    git clone --depth 1 --branch "$ref" "$url" "$dest" || \
      (git clone "$url" "$dest" && git -C "$dest" checkout --detach "$ref")
  fi
}

clone_or_update "$OPENJPEG_URL" "$OPENJPEG_REF" "$TP/openjpeg"

if [ "${SKIP_GROK:-0}" = "1" ]; then
  echo "[setup] SKIP_GROK=1; not fetching grok"
else
  clone_or_update "$GROK_URL" "$GROK_REF" "$TP/grok"
fi

echo "[setup] done. Next: ./scripts/build.sh"
