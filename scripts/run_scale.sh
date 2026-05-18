#!/usr/bin/env bash
# Scale-track orchestrator. Runs jp2k-bench --scale-track once per
# (file, decoder) pair under a per-invocation systemd-run --scope memory
# cap. This protects the host from OOM cascades when a decode genuinely
# needs >host-RAM (we cap the *invocation*, not the global system).
#
# Output: a JSONL stream — one type="run" header line, then one
#         type="result" line per invocation. Result rows carry
#         scale_track=true and rss_peak_kb_sampled.
#
# Example (recommended invocation):
#   TS=$(date +%Y%m%d_%H%M%S)
#   nohup ./scripts/run_scale.sh corpus/public/scale/ \
#         > results/scale_${TS}.jsonl \
#         2> results/scale_${TS}.log &
#   disown
#
# See docs/superpowers/specs/2026-05-18-scale-track.md for design notes.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${BIN:-$ROOT/build/jp2k-bench}"

if [ ! -x "$BIN" ]; then
  echo "missing $BIN; run scripts/build.sh first" >&2
  exit 1
fi
if ! command -v systemd-run >/dev/null 2>&1; then
  echo "systemd-run not on PATH; required to cap per-invocation memory" >&2
  exit 1
fi

if [ $# -eq 0 ]; then
  echo "usage: run_scale.sh path-to-scale-corpus/" >&2
  exit 2
fi
PATH_ROOT="$1"
# Safety: insist the input is under corpus/scale/. The whole point of
# scale-track is to wrap huge-decode operations in a per-invocation
# MemoryMax cap; running it against a normal corpus path would put the
# perf bench under a cgroup unnecessarily.
# Accept any path with /scale/ in it — covers corpus/scale/, corpus/public/scale/
# (via the openjp2k-data symlink), and arbitrary local scale/ subdirs.
case "$PATH_ROOT" in
  */scale/*|*/scale|scale/*|scale)
    ;;
  *)
    echo "run_scale.sh: input '$PATH_ROOT' must contain /scale/ in the path." >&2
    echo "  This is a safety guardrail — scale-track means huge JP2s under" >&2
    echo "  MemoryMax caps; running it on the regular corpus is rarely what" >&2
    echo "  you want. Set ALLOW_NON_SCALE=1 to override (only for tests)." >&2
    [ "${ALLOW_NON_SCALE:-0}" = "1" ] || exit 2
    ;;
esac
if [ ! -d "$PATH_ROOT" ] && [ ! -f "$PATH_ROOT" ]; then
  echo "no such file or directory: $PATH_ROOT" >&2
  exit 2
fi

RSS_SAMPLE_MS="${RSS_SAMPLE_MS:-100}"
# Default decoder list. Override with DECODERS=openjpeg,openjp2k (no grok).
DECODERS="${DECODERS:-openjpeg,openjp2k,grok}"
IFS=',' read -r -a DECODER_ARR <<< "$DECODERS"

# Pick MemoryMax tier by compressed file size. Numbers come from the spec's
# observed-peak-times-~2 table. Override the whole tier table via env:
#   MEMMAX_25_50G=8  MEMMAX_50_100G=12  MEMMAX_100_200G=16  MEMMAX_OVER_200G=24
MEMMAX_25_50G="${MEMMAX_25_50G:-8}"
MEMMAX_50_100G="${MEMMAX_50_100G:-12}"
MEMMAX_100_200G="${MEMMAX_100_200G:-16}"
MEMMAX_OVER_200G="${MEMMAX_OVER_200G:-24}"

# Returns memory cap (in GB) for a given compressed-byte size.
mem_max_g_for() {
  local sz="$1"
  if   [ "$sz" -lt 26214400 ]; then echo "$MEMMAX_25_50G"            # <25MB: still cap (it'll never trip)
  elif [ "$sz" -lt 52428800 ]; then echo "$MEMMAX_25_50G"            # 25-50MB
  elif [ "$sz" -lt 104857600 ]; then echo "$MEMMAX_50_100G"          # 50-100MB
  elif [ "$sz" -lt 209715200 ]; then echo "$MEMMAX_100_200G"         # 100-200MB
  else echo "$MEMMAX_OVER_200G"                                       # >200MB
  fi
}

FILES=()
if [ -d "$PATH_ROOT" ]; then
  while IFS= read -r -d '' f; do FILES+=("$f"); done < \
    <(find -L "$PATH_ROOT" -type f \( -iname '*.jp2' -o -iname '*.j2k' -o -iname '*.jpc' \) -print0)
elif [ -f "$PATH_ROOT" ]; then
  FILES+=("$PATH_ROOT")
fi
if [ ${#FILES[@]} -eq 0 ]; then
  echo "no .jp2/.j2k files found under: $PATH_ROOT" >&2
  exit 2
fi

# Sort by size, largest first — that way an OOM-prone file fails early
# and the orchestrator reports its failure before the run wall-clock
# eats a long tail.
mapfile -t FILES < <(
  for f in "${FILES[@]}"; do
    sz=$(stat -c '%s' "$f")
    printf '%020d\t%s\n' "$sz" "$f"
  done | sort -rn | cut -f2-
)

TOTAL_BYTES=0
MAX_BYTES=0
for f in "${FILES[@]}"; do
  sz=$(stat -c '%s' "$f")
  TOTAL_BYTES=$((TOTAL_BYTES + sz))
  if [ "$sz" -gt "$MAX_BYTES" ]; then MAX_BYTES="$sz"; fi
done

# Emit a JSONL run header that mirrors what jp2k-bench would have emitted
# in a single-process run, with scale-track-specific fields added.
TS_ISO="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
# Compact decoder list as JSON array
decoders_json='[' ; first=1
for d in "${DECODER_ARR[@]}"; do
  [ $first -eq 0 ] && decoders_json="$decoders_json, "
  decoders_json="${decoders_json}\"${d}\""
  first=0
done
decoders_json="${decoders_json}]"

cat <<EOF
{"type": "run", "schema_version": 2, "started_at": "${TS_ISO}", "mode": "scale_track", "argv": ["$0", "$PATH_ROOT"], "concurrent_files": 1, "scale_track_config": {"rss_sample_ms": ${RSS_SAMPLE_MS}, "decoders": ${decoders_json}, "memory_max_tiers_gb": {"25-50MB": ${MEMMAX_25_50G}, "50-100MB": ${MEMMAX_50_100G}, "100-200MB": ${MEMMAX_100_200G}, ">200MB": ${MEMMAX_OVER_200G}}, "input_count": ${#FILES[@]}, "input_total_bytes": ${TOTAL_BYTES}, "input_max_bytes": ${MAX_BYTES}}}
EOF

echo "[scale] ${#FILES[@]} files, $(numfmt --to=iec "$TOTAL_BYTES" 2>/dev/null || echo "$TOTAL_BYTES") total, max $(numfmt --to=iec "$MAX_BYTES" 2>/dev/null || echo "$MAX_BYTES")" >&2

# Per-(file, decoder) loop. Each invocation runs jp2k-bench through
# systemd-run --scope, which puts the bench process in its own cgroup
# with a hard memory ceiling. An overrun kills only the bench process;
# this orchestrator + the terminal stay alive.
for f in "${FILES[@]}"; do
  sz=$(stat -c '%s' "$f")
  cap_g=$(mem_max_g_for "$sz")
  cap_bytes=$((cap_g * 1024 * 1024 * 1024))
  echo "[scale] $f  size=$(numfmt --to=iec "$sz" 2>/dev/null || echo "$sz")  MemoryMax=${cap_g}G" >&2
  for d in "${DECODER_ARR[@]}"; do
    # systemd-run --scope runs the command in a transient scope unit
    # synchronously (i.e. waits for completion). The cap kills the
    # bench process on overrun (exit 137 = SIGKILL by cgroup OOM).
    # --user avoids needing root.
    # We pipe just the result line through; the run header was emitted
    # once above. -P (no PTY), --collect (clean up failed scopes).
    set +e
    out=$(systemd-run --user --scope --quiet --collect \
            -p "MemoryMax=${cap_g}G" \
            -- "$BIN" \
              --scale-track \
              --rss-sample-ms "$RSS_SAMPLE_MS" \
              --memory-max-bytes "$cap_bytes" \
              --decoder "$d" \
              --jsonl \
              "$f" 2>>/tmp/scale-stderr.log)
    rc=$?
    set -e
    if [ $rc -eq 0 ]; then
      # The bench emitted its own run header line we don't want (we
      # have one already from the orchestrator). Keep only result/
      # correctness lines.
      printf '%s\n' "$out" | grep -v '"type": "run"' || true
    elif [ $rc -eq 137 ] || [ $rc -eq 134 ] || [ $rc -eq 135 ]; then
      # 137 = SIGKILL (cgroup OOM). 134/135 = SIGABRT/SIGBUS — surface
      # as OOM-class for now; the regression gate cares only about
      # "did the invocation survive within its budget."
      esc_f=$(printf '%s' "$f" | python3 -c "import json,sys; print(json.dumps(sys.stdin.read()))")
      printf '{"type": "result", "scale_track": true, "file": %s, "bytes": %s, "decoder": "%s", "decoder_version": "unknown", "threads": 1, "memory_max_bytes": %s, "oom_killed": true, "rss_peak_kb_sampled": %s, "error": "oom_killed by cgroup (exit %d)"}\n' \
        "$esc_f" "$sz" "$d" "$cap_bytes" $((cap_bytes / 1024)) "$rc"
      echo "[scale] $d OOM-killed at ${cap_g}G budget" >&2
    else
      # Other non-zero: surface as an error row without the OOM flag.
      esc_f=$(printf '%s' "$f" | python3 -c "import json,sys; print(json.dumps(sys.stdin.read()))")
      printf '{"type": "result", "scale_track": true, "file": %s, "bytes": %s, "decoder": "%s", "decoder_version": "unknown", "threads": 1, "memory_max_bytes": %s, "oom_killed": false, "error": "orchestrator: bench exited %d"}\n' \
        "$esc_f" "$sz" "$d" "$cap_bytes" "$rc"
      echo "[scale] $d exited $rc (not OOM)" >&2
    fi
  done
done
