#!/usr/bin/env bash
# test-air-invariance.sh — position invariance (AirBuilder, the 0.1.8 recompile bundle): d9mt writes
# exactly the form Apple's toolchain writes for `[[position, invariant]]` with -fpreserve-invariance,
# for every vertex shader, and nothing changes for pixel shaders. Every corpus shader
# (air/tests/bin/*.bin) goes through the d9mtair compile path (airlib_loopback), under targets 26
# and 27, in four modes:
#   relaxed math (the default)       VS = Apple's relaxed math, -fpreserve-invariance
#   D9MT_AIR_INVARIANT=0             VS = Apple's relaxed math, no invariance
#   D9MT_AIR_FASTMATH=1              VS = Apple's fast math, -fpreserve-invariance (fast_math_disable)
#   both                             VS = Apple's fast math, no invariance
# A pixel shader in every mode = Apple's same math mode without invariance.
# The form (scripts/air-invariance.py): the floating-point function attributes, the fast-math
# flags on instructions (ours must be a subset of Apple's), air.invariant on the position output
# and the compile option fast_math_enable/disable.
# Needs build/airlib from scripts/build-bridge.sh (or build-d9mtair.sh), Xcode's metal toolchain
# and an LLVM 15 llvm-dis (as test-air-versions.sh).
set -euo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$V2/build/airlib"
WORK="${WORK:-$OUT/invariance-test}"
LOOP="$OUT/airlib_loopback"
[ -x "$LOOP" ] && [ -f "$OUT/d9mtair.so" ] || { echo "FATAL: no $LOOP or d9mtair.so (run scripts/build-bridge.sh)" >&2; exit 1; }
if [ -z "${LLVM_DIS:-}" ]; then
  for c in "$V2/toolchains/llvm-build/bin/llvm-dis" /opt/homebrew/opt/llvm@15/bin/llvm-dis; do
    [ -x "$c" ] && { LLVM_DIS=$c; break; }
  done
fi
[ -n "${LLVM_DIS:-}" ] && [ -x "$LLVM_DIS" ] \
  || { echo "FATAL: no llvm-dis (set LLVM_DIS, or: ninja -C toolchains/llvm-build llvm-dis)" >&2; exit 1; }
rm -rf "$WORK"; mkdir -p "$WORK/apple"
FORM=(python3 -I "$V2/scripts/air-invariance.py" --llvm-dis "$LLVM_DIS")

pass=0; fail=0
result() {  # result <name> <0|1 ok> <detail>
  if [ "$2" = 1 ]; then printf '%-34s PASS (%s)\n' "$1" "$3"; pass=$((pass+1))
  else printf '%-34s FAIL (%s)\n' "$1" "$3"; fail=$((fail+1)); fi
}

# Apple's oracle: one vertex and one fragment function, with and without invariance, in both math
# modes, per macOS.
for inv in on off; do
  attr=""; [ "$inv" = on ] && attr=", invariant"
  cat > "$WORK/apple/probe-$inv.metal" <<EOF
