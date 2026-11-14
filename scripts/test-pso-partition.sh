#!/usr/bin/env bash
# test-pso-partition.sh — the PSO archives' partition (bridge/unix/winemetal_unix.c pso_archive_init),
# without Wine, on scratch directories under $WORK only. Each start is one bridge start
# (bridge/tests/pso_partition_test.cpp: the first Metal device, one MTL4 compiler), which names its
# archive directory os-<OS build>-<GPU>-c<effective AIR identity, 8 bytes>-t<AIR target T>, holds
# that partition's lease (<base>/.locks/<leaf>.lock, shared) and removes every other os-* entry
# whose lease it can take exclusively. Every start's harness must exit 0 (checked at the end).
#  1. This build: c = the codegen id's first 8 bytes (no lineage), t = this Mac's target; 0.1.7's
#     directory (os-<build>-<gpu>) and another identity's are removed; other entries left alone.
#  2. Two starts keep the directory and what's in it.
#  3. D9MT_AIR_CACHE=0: the same directory (the identity opens no database).
#  4. Targets 26, 27, legacy 26 and legacy 27: t2828, t2929, t12727, t12729, four directories;
#     one base switched from one target to the next keeps only the last (no live holder).
#  5. Two live processes, one base: A (scripts/test-compile-mt.sh's compile_mt_test, target 26)
#     builds and flushes PSO archives while B (target 27) starts and sweeps; A's partition
#     survives and A writes archives into it after B's sweep; once A has exited, the next start
#     removes A's partition.
#  6. A lineaged id keeps its predecessor's identity: a copy of winemetal.so whose codegen id
#     string is the 0.1.5.1 lineage's uses c<that lineage's identity>, and a directory of that
#     name already there is kept with its contents.
#  7. A malformed id (a copy with a non-hex id string) uses cunknown and removes no other
#     partition.
#  8. Links: a selected partition that is a link is refused (archives off for that start) and
#     nothing behind it is touched; a stale partition that is a link is unlinked itself, its
#     target kept; a link inside the partition named like an archive is unlinked, its target kept.
#  9. FIFOs never block setup: an archive's .sum, an archive and a .tmp that are FIFOs (no
#     writer) are rejected and removed; a stale partition whose lock file is a FIFO is left alone;
#     the partition's own lock file a FIFO turns archives off for that start. Every start runs
#     under a 60 s alarm, so a hang fails the run instead of stalling it.
# 10. A killed exit keeps the session's archive (#191): compile_mt_test (MT_EXIT=kill) builds
#     pipelines with no flush due, makes the call of bridge/main.c's DLL_PROCESS_DETACH and is
#     killed (SIGKILL), as wineserver kills a Wine process at exit (#164); the next start loads
#     an archive.
# 11. Metal's temporaries (#191): an interrupted serializeAsArchiveAndFlushToURL leaves Metal's own
#     <UUID>.metallib in the partition. At the next start one older than 600 s is removed, a newer
#     one (a live process's flush) is kept, and an old link of that name is unlinked itself, its
#     target kept.
# Needs build/bridge from scripts/build-bridge.sh (and build/airlib's objects for step 5).
set -uo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
BR="$V2/build/bridge"
SO="$BR/winemetal.so"
WORK="${WORK:-$BR/pso-partition-test}"
[ -f "$SO" ] && [ -f "$BR/air-codegen-id.txt" ] || { echo "FATAL: no $SO (run scripts/build-bridge.sh)" >&2; exit 1; }
rm -rf "$WORK"; mkdir -p "$WORK"
clang++ -std=c++17 -O1 -arch x86_64 -I"$V2" -I"$V2/bridge" -I"$V2/airlib" \
  "$V2/bridge/tests/pso_partition_test.cpp" -o "$WORK/pso_partition_test" \
  || { echo "FATAL: harness build failed" >&2; exit 1; }

ID=$(sed -n 's/^AIR codegen \([0-9a-f]\{64\}\) .*/\1/p' "$BR/air-codegen-id.txt")
[ -n "$ID" ] || { echo "FATAL: no codegen id in $BR/air-codegen-id.txt" >&2; exit 1; }
OSB=$(sysctl -n kern.osversion)
HOSTT=2828; [ "$(sw_vers -productVersion | cut -d. -f1)" -ge 27 ] && HOSTT=2929

