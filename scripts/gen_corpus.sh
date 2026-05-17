#!/usr/bin/env bash
# Generate a parametric JPEG 2000 corpus exercising both common and dusty
# corners of the spec. Designed to surface decoder bugs/regressions on
# rarely-used-but-legal codestreams.
#
# Requires: opj_compress (apt: libopenjp2-tools, or build OpenJPEG with
#           BUILD_CODEC=ON).
#
# No external image dependency: each source raster is generated on the
# fly as a deterministic PGM/PPM gradient + noise pattern.
#
# Output layout:
#   corpus/synthetic/<rastername>/<axis_value...>.{jp2,j2k}
#
# Usage:
#   ./scripts/gen_corpus.sh                 # full sweep
#   ./scripts/gen_corpus.sh --quick         # smaller sweep for smoke
#
# Re-running is idempotent: existing files are skipped.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/corpus/synthetic"
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

command -v opj_compress >/dev/null || { echo "need opj_compress on PATH" >&2; exit 1; }

QUICK=0
if [ "${1:-}" = "--quick" ]; then QUICK=1; fi

# --- Source rasters --------------------------------------------------------
# 12/16-bit are written as P5 with maxval > 255 (PNM supports up to 65535).
gen_raster() {
  local name="$1" w="$2" h="$3" comps="$4" bits="$5"
  local out="$TMP/${name}.pnm"
  local maxval=$(( (1 << bits) - 1 ))
  local magic="P5"; [ "$comps" -eq 3 ] && magic="P6"
  {
    printf '%s\n%d %d\n%d\n' "$magic" "$w" "$h" "$maxval"
    python3 -c "
import sys, struct
w,h,c,bits = $w,$h,$comps,$bits
mx = (1<<bits) - 1
out = sys.stdout.buffer
big = bits > 8
for y in range(h):
    for x in range(w):
        base = ((x ^ y) * 131 + x * 17) & mx
        for ch in range(c):
            v = (base + ch * 37) & mx
            if big: out.write(struct.pack('>H', v))
            else:   out.write(bytes([v]))
"
  } > "$out"
  echo "$out"
}

# Raster set: (name w h comps bits)
RASTERS=(
  "rgb8_1024     1024 1024 3  8"
  "mono16_1024   1024 1024 1 16"
  "mono12_1024   1024 1024 1 12"
  "rgb8_4096     4096 4096 3  8"
  "mono16_4096   4096 4096 1 16"
)

# 4-component PNM doesn't exist; real 4-component coverage comes from the
# public bucket if/when a GeoJP2 sample lands. Noted limitation.

if [ "$QUICK" -eq 1 ]; then
  RASTERS=("rgb8_1024 1024 1024 3 8" "mono16_1024 1024 1024 1 16")
fi

# --- Axis sweeps -----------------------------------------------------------
PROGRESSIONS=(LRCP RLCP RPCL PCRL CPRL)
DECOMPS=(0 1 5 8)
CBLK_SIZES=(32 64)
TILE_MODES=("none" "1024,1024")
CONTAINERS=(jp2 j2k)
MCT_MODES=(on off)
SOP_EPH=("none" "sop,eph")
QUALITY_LAYERS=(1 4)
RATES=(lossless lossy)

[ "$QUICK" -eq 1 ] && {
  PROGRESSIONS=(LRCP RPCL)
  DECOMPS=(1 5)
  CBLK_SIZES=(64)
  TILE_MODES=("none" "1024,1024")
  CONTAINERS=(jp2)
  MCT_MODES=(on)
  SOP_EPH=("none")
  QUALITY_LAYERS=(1)
}

mkdir -p "$OUT"

emit() {
  local src="$1" name="$2" out_dir="$3" container="$4" rate="$5"
  shift 5
  local outpath="$out_dir/$name.$container"
  [ -f "$outpath" ] && return 0
  local cmd=(opj_compress -i "$src" -o "$outpath" "$@")
  # -I selects the irreversible (lossy) 9-7 wavelet; omit for reversible 5-3.
  if [ "$rate" = "lossless" ]; then cmd+=(-r 1); else cmd+=(-r 20 -I); fi
  if ! "${cmd[@]}" >/dev/null 2>&1; then
    echo "  [skip] $name ($container, $rate) — opj_compress rejected the combo"
    return 0
  fi
}

total=0
for entry in "${RASTERS[@]}"; do
  # shellcheck disable=SC2086
  set -- $entry
  rname="$1" w="$2" h="$3" comps="$4" bits="$5"
  echo "[gen] raster $rname ${w}x${h} c=$comps b=$bits"
  src=$(gen_raster "$rname" "$w" "$h" "$comps" "$bits")
  rdir="$OUT/$rname"
  mkdir -p "$rdir"

  for prog in "${PROGRESSIONS[@]}"; do
    for decomp in "${DECOMPS[@]}"; do
      for cblk in "${CBLK_SIZES[@]}"; do
        for tile in "${TILE_MODES[@]}"; do
          for cont in "${CONTAINERS[@]}"; do
            for mct in "${MCT_MODES[@]}"; do
              [ "$comps" -ne 3 ] && [ "$mct" = "on" ] && continue
              for marker in "${SOP_EPH[@]}"; do
                for layers in "${QUALITY_LAYERS[@]}"; do
                  for rate in "${RATES[@]}"; do
                    [ "$rate" = "lossless" ] && [ "$layers" -gt 1 ] && continue

                    args=(-p "$prog" -n "$decomp"
                          -b "$cblk,$cblk")
                    [ "$tile" != "none" ] && args+=(-t "$tile")
                    [ "$marker" != "none" ] && args+=(-SOP -EPH)
                    [ "$layers" -gt 1 ] && args+=(-q "20,30,40,50")
                    if [ "$mct" = "off" ] && [ "$comps" -eq 3 ]; then
                      args+=(-mct 0)
                    fi

                    tag="p${prog}_d${decomp}_b${cblk}"
                    [ "$tile" = "none" ] && tag+="_tNone" || tag+="_t${tile//,/x}"
                    tag+="_${rate}_l${layers}_m${mct}_e${marker//,/_}"

                    emit "$src" "$tag" "$rdir" "$cont" "$rate" "${args[@]}"
                    total=$((total+1))
                  done
                done
              done
            done
          done
        done
      done
    done
  done
done

echo "[gen] sweep attempted: $total combinations (some skipped by opj_compress)"
echo "[gen] outputs: $OUT"
find "$OUT" -type f \( -name '*.jp2' -o -name '*.j2k' \) | wc -l \
  | awk '{print "[gen] files on disk:", $1}'
