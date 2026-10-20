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
#   TSan     the cold and warm runs under ThreadSanitizer (TSAN=0 skips; needs test-compile-mt.sh's
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
stat_line() { sed -n 's/^d9mt: PSO archive first: \(.*\)$/\1/p' "$OUT/$1.log"; }
deltas() { ls "$1"/pso/os-*/ 2>/dev/null | grep -E '^[0-9]+-[0-9]+-[0-9]+\.mtl4archive$' | wc -l | tr -d ' '; }
own_deltas() { ls "$1"/pso/os-*/ 2>/dev/null | grep -cE "^[0-9]+-$2-[0-9]+\.mtl4archive$"; }
pid_of() { sed -n 's/^pid: //p' "$OUT/$1.log"; }
consolidated() { ls "$1"/pso/os-*/ 2>/dev/null | grep -cE '^c-.*\.mtl4archive$'; }

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
  run "$N" "$1-r2" D9MT_PSO_CONSOLIDATE_IDLE_MS=600000 MT_WAIT_CONSOLIDATED=3 -- "${SET_A[@]}"
  check "R2: harness run not taken for a pause" "$(grep -c 'consolidated archive: written' "$OUT/$1-r2.log")" 0

  echo "[pso-first:$1] cold"
  run "$W" "$1-cold" MT_WAIT_CONSOLIDATED=20 -- "${SET_A[@]}"
  na=$(sed -n 's/.*, \([0-9]*\) not archived yet$/\1/p' "$OUT/$1-cold.log")
  echo "  cold: consolidation never compiles: ${na:-?} unflushed misses left to a later delta"
  local cname; cname=$(ls "$W"/pso/os-*/ | grep -E '^c-.*\.mtl4archive$' | head -1)
  local first; first=$(ls "$W"/pso/os-*/ | grep -E '^[0-9]+-' | sort | head -1)
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

  echo "[pso-first:$1] R1: state cache off"
  local S="$OUT/$1-r1"; cp -Rp "$W" "$S"
  run "$S" "$1-r1" D9MT_STATE_CACHE=0 MT_WAIT_CONSOLIDATED=3 -- "${SET_A[@]}"
  check "R1: archive first off" "$(grep -c 'archive first off' "$OUT/$1-r1.log")" 1
  [ "$(own_deltas "$S" "$(pid_of "$1-r1")")" -gt 0 ] && echo "  R1: the run captured its PSOs (a delta of its own)" \
    || { echo "  R1: FAIL (nothing captured)"; fails=$((fails + 1)); }
  check "R1: no consolidation" "$(grep -c 'consolidated archive: written' "$OUT/$1-r1.log")" 0
}

HARNESS="$OUT/compile_mt_test"
suite plain
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