#include <metal_stdlib>
using namespace metal;
struct V { float4 pos [[position$attr]]; float4 c [[user(locn0)]]; };
vertex V vs_main(uint vid [[vertex_id]], constant float4* p [[buffer(0)]], constant float4x4& m [[buffer(1)]]) {
  V v; v.pos = m * p[vid] + p[vid + 1u] * p[vid + 2u]; v.c = p[vid + 3u] * 2.0f + p[vid]; return v;
}
fragment float4 fs_main(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {
  return in.c * t.sample(s, in.c.xy) + in.c.wzyx * in.c;
}
EOF
done
for os in 26 27; do
  for math in relaxed fast; do
    mflag=(); [ "$math" = relaxed ] && mflag=(-fmetal-math-mode=relaxed)
    xcrun -sdk macosx metal -O2 -mmacosx-version-min=$os.0 "${mflag[@]}" -fpreserve-invariance -S -emit-llvm \
      "$WORK/apple/probe-on.metal" -o "$WORK/apple/$os-$math-on.ll"
    xcrun -sdk macosx metal -O2 -mmacosx-version-min=$os.0 "${mflag[@]}" -S -emit-llvm \
      "$WORK/apple/probe-off.metal" -o "$WORK/apple/$os-$math-off.ll"
    for inv in on off; do
      "${FORM[@]}" "$WORK/apple/$os-$math-$inv.ll" > "$WORK/apple/$os-$math-$inv.json"
      echo "Apple, macOS $os, $math math, invariance $inv: $(cat "$WORK/apple/$os-$math-$inv.json")"
    done
  done
done

# compare <ours.json> <apple.json> <vs|ps>: prints the differences, exits 1 on any
compare() {
  python3 -I - "$1" "$2" "$3" <<'EOF'
import json, sys
ours, apple, stage = (json.load(open(sys.argv[1])), json.load(open(sys.argv[2])), sys.argv[3])
errs = []
if ours['attrs'] is None or ours['attrs'] != apple['attrs']:
    errs.append('attrs %s, not %s' % (ours['attrs'], apple['attrs']))
if not set(ours['fmf']) <= set(apple['fmf']):
    errs.append('fast-math flags %s, not within %s' % (ours['fmf'], apple['fmf']))
if ours['fast_math'] != apple['fast_math']:
    errs.append('fast_math_enable %s, not %s' % (ours['fast_math'], apple['fast_math']))
if stage == 'vs' and ours['invariant'] != apple['invariant']:
    errs.append('air.invariant %s, not %s' % (ours['invariant'], apple['invariant']))
if stage == 'ps' and ours['invariant'] is not None:
    errs.append('a pixel shader with a vertex position output')
print('; '.join(errs))
sys.exit(1 if errs else 0)
EOF
}

# mode: <name>:<D9MT_AIR_FASTMATH>:<D9MT_AIR_INVARIANT>:<Apple math>:<Apple invariance for a VS>
MODES=(default:unset:unset:relaxed:on noinv:unset:0:relaxed:off fast:1:unset:fast:on fastnoinv:1:0:fast:off)
nvs=0
for bin in "$V2"/air/tests/bin/*.bin; do
  name="$(basename "$bin" .bin)"
  tok=$(od -An -tx4 -N4 "$bin" | tr -d ' ')
  stage=ps; case "$tok" in fffe*) stage=vs; nvs=$((nvs+1));; esac
  ok=1; detail=""
  for os in 26 27; do
    for m in "${MODES[@]}"; do
      IFS=: read -r mode fm inv amath ainv <<< "$m"
      [ "$stage" = ps ] && ainv=off
      out="$WORK/$name.$os.$mode.metallib"
      env_args=(env -u D9MT_AIR_LEGACY -u D9MT_AIR_FASTMATH -u D9MT_AIR_INVARIANT "D9MT_AIR_MACOS=$os")
      [ "$fm" != unset ] && env_args+=("D9MT_AIR_FASTMATH=$fm")
      [ "$inv" != unset ] && env_args+=("D9MT_AIR_INVARIANT=$inv")
      if ! "${env_args[@]}" "$LOOP" "$OUT/d9mtair.so" "$bin" "$out" >/dev/null 2> "$out.err"; then
        ok=0; detail+="$os $mode: compile failed ($(head -1 "$out.err")); "; continue
      fi
      "${FORM[@]}" "$out" > "$out.json"
      d=$(compare "$out.json" "$WORK/apple/$os-$amath-$ainv.json" "$stage") || { ok=0; detail+="$os $mode: $d; "; }
    done
    # invariance off changes a VS's bytes and leaves a PS's alone
    if [ -f "$WORK/$name.$os.default.metallib" ] && [ -f "$WORK/$name.$os.noinv.metallib" ]; then
      if cmp -s "$WORK/$name.$os.default.metallib" "$WORK/$name.$os.noinv.metallib"; then
        [ "$stage" = vs ] && { ok=0; detail+="$os: invariance off gives the same bytes; "; }
      else
        [ "$stage" = ps ] && { ok=0; detail+="$os: invariance off changes a pixel shader; "; }
      fi
    fi
  done
  result "$name ($stage)" "$ok" "${detail:-the form equals Apple in all four modes, targets 26 and 27}"
done
result "corpus has vertex shaders" "$([ "$nvs" -gt 0 ] && echo 1 || echo 0)" "$nvs"
echo "----"
echo "pass=$pass fail=$fail"
[ "$fail" -eq 0 ] && [ "$pass" -gt 0 ]
