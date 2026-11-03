#!/usr/bin/env bash
# test-air-forms.sh — the AIR built-in forms contract: every intrinsic declaration AirBuilder can
# emit (air/tests/forms/inventory.txt) equals Apple's for the same name, for targets 26 and 27.
#  1. Inventory completeness: every "air.… literal in air/air_builder.cpp is an inventory site or
#     metadata, so a new emitter can't bypass the test (scripts/air-forms.py sites).
#  2. Fixtures: air/tests/forms/gen_ir.cpp's hand-built IR (airlib_loopback --ir) and the SM3
#     corpus shaders the inventory names (airlib_loopback, the SM3 path) must declare their forms.
#  3. Apple: each forms.metal group through `xcrun metal -mmacosx-version-min=<target>.0 -O2 -S
#     -emit-llvm`; each form's normalized declaration must equal Apple's (scripts/air-forms.py).
#  Nothing any fixture or corpus shader declares may be missing from the inventory.
# Until this passes, the AIR target's claim of Apple-equal built-ins covers only the sampled forms
# of the 2026-11-01 probe. Needs build/airlib from scripts/build-bridge.sh (or build-d9mtair.sh),
# Xcode's metal toolchain and an LLVM 15 llvm-dis (as test-air-versions.sh).
set -euo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$V2/build/airlib"
WORK="${WORK:-$OUT/forms-test}"
LOOP="$OUT/airlib_loopback"
FORMS="$V2/air/tests/forms"
[ -x "$LOOP" ] && [ -f "$OUT/d9mtair.so" ] || { echo "FATAL: no $LOOP or d9mtair.so (run scripts/build-bridge.sh)" >&2; exit 1; }
xcrun -sdk macosx --find metal >/dev/null || { echo "FATAL: no metal (Xcode's metal toolchain)" >&2; exit 1; }
if [ -z "${LLVM_DIS:-}" ]; then
  for c in "$V2/toolchains/llvm-build/bin/llvm-dis" /opt/homebrew/opt/llvm@15/bin/llvm-dis; do
    [ -x "$c" ] && { LLVM_DIS=$c; break; }
  done
fi
[ -n "${LLVM_DIS:-}" ] && [ -x "$LLVM_DIS" ] \
  || { echo "FATAL: no llvm-dis (set LLVM_DIS, or: ninja -C toolchains/llvm-build llvm-dis)" >&2; exit 1; }
rm -rf "$WORK"; mkdir -p "$WORK"
PY=(python3 -I "$V2/scripts/air-forms.py")
fail=0

# 1. completeness
"${PY[@]}" sites "$FORMS/inventory.txt" "$V2/air/air_builder.cpp" || fail=1

# 2. the IR fixture generator
clang++ -std=c++17 -O1 -arch x86_64 -I"$V2/third_party/dxbc-spirv" \
  "$FORMS/gen_ir.cpp" "$V2"/build/airlib/obj/third_party_dxbc-spirv_*.o -o "$WORK/gen_ir"
mkdir -p "$WORK/ir"
for c in $("$WORK/gen_ir" --list); do
  "$WORK/gen_ir" "$c" "$WORK/ir/$c.ir"
done

# dis <metallib> <out.ll>
dis() {
  python3 -I - "$1" "$1.bc" <<'EOF'
import struct, sys
d = open(sys.argv[1], 'rb').read()
off, size = struct.unpack_from('<QQ', d, 0x48)
open(sys.argv[2], 'wb').write(d[off:off+size])
EOF
  "$LLVM_DIS" "$1.bc" -o "$2"
}

# group <name> <out.metal>: forms.metal's header plus one group
group() {
  python3 -I - "$FORMS/forms.metal" "$1" > "$2" <<'EOF'
import re, sys
src = open(sys.argv[1]).read()
parts = re.split(r'^// group: ([a-z0-9_]+)\n', src, flags=re.M)
groups = dict(zip(parts[1::2], parts[2::2]))
sys.stdout.write(parts[0] + groups[sys.argv[2]])
EOF
}

for target in 26 27; do
  T="$WORK/t$target"
  mkdir -p "$T/ours" "$T/apple" "$T/corpus"
  # our fixtures: every ir case and every sm3 fixture the inventory names
  for c in $("$WORK/gen_ir" --list); do
    if env -u D9MT_AIR_LEGACY D9MT_AIR_MACOS=$target "$LOOP" "$OUT/d9mtair.so" --ir "$WORK/ir/$c.ir" "$T/ours/ir_$c.metallib" \
         >/dev/null 2> "$T/ours/ir_$c.err"; then
      dis "$T/ours/ir_$c.metallib" "$T/ours/ir_$c.ll"
    else
      echo "ir:$c [$target] compile failed: $(tail -1 "$T/ours/ir_$c.err")"
    fi
  done
  for s in $(sed -n 's/^form .*sm3:\([a-z0-9_]*\).*/\1/p' "$FORMS/inventory.txt" | sort -u); do
    if env -u D9MT_AIR_LEGACY D9MT_AIR_MACOS=$target "$LOOP" "$OUT/d9mtair.so" "$V2/air/tests/bin/$s.bin" "$T/ours/sm3_$s.metallib" \
         >/dev/null 2> "$T/ours/sm3_$s.err"; then
      dis "$T/ours/sm3_$s.metallib" "$T/ours/sm3_$s.ll"
    else
      echo "sm3:$s [$target] compile failed: $(tail -1 "$T/ours/sm3_$s.err")"
    fi
  done
  # the whole SM3 corpus: nothing outside the inventory
  for bin in "$V2"/air/tests/bin/*.bin; do
    n=$(basename "$bin" .bin)
    env -u D9MT_AIR_LEGACY D9MT_AIR_MACOS=$target "$LOOP" "$OUT/d9mtair.so" "$bin" "$T/corpus/$n.metallib" >/dev/null 2>&1 \
      && dis "$T/corpus/$n.metallib" "$T/corpus/$n.ll"
  done
  # Apple, per group
  for g in $("${PY[@]}" groups "$FORMS/inventory.txt"); do
    group "$g" "$T/apple/$g.metal"
    xcrun -sdk macosx metal -O2 -mmacosx-version-min=$target.0 -S -emit-llvm "$T/apple/$g.metal" -o "$T/apple/$g.ll" \
      2> "$T/apple/$g.err" || { echo "Apple rejected group $g [$target]: $(head -1 "$T/apple/$g.err")"; rm -f "$T/apple/$g.ll"; }
  done
  "${PY[@]}" compare "$FORMS/inventory.txt" "$target" "$T/ours" "$T/apple" "$T/corpus" || fail=1
done
echo "----"
[ "$fail" = 0 ] && echo "forms: PASS (targets 26 and 27)" || echo "forms: FAIL"
[ "$fail" = 0 ]
