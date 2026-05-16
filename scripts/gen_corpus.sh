#!/usr/bin/env bash
# Generate a parametric JPEG 2000 corpus from a seed image.
#
# Requires: opj_compress (apt: libopenjp2-tools, or build OpenJPEG with
#           BUILD_CODEC=ON), ImageMagick `magick` for resizing.
#
# Produces: corpus/synthetic/<scenario>/<size>_<params>.jp2
#
# Usage:
#   ./scripts/gen_corpus.sh seed.png

set -euo pipefail

SEED="${1:-}"
if [ -z "$SEED" ] || [ ! -f "$SEED" ]; then
  echo "usage: gen_corpus.sh seed.{png,tif}" >&2
  exit 2
fi

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/corpus/synthetic"
mkdir -p "$OUT"

command -v opj_compress >/dev/null || { echo "need opj_compress on PATH" >&2; exit 1; }
command -v magick      >/dev/null || { echo "need ImageMagick 'magick'" >&2; exit 1; }

SIZES=(256 1024 4096 16384)
RATES=(1.0 0.5 0.25)        # bpp targets for lossy
DECOMPS=(3 5 6)

TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

for size in "${SIZES[@]}"; do
  resized="$TMPDIR/seed_${size}.tif"
  magick "$SEED" -resize "${size}x${size}^" -gravity center \
         -extent "${size}x${size}" -depth 8 "$resized"

  # Lossless (reversible)
  for r in "${DECOMPS[@]}"; do
    out="$OUT/${size}_lossless_r${r}.jp2"
    [ -f "$out" ] || opj_compress -i "$resized" -o "$out" -r 1 -n "$r" -I >/dev/null
  done

  # Lossy at multiple rates
  for r in "${DECOMPS[@]}"; do
    for rate in "${RATES[@]}"; do
      # opj_compress -r expects compression ratio (e.g. 24 -> 1 bpp on 24bpp RGB)
      ratio=$(awk "BEGIN { printf \"%.0f\", 24/$rate }")
      out="$OUT/${size}_lossy_${rate}bpp_r${r}.jp2"
      [ -f "$out" ] || opj_compress -i "$resized" -o "$out" -r "$ratio" -n "$r" >/dev/null
    done
  done

  # 256x256 tile mode (matches WSI tile codec usage)
  if [ "$size" -ge 1024 ]; then
    for rate in "${RATES[@]}"; do
      ratio=$(awk "BEGIN { printf \"%.0f\", 24/$rate }")
      out="$OUT/${size}_lossy_${rate}bpp_tile256.jp2"
      [ -f "$out" ] || opj_compress -i "$resized" -o "$out" -r "$ratio" \
        -t 256,256 >/dev/null
    done
  fi
done

echo "[corpus] wrote files under $OUT"
ls -la "$OUT" | head
