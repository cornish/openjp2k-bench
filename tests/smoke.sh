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

# 5. --reuse-codec accepted; reused_codec field present per row. Current
#    state: both adapters report false (Grok v20 segfaults on second
#    grk_decompress; OpenJPEG has no reset API). The flag is wired but a
#    no-op until an adapter grows real reuse; smoke just asserts the
#    field exists and the bench doesn't crash.
out=$(run reuse --iters 2 --warmup 1 --reuse-codec)
python3 -c "
import json
d = json.load(open('$out'))
for r in d['results']:
    assert 'reused_codec' in r, 'reused_codec field missing'
" || { echo "FAIL reuse-codec"; exit 1; }

# 6. --require-clean exits 0 when trees are clean (this CI's trees are
#    pinned upstream tags, so always clean from the bench's POV).
"$BIN" --require-clean --iters 1 --warmup 1 "$FIX" >/dev/null 2>&1 \
  || { echo "FAIL require-clean against clean trees"; exit 1; }

# 7. --header-only times the setup-only pass and marks rows accordingly.
out=$(run header_only --iters 2 --warmup 1 --header-only)
python3 -c "
import json
d = json.load(open('$out'))
for r in d['results']:
    assert r['header_only'] is True, r
    # In header-only mode width/height are 0 (no decode happened);
    # timing should still be non-zero.
    assert r['timing_s']['min'] > 0, r['timing_s']
" || { echo "FAIL header-only"; exit 1; }

# 8. report.py runs against the basic output.
python3 "$ROOT/scripts/report.py" "$TMP/basic.json" >/dev/null \
  || { echo "FAIL report.py"; exit 1; }

echo "[smoke] all checks passed"
