#!/usr/bin/env bash
# Native (no Wine) concurrency tests: the driver's compile task / worker
# pool under TSan (driver/mtl/tests/mtl_compile_test.cpp), then the bridge
# compile path (bridge/tests/compile_mt_test.cpp). Runs the harness twice (cold, then warm
# AIR cache + PSO archives) against build/bridge/winemetal.so, then again
# against a ThreadSanitizer build of the unix side (TSAN=0 skips that).
# Needs build-bridge.sh to have run (airlib objects, LLVM toolchain).
# Heavy (GPU compiler service + LLVM on 6 threads): prefix with
# `taskpolicy -b` while something latency-sensitive runs.
set -euo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
BR="$V2/bridge"
DX="$V2/third_party/dxbc-spirv"
LLVM="$V2/toolchains/llvm-x86_64"
AIRLIB_OBJ="$V2/build/airlib/obj"
OUT="$V2/build/compile-mt"
mkdir -p "$OUT"

INC=(-std=c++17 -O1 -g -arch x86_64 -I"$DX" -I"$V2" -I"$LLVM/include" -I"$BR" -I"$V2/airlib"
     -D__STDC_CONSTANT_MACROS -D__STDC_FORMAT_MACROS -D__STDC_LIMIT_MACROS)
DXBC_OBJS=("$AIRLIB_OBJ"/third_party_dxbc-spirv_*.o)
# MT_BINS="a.bin b.bin" limits the shader set (all of air/tests/bin by default)
if [ -n "${MT_BINS:-}" ]; then read -r -a BINS <<< "$MT_BINS"; else BINS=("$V2"/air/tests/bin/*.bin); fi

# nothing is flushed at exit (bundle part 5): time for the cold run's big flush
export MT_SETTLE_MS="${MT_SETTLE_MS:-5000}"
run_pair() { # <harness> <winemetal.so> <workdir>
  rm -rf "$3"; mkdir -p "$3"
  echo "--- $(basename "$1"): cold"
  "$1" "$2" "$3" "${BINS[@]}"
  echo "--- $(basename "$1"): warm"
  "$1" "$2" "$3" "${BINS[@]}"
  ls "$3/pso"/mt.exe/os-*/ 2>/dev/null | grep -c 'mtl4archive$' | sed 's/^/    archives written: /'
}

# Needs DXVK's native Windows headers (include/native/{windows,directx} of a
# DXVK checkout with its submodules); skipped when DXVK_NATIVE is not found.
NATIVE="${DXVK_NATIVE:-$V2/DXVK-3.0-uptsream/include/native}"
if [ -d "$NATIVE/windows" ] && [ -d "$NATIVE/directx" ]; then
  echo "[compile-mt] driver compile tasks / worker pool (native arm64, ThreadSanitizer)"
  clang++ -std=c++17 -g -O1 -fsanitize=thread -I"$V2/driver" -I"$NATIVE/windows" -I"$NATIVE/directx" \
    "$V2/driver/mtl/tests/mtl_compile_test.cpp" -o "$OUT/mtl_compile_test"
  "$OUT/mtl_compile_test"
else
  echo "[compile-mt] worker pool test skipped: no DXVK native headers at $NATIVE (set DXVK_NATIVE)"
fi

echo "[compile-mt] harness"
clang++ "${INC[@]}" "$BR/tests/compile_mt_test.cpp" "${DXBC_OBJS[@]}" -lpthread -o "$OUT/compile_mt_test"
run_pair "$OUT/compile_mt_test" "$V2/build/bridge/winemetal.so" "$OUT/work"

[ "${TSAN:-1}" = 0 ] && exit 0

echo "[compile-mt] ThreadSanitizer build of the unix side"
TS=(-fsanitize=thread -g -O1)
CFLAGS_UNIX=(-ObjC -arch x86_64 -I"$BR" -I"$BR/unix" -I"$V2/airlib")
for src in "$BR/unix/winemetal_unix.c" "$BR/unix/air_cache.c" "$BR/unix/cache.c" "$BR/unix/dxso_stubs.c" "$BR/unix/pso_archive_check.c"; do
  clang "${CFLAGS_UNIX[@]}" "${TS[@]}" -c "$src" -o "$OUT/$(basename "$src").tsan.o"
done
clang++ "${INC[@]}" "${TS[@]}" -fvisibility=hidden \
  -D__wine_unix_call_funcs=d9mtair_private_call_funcs \
  -D__wine_unix_call_wow64_funcs=d9mtair_private_wow64_funcs \
  -c "$V2/airlib/unix.cpp" -o "$OUT/airlib_unix.tsan.o"
printf '%s\n' ___wine_unix_call_funcs ___wine_unix_call_wow64_funcs _d9mtair_version _d9mtair_compile _d9mtair_free \
  > "$OUT/exports.list"
# AirBuilder/LLVM/dxbc-spirv stay uninstrumented (prebuilt): TSan still sees
# every lock and shared access in the bridge code that drives them.
clang++ -arch x86_64 -dynamiclib -fsanitize=thread -o "$OUT/winemetal-tsan.so" \
  "$OUT"/winemetal_unix.c.tsan.o "$OUT"/air_cache.c.tsan.o "$OUT"/cache.c.tsan.o "$OUT"/dxso_stubs.c.tsan.o "$OUT"/pso_archive_check.c.tsan.o "$OUT/airlib_unix.tsan.o" \
  "$AIRLIB_OBJ"/third_party_dxbc-spirv_ir_*.o "$AIRLIB_OBJ"/third_party_dxbc-spirv_util_*.o \
  "$AIRLIB_OBJ/air_air_builder.cpp.o" "$AIRLIB_OBJ/air_air_metallib.cpp.o" \
  "$LLVM"/lib/libLLVM*.a \
  -install_name @rpath/winemetal.so -Wl,-exported_symbols_list,"$OUT/exports.list" \
  -Wl,-U,_NtSetEvent -Wl,-U,_macdrv_get_metal_layer -undefined dynamic_lookup \
  -weak_framework CoreFoundation -weak_framework Metal -weak_framework Foundation \
  -weak_framework CoreGraphics -weak_framework QuartzCore -weak_framework ColorSync \
  -weak_framework Cocoa -framework MetalFX -lsqlite3 -lm -lpthread
clang++ "${INC[@]}" "${TS[@]}" "$BR/tests/compile_mt_test.cpp" "${DXBC_OBJS[@]}" -lpthread \
  -o "$OUT/compile_mt_test_tsan"
TSAN_OPTIONS="halt_on_error=0 second_deadlock_stack=1 ${TSAN_OPTIONS:-}" \
  run_pair "$OUT/compile_mt_test_tsan" "$OUT/winemetal-tsan.so" "$OUT/work-tsan"
