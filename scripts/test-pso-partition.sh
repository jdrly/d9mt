#!/usr/bin/env bash
# test-pso-partition.sh — the PSO archives' partition (bridge/unix/winemetal_unix.c pso_archive_init),
# without Wine, on scratch directories under $WORK only. Each run is one bridge start
# (bridge/tests/pso_partition_test.cpp: the first Metal device, one MTL4 compiler), which names
# its archive directory os-<OS build>-<GPU>-c<effective AIR identity, 8 bytes>-t<AIR target T>
# and removes every other os-* directory under D9MT_PSO_ARCHIVE_DIR.
#  - This build: c = the codegen id's first 8 bytes (no lineage), t = this Mac's target.
#  - 0.1.7's directory (os-<build>-<gpu>) and another identity's are removed; anything not
#    named os-* is left alone.
#  - Two starts keep the directory and what's in it.
#  - D9MT_AIR_CACHE=0: the same directory (the identity opens no database).
#  - Targets 26, 27, legacy 26 and legacy 27: t2828, t2929, t12727, t12729, four directories;
#    one base switched from one target to the next keeps only the last.
#  - A lineaged id keeps its predecessor's identity: a copy of winemetal.so whose codegen id
#    string is the 0.1.5.1 lineage's (bridge/unix/air_cache_lineage.c) uses
#    c<that lineage's identity>, and a directory of that name already there is kept, contents
#    and all.
# Needs build/bridge from scripts/build-bridge.sh.
set -euo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
BR="$V2/build/bridge"
SO="$BR/winemetal.so"
WORK="${WORK:-$BR/pso-partition-test}"
[ -f "$SO" ] && [ -f "$BR/air-codegen-id.txt" ] || { echo "FATAL: no $SO (run scripts/build-bridge.sh)" >&2; exit 1; }
rm -rf "$WORK"; mkdir -p "$WORK"
clang++ -std=c++17 -O1 -arch x86_64 -I"$V2" -I"$V2/bridge" -I"$V2/airlib" \
  "$V2/bridge/tests/pso_partition_test.cpp" -o "$WORK/pso_partition_test"

ID=$(sed -n 's/^AIR codegen \([0-9a-f]\{64\}\) .*/\1/p' "$BR/air-codegen-id.txt")
[ -n "$ID" ] || { echo "FATAL: no codegen id in $BR/air-codegen-id.txt" >&2; exit 1; }
OSB=$(sysctl -n kern.osversion)
HOSTT=2828; [ "$(sw_vers -productVersion | cut -d. -f1)" -ge 27 ] && HOSTT=2929

pass=0; fail=0
check() {  # check <name> <0|1 ok> [detail]
  if [ "$2" = 1 ]; then printf '%-66s PASS %s\n' "$1" "${3:-}"; pass=$((pass+1))
  else printf '%-66s FAIL %s\n' "$1" "${3:-}"; fail=$((fail+1)); fi
}
# start <name> <winemetal.so> <base dir> [VAR=value...]: one bridge start; prints its directory's name
start() {
  local n=$1 so=$2 base=$3; shift 3
  env -u D9MT_AIR_MACOS -u D9MT_AIR_LEGACY -u D9MT_AIR_CACHE -u D9MT_PSO_ARCHIVE \
    D9MT_AIR_CACHE="$WORK/$n.air-cache.sqlite3" "$@" D9MT_PSO_ARCHIVE_DIR="$base" \
    "$WORK/pso_partition_test" "$so" > "$WORK/$n.log" 2>&1 || { echo "start $n failed:" >&2; cat "$WORK/$n.log" >&2; }
  sed -n 's|.*d9mt: PSO archives: .* loaded from \(.*\), capture.*|\1|p' "$WORK/$n.log" | head -1 | xargs -I{} basename {}
}
osdirs() { (cd "$1" && ls -d os-* 2>/dev/null | sort | tr '\n' ' '); }

# 1. this build, with stale directories next to it
B=$WORK/base1
mkdir -p "$B/os-$OSB-Apple_GPU" "$B/os-$OSB-Apple_GPU-c0000000000000000-t$HOSTT" "$B/keepme"
L=$(start own "$SO" "$B")
check "this build: c<codegen id, 8 bytes>-t<this Mac's target>" \
  "$([[ "$L" == os-"$OSB"-*-c"${ID:0:16}"-t"$HOSTT" ]] && echo 1 || echo 0)" "$L"