pass=0; fail=0
check() {  # check <name> <0|1 ok> [detail]
  if [ "$2" = 1 ]; then printf '%-70s PASS %s\n' "$1" "${3:-}"; pass=$((pass+1))
  else printf '%-70s FAIL %s\n' "$1" "${3:-}"; fail=$((fail+1)); fi
}
# start <name> <winemetal.so> <base dir> [VAR=value...]: one bridge start. Prints the name of the
# directory it logged (empty if none); its exit status goes to $WORK/<name>.rc, checked at the end.
start() {
  local n=$1 so=$2 base=$3 rc; shift 3
  perl -e 'alarm shift @ARGV; exec @ARGV or die "exec: $!"' 60 \
    env -u D9MT_AIR_MACOS -u D9MT_AIR_LEGACY -u D9MT_AIR_CACHE -u D9MT_PSO_ARCHIVE \
    D9MT_AIR_CACHE="$WORK/$n.air-cache.sqlite3" "$@" D9MT_PSO_ARCHIVE_DIR="$base" \
    "$WORK/pso_partition_test" "$so" > "$WORK/$n.log" 2>&1
  rc=$?
  echo "$rc" > "$WORK/$n.rc"
  [ "$rc" = 0 ] || return 0
  sed -n 's|.*d9mt: PSO archives: .* loaded from \(.*\), capture.*|\1|p' "$WORK/$n.log" | head -1 | xargs -I{} basename {}
}
osdirs() { (cd "$1" && ls -d os-* 2>/dev/null | sort | tr '\n' ' '); }
envs_of() { case $1 in L*) echo "D9MT_AIR_LEGACY=1 D9MT_AIR_MACOS=${1#L}";; *) echo "D9MT_AIR_MACOS=$1";; esac; }
# patched <out.so> <new id string, 64 chars>: a copy of the bridge with another codegen id string
patched() {
  python3 -I - "$SO" "$1" "$ID" "$2" <<'EOF'
import sys
d = open(sys.argv[1], 'rb').read()
old, new = ('D9MT-AIR-CODEGEN-ID ' + sys.argv[3]).encode(), ('D9MT-AIR-CODEGEN-ID ' + sys.argv[4]).encode()
assert len(old) == len(new) and d.count(old) == 1, d.count(old)
open(sys.argv[2], 'wb').write(d.replace(old, new))
EOF
  codesign -f -s - "$1" >/dev/null 2>&1 || true
}

# 1. this build, with stale directories next to it
B=$WORK/base1
mkdir -p "$B/os-$OSB-Apple_GPU" "$B/os-$OSB-Apple_GPU-c0000000000000000-t$HOSTT" "$B/keepme"
L=$(start own "$SO" "$B")
GPU=$(echo "$L" | sed "s/^os-$OSB-//; s/-c${ID:0:16}-t$HOSTT\$//")
check "this build: c<codegen id, 8 bytes>-t<this Mac's target>" \
  "$([[ "$L" == os-"$OSB"-*-c"${ID:0:16}"-t"$HOSTT" ]] && echo 1 || echo 0)" "$L"
check "0.1.7's and another identity's directories removed, only this one left" \
  "$([ -n "$L" ] && [ "$(osdirs "$B")" = "$L " ] && echo 1 || echo 0)" "$(osdirs "$B")"
check "an entry not named os-* is left alone" "$([ -d "$B/keepme" ] && echo 1 || echo 0)"
check "the partition's lease file is outside the partitions" "$([ -f "$B/.locks/$L.lock" ] && echo 1 || echo 0)"

# 2. two starts keep the directory and its contents
[ -n "$L" ] && echo marker > "$B/$L/marker.txt"
L2=$(start own2 "$SO" "$B")
check "second start: the same directory, its contents kept" \
  "$([ -n "$L" ] && [ "$L2" = "$L" ] && [ -f "$B/$L/marker.txt" ] && echo 1 || echo 0)" "$L2"

# 3. without the AIR cache
L3=$(start nocache "$SO" "$WORK/base3" D9MT_AIR_CACHE=0)
check "D9MT_AIR_CACHE=0: the same directory" "$([ -n "$L" ] && [ "$L3" = "$L" ] && echo 1 || echo 0)" "$L3"

