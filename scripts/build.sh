#!/usr/bin/env bash
# Configure and build jp2k-bench in Release with LTO and -march=native.
#
# Flags can be overridden:
#   BUILD_DIR=build-x86_64   ./scripts/build.sh
#   ./scripts/build.sh --openjpeg-source /path/to/vanilla   # baseline source
#   ./scripts/build.sh --openjp2k-source /path/to/fork      # decoder under test
#   ./scripts/build.sh --no-grok
#   ./scripts/build.sh --no-native    # portable binary, no -march=native

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"

OPENJPEG_SOURCE="$ROOT/third_party/openjpeg"
OPENJP2K_SOURCE="$ROOT/third_party/openjp2k"
GROK_SOURCE="$ROOT/third_party/grok"
JP2KBENCH_NATIVE=ON
JP2KBENCH_BUILD_GROK=ON

while [ $# -gt 0 ]; do
  case "$1" in
    --openjpeg-source) OPENJPEG_SOURCE="$2"; shift 2 ;;
    --openjp2k-source) OPENJP2K_SOURCE="$2"; shift 2 ;;
    --grok-source)     GROK_SOURCE="$2";     shift 2 ;;
    --no-grok)         JP2KBENCH_BUILD_GROK=OFF; shift ;;
    --no-native)       JP2KBENCH_NATIVE=OFF;     shift ;;
    --build-dir)       BUILD_DIR="$2"; shift 2 ;;
    -h|--help)
      sed -n '2,9p' "$0"; exit 0 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done

echo "[build] root          = $ROOT"
echo "[build] build dir     = $BUILD_DIR"
echo "[build] openjpeg src  = $OPENJPEG_SOURCE"
echo "[build] openjp2k src  = $OPENJP2K_SOURCE"
echo "[build] grok src      = $GROK_SOURCE  (enabled=$JP2KBENCH_BUILD_GROK)"
echo "[build] -march=native = $JP2KBENCH_NATIVE"

cmake -S "$ROOT" -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE=Release \
  -DJP2KBENCH_OPENJPEG_SOURCE="$OPENJPEG_SOURCE" \
  -DJP2KBENCH_OPENJP2K_SOURCE="$OPENJP2K_SOURCE" \
  -DJP2KBENCH_GROK_SOURCE="$GROK_SOURCE" \
  -DJP2KBENCH_BUILD_GROK="$JP2KBENCH_BUILD_GROK" \
  -DJP2KBENCH_NATIVE="$JP2KBENCH_NATIVE" \
  -DJP2KBENCH_LTO=ON

cmake --build "$BUILD_DIR" -j --target jp2k-bench

echo
echo "[build] built: $BUILD_DIR/jp2k-bench"
"$BUILD_DIR/jp2k-bench" --list-decoders
