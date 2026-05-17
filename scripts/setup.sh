#!/usr/bin/env bash
# Fetch openjpeg, grok, and openjp2k at pinned commits into third_party/.
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
GROK_REF="v20.3.3"   # adjust if API changes break adapter_grok.cpp

OPENJP2K_URL="https://github.com/cornish/openjp2k.git"
OPENJP2K_REF="101b3a0b60073a7236acbe2d0b1ccf4512a30b0e"   # main @ 2026-05-17
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
  git -C "$dest" submodule update --init --recursive --depth 1
}

clone_or_update "$OPENJPEG_URL" "$OPENJPEG_REF" "$TP/openjpeg"

if [ "${SKIP_GROK:-0}" = "1" ]; then
  echo "[setup] SKIP_GROK=1; not fetching grok"
else
  clone_or_update "$GROK_URL" "$GROK_REF" "$TP/grok"
fi

if [ "${SKIP_OPENJP2K:-0}" = "1" ]; then
  echo "[setup] SKIP_OPENJP2K=1; not fetching openjp2k"
else
  clone_or_update "$OPENJP2K_URL" "$OPENJP2K_REF" "$TP/openjp2k"
fi

echo "[setup] done. Next: ./scripts/build.sh"
