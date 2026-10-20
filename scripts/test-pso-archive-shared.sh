#!/usr/bin/env bash
# PSO archive directory shared by several processes (bridge bundle, docs/bridge-bundle.md):
# bridge/tests/compile_mt_test (no Wine) against build/bridge/winemetal.so.
#   young    an archive whose .sum isn't there yet (another process between its two
#            renames) is left alone while young, deleted once old (D9MT_PSO_YOUNG_S, 600 s)
#   orphan   a .sum whose archive isn't there: the same
#   owner    an old delta (older than the consolidated archive) of a process that still runs
#            is left alone; of one that ended, deleted
#   stress   [RUNS] rounds of two processes on one archive directory: one consolidates
#            (idle 200 ms) while the other starts, checks and builds. No crash, no FAIL line,
#            no "dropping PSO archive" (a file passed to newArchiveWithURL that wasn't
#            fine), and afterwards every archive has its sum and the shape.
set -uo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
BR="$V2/bridge"; DX="$V2/third_party/dxbc-spirv"; LLVM="$V2/toolchains/llvm-x86_64"
OUT="$V2/build/pso-shared"; SO="$V2/build/bridge/winemetal.so"
rm -rf "$OUT"; mkdir -p "$OUT"
clang++ -std=c++17 -O1 -g -arch x86_64 -I"$DX" -I"$V2" -I"$LLVM/include" -I"$BR" -I"$V2/airlib" \
  -D__STDC_CONSTANT_MACROS -D__STDC_FORMAT_MACROS -D__STDC_LIMIT_MACROS \
  "$BR/tests/compile_mt_test.cpp" "$V2"/build/airlib/obj/third_party_dxbc-spirv_*.o -lpthread -o "$OUT/compile_mt_test" \
  || { echo "FATAL: harness does not build" >&2; exit 1; }
clang -O1 "$BR/tests/pso_archive_check_test.c" "$BR/unix/pso_archive_check.c" -o "$OUT/pso_archive_check_test" || exit 1
B="$V2/air/tests/bin"; BINS=("$B/vs_mvp.bin" "$B/ps_show.bin" "$B/vs_base.bin" "$B/ps_pp.bin")
fails=0
check() { if [ "$2" = "$3" ]; then echo "  $1: ok"; else echo "  $1: FAIL (got '$2', want '$3')"; fails=$((fails + 1)); fi; }
EXTRA=($(ls "$B"/ps_*.bin))
run() { # <workdir> <log> [env...]: one harness run whose pso dir is $P (bins: BINS + RUN_EXTRA)
  local w=$1 log=$2; shift 2
  mkdir -p "$w"; [ -e "$w/pso" ] || ln -s "$P" "$w/pso"
  env MT_THREADS=2 D9MT_PSO_CONSOLIDATE_IDLE_MS=200 "$@" "$OUT/compile_mt_test" "$SO" "$w" "${BINS[@]}" ${RUN_EXTRA:-} \
    > "$OUT/$log.log" 2>&1
  local rc=$?; [ $rc -ge 128 ] && { echo "  $log: CRASH (signal $((rc - 128)))"; fails=$((fails + 1)); }
  return 0
}
ages() { python3 -I -c 'import os,sys,time; t=time.time()-float(sys.argv[2]); [os.utime(f,(t,t)) for f in sys.argv[3:]]' x "$@"; }
dirof() { ls -d "$P"/os-* | head -1; }

echo "[pso-shared] young / orphan / owner"
P="$OUT/p1"; mkdir -p "$P"
run "$OUT/w1" seed MT_WAIT_CONSOLIDATED=10
D=$(dirof); c=$(ls "$D" | grep -E '^c-.*\.mtl4archive$' | head -1)
[ -n "$c" ] || { echo "FATAL: no consolidated archive to work with" >&2; exit 1; }
# young: a copy without its sum, as between another process's renames
cp -p "$D/$c" "$D/9999999990-1-0000.mtl4archive"; touch "$D/9999999990-1-0000.mtl4archive"
# old: the same, 700 s old
cp -p "$D/$c" "$D/9999999991-1-0000.mtl4archive"; ages 700 "$D/9999999991-1-0000.mtl4archive"
# orphan sums, young and old
cp -p "$D/$c.sum" "$D/9999999992-1-0000.mtl4archive.sum"; touch "$D/9999999992-1-0000.mtl4archive.sum"
cp -p "$D/$c.sum" "$D/9999999993-1-0000.mtl4archive.sum"; ages 700 "$D/9999999993-1-0000.mtl4archive.sum"
# owner: deltas older than the consolidated archive, of a live and of an ended process
sleep 600 & live=$!
bash -c 'exit 0' & dead=$!; wait $dead
cp -p "$D/$c" "$D/0000000001-$live-0000.mtl4archive"; cp -p "$D/$c.sum" "$D/0000000001-$live-0000.mtl4archive.sum"
cp -p "$D/$c" "$D/0000000002-$dead-0000.mtl4archive"; cp -p "$D/$c.sum" "$D/0000000002-$dead-0000.mtl4archive.sum"
run "$OUT/w1" judge
e() { [ -e "$D/$1" ] && echo kept || echo deleted; }
check "young archive without sum" "$(e 9999999990-1-0000.mtl4archive)" kept
check "old archive without sum" "$(e 9999999991-1-0000.mtl4archive)" deleted
check "young orphan sum" "$(e 9999999992-1-0000.mtl4archive.sum)" kept
check "old orphan sum" "$(e 9999999993-1-0000.mtl4archive.sum)" deleted
check "old delta of a live process" "$(e 0000000001-$live-0000.mtl4archive)" kept
check "old delta of an ended process" "$(e 0000000002-$dead-0000.mtl4archive)" deleted
kill $live 2>/dev/null

echo "[pso-shared] stress: ${RUNS:-20} rounds of two processes on one directory"
P="$OUT/p2"; mkdir -p "$P"
for i in $(seq "${RUNS:-20}"); do
  rm -rf "$P"/os-*/*.tmp 2>/dev/null
  # one more pixel shader per round: new PSOs, so every round has misses, a delta and a consolidation
  RUN_EXTRA="${EXTRA[$(( (i - 1) % ${#EXTRA[@]} ))]}" run "$OUT/a$i" "a$i" MT_WAIT_CONSOLIDATED=3 &
  a=$!
  sleep 0.$((RANDOM % 9 + 1))
  run "$OUT/b$i" "b$i"
  wait $a
done
check "FAIL lines" "$(cat "$OUT"/a*.log "$OUT"/b*.log | grep -c '^FAIL')" 0
check "dropped archives" "$(cat "$OUT"/a*.log "$OUT"/b*.log | grep -c 'dropping PSO archive')" 0
D=$(dirof)
nosum=0; for f in "$D"/*.mtl4archive; do [ -e "$f.sum" ] || nosum=$((nosum + 1)); done
check "archives without a sum afterwards" "$nosum" 0
"$OUT/pso_archive_check_test" "$D" | grep -E 'archives have the shape|FAIL real'
"$OUT/pso_archive_check_test" "$D" > /dev/null || fails=$((fails + 1))
echo "  consolidations: $(cat "$OUT"/a*.log "$OUT"/b*.log | grep -c 'PSO archives consolidated')"
echo "[pso-shared] $fails failure(s)"
[ "$fails" -eq 0 ]
