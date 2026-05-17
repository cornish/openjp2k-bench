#!/usr/bin/env bash
# End-to-end smoke test for jp2k-bench. Asserts schema v2 envelope and that
# the four optional axes plumb through correctly.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build/_grok/bin/jp2k-bench"
[ -x "$BIN" ] || BIN="$ROOT/build/jp2k-bench"
FIX="$ROOT/tests/fixtures/tiny.jp2"

[ -x "$BIN" ] || { echo "missing jp2k-bench binary; run scripts/build.sh first" >&2; exit 1; }
[ -f "$FIX" ] || { echo "missing fixture $FIX" >&2; exit 1; }

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

run() {
  local label="$1"; shift
  local out="$TMP/${label}.json"
  echo "--- $label" >&2
  "$BIN" "$@" "$FIX" > "$out" 2>/dev/null
  python3 -c "
import json
d = json.load(open('$out'))
assert d['schema_version'] == 2, d['schema_version']
assert 'env' in d['run'], 'env missing'
assert isinstance(d['results'], list) and d['results'], 'no results'
" || { echo "FAIL $label: envelope check"; exit 1; }
  echo "$out"
}

# 1. Basic: schema_version=2, env present.
run basic --iters 2 --warmup 1 >/dev/null

# 2. --threads N,M produces two rows per decoder.
out=$(run threads --iters 2 --warmup 1 --threads 1,2)
python3 -c "
import json
d = json.load(open('$out'))
assert len({r['threads'] for r in d['results']}) >= 2, 'expected multiple thread counts'
" || { echo "FAIL threads"; exit 1; }

# 3. --roi populates roi field.
out=$(run roi --iters 2 --warmup 1 --roi 64x64@0,0)
python3 -c "
import json
d = json.load(open('$out'))
for r in d['results']:
    assert r['roi'] and r['roi']['x1'] == 64, r.get('roi')
    assert r.get('pixel_match', -1) in (1, -1), r.get('pixel_match')
" || { echo "FAIL roi"; exit 1; }

# 4. --concurrent-files populates aggregate.
out=$(run conc --iters 2 --warmup 1 --concurrent-files 2)
python3 -c "
import json
d = json.load(open('$out'))
assert d['run']['concurrent_files'] == 2
agg = d['aggregate']
assert 'concurrent_throughput_mpix_s' in agg, agg
" || { echo "FAIL concurrent"; exit 1; }

# 5. report.py runs against the basic output.
python3 "$ROOT/scripts/report.py" "$TMP/basic.json" >/dev/null \
  || { echo "FAIL report.py"; exit 1; }

echo "[smoke] all checks passed"
