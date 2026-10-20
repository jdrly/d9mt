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
  env MT_THREADS=2 D9MT_PSO_CONSOLIDATE_IDLE_MS=200 MT_COVERAGE=1 "$@" "$OUT/compile_mt_test" "$SO" "$w" "${BINS[@]}" ${RUN_EXTRA:-} \
    > "$OUT/$log.log" 2>&1
  local rc=$?; [ $rc -ge 128 ] && { echo "  $log: CRASH (signal $((rc - 128)))"; echo "CRASH signal $((rc - 128))" >> "$OUT/$log.log"; fails=$((fails + 1)); }
  return 0
}
ages() { python3 -I -c 'import os,sys,time; t=time.time()-float(sys.argv[2]); [os.utime(f,(t,t)) for f in sys.argv[3:]]' x "$@"; }
# resum <archive>: a v2 .sum matching the archive as it is now (size, mtime, SHA-256)
resum() { python3 -I -c 'import os,sys,hashlib; f=sys.argv[1]; st=os.stat(f); h=hashlib.sha256(open(f,"rb").read()).hexdigest(); open(f+".sum","w").write("2 %d %d %s" % (st.st_size, st.st_mtime_ns, h))' "$1"; }
dirof() { ls -d "$P"/os-* | head -1; }

echo "[pso-shared] young / orphan / owner"
P="$OUT/p1"; mkdir -p "$P"
run "$OUT/w1" seed MT_WAIT_CONSOLIDATED=10
D=$(dirof); c=$(ls "$D" | grep -E '^c-.*\.mtl4archive$' | head -1)
[ -n "$c" ] || { echo "FATAL: no consolidated archive to work with" >&2; exit 1; }
# a pid that no longer runs, for files nobody owns
bash -c 'exit 0' & gone=$!; wait $gone
# young: a copy without its sum, as between another process's renames
cp -p "$D/$c" "$D/9999999990-$gone-0000.mtl4archive"; touch "$D/9999999990-$gone-0000.mtl4archive"
# old: the same, 700 s old
cp -p "$D/$c" "$D/9999999991-$gone-0000.mtl4archive"; ages 700 "$D/9999999991-$gone-0000.mtl4archive"
# orphan sums, young and old
cp -p "$D/$c.sum" "$D/9999999992-1-0000.mtl4archive.sum"; touch "$D/9999999992-1-0000.mtl4archive.sum"
cp -p "$D/$c.sum" "$D/9999999993-1-0000.mtl4archive.sum"; ages 700 "$D/9999999993-1-0000.mtl4archive.sum"
# owner: deltas older than the consolidated archive, of a live and of an ended process
sleep 600 & live=$!
bash -c 'exit 0' & dead=$!; wait $dead
# (written 700 s before the consolidated archive, sums matching, so only the age rule decides)
for f in "0000000001-$live-0000" "0000000002-$dead-0000"; do
  cp -p "$D/$c" "$D/$f.mtl4archive"; ages 700 "$D/$f.mtl4archive"; resum "$D/$f.mtl4archive"
done
run "$OUT/w1" judge
e() { [ -e "$D/$1" ] && echo kept || echo deleted; }
check "young archive without sum" "$(e 9999999990-$gone-0000.mtl4archive)" kept
check "old archive without sum" "$(e 9999999991-$gone-0000.mtl4archive)" deleted
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
echo "[pso-shared] quit during consolidation: ${QUITS:-20} runs that exit while it runs"
# idle 0 and a coverage signal: the consolidation starts as the builds end, and the process
# exits at once (this branch still has the atexit flush): no crash, no hang, and the next
# start finds only sound archives
P="$OUT/p3"; mkdir -p "$P"
hangs=0
for i in $(seq "${QUITS:-20}"); do
  RUN_EXTRA="${EXTRA[$(( (i - 1) % ${#EXTRA[@]} ))]}" run "$OUT/q$i" "q$i" D9MT_PSO_CONSOLIDATE_IDLE_MS=0 MT_COVERAGE=1 \
    MT_EXIT_DELAY_MS=$((RANDOM % 1500 + 300)) &
  q=$!
  for t in $(seq 120); do kill -0 $q 2>/dev/null || break; sleep 0.5; done
  if kill -0 $q 2>/dev/null; then hangs=$((hangs + 1)); pkill -9 -f "$OUT/compile_mt_test" 2>/dev/null; fi
  wait $q 2>/dev/null
done
check "hangs at exit" "$hangs" 0
check "crashes at exit" "$(grep -l CRASH "$OUT"/q*.log 2>/dev/null | wc -l | tr -d ' ')" 0
run "$OUT/q-after" q-after
check "next start: dropped archives" "$(grep -c 'dropping PSO archive' "$OUT/q-after.log")" 0
D=$(dirof); "$OUT/pso_archive_check_test" "$D" | grep -E 'archives have the shape|FAIL real'
"$OUT/pso_archive_check_test" "$D" > /dev/null || fails=$((fails + 1))
echo "  consolidations finished before exit: $(cat "$OUT"/q*.log | grep -c 'PSO archives consolidated') of ${QUITS:-20} (the rest were cut by the exit)"

echo "[pso-shared] $fails failure(s)"
[ "$fails" -eq 0 ]
