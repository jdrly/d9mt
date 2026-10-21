#!/usr/bin/env bash
# PSO archive directory shared by several processes (bridge bundle, docs/bridge-bundle.md):
# bridge/tests/compile_mt_test (no Wine) against build/bridge/winemetal.so.
#   young    an archive whose .sum isn't there yet (another process between its two
#            renames) is left alone while young, deleted once old (D9MT_PSO_YOUNG_S, 600 s)
#   orphan   a .sum whose archive isn't there: the same
#   owner    an old delta (older than the consolidated archive) of a process that still runs
#            is left alone; of one that ended, deleted
#   exe      the directory per executable (<base>/<exe>/os-...): argv[0]'s basename as Wine
#            gives it, sanitized; not an .exe: the flat layout with archive first off
#   migrate  [MIGRATIONS] rounds: two executables start at once on a flat directory of an
#            earlier release: exactly one takes it, the other starts cold, no crash; a flat
#            start afterwards doesn't touch either executable's directory
#   quit     [QUITS] runs that exit while a consolidation or flush runs: no crash, no hang;
#            the next start finds sound archives and removes stray .metallib files older
#            than 600 s (Metal's intermediate of an interrupted flush), keeping a young one
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
dirof() { ls -d "$P"/mt.exe/os-* | head -1; }

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

echo "[pso-shared] exe: the directory per executable"
loaded_from() { sed -n 's/^d9mt: PSO archives: [0-9]* loaded from \(.*\), capture.*/\1/p' "$OUT/$1.log"; }
P="$OUT/e"; mkdir -p "$P"; i=0
for c in 'C:\Program Files\SWTOR\retailclient\SWTOR.EXE|swtor.exe' '/games/My Game!.exe|my_game_.exe' \
         'C:\x\OS-x.exe|os_x.exe' "C:\\$(printf 'a%.0s' $(seq 70)).exe|$(printf 'a%.0s' $(seq 64))"; do
  i=$((i + 1)); run "$OUT/e$i" "e$i" MT_COVERAGE=0 MT_EXE="${c%%|*}"
  check "exe ${c%%|*}" "$(loaded_from "e$i" | sed 's|.*/pso/||; s|/os-.*||')" "${c##*|}"
done
for c in 'C:\games\launcher' 'C:\games\' 'game.exe.bak'; do
  i=$((i + 1)); run "$OUT/e$i" "e$i" MT_COVERAGE=0 MT_EXE="$c"
  check "not an exe: $c: flat" "$(loaded_from "e$i" | sed 's|.*/pso/||')" "$(basename "$(loaded_from "e$i")")"
  check "not an exe: $c: archive first off" "$(grep -c 'archive first off' "$OUT/e$i.log")" 1
done

