#!/usr/bin/env bash
# PSO archive first + consolidation (bridge bundle parts 3 and 4, docs/bridge-bundle.md):
# bridge/tests/compile_mt_test (no Wine) against build/bridge/winemetal.so.
#   cold     everything misses and is captured; after the idle period one consolidated
#            archive (c-...) replaces the deltas flushed before it
#   warm     every PSO is an archive hit ("N hits, 0 misses"); hits are not captured: the
#            run writes no delta of its own (the regression test of the verification's item 5)
#   miss     a run with more shaders on the warm archives: the new PSOs miss and are compiled;
#            the results (metallib hashes, PSO ok/fail) equal a cold run of the same set
#   R1       D9MT_STATE_CACHE=0: archive first off, hits through lookupArchives are captured
#            (a delta of this run), no consolidation
#   F-1      a delta flushed while the consolidation rebuilds (D9MT_PSO_CONSOLIDATE_DELAY_MS
#            holds it after its snapshot) is newer than the consolidated archive's name and so
#            kept at the next start, where its PSOs are hits
#   cap      only the newest D9MT_PSO_ARCHIVE_FIRST_MAX (8) deltas are asked first: a PSO only
#            in an older one is a miss built through the compiler's lookupArchives, consistent
#            with the run that recorded it, and captured again; with a cap of 64, a hit
#   restore  archives copied without their times: the SHA-256 decides (same size, other time),
#            the sums are rewritten; a byte changed in the copy: deleted
#   part 5   the flush thread: idle (a delta before exit, the next run all hits); nothing at
#            exit (idle never reached: no delta, the next run misses); the Reset hint and the
#            backstop flush without the idle; M9: builds while a flush holds the lock (held
#            3 s, D9MT_PSO_FLUSH_HOLD_MS) go to the serializer-less compiler, all succeed, are
#            not captured, and miss again in the next run; S-1: a build's count comes before
#            its unlock (every other count held 600 ms: no flush of a drained serializer)
#   part 6   consolidated sets: the startup window (at once at coverage) and outside it (none);
#            chunks + marker (the next start loads the set); an exit between chunks (no marker:
#            the old archives serve, the chunks go only once old and ownerless); a second process
#            while one consolidates (neither loads nor deletes its chunks); yields to requests;
#            the capture flush at FLUSH_EVERY after a quiet gap; a warm start whose consolidation
#            deletes the loaded parts, then misses (Metal throws for a deleted lookup archive it
#            never converted: pso_convert_lookups; unlink_lookup_test shows both); the conversion
#            late (no consolidation that session); .verify (a one-time SHA-256 check), written by
#            whichever compile meets the first lookup failure while every loaded file is there
#   TSan     the cold, warm, part 5 and part 6 runs under ThreadSanitizer (TSAN=0 skips; needs test-compile-mt.sh's
#            TSan winemetal, built here)
# Needs build-bridge.sh to have run (airlib objects, LLVM toolchain).
set -uo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
BR="$V2/bridge"; DX="$V2/third_party/dxbc-spirv"; LLVM="$V2/toolchains/llvm-x86_64"
OUT="$V2/build/pso-first"; SO="$V2/build/bridge/winemetal.so"
rm -rf "$OUT"; mkdir -p "$OUT"
INC=(-std=c++17 -O1 -g -arch x86_64 -I"$DX" -I"$V2" -I"$LLVM/include" -I"$BR" -I"$V2/airlib"
     -D__STDC_CONSTANT_MACROS -D__STDC_FORMAT_MACROS -D__STDC_LIMIT_MACROS)
clang++ "${INC[@]}" "$BR/tests/compile_mt_test.cpp" "$V2"/build/airlib/obj/third_party_dxbc-spirv_*.o -lpthread \
  -o "$OUT/compile_mt_test" || { echo "FATAL: harness does not build" >&2; exit 1; }
B="$V2/air/tests/bin"
SET_A=("$B/vs_mvp.bin" "$B/ps_show.bin" "$B/vs_base.bin" "$B/ps_pp.bin")
SET_B=("${SET_A[@]}" "$B/vs_texcoords.bin" "$B/ps_texcoords.bin")
fails=0
check() { if [ "$2" = "$3" ]; then echo "  $1: ok ($2)"; else echo "  $1: FAIL (got '$2', want '$3')"; fails=$((fails + 1)); fi; }
run() { # <dir> <log> <env...> -- <bins...>
  local d=$1 log=$2; shift 2
  local envs=(); while [ "$1" != -- ]; do envs+=("$1"); shift; done; shift
  env MT_THREADS=2 D9MT_PSO_CONSOLIDATE_IDLE_MS=500 MT_COVERAGE=1 ${envs[@]+"${envs[@]}"} "$HARNESS" "$SO" "$d" "$@" > "$OUT/$log.log" 2>&1
  local rc=$?; [ $rc -ge 128 ] && { echo "  $log: CRASH (signal $((rc - 128)))"; fails=$((fails + 1)); }
  grep -q '^FAIL' "$OUT/$log.log" && { echo "  $log: FAIL lines:"; grep '^FAIL' "$OUT/$log.log" | head -5; fails=$((fails + 1)); }
}
ages() { python3 -I -c 'import os,sys,time; t=time.time()-float(sys.argv[1]); [os.utime(f,(t,t)) for f in sys.argv[2:]]' "$@"; }
stat_line() { sed -n 's/^d9mt: PSO archive first: \(.*\)$/\1/p' "$OUT/$1.log"; }
deltas() { ls "$1"/pso/mt.exe/os-*/ 2>/dev/null | grep -E '^[0-9]+-[0-9]+-[0-9]+\.mtl4archive$' | wc -l | tr -d ' '; }
own_deltas() { ls "$1"/pso/mt.exe/os-*/ 2>/dev/null | grep -cE "^[0-9]+-$2-[0-9]+\.mtl4archive$"; }
pid_of() { sed -n 's/^pid: //p' "$OUT/$1.log"; }
consolidated() { ls "$1"/pso/mt.exe/os-*/ 2>/dev/null | grep -cE '^c-.*\.mtl4archive$'; }