# 4. the AIR targets
declare -A T
for m in 26:2828 27:2929 L26:12727 L27:12729; do
  mode=${m%%:*} want=${m#*:}
  # shellcheck disable=SC2046
  T[$mode]=$(start "t$mode" "$SO" "$WORK/base4-$mode" $(envs_of "$mode"))
  check "target $mode: -t$want, the same identity" \
    "$([[ "${T[$mode]}" == *-c"${ID:0:16}"-t"$want" ]] && echo 1 || echo 0)" "${T[$mode]}"
done
check "four targets, four directories" \
  "$([ "$(printf '%s\n' "${T[@]}" | sort -u | grep -c .)" = 4 ] && echo 1 || echo 0)"
B=$WORK/base5
for mode in 26 L26 27; do
  # shellcheck disable=SC2046
  last=$(start "switch$mode" "$SO" "$B" $(envs_of "$mode"))
done
check "one base, target 26 -> legacy 26 -> 27: only 27's directory left" \
  "$([ -n "$last" ] && [ "$(osdirs "$B")" = "$last " ] && [ "$last" = "${T[27]}" ] && echo 1 || echo 0)" "$(osdirs "$B")"

# 5. two live processes on one base
CM=$WORK/compile_mt_test CM_OK=0
DX="$V2/third_party/dxbc-spirv" LLVM="$V2/toolchains/llvm-x86_64"
if clang++ -std=c++17 -O1 -g -arch x86_64 -I"$DX" -I"$V2" -I"$LLVM/include" -I"$V2/bridge" -I"$V2/airlib" \
     -D__STDC_CONSTANT_MACROS -D__STDC_FORMAT_MACROS -D__STDC_LIMIT_MACROS \
     "$V2/bridge/tests/compile_mt_test.cpp" "$V2"/build/airlib/obj/third_party_dxbc-spirv_*.o -lpthread -o "$CM"; then
  CM_OK=1
  A=$WORK/live-a   # compile_mt_test's archives go to $A/pso
  mkdir -p "$A"
  D9MT_AIR_MACOS=26 D9MT_PSO_ASYNC_FLUSH=0 "$CM" "$SO" "$A" "$V2"/air/tests/bin/*.bin > "$WORK/live-a.log" 2>&1 &
  apid=$!
  for i in $(seq 1 300); do grep -q 'd9mt: PSO archives: .* loaded from' "$WORK/live-a.log" && break; sleep 0.05; done
  LA=$(sed -n 's|.*d9mt: PSO archives: .* loaded from \(.*\), capture.*|\1|p' "$WORK/live-a.log" | head -1 | xargs -I{} basename {})
  LB=$(start liveb "$SO" "$A/pso" D9MT_AIR_MACOS=27)
  touch "$WORK/after-b"
  b_during=$(kill -0 "$apid" 2>/dev/null && echo 1 || echo 0)
  check "two live processes: B (27) started while A (26) was running" "$b_during" "A $LA, B $LB"
  check "two live processes: B's sweep kept A's partition" \
    "$([ -n "$LA" ] && [ -d "$A/pso/$LA" ] && [ -n "$LB" ] && [ -d "$A/pso/$LB" ] && echo 1 || echo 0)" "$(osdirs "$A/pso")"
  wait "$apid"; arc=$?
  check "two live processes: A finished cleanly" "$([ "$arc" = 0 ] && echo 1 || echo 0)" "rc $arc"
  newer=$(find "$A/pso/$LA" -name '*.mtl4archive' -newer "$WORK/after-b" 2>/dev/null | wc -l | tr -d ' ')
  check "two live processes: A wrote archives into its partition after B's sweep" \
    "$([ "${newer:-0}" -gt 0 ] && echo 1 || echo 0)" "$newer archives"
  LB2=$(start liveb2 "$SO" "$A/pso" D9MT_AIR_MACOS=27)
  check "after A exited: the next start removes A's partition" \
    "$([ -n "$LB2" ] && [ "$(osdirs "$A/pso")" = "$LB2 " ] && echo 1 || echo 0)" "$(osdirs "$A/pso")"
else
  check "two live processes: compile_mt_test builds (needs build/airlib objects)" 0
fi

# 6. a lineaged id: 0.1.5.1's entry, by a copy of the bridge carrying that id
read -r LID LIDENT < <(python3 -I - "$V2/bridge/unix/air_cache_lineage.c" <<'EOF'
import re, sys
m = re.search(r'\{\s*"([0-9a-f]{64})",\s*\{([^}]*)\}', open(sys.argv[1]).read())
print(m.group(1), ''.join('%02x' % int(x, 16) for x in m.group(2).split(',')[:8]))
EOF
)
patched "$WORK/winemetal-lineage.so" "$LID"
B=$WORK/base6
LL="os-$OSB-$GPU-c$LIDENT-t$HOSTT"
mkdir -p "$B/$LL" "$B/$L"
echo marker > "$B/$LL/marker.txt"
L6=$(start lineage "$WORK/winemetal-lineage.so" "$B")
check "lineaged id: c<its lineage's identity> (0.1.5.1's), not its own" "$([ "$L6" = "$LL" ] && echo 1 || echo 0)" "$L6"
check "lineaged id: that directory kept with its contents, the unlisted id's removed" \
  "$([ -f "$B/$LL/marker.txt" ] && [ "$(osdirs "$B")" = "$LL " ] && echo 1 || echo 0)" "$(osdirs "$B")"

# 7. a malformed id: cunknown, and no other partition removed
patched "$WORK/winemetal-badid.so" "$(printf 'z%.0s' $(seq 1 64))"
B=$WORK/base7
mkdir -p "$B/$L" "$B/os-$OSB-Apple_GPU"
echo marker > "$B/$L/marker.txt"
L7=$(start badid "$WORK/winemetal-badid.so" "$B")
check "malformed id: the cunknown partition" "$([ "$L7" = "os-$OSB-$GPU-cunknown-t$HOSTT" ] && echo 1 || echo 0)" "$L7"
check "malformed id: every other partition kept, contents and all" \
  "$([ -f "$B/$L/marker.txt" ] && [ -d "$B/os-$OSB-Apple_GPU" ] && echo 1 || echo 0)" "$(osdirs "$B")"
check "malformed id: the log says so" "$(grep -q 'no AIR identity: other partitions kept' "$WORK/badid.log" && echo 1 || echo 0)"

# 8. links
EXT=$WORK/external
mkdir -p "$EXT/sel" "$EXT/stale"
for d in sel stale; do
  echo sentinel > "$EXT/$d/0000000001-1-0000.mtl4archive"
  echo sentinel > "$EXT/$d/0000000001-1-0000.mtl4archive.sum.orphan"
  echo sentinel > "$EXT/$d/old.tmp"; touch -t 202001010000 "$EXT/$d/old.tmp"
done
echo sentinel > "$EXT/file.mtl4archive"
B=$WORK/base8
mkdir -p "$B"
ln -s "$EXT/sel" "$B/$L"
ln -s "$EXT/stale" "$B/os-$OSB-Apple_GPU-c0000000000000000-t$HOSTT"
L8=$(start links "$SO" "$B")
check "a linked partition: refused, archives off for that start" \
  "$(grep -q 'is not a plain directory' "$WORK/links.log" && [ -z "$L8" ] && echo 1 || echo 0)" "$L8"
check "a linked partition: nothing behind the link touched" \
  "$([ "$(ls "$EXT/sel" | wc -l | tr -d ' ')" = 3 ] && echo 1 || echo 0)" "$(ls "$EXT/sel" | tr '\n' ' ')"
rm -f "$B/$L"
L8b=$(start links2 "$SO" "$B")
check "a linked stale partition: the link removed, its target kept whole" \
  "$([ ! -e "$B/os-$OSB-Apple_GPU-c0000000000000000-t$HOSTT" ] && [ "$(ls "$EXT/stale" | wc -l | tr -d ' ')" = 3 ] && echo 1 || echo 0)" \
  "$(ls "$EXT/stale" | tr '\n' ' ')"
if [ -n "$L8b" ]; then
  ln -s "$EXT/file.mtl4archive" "$B/$L8b/0000000002-1-0000.mtl4archive"
  start links3 "$SO" "$B" > /dev/null
  check "a link inside the partition: unlinked itself, its target kept" \
    "$([ ! -L "$B/$L8b/0000000002-1-0000.mtl4archive" ] && [ -f "$EXT/file.mtl4archive" ] && echo 1 || echo 0)"
else
  check "a link inside the partition: (no partition: the previous start failed)" 0
fi

# 9. FIFOs (no writer) never block setup
B=$WORK/base9
L9=$(start fifo0 "$SO" "$B")
if [ -n "$L9" ]; then
  P=$B/$L9
  echo junk > "$P/0000000001-1-0000.mtl4archive"; mkfifo "$P/0000000001-1-0000.mtl4archive.sum"
  mkfifo "$P/0000000002-1-0000.mtl4archive"; echo "0 x" > "$P/0000000002-1-0000.mtl4archive.sum"
  mkfifo "$P/0000000003-1-0000.mtl4archive.sum.tmp"
  STALE="os-$OSB-$GPU-c0000000000000000-t$HOSTT"
  mkdir -p "$B/$STALE"; mkfifo "$B/.locks/$STALE.lock"
  t0=$(date +%s)
  L9b=$(start fifo1 "$SO" "$B")
  t1=$(date +%s)
  check "FIFOs: setup finished (no hang) in the same partition" \
    "$([ "$L9b" = "$L9" ] && [ $((t1 - t0)) -lt 30 ] && echo 1 || echo 0)" "$((t1 - t0)) s"
  check "FIFOs: the FIFO .sum and the FIFO archive rejected and removed with their pairs" \
    "$([ ! -e "$P/0000000001-1-0000.mtl4archive.sum" ] && [ ! -e "$P/0000000001-1-0000.mtl4archive" ] \
       && [ ! -e "$P/0000000002-1-0000.mtl4archive" ] && [ ! -e "$P/0000000002-1-0000.mtl4archive.sum" ] && echo 1 || echo 0)" \
    "$(ls "$P" | tr '\n' ' ')"
  check "FIFOs: a stale partition whose lock file is a FIFO is left alone" "$([ -d "$B/$STALE" ] && echo 1 || echo 0)"
  B=$WORK/base10
  mkdir -p "$B/.locks"; mkfifo "$B/.locks/$L.lock"
  L10=$(start fifolock "$SO" "$B")
  check "FIFOs: the own lock file a FIFO: archives off for that start, no hang" \
    "$(grep -q 'no lease on' "$WORK/fifolock.log" && [ -z "$L10" ] && echo 1 || echo 0)"
else
  check "FIFOs: (no partition: the first start failed)" 0
fi

# 10. a killed exit (#191)
if [ "$CM_OK" = 1 ]; then
  K=$WORK/kill
  mkdir -p "$K"
  perl -e 'alarm shift @ARGV; exec @ARGV or die "exec: $!"' 300 \
    env -u D9MT_AIR_MACOS -u D9MT_AIR_LEGACY -u D9MT_PSO_ARCHIVE D9MT_PSO_ASYNC_FLUSH=0 MT_EXIT=kill \
    "$CM" "$SO" "$K" "$V2"/air/tests/bin/{vs_base,vs_mvp,ps_show,ps_texld}.bin > "$WORK/kill-run.log" 2>&1
  krc=$?
  nb=$(sed -n 's/^psos: \([0-9]*\) ok.*/\1/p' "$WORK/kill-run.log")
  check "killed exit: the run built pipelines, then was killed (SIGKILL)" \
    "$([ "$krc" = 137 ] && [ "${nb:-0}" -gt 0 ] && echo 1 || echo 0)" "rc $krc, ${nb:-0} pipelines"
  start killnext "$SO" "$K/pso" > /dev/null
  nl=$(sed -n 's/.*d9mt: PSO archives: \([0-9]*\) loaded from.*/\1/p' "$WORK/killnext.log" | head -1)
  check "killed exit: the next start loads the session's archive" \
    "$([ "${nl:-0}" -gt 0 ] && echo 1 || echo 0)" "${nl:-no log line}: $(ls "$K/pso"/os-*/ 2>/dev/null | tr '\n' ' ')"
else
  check "killed exit: compile_mt_test builds (needs build/airlib objects)" 0
fi

# 11. Metal's temporaries in the partition (#191)
B=$WORK/base11
L11=$(start metal0 "$SO" "$B")
if [ -n "$L11" ]; then
  P=$B/$L11
  OLD=8F0C2A4E-6B1D-4C7A-9E35-0D2B7A1C4E58.metallib
  NEW=1A2B3C4D-5E6F-4A1B-8C2D-3E4F5A6B7C8D.metallib
  LNK=5C6D7E8F-9A0B-4C1D-8E2F-3A4B5C6D7E8F.metallib
  head -c 65536 /dev/zero > "$P/$OLD"; touch -t 202001010000 "$P/$OLD"
  head -c 65536 /dev/zero > "$P/$NEW"
  echo sentinel > "$WORK/external.metallib"; touch -t 202001010000 "$WORK/external.metallib"
  ln -s "$WORK/external.metallib" "$P/$LNK"; touch -h -t 202001010000 "$P/$LNK"
  start metal1 "$SO" "$B" > /dev/null
  check "Metal's temporaries: a <UUID>.metallib older than 600 s removed" \
    "$([ ! -e "$P/$OLD" ] && echo 1 || echo 0)" "$(ls "$P" | tr '\n' ' ')"
  check "Metal's temporaries: a newer one kept" "$([ -f "$P/$NEW" ] && echo 1 || echo 0)"
  check "Metal's temporaries: an old link of that name unlinked itself, its target kept" \
    "$([ ! -L "$P/$LNK" ] && [ -f "$WORK/external.metallib" ] && echo 1 || echo 0)"
else
  check "Metal's temporaries: (no partition: the first start failed)" 0
fi

# every start's harness exited 0
bad=$(cd "$WORK" && grep -L '^0$' ./*.rc 2>/dev/null | sed 's|^\./||; s|\.rc$||' | tr '\n' ' ')
check "every start's harness exited 0" "$([ -z "$bad" ] && echo 1 || echo 0)" "${bad:+failed: $bad}"

echo "----"
echo "pass=$pass fail=$fail"
[ "$fail" -eq 0 ] && [ "$pass" -gt 0 ]