echo "[pso-shared] migrate: ${MIGRATIONS:-10} rounds of two executables on a flat directory"
claims=0; colds=0; flatleft=0
for r in $(seq "${MIGRATIONS:-10}"); do
  P="$OUT/m$r"; mkdir -p "$P"
  run "$OUT/m$r-seed" "m$r-seed" MT_COVERAGE=0 MT_EXE=          # an earlier release: flat
  flat=$(loaded_from "m$r-seed")
  run "$OUT/m$r-a" "m$r-a" MT_COVERAGE=0 MT_EXE='C:\g\a.exe' & pa=$!
  run "$OUT/m$r-b" "m$r-b" MT_COVERAGE=0 MT_EXE='C:\g\b.exe' & pb=$!
  wait $pa $pb
  got=$(cat "$OUT/m$r-a.log" "$OUT/m$r-b.log" | grep -c 'of earlier releases now')
  [ "$got" = 1 ] && claims=$((claims + 1))
  na=$(sed -n 's/^d9mt: PSO archives: \([0-9]*\) loaded.*/\1/p' "$OUT/m$r-a.log")
  nb=$(sed -n 's/^d9mt: PSO archives: \([0-9]*\) loaded.*/\1/p' "$OUT/m$r-b.log")
  { [ "$na" = 0 ] && [ "$nb" -gt 0 ]; } || { [ "$nb" = 0 ] && [ "$na" -gt 0 ]; } && colds=$((colds + 1))
  [ -e "$flat" ] && flatleft=$((flatleft + 1))
  if [ "$r" = 1 ]; then
    # a flat start (an executable that isn't one, or an earlier release) and an archive set of
    # another macOS build: the flat cleanup takes the latter, never an executable's directory
    before=$(ls "$P"/a.exe/os-*/ "$P"/b.exe/os-*/ | grep -c 'mtl4archive$')
    mkdir -p "$P/os-0OLD-gpu" "$P/a.exe/os-0OLD-gpu"
    run "$OUT/m$r-flat" "m$r-flat" MT_COVERAGE=0 MT_EXE=
    check "flat start: executables' archives untouched" "$(ls "$P"/a.exe/os-*/ "$P"/b.exe/os-*/ | grep -c 'mtl4archive$')" "$before"
    check "flat start: another build's flat set removed" "$([ -e "$P/os-0OLD-gpu" ] && echo kept || echo removed)" removed
    run "$OUT/m$r-a2" "m$r-a2" MT_COVERAGE=0 MT_EXE='C:\g\a.exe'
    check "exe start: another build's set in its directory removed" "$([ -e "$P/a.exe/os-0OLD-gpu" ] && echo kept || echo removed)" removed
    check "exe start: the flat directory left alone" "$([ -e "$flat" ] && echo kept || echo removed)" kept
  fi
done
check "rounds where exactly one took the flat directory" "$claims" "${MIGRATIONS:-10}"
check "rounds where the other started cold" "$colds" "${MIGRATIONS:-10}"
check "rounds with the flat directory left behind" "$flatleft" 0
check "crashes" "$(cat "$OUT"/m*.log | grep -c '^CRASH')" 0

echo "[pso-shared] stress: ${RUNS:-20} rounds of two processes on one directory"
P="$OUT/p2"; mkdir -p "$P"
for i in $(seq "${RUNS:-20}"); do
  rm -rf "$P"/mt.exe/os-*/*.tmp 2>/dev/null
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
# an exit during a flush can leave serializeAsArchiveAndFlushToURL's intermediate file (a
# UUID .metallib, as large as the archive): the next start removes those older than
# D9MT_PSO_YOUNG_S (600 s) and leaves a young one (a flush of another process) alone
D=$(dirof)
strays=$(ls "$D" | grep -c '\.metallib$')
echo "  stray .metallib after the quit runs: $strays (aged 700 s for the next start)"
printf 'old' > "$D/11111111-1111-1111-1111-111111111111.metallib"   # one for certain
for f in "$D"/*.metallib; do [ -e "$f" ] && ages 700 "$f"; done
printf 'young' > "$D/00000000-0000-0000-0000-000000000000.metallib"
run "$OUT/q-after" q-after
check "next start: dropped archives" "$(grep -c 'dropping PSO archive' "$OUT/q-after.log")" 0
check "next start: old stray .metallib left" "$(ls "$D" | grep -v '^00000000-' | grep -c '\.metallib$')" 0
check "next start: a young .metallib kept" "$([ -e "$D/00000000-0000-0000-0000-000000000000.metallib" ] && echo kept || echo deleted)" kept
rm -f "$D/00000000-0000-0000-0000-000000000000.metallib"
D=$(dirof); "$OUT/pso_archive_check_test" "$D" | grep -E 'archives have the shape|FAIL real'
"$OUT/pso_archive_check_test" "$D" > /dev/null || fails=$((fails + 1))
echo "  consolidations finished before exit: $(cat "$OUT"/q*.log | grep -c 'PSO archives consolidated') of ${QUITS:-20} (the rest were cut by the exit)"

echo "[pso-shared] $fails failure(s)"
[ "$fails" -eq 0 ]
