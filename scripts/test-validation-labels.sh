#!/usr/bin/env bash
# Bridge bundle part 9b: a validation session's labels (docs/bridge-bundle.md). Native, no Wine:
# bridge/tests/validation_label_test renders one pass through build/bridge/winemetal.so's thunks.
#   off      no validation, an in-bounds read: no report
#   in       MTL_SHADER_VALIDATION=1 + REPORT_TO_STDERR, in bounds: no report
#   oob      the same, 64 bytes past a 1000-byte buffer (inside its own page): a report naming
#            the pipeline "d9mt vs:<8 hex>/vmain fs:<8 hex>/fmain" and the encoder
#            "d9mt enc#<n> render 80 4x4"; zero-filled, the process goes on
# The out-of-bounds read only ever runs under validation (the test refuses it otherwise). After
# each case the unified log is checked for IOGPUMetalError / GPU restart / WindowServer stuck:
# any hit stops the run. Needs build-bridge.sh to have run and Xcode's metal compiler.
set -uo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
BR="$V2/bridge"
OUT="$V2/build/validation-labels"; SO="$V2/build/bridge/winemetal.so"
rm -rf "$OUT"; mkdir -p "$OUT"
cat > "$OUT/val.metal" <<'MSL'
#include <metal_stdlib>
using namespace metal;
struct VOut { float4 pos [[position]]; };
vertex VOut vmain(uint vid [[vertex_id]]) {
  float2 p = float2((vid << 1) & 2, vid & 2);
  VOut o; o.pos = float4(p * 2 - 1, 0, 1); return o;
}
fragment float4 fmain(device const float *src [[buffer(0)]], constant uint &idx [[buffer(1)]]) {
  return float4(src[idx] / 250.0, 0, 0, 1);
}
MSL
xcrun -sdk macosx metal -c "$OUT/val.metal" -o "$OUT/val.air" && xcrun -sdk macosx metallib "$OUT/val.air" -o "$OUT/val.metallib" \
  || { echo "FATAL: the test shader does not compile" >&2; exit 1; }
clang++ -std=c++17 -O1 -g -arch x86_64 -I"$BR" "$BR/tests/validation_label_test.cpp" -o "$OUT/validation_label_test" \
  || { echo "FATAL: validation_label_test does not build" >&2; exit 1; }
fails=0
check() { if [ "$2" = "$3" ]; then echo "  $1: ok ($2)"; else echo "  $1: FAIL (got '$2', want '$3')"; fails=$((fails + 1)); fi; }
alarm() {
  /usr/bin/log show --start "$1" --style compact --predicate \
    'process != "log" AND (eventMessage CONTAINS "IOGPUMetalError" OR eventMessage CONTAINS[c] "GPU Restart" OR (process == "WindowServer" AND eventMessage CONTAINS[c] "stuck"))' \
    2>/dev/null | grep -vE '^(Timestamp|Filtering|Skipping)' | grep -c .
}
run() { # <case> <env...> -- <in|oob>
  local c=$1; shift
  local envs=(); while [ "$1" != -- ]; do envs+=("$1"); shift; done; shift
  local start; start=$(date '+%Y-%m-%d %H:%M:%S')
  env ${envs[@]+"${envs[@]}"} "$OUT/validation_label_test" "$SO" "$OUT/val.metallib" "$@" > "$OUT/$c.log" 2>&1
  local rc=$?
  [ $rc = 0 ] || { echo "  $c: FAIL (rc $rc)"; grep FAIL "$OUT/$c.log" | head -3; fails=$((fails + 1)); }
  sleep 1
  local a; a=$(alarm "$start")
  if [ "${a:-0}" -gt 0 ]; then echo "STOP: GPU alarm after $c"; exit 3; fi
}
V=(MTL_SHADER_VALIDATION=1 MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1)

echo "[validation-labels] off"
run off -- in
check "no report" "$(grep -c 'Invalid Metal usage' "$OUT/off.log")" 0
check "done" "$(grep -c 'VALTEST in done' "$OUT/off.log")" 1

echo "[validation-labels] in"
run in "${V[@]}" -- in
check "validation on" "$(grep -c 'Metal GPU Validation Enabled' "$OUT/in.log")" 1
check "no report" "$(grep -c 'Invalid Metal usage' "$OUT/in.log")" 0

echo "[validation-labels] oob"
run oob "${V[@]}" -- oob
check "one report" "$(grep -c 'Invalid Metal usage' "$OUT/oob.log")" 1
check "the pipeline named by its shaders" "$(grep -cE '^pipeline: "d9mt vs:[0-9a-f]{8}/vmain fs:[0-9a-f]{8}/fmain"' "$OUT/oob.log")" 1
check "the encoder named" "$(grep -cE 'in encoder with label "d9mt enc#[0-9]+ render 80 4x4"' "$OUT/oob.log")" 1
check "zero-filled, went on" "$(grep -c 'VALTEST oob done' "$OUT/oob.log")" 1
sed -n '/^=====/,/^=====/p' "$OUT/oob.log" | head -10 | sed 's/^/    | /'

echo "[validation-labels] $fails failure(s)"
[ "$fails" -eq 0 ]
