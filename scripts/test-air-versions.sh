#!/usr/bin/env bash
# test-air-versions.sh — the AIR target per macOS (air/air_metallib.h AirTarget), inside the bitcode
# and out in the metallib container. Every corpus shader (air/tests/bin/*.bin) goes through the
# d9mtair compile path (airlib_loopback: the AirBuilder and metallib writer winemetal.so links).
#
# Set 1, targets 26 and 27 (D9MT_AIR_MACOS=26, =27, and unset on this Mac):
#  - the container: the header's file version and OS version, and every function's VERS tag,
#    equal what Apple's toolchain writes for that macOS (`xcrun metal -mmacosx-version-min=<os>.0`
#    and `metallib` on a probe shader);
#  - the bitcode (llvm-dis): target triple, air.version, air.language_version and datalayout
#    equal Apple's for the same macOS;
#  - unset equals the mode of the macOS this runs on.
# Set 2, legacy 26 and legacy 27 (D9MT_AIR_LEGACY=1): the fields 0.1.7 (d9mt 73511be) wrote:
#  triple air64-apple-macosx15.0.0, AIR 2.7, Metal 3.0, container 2.7, header OS 15.0, and VERS
#  2.7/3.0 under 26 and 2.9/3.0 under 27 (the known #110 mismatch, asserted as expected).
# Set 3, only with OLD_LOOPBACK=<build/airlib of a 73511be tree> (PR "AIR target per macOS"'s
#  acceptance check): legacy-26 and legacy-27 output byte-identical to that build's under the
#  same D9MT_AIR_MACOS. Later codegen PRs change legacy bitcode on purpose, so default runs skip it.
# Then the parse cases on the first shader: 27.2 and 26.6.2 parse; 27junk, -1, an overflow, 1000
# and "27." give target 26; D9MT_AIR_LEGACY=0 is not legacy.
# Needs build/airlib from scripts/build-bridge.sh (or build-d9mtair.sh), Xcode's metal toolchain
# and an LLVM 15 llvm-dis (LLVM_DIS, else toolchains/llvm-build/bin/llvm-dis, built with
# `ninja -C toolchains/llvm-build llvm-dis`, else Homebrew's llvm@15).
set -euo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$V2/build/airlib"
WORK="${WORK:-$OUT/versions-test}"
LOOP="$OUT/airlib_loopback"
[ -x "$LOOP" ] && [ -f "$OUT/d9mtair.so" ] || { echo "FATAL: no $LOOP or d9mtair.so (run scripts/build-bridge.sh)" >&2; exit 1; }
xcrun --find metallib >/dev/null || { echo "FATAL: no metallib (Xcode's metal toolchain)" >&2; exit 1; }
if [ -z "${LLVM_DIS:-}" ]; then
  for c in "$V2/toolchains/llvm-build/bin/llvm-dis" /opt/homebrew/opt/llvm@15/bin/llvm-dis; do
    [ -x "$c" ] && { LLVM_DIS=$c; break; }
  done
fi
[ -n "${LLVM_DIS:-}" ] && [ -x "$LLVM_DIS" ] \
  || { echo "FATAL: no llvm-dis (set LLVM_DIS, or: ninja -C toolchains/llvm-build llvm-dis)" >&2; exit 1; }
if [ -n "${OLD_LOOPBACK:-}" ]; then
  [ -x "$OLD_LOOPBACK/airlib_loopback" ] && [ -f "$OLD_LOOPBACK/d9mtair.so" ] \
    || { echo "FATAL: OLD_LOOPBACK=$OLD_LOOPBACK has no airlib_loopback and d9mtair.so" >&2; exit 1; }
fi
HOST=$(sw_vers -productVersion | cut -d. -f1)
HOSTMODE=26; [ "$HOST" -ge 27 ] && HOSTMODE=27
rm -rf "$WORK"; mkdir -p "$WORK/apple"
echo "macOS $HOST: the unset runs must match the macOS $HOSTMODE mode"

pass=0; fail=0
result() {  # result <name> <0|1 ok> <detail>
  if [ "$2" = 1 ]; then printf '%-30s PASS (%s)\n' "$1" "$3"; pass=$((pass+1))
  else printf '%-30s FAIL (%s)\n' "$1" "$3"; fail=$((fail+1)); fi
}