check "0.1.7's and another identity's directories removed, only this one left" \
  "$([ "$(osdirs "$B")" = "$L " ] && echo 1 || echo 0)" "$(osdirs "$B")"
check "a directory not named os-* is left alone" "$([ -d "$B/keepme" ] && echo 1 || echo 0)"

# 2. two starts keep the directory and its contents
echo marker > "$B/$L/marker.txt"
L2=$(start own2 "$SO" "$B")
check "second start: the same directory, its contents kept" \
  "$([ "$L2" = "$L" ] && [ -f "$B/$L/marker.txt" ] && echo 1 || echo 0)" "$L2"

# 3. without the AIR cache
L3=$(start nocache "$SO" "$WORK/base3" D9MT_AIR_CACHE=0)
check "D9MT_AIR_CACHE=0: the same directory" "$([ "$L3" = "$L" ] && echo 1 || echo 0)" "$L3"

# 4. the AIR targets
declare -A T
for m in 26:2828 27:2929 L26:12727 L27:12729; do
  mode=${m%%:*} want=${m#*:}
  case $mode in L*) envs=(D9MT_AIR_LEGACY=1 "D9MT_AIR_MACOS=${mode#L}");; *) envs=("D9MT_AIR_MACOS=$mode");; esac
  T[$mode]=$(start "t$mode" "$SO" "$WORK/base4-$mode" "${envs[@]}")
  check "target $mode: -t$want, the same identity" \
    "$([[ "${T[$mode]}" == *-c"${ID:0:16}"-t"$want" ]] && echo 1 || echo 0)" "${T[$mode]}"
done
check "four targets, four directories" \
  "$([ "$(printf '%s\n' "${T[@]}" | sort -u | wc -l | tr -d ' ')" = 4 ] && echo 1 || echo 0)"
B=$WORK/base5
for mode in 26 L26 27; do
  case $mode in L*) envs=(D9MT_AIR_LEGACY=1 "D9MT_AIR_MACOS=${mode#L}");; *) envs=("D9MT_AIR_MACOS=$mode");; esac
  last=$(start "switch$mode" "$SO" "$B" "${envs[@]}")
done
check "one base, target 26 -> legacy 26 -> 27: only 27's directory left" \
  "$([ "$(osdirs "$B")" = "$last " ] && [ "$last" = "${T[27]}" ] && echo 1 || echo 0)" "$(osdirs "$B")"

# 5. a lineaged id: 0.1.5.1's entry, by a copy of the bridge carrying that id
read -r LID LIDENT < <(python3 -I - "$V2/bridge/unix/air_cache_lineage.c" <<'EOF'
import re, sys
m = re.search(r'\{\s*"([0-9a-f]{64})",\s*\{([^}]*)\}', open(sys.argv[1]).read())
print(m.group(1), ''.join('%02x' % int(x, 16) for x in m.group(2).split(',')[:8]))
EOF
)
python3 -I - "$SO" "$WORK/winemetal-lineage.so" "$ID" "$LID" <<'EOF'
import sys
d = open(sys.argv[1], 'rb').read()
old, new = ('D9MT-AIR-CODEGEN-ID ' + sys.argv[3]).encode(), ('D9MT-AIR-CODEGEN-ID ' + sys.argv[4]).encode()
assert d.count(old) == 1, d.count(old)
open(sys.argv[2], 'wb').write(d.replace(old, new))
EOF
codesign -f -s - "$WORK/winemetal-lineage.so" >/dev/null 2>&1 || true
B=$WORK/base6
LL="os-$OSB-$(echo "$L" | sed "s/^os-$OSB-//; s/-c${ID:0:16}-t$HOSTT\$//")-c$LIDENT-t$HOSTT"
mkdir -p "$B/$LL" "$B/$L"
echo marker > "$B/$LL/marker.txt"
L6=$(start lineage "$WORK/winemetal-lineage.so" "$B")
check "lineaged id: c<its lineage's identity> (0.1.5.1's), not its own" "$([ "$L6" = "$LL" ] && echo 1 || echo 0)" "$L6"
check "lineaged id: that directory kept with its contents, the unlisted id's removed" \
  "$([ -f "$B/$LL/marker.txt" ] && [ "$(osdirs "$B")" = "$LL " ] && echo 1 || echo 0)" "$(osdirs "$B")"

echo "----"
echo "pass=$pass fail=$fail"
[ "$fail" -eq 0 ] && [ "$pass" -gt 0 ]