suite() { # <label>
  local W="$OUT/$1-w"
  echo "[pso-first:$1] C-1: no consolidation without the state cache's coverage signal"
  local N="$OUT/$1-nocov"
  run "$N" "$1-nocov-seed" MT_COVERAGE=0 -- "${SET_A[@]}"
  run "$N" "$1-nocov-unset" MT_COVERAGE= MT_WAIT_CONSOLIDATED=3 -- "${SET_A[@]}"
  check "no signal: no consolidation" "$(grep -c 'consolidated archive: written' "$OUT/$1-nocov-unset.log")" 0
  run "$N" "$1-nocov-0" MT_COVERAGE=0 MT_WAIT_CONSOLIDATED=3 -- "${SET_A[@]}"
  check "incomplete coverage: no consolidation" "$(grep -c 'consolidated archive: written' "$OUT/$1-nocov-0.log")" 0
  check "no consolidated archive" "$(consolidated "$N")" 0
  echo "[pso-first:$1] R2: no visibility query, no pause: a long idle means no consolidation"
  run "$N" "$1-r2" D9MT_PSO_STARTUP_WINDOW_S=0 D9MT_PSO_CONSOLIDATE_IDLE_MS=600000 MT_WAIT_CONSOLIDATED=3 -- "${SET_A[@]}"
  check "R2: harness run not taken for a pause" "$(grep -c 'consolidated archive: written' "$OUT/$1-r2.log")" 0

  echo "[pso-first:$1] cold"
  run "$W" "$1-cold" MT_WAIT_CONSOLIDATED=20 -- "${SET_A[@]}"
  na=$(sed -n 's/.*, \([0-9]*\) not archived yet$/\1/p' "$OUT/$1-cold.log")
  echo "  cold: consolidation never compiles: ${na:-?} unflushed misses left to a later delta"
  local cname; cname=$(ls "$W"/pso/mt.exe/os-*/ | grep -E '^c-.*\.mtl4archive$' | head -1)
  local first; first=$(ls "$W"/pso/mt.exe/os-*/ | grep -E '^[0-9]+-' | sort | head -1)
  echo "  cold: consolidated as $cname (named by consolidation time)"
  check "consolidated archive written" "$(grep -c 'consolidated archive: written' "$OUT/$1-cold.log")" 1
  check "archives after cold: consolidated" "$(consolidated "$W")" 1
  check "first-run misses" "$(stat_line "$1-cold" | sed 's/.* \([0-9]*\) misses/\1/')" "$(grep -o 'psos: [0-9]* ok' "$OUT/$1-cold.log" | grep -o '[0-9]*' | head -1)"

  echo "[pso-first:$1] warm"
  run "$W" "$1-warm" -- "${SET_A[@]}"
  local ok; ok=$(grep -o 'psos: [0-9]* ok' "$OUT/$1-warm.log" | grep -o '[0-9]*' | head -1)
  check "warm: all hits" "$(stat_line "$1-warm")" "$ok hits, 0 misses"
  check "warm: no delta of its own (hits not captured)" "$(own_deltas "$W" "$(pid_of "$1-warm")")" 0
  grep -q 'consistent' "$OUT/$1-warm.log" && echo "  warm: results consistent with cold" \
    || { echo "  warm: FAIL (results differ)"; fails=$((fails + 1)); }

  echo "[pso-first:$1] miss falls back to the compiler"
  local M="$OUT/$1-miss" R="$OUT/$1-ref"
  cp -Rp "$W" "$M"; rm -f "$M/results.txt"; mkdir -p "$R"
  run "$R" "$1-ref" -- "${SET_B[@]}"
  run "$M" "$1-miss" -- "${SET_B[@]}"
  local misses; misses=$(stat_line "$1-miss" | sed 's/.* \([0-9]*\) misses/\1/')
  [ "${misses:-0}" -gt 0 ] && echo "  miss: $misses misses compiled" || { echo "  miss: FAIL (no misses)"; fails=$((fails + 1)); }
  cmp -s "$R/results.txt" "$M/results.txt" && echo "  miss: results equal a cold run" \
    || { echo "  miss: FAIL (results differ from a cold run)"; diff "$R/results.txt" "$M/results.txt" | head -5; fails=$((fails + 1)); }

  echo "[pso-first:$1] part 5: the flush thread"
  local N5="$OUT/$1-p5" lateok
  run "$N5-idle" "$1-p5-idle" MT_COVERAGE=0 MT_FLUSH_EVERY=1000 -- "${SET_A[@]}"
  [ "$(own_deltas "$N5-idle" "$(pid_of "$1-p5-idle")")" -gt 0 ] && echo "  idle: a delta before exit" \
    || { echo "  idle: FAIL (no delta)"; fails=$((fails + 1)); }
  run "$N5-idle" "$1-p5-idle2" MT_COVERAGE=0 -- "${SET_A[@]}"
  check "idle: next run misses" "$(stat_line "$1-p5-idle2" | sed 's/.* \([0-9]*\) misses/\1/')" 0
  run "$N5-exit" "$1-p5-exit" MT_COVERAGE=0 D9MT_PSO_FLUSH_IDLE_MS=600000 MT_FLUSH_EVERY=1000 -- "${SET_A[@]}"
  check "no flush at exit: deltas" "$(own_deltas "$N5-exit" "$(pid_of "$1-p5-exit")")" 0
  run "$N5-hint" "$1-p5-hint" MT_COVERAGE=0 D9MT_PSO_FLUSH_IDLE_MS=600000 MT_FLUSH_EVERY=1000 MT_HINT=1 -- "${SET_A[@]}"
  [ "$(own_deltas "$N5-hint" "$(pid_of "$1-p5-hint")")" -gt 0 ] && echo "  Reset hint: a delta without the idle" \
    || { echo "  Reset hint: FAIL (no delta)"; fails=$((fails + 1)); }
  run "$N5-back" "$1-p5-back" MT_COVERAGE=0 D9MT_PSO_FLUSH_IDLE_MS=600000 D9MT_PSO_FLUSH_BACKSTOP_MS=300 -- "${SET_A[@]}"
  [ "$(own_deltas "$N5-back" "$(pid_of "$1-p5-back")")" -gt 0 ] && echo "  backstop: a delta without the idle" \
    || { echo "  backstop: FAIL (no delta)"; fails=$((fails + 1)); }
  run "$N5-m9" "$1-p5-m9" MT_COVERAGE=0 D9MT_PSO_FLUSH_HOLD_MS=3000 MT_LATE_PSOS=1000 MT_SETTLE_MS=3500 -- "${SET_A[@]}"
  lateok=$(sed -n 's/^late psos: \([0-9]*\) ok.*/\1/p' "$OUT/$1-p5-m9.log")
  check "M9: late builds during the held flush all succeed" "$(sed -n 's/^late psos: [0-9]* ok, \([0-9]*\) failed/\1/p' "$OUT/$1-p5-m9.log")" 0
  check "M9: built by the serializer-less compiler, not captured" "$(sed -n 's/.*not captured (a flush held the lock): //p' "$OUT/$1-p5-m9.log")" "$lateok"
  run "$N5-m9" "$1-p5-m9b" MT_COVERAGE=0 MT_LATE_PSOS=0 -- "${SET_A[@]}"
  check "M9: those miss again in the next run" "$(stat_line "$1-p5-m9b" | sed 's/.* \([0-9]*\) misses/\1/')" "$lateok"

  # S-1: every other build's count delayed 600 ms (D9MT_PSO_NOTE_DELAY_MS): counted before the
  # unlock, no flush can come between a build and its count, so none writes a drained serializer
  run "$N5-s1" "$1-p5-s1" MT_COVERAGE=0 D9MT_PSO_NOTE_DELAY_MS=600 MT_SETTLE_MS=2500 -- "${SET_A[@]}"
  check "S-1: no flush of a drained serializer" "$(grep -c 'PSO archive flush failed' "$OUT/$1-p5-s1.log")" 0
  run "$N5-s1" "$1-p5-s1b" MT_COVERAGE=0 -- "${SET_A[@]}"
  check "S-1: next run misses" "$(stat_line "$1-p5-s1b" | sed 's/.* \([0-9]*\) misses/\1/')" 0

  echo "[pso-first:$1] part 6: consolidated sets"
  local N6="$OUT/$1-p6" L
  ldd() { sed -n 's/^d9mt: PSO archives: \([0-9]*\) loaded.*/\1/p' "$OUT/$1.log"; }
  chunks() { ls "$1"/pso/mt.exe/os-*/ 2>/dev/null | grep -cE '^c-[0-9]+-[0-9]+-[0-9]+\.mtl4archive$'; }
  markers() { ls "$1"/pso/mt.exe/os-*/ 2>/dev/null | grep -cE '^c-[0-9]+-[0-9]+\.set$'; }
  misses_in() { stat_line "$1" | sed 's/.* \([0-9]*\) misses/\1/'; }
  # the startup window: the coverage signal (right after the builds) is in it, so at once, no idle
  run "$N6-sw" "$1-p6-sw" D9MT_PSO_CONSOLIDATE_IDLE_MS=600000 MT_WAIT_CONSOLIDATED=20 -- "${SET_A[@]}"
  check "startup window: decided" "$(grep -c 'consolidating now (startup window)' "$OUT/$1-p6-sw.log")" 1
  check "startup window: a complete set without the idle" "$(markers "$N6-sw")" 1
  # outside it (window 0), no pause in the harness, no fallback yet: none
  run "$N6-out" "$1-p6-out" D9MT_PSO_STARTUP_WINDOW_S=0 D9MT_PSO_CONSOLIDATE_IDLE_MS=300 MT_WAIT_CONSOLIDATED=3 -- "${SET_A[@]}"
  check "outside the startup window: decided" "$(grep -c 'outside the startup window: at a pause$' "$OUT/$1-p6-out.log")" 1
  check "outside the startup window: no set" "$(markers "$N6-out")" 0
  # chunks of 4: 4 chunks and a marker; the next start loads the 4 as consolidated, 0 misses
  run "$N6-ch" "$1-p6-ch" D9MT_PSO_CONSOLIDATE_CHUNK=4 MT_WAIT_CONSOLIDATED=20 -- "${SET_A[@]}"
  check "chunked: chunks" "$(chunks "$N6-ch")" 4
  check "chunked: marker" "$(markers "$N6-ch")" 1
  run "$N6-ch" "$1-p6-ch2" MT_COVERAGE=0 -- "${SET_A[@]}"
  check "chunked: next start loads the set" "$(ldd "$1-p6-ch2")" 4
  check "chunked: next start misses" "$(misses_in "$1-p6-ch2")" 0
  # an exit between chunks (held 3 s after each): no marker; the next start uses the old archives
  run "$N6-ex" "$1-p6-ex0" MT_COVERAGE=0 -- "${SET_A[@]}"
  # (SET_B: new PSOs, so the consolidation has work; SET_A's 16 are archived: 4 chunks of 4)
  mkdir -p "$N6-exB"; ln -sfn "$N6-ex/pso" "$N6-exB/pso"   # SET_B in a workdir of its own, the same archives
  run "$N6-exB" "$1-p6-ex" D9MT_PSO_CONSOLIDATE_CHUNK=4 D9MT_PSO_CHUNK_HOLD_MS=3000 MT_SETTLE_MS=0 MT_EXIT_DELAY_MS=2500 -- "${SET_B[@]}"
  local cn; cn=$(chunks "$N6-ex")
  if [ "$cn" -ge 1 ] && [ "$(markers "$N6-ex")" = 0 ]; then echo "  exit between chunks: $cn chunk(s), no marker: ok"
  else echo "  exit between chunks: FAIL ($cn chunks, $(markers "$N6-ex") markers)"; fails=$((fails + 1)); fi
  L=$(deltas "$N6-ex")
  run "$N6-ex" "$1-p6-ex2" MT_COVERAGE=0 -- "${SET_A[@]}"
  check "exit between chunks: next start loads only the deltas" "$(ldd "$1-p6-ex2")" "$L"
  check "exit between chunks: next start misses" "$(misses_in "$1-p6-ex2")" 0
  check "exit between chunks: young chunks of an ended owner kept" "$(chunks "$N6-ex")" "$cn"
  for f in "$N6-ex"/pso/mt.exe/os-*/c-*; do [ -e "$f" ] && ages 700 "$f"; done
  run "$N6-ex" "$1-p6-ex3" MT_COVERAGE=0 -- "${SET_A[@]}"
  check "exit between chunks: old chunks of an ended owner removed" "$(chunks "$N6-ex")" 0
  check "exit between chunks: then still no misses" "$(misses_in "$1-p6-ex3")" 0
  # a second process while one consolidates: A holds after each chunk; B (A's chunks aged, A alive)
  # neither loads nor deletes them; A then completes its set
  local A6="$N6-a"; run "$A6" "$1-p6-a0" MT_COVERAGE=0 -- "${SET_A[@]}"
  mkdir -p "$A6-B"; ln -sfn "$A6/pso" "$A6-B/pso"
  run "$A6-B" "$1-p6-a" D9MT_PSO_CONSOLIDATE_CHUNK=4 D9MT_PSO_CHUNK_HOLD_MS=1500 MT_WAIT_CONSOLIDATED=40 -- "${SET_B[@]}" &
  local apid=$!
  for t in $(seq 150); do [ "$(chunks "$A6")" -ge 1 ] && break; sleep 0.1; done
  for f in "$A6"/pso/mt.exe/os-*/c-*; do [ -e "$f" ] && ages 700 "$f"; done
  local before; before=$(chunks "$A6")
  local dl; dl=$(deltas "$A6")
  run "$A6" "$1-p6-b" MT_COVERAGE=0 -- "${SET_A[@]}"
  if [ "$before" -ge 1 ] && [ "$(chunks "$A6")" -ge "$before" ]; then echo "  second process: A's chunks kept ($before): ok"
  else echo "  second process: FAIL (A's chunks $before -> $(chunks "$A6"))"; fails=$((fails + 1)); fi
  check "second process: B loads the deltas, not A's chunks" "$(ldd "$1-p6-b")" "$dl"
  wait $apid
  check "second process: A's set completed" "$(markers "$A6")" 1
  # requests during the consolidation (2.5 s of them after the coverage signal): it yields, and completes
  run "$N6-y" "$1-p6-y" MT_BURST_MS=2500 MT_WAIT_CONSOLIDATED=30 -- "${SET_A[@]}"
  local y; y=$(sed -n 's/.*not archived yet, \([0-9]*\) yield(s)$/\1/p' "$OUT/$1-p6-y.log")
  if [ "${y:-0}" -gt 0 ] && [ "$(markers "$N6-y")" = 1 ]; then echo "  burst: yielded $y time(s), then completed: ok"
  else echo "  burst: FAIL (yields '${y:-}', $(markers "$N6-y") markers)"; fails=$((fails + 1)); fi
  # FLUSH_EVERY (5 here) pending: flushed at a quiet gap, with the idle flush out of reach
  run "$N6-f" "$1-p6-f" MT_COVERAGE=0 D9MT_PSO_FLUSH_IDLE_MS=600000 MT_SETTLE_MS=2000 -- "${SET_A[@]}"
  if [ "$(grep -c 'flush starts (chunk' "$OUT/$1-p6-f.log")" -gt 0 ]; then echo "  capture flush at FLUSH_EVERY after a quiet gap: ok"
  else echo "  capture flush at FLUSH_EVERY: FAIL"; fails=$((fails + 1)); fi

  # a warm start (only archive hits: the game's compiler unused) whose startup-window consolidation
  # deletes the loaded parts, then new PSOs (misses, compiled with those lookupArchives): Metal threw
  # in convertBinaryArchives before pso_convert_lookups; no crash, all built, no fallback
  # More deltas than archive first asks (8): the consolidation's own compiles convert only the
  # archives it looks up (the newest 8 and the session's), and deletes all of them; the older
  # ones are only the game compiler's lookupArchives. SET_A newest, so the warm run hits only.
  local NU="$OUT/$1-p6-unlink" i
  mkdir -p "$NU/pso"
  local more=($(ls "$B"/ps_*.bin | grep -v -e ps_show -e ps_pp))
  for i in "${!more[@]}"; do
    [ "$(deltas "$NU")" -ge 10 ] && break
    mkdir -p "$NU-$i"; ln -sfn "$NU/pso" "$NU-$i/pso"
    run "$NU-$i" "$1-p6-u0-$i" MT_COVERAGE=0 -- "$B/vs_mvp.bin" "${more[$i]}"
  done
  sleep 1
  run "$NU" "$1-p6-u1" MT_COVERAGE=0 -- "${SET_A[@]}"
  local parts; parts=$(deltas "$NU")
  run "$NU" "$1-p6-u3" MT_LATE_PSOS=1500 MT_WAIT_CONSOLIDATED=30 -- "${SET_A[@]}"
  check "warm consolidation, then misses: all hits first (the compiler unused)" "$(stat_line "$1-p6-u3" | sed 's/^\([0-9]*\) hits.*/\1/')" "$(grep -o 'psos: [0-9]* ok' "$OUT/$1-p6-u3.log" | grep -o '[0-9]*' | head -1)"
  check "warm consolidation, then misses: it replaced every loaded delta ($parts)" "$(sed -n 's/.*builds into .* chunk(s)), \([0-9]*\) replaced.*/\1/p' "$OUT/$1-p6-u3.log")" "$parts"
  check "warm consolidation, then misses: consolidated" "$(grep -c 'PSO archives consolidated' "$OUT/$1-p6-u3.log")" 1
  # (a crash is counted by run(); the late builds compiled after the deletion:)
  local lok; lok=$(sed -n 's/^late psos: \([0-9]*\) ok.*/\1/p' "$OUT/$1-p6-u3.log")
  [ "${lok:-0}" -gt 0 ] && echo "  warm consolidation, then misses: $lok late PSOs built after the deletion: ok" \
    || { echo "  warm consolidation, then misses: FAIL (no late PSO built)"; fails=$((fails + 1)); }
  check "warm consolidation, then misses: no lookup archive gone" "$(grep -c "lookup archive's file is gone" "$OUT/$1-p6-u3.log")" 0
  check "warm consolidation, then misses: converted at load" "$(grep -c 'lookup archive(s) converted in .* (ok)' "$OUT/$1-p6-u3.log")" 1
  if [ "$1" = plain ]; then
    clang -arch x86_64 -O1 -fobjc-arc -fobjc-arc-exceptions "$BR/tests/unlink_lookup_test.m" -framework Metal -framework Foundation \
      -o "$OUT/unlink_lookup_test" || { echo "FATAL: unlink_lookup_test does not build" >&2; exit 1; }
    mkdir -p "$OUT/unlink"
    check "Metal: a deleted lookup archive the compiler never used throws" \
      "$("$OUT/unlink_lookup_test" "$OUT/unlink" unused 2>&1 | grep -c 'EXCEPTION after unlink')" 1
    check "Metal: converted before the deletion (the bridge's fix), no exception" \
      "$("$OUT/unlink_lookup_test" "$OUT/unlink" warmup 2>&1 | grep -c 'EXCEPTION')" 0
  fi

  # the conversion not done within 5 s (held 6 s here): no consolidation this session, nothing deleted
  local NV="$OUT/$1-p6-conv"
  run "$NV" "$1-p6-cv0" MT_COVERAGE=0 -- "${SET_A[@]}"
  mkdir -p "$NV-B"; ln -sfn "$NV/pso" "$NV-B/pso"
  run "$NV-B" "$1-p6-cv1" MT_COVERAGE=0 -- "${SET_B[@]}"
  local dn; dn=$(deltas "$NV")
  run "$NV" "$1-p6-cv2" D9MT_PSO_CONVERT_DELAY_MS=6000 MT_WAIT_CONSOLIDATED=10 -- "${SET_A[@]}"
  check "conversion late: no consolidation this session" "$(grep -c 'no consolidation this session (the loaded archives not converted within 5 s)' "$OUT/$1-p6-cv2.log")" 1
  check "conversion late: no set" "$(markers "$NV")" 0
  check "conversion late: the deltas kept" "$(deltas "$NV")" "$dn"
  # .verify (left by a session whose conversion failed with every file there): the next start checks
  # every archive's SHA-256 once; a delta damaged with its size and time kept goes, the marker too
  local NW="$OUT/$1-p6-verify" d1
  run "$NW" "$1-p6-vf0" MT_COVERAGE=0 -- "${SET_A[@]}"
  mkdir -p "$NW-B"; ln -sfn "$NW/pso" "$NW-B/pso"
  run "$NW-B" "$1-p6-vf1" MT_COVERAGE=0 -- "${SET_B[@]}"
  d1=$(ls -d "$NW"/pso/mt.exe/os-*)/$(ls "$NW"/pso/mt.exe/os-*/ | grep -E '^[0-9]+-[0-9]+-[0-9]+\.mtl4archive$' | head -1)
  python3 -I -c 'import os,sys; f=sys.argv[1]; st=os.stat(f); b=bytearray(open(f,"rb").read()); i=len(b)//2; b[i]^=0xff; open(f,"wb").write(b); os.utime(f, ns=(st.st_atime_ns, st.st_mtime_ns))' "$d1"
  printf '1\n' > "$(ls -d "$NW"/pso/mt.exe/os-*)/.verify"
  local others=() f kept=0
  for f in "$(dirname "$d1")"/*.mtl4archive; do [ "$f" != "$d1" ] && others+=("$f"); done
  run "$NW" "$1-p6-vf2" MT_COVERAGE=0 -- "${SET_A[@]}"
  check ".verify: SHA-256 checked this once" "$(grep -c "checking every archive's SHA-256 this once" "$OUT/$1-p6-vf2.log")" 1
  check ".verify: the damaged delta removed" "$([ -e "$d1" ] && echo kept || echo removed)" removed
  for f in "${others[@]}"; do [ -e "$f" ] && kept=$((kept + 1)); done
  check ".verify: the others kept" "$kept" "${#others[@]}"
  check ".verify: the marker gone" "$([ -e "$(ls -d "$NW"/pso/mt.exe/os-*)/.verify" ] && echo kept || echo gone)" gone
  # .verify's producer: the game's compile meets the lookup failure first (D9MT_PSO_TEST_LOOKUP_THROW:
  # every compile with lookups throws, as Metal does; the conversion held 2 s, so the game's miss
  # comes before it), every loaded file is there -> the marker; the next start checks once
  local NP="$OUT/$1-p6-vprod" vd
  run "$NP" "$1-p6-vp0" MT_COVERAGE=0 -- "${SET_A[@]}"
  mkdir -p "$NP-B"; ln -sfn "$NP/pso" "$NP-B/pso"   # SET_B's new PSOs miss: compiled with lookups
  run "$NP-B" "$1-p6-vp1" MT_COVERAGE=0 D9MT_PSO_TEST_LOOKUP_THROW=1 D9MT_PSO_CONVERT_DELAY_MS=2000 MT_SETTLE_MS=3000 -- "${SET_B[@]}"
  vd=$(ls -d "$NP"/pso/mt.exe/os-*)
  check "producer: the game's compile failed first" "$(grep -c "lookup archive's file is gone before its first use (D9MT_PSO_TEST_LOOKUP_THROW)" "$OUT/$1-p6-vp1.log")" 1
  check "producer: every file there, so the marker" "$([ -e "$vd/.verify" ] && echo written || echo missing)" written
  check "producer: said so" "$(grep -c "every archive's SHA-256 is checked at the next start" "$OUT/$1-p6-vp1.log")" 1
  run "$NP" "$1-p6-vp2" MT_COVERAGE=0 -- "${SET_A[@]}"
  check "producer: the next start checks once" "$(grep -c "checking every archive's SHA-256 this once" "$OUT/$1-p6-vp2.log")" 1
  check "producer: the marker consumed" "$([ -e "$vd/.verify" ] && echo kept || echo gone)" gone
  # a lookup failure with a loaded file deleted is a deletion, not damage: no marker
  # (D9MT_PSO_TEST_UNLINK_LOADED deletes the one loaded delta right after the load; SET_B's new
  # PSOs miss, and the test hook throws at the first)
  local NQ="$OUT/$1-p6-vgone" qd
  run "$NQ" "$1-p6-vg0" MT_COVERAGE=0 -- "${SET_A[@]}"
  qd=$(ls -d "$NQ"/pso/mt.exe/os-*)
  mkdir -p "$NQ-B"; ln -sfn "$NQ/pso" "$NQ-B/pso"
  run "$NQ-B" "$1-p6-vg1" MT_COVERAGE=0 D9MT_PSO_TEST_LOOKUP_THROW=1 D9MT_PSO_TEST_UNLINK_LOADED=1 D9MT_PSO_CONVERT_DELAY_MS=2000 MT_SETTLE_MS=3000 -- "${SET_B[@]}"
  check "deleted, not damaged: the failure" "$(grep -c "lookup archive's file is gone before its first use" "$OUT/$1-p6-vg1.log")" 1
  check "deleted, not damaged: no marker" "$([ -e "$qd/.verify" ] && echo written || echo none)" none

  echo "[pso-first:$1] R1: state cache off"
  local S="$OUT/$1-r1"; cp -Rp "$W" "$S"
  run "$S" "$1-r1" D9MT_STATE_CACHE=0 MT_WAIT_CONSOLIDATED=3 -- "${SET_A[@]}"
  check "R1: archive first off" "$(grep -c 'archive first off' "$OUT/$1-r1.log")" 1
  [ "$(own_deltas "$S" "$(pid_of "$1-r1")")" -gt 0 ] && echo "  R1: the run captured its PSOs (a delta of its own)" \
    || { echo "  R1: FAIL (nothing captured)"; fails=$((fails + 1)); }
  check "R1: no consolidation" "$(grep -c 'consolidated archive: written' "$OUT/$1-r1.log")" 0
}

misses_of() { stat_line "$1" | sed -n 's/.* \([0-9]*\) misses$/\1/p'; }
hits_of() { stat_line "$1" | sed -n 's/^\([0-9]*\) hits.*/\1/p'; }
shared() { mkdir -p "$1" "$2"; ln -sfn "$2" "$1/pso"; }   # <workdir> <pso dir>: workdirs sharing a pso dir

m6() {
  echo "[pso-first] F-1: a delta flushed during the consolidation's rebuild survives the next start"
  local F="$OUT/f1"
  # idle 0: the consolidation snapshots within 1 s of the coverage signal, then holds 4 s;
  # the late PSOs come 1.5 s after the signal and flush every 5 (FLUSH_EVERY) into deltas
  run "$F" f1-cold D9MT_PSO_CONSOLIDATE_IDLE_MS=0 D9MT_PSO_CONSOLIDATE_DELAY_MS=4000 MT_LATE_PSOS=1500 \
    MT_WAIT_CONSOLIDATED=30 -- "${SET_A[@]}"
  local late
  late=$(python3 -I -c '
import os, re, sys
d, pid = sys.argv[1], sys.argv[2]
fs = os.listdir(d)
c = [f for f in fs if re.match(r"c-\d+-%s\.set$" % pid, f)]
if c:
    stamp, cm = int(c[0][2:12]), os.stat(os.path.join(d, c[0])).st_mtime
    print(" ".join(f for f in fs if re.match(r"\d+-%s-\d+\.mtl4archive$" % pid, f)
                   and stamp <= os.stat(os.path.join(d, f)).st_mtime < cm))' "$(ls -d "$F"/pso/mt.exe/os-*)" "$(pid_of f1-cold)")
  local n; n=$(wc -w <<< "$late" | tr -d ' ')
  [ "$n" -gt 0 ] && echo "  F-1: $n delta(s) flushed between the snapshot and the consolidated archive" \
    || { echo "  F-1: FAIL (no delta flushed during the rebuild: the test didn't hit the window)"; fails=$((fails + 1)); }
  run "$F" f1-next MT_COVERAGE=0 MT_LATE_PSOS=0 -- "${SET_A[@]}"
  local gone=0 f
  for f in $late; do [ -e "$(ls -d "$F"/pso/mt.exe/os-*)/$f" ] || gone=$((gone + 1)); done
  check "F-1: those deltas kept at the next start" "$gone" 0
  check "F-1: next start, late PSOs included: misses" "$(misses_of f1-next)" 0

  echo "[pso-first] lookup cap: a PSO only in a delta past the newest 8"
  local K="$OUT/cap" i
  shared "$K/w0" "$K/pso"
  run "$K/w0" cap-old MT_COVERAGE=0 -- "${SET_A[@]}"
  local oldn; oldn=$(deltas "$K/w0")
  sleep 1   # later sessions' names sort newer
  # sessions with one new pixel shader each (one delta each) until more than 8 deltas are newer
  local more=($(ls "$B"/ps_*.bin | grep -v -e ps_show -e ps_pp))
  for i in "${!more[@]}"; do
    [ $(( $(deltas "$K/w0") - oldn )) -gt 8 ] && break
    shared "$K/w$((i + 1))" "$K/pso"
    run "$K/w$((i + 1))" "cap-$i" MT_COVERAGE=0 -- "$B/vs_mvp.bin" "${more[$i]}"
  done
  local newer=$(( $(deltas "$K/w0") - oldn ))
  [ "$newer" -gt 8 ] && echo "  cap: $newer newer deltas over the $oldn of the recording run" \
    || { echo "  cap: FAIL (only $newer newer deltas, want more than 8)"; fails=$((fails + 1)); }
  # the control: the same directory with every delta asked first
  cp -Rp "$K/pso" "$K/pso64"; mkdir -p "$K/c64"; cp -p "$K/w0/results.txt" "$K/c64/"; ln -sfn "$K/pso64" "$K/c64/pso"
  run "$K/c64" cap-64 MT_COVERAGE=0 D9MT_PSO_ARCHIVE_FIRST_MAX=64 -- "${SET_A[@]}"
  check "cap 64: all hits" "$(misses_of cap-64)" 0
  run "$K/w0" cap-8 MT_COVERAGE=0 -- "${SET_A[@]}"
  local ok; ok=$(grep -o 'psos: [0-9]* ok' "$OUT/cap-8.log" | grep -o '[0-9]*' | head -1)
  check "cap 8: the old delta's PSOs miss archive first" "$(misses_of cap-8)" "$ok"
  grep -q 'consistent' "$OUT/cap-8.log" && echo "  cap 8: results consistent with the recording run" \
    || { echo "  cap 8: FAIL (results differ)"; fails=$((fails + 1)); }
  [ "$(own_deltas "$K/w0" "$(pid_of cap-8)")" -gt 0 ] && echo "  cap 8: captured again into a delta of its own" \
    || { echo "  cap 8: FAIL (not captured)"; fails=$((fails + 1)); }

  echo "[pso-first] restore: archives copied without their file times"
  local Rs="$OUT/restore" Rb="$OUT/restore-bad"
  cp -R "$OUT/plain-w" "$Rs"; cp -R "$OUT/plain-w" "$Rb"
  run "$Rs" restore MT_COVERAGE=0 -- "${SET_A[@]}"
  check "restore: all hits" "$(misses_of restore)" 0
  check "restore: dropped archives" "$(grep -c 'dropping PSO archive' "$OUT/restore.log")" 0
  check "restore: sums now carry the copies' times" "$(python3 -I -c '
import glob, os, sys
bad = 0
for f in glob.glob(sys.argv[1] + "/pso/mt.exe/os-*/*.mtl4archive"):
    t = open(f + ".sum").read().split()
    bad += len(t) != 4 or t[0] != "2" or int(t[2]) != os.stat(f).st_mtime_ns
print(bad)' "$Rs")" 0
  local c; c=$(ls -d "$Rb"/pso/mt.exe/os-*)/$(ls "$Rb"/pso/mt.exe/os-*/ | grep -E '^c-.*\.mtl4archive$' | head -1)
  python3 -I -c 'import os,sys; f=open(sys.argv[1],"r+b"); n=os.path.getsize(sys.argv[1]); f.seek(n//2); b=f.read(1); f.seek(n//2); f.write(bytes([b[0]^1]))' "$c"
  run "$Rb" restore-bad MT_COVERAGE=0 -- "${SET_A[@]}"
  check "restore: a changed byte (same size, other time) deleted" "$([ -e "$c" ] && echo kept || echo deleted)" deleted
}

HARNESS="$OUT/compile_mt_test"
suite plain
m6
if [ "${TSAN:-1}" != 0 ]; then
  TS=(-fsanitize=thread -g -O1)
  CF=(-ObjC -arch x86_64 -I"$BR" -I"$BR/unix" -I"$V2/airlib")
  objs=()
  for src in "$BR/unix/winemetal_unix.c" "$BR/unix/air_cache.c" "$BR/unix/cache.c" "$BR/unix/dxso_stubs.c" "$BR/unix/pso_archive_check.c"; do
    clang "${CF[@]}" "${TS[@]}" -c "$src" -o "$OUT/$(basename "$src").tsan.o" || exit 1
    objs+=("$OUT/$(basename "$src").tsan.o")
  done
  clang++ "${INC[@]}" "${TS[@]}" -fvisibility=hidden -D__wine_unix_call_funcs=d9mtair_private_call_funcs \
    -D__wine_unix_call_wow64_funcs=d9mtair_private_wow64_funcs -c "$V2/airlib/unix.cpp" -o "$OUT/airlib_unix.tsan.o"
  printf '%s\n' ___wine_unix_call_funcs ___wine_unix_call_wow64_funcs _d9mtair_version _d9mtair_compile _d9mtair_free > "$OUT/exports.list"
  clang++ -arch x86_64 -dynamiclib -fsanitize=thread -o "$OUT/winemetal-tsan.so" "${objs[@]}" "$OUT/airlib_unix.tsan.o" \
    "$V2"/build/airlib/obj/third_party_dxbc-spirv_ir_*.o "$V2"/build/airlib/obj/third_party_dxbc-spirv_util_*.o \
    "$V2/build/airlib/obj/air_air_builder.cpp.o" "$V2/build/airlib/obj/air_air_metallib.cpp.o" "$LLVM"/lib/libLLVM*.a \
    -install_name @rpath/winemetal.so -Wl,-exported_symbols_list,"$OUT/exports.list" \
    -Wl,-U,_NtSetEvent -Wl,-U,_macdrv_get_metal_layer -undefined dynamic_lookup \
    -weak_framework CoreFoundation -weak_framework Metal -weak_framework Foundation -weak_framework CoreGraphics \
    -weak_framework QuartzCore -weak_framework ColorSync -weak_framework Cocoa -framework MetalFX -lsqlite3 -lm -lpthread || exit 1
  clang++ "${INC[@]}" "${TS[@]}" "$BR/tests/compile_mt_test.cpp" "$V2"/build/airlib/obj/third_party_dxbc-spirv_*.o -lpthread \
    -o "$OUT/compile_mt_test_tsan" || exit 1
  SO="$OUT/winemetal-tsan.so"; HARNESS="$OUT/compile_mt_test_tsan"
  export TSAN_OPTIONS="halt_on_error=0 second_deadlock_stack=1 ${TSAN_OPTIONS:-}"
  suite tsan
  races=$(cat "$OUT"/tsan-*.log | grep -c 'WARNING: ThreadSanitizer')
  check "TSan reports" "$races" 0
fi
echo "[pso-first] $fails failure(s)"
[ "$fails" -eq 0 ]