# Apple's oracle per macOS: a probe shader through xcrun metal (-S -emit-llvm for the bitcode
# fields, -c + metallib for the container).
cat > "$WORK/apple/probe.metal" <<'EOF'
#include <metal_stdlib>
using namespace metal;
struct V { float4 pos [[position]]; float4 c [[user(locn0)]]; };
vertex V vs_main(uint vid [[vertex_id]], constant float4* p [[buffer(0)]]) {
  V v; v.pos = p[vid]; v.c = p[vid + 3u]; return v;
}
fragment float4 fs_main(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {
  return in.c * t.sample(s, in.c.xy);
}
EOF
for os in 26 27; do
  xcrun -sdk macosx metal -O2 -mmacosx-version-min=$os.0 -S -emit-llvm "$WORK/apple/probe.metal" -o "$WORK/apple/$os.ll"
  xcrun -sdk macosx metal -O2 -mmacosx-version-min=$os.0 -c "$WORK/apple/probe.metal" -o "$WORK/apple/$os.air"
  xcrun -sdk macosx metallib "$WORK/apple/$os.air" -o "$WORK/apple/$os.metallib"
done

# compile <out.metallib> <D9MT_AIR_MACOS or "unset"> <D9MT_AIR_LEGACY or "unset"> <shader> [<loopback dir>]
compile() {
  local out=$1 mac=$2 leg=$3 bin=$4 dir=${5:-$OUT}
  local -a env_args=(env -u D9MT_AIR_MACOS -u D9MT_AIR_LEGACY)
  [ "$mac" != unset ] && env_args+=("D9MT_AIR_MACOS=$mac")
  [ "$leg" != unset ] && env_args+=("D9MT_AIR_LEGACY=$leg")
  "${env_args[@]}" "$dir/airlib_loopback" "$dir/d9mtair.so" "$bin" "$out" >/dev/null 2> "$out.err"
}
FIELDS=(python3 -I "$V2/scripts/air-fields.py" --llvm-dis "$LLVM_DIS")

# The wanted fields per mode: Apple's for targets 26 and 27 (container and VERS from its metallib,
# the bitcode fields from its LLVM text), the 0.1.7 fields for legacy (air-fields.py's table).
for os in 26 27; do
  python3 -I - "$WORK/apple/$os.metallib" "$WORK/apple/$os.ll" "$WORK/want.$os.json" "$V2/scripts/air-fields.py" <<'EOF'
import json, subprocess, sys
c, b = (json.loads(subprocess.run(['python3', '-I', sys.argv[4], f], capture_output=True, text=True,
                                  check=True).stdout) for f in sys.argv[1:3])
assert c['vers'] and all(v == c['vers'][0] for v in c['vers']), c['vers']
want = {'container': c['container'], 'os': c['os'], 'vers': c['vers'][0]}
want.update(b)
json.dump(want, open(sys.argv[3], 'w'), sort_keys=True)
EOF
  echo "Apple, macOS $os: $(cat "$WORK/want.$os.json")"
done
DL=$(python3 -I -c 'import json,sys; print(json.load(open(sys.argv[1]))["datalayout"])' "$WORK/want.26.json")
# check_mode <metallib> <26|27|L26|L27>: prints the differences from the mode's fields
check_mode() {
  case "$2" in
    L*) "${FIELDS[@]}" --expect "$2" "$1" && "${FIELDS[@]}" --want <(printf '{"datalayout": "%s"}' "$DL") "$1";;
    *) "${FIELDS[@]}" --want "$WORK/want.$2.json" "$1";;
  esac
}

MODES=(26:26:unset 27:27:unset L26:26:1 L27:27:1)
for bin in "$V2"/air/tests/bin/*.bin; do
  name="$(basename "$bin" .bin)"
  w="$WORK/$name"
  ok=1; detail=""
  for m in "${MODES[@]}"; do
    IFS=: read -r mode mac leg <<< "$m"
    if ! compile "$w.$mode.metallib" "$mac" "$leg" "$bin"; then
      ok=0; detail+="$mode: compile failed ($(head -1 "$w.$mode.metallib.err")); "; continue
    fi
    diff=$(check_mode "$w.$mode.metallib" "$mode") || { ok=0; detail+="$mode: ${diff#*: }; "; }
  done
  if compile "$w.host.metallib" unset unset "$bin" && compile "$w.Lhost.metallib" unset 1 "$bin"; then
    cmp -s "$w.host.metallib" "$w.$HOSTMODE.metallib" || { ok=0; detail+="unset differs from target $HOSTMODE; "; }
    cmp -s "$w.Lhost.metallib" "$w.L$HOSTMODE.metallib" || { ok=0; detail+="legacy unset differs from legacy $HOSTMODE; "; }
  else
    ok=0; detail+="unset: compile failed; "
  fi
  if [ -n "${OLD_LOOPBACK:-}" ]; then
    for os in 26 27; do
      if compile "$w.old$os.metallib" "$os" unset "$bin" "$OLD_LOOPBACK"; then
        cmp -s "$w.old$os.metallib" "$w.L$os.metallib" || { ok=0; detail+="set 3: legacy $os differs from OLD_LOOPBACK's; "; }
      else
        ok=0; detail+="set 3: OLD_LOOPBACK compile failed under $os; "
      fi
    done
  fi
  result "$name" "$ok" "${detail:-26 and 27 = Apple, legacy = the 0.1.7 fields${OLD_LOOPBACK:+ and bytes}}"
done

first="$(ls "$V2"/air/tests/bin/*.bin | head -1)"
w="$WORK/$(basename "$first" .bin)"
for os in 26 27; do
  want="d9mt: AIR target macOS $os: AIR 2.$((os - 18)) / Metal 4.$((os - 26))"
  result "log: target $os" "$(grep -qxF "$want" "$w.$os.metallib.err" && echo 1 || echo 0)" "$want"
done
result "log: legacy 27" "$(grep -qxF 'd9mt: AIR target macOS 27 (legacy, D9MT_AIR_LEGACY=1): AIR 2.7 / Metal 3.0, VERS 2.9 / 3.0' "$w.L27.metallib.err" && echo 1 || echo 0)" "legacy names its VERS"
for spec in 27.2:27:unset 26.6.2:26:unset 27junk:26:unset -1:26:unset 99999999999999999999:26:unset 1000:26:unset 27.:26:unset 27:27:0; do
  IFS=: read -r value want leg <<< "$spec"
  if compile "$w.override.metallib" "$value" "$leg" "$first" && cmp -s "$w.override.metallib" "$w.$want.metallib"; then
    result override 1 "D9MT_AIR_MACOS=$value D9MT_AIR_LEGACY=$leg gives target $want"
  else
    result override 0 "D9MT_AIR_MACOS=$value D9MT_AIR_LEGACY=$leg does not give target $want"
  fi
done
echo "----"
echo "pass=$pass fail=$fail${OLD_LOOPBACK:+ (with set 3, OLD_LOOPBACK=$OLD_LOOPBACK)}"
[ "$fail" -eq 0 ] && [ "$pass" -gt 0 ]
