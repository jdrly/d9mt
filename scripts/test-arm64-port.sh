#!/usr/bin/env bash
# Verification for the arm64 / ARM64EC port (docs/arm64ec-port.md):
#
#  1. bridge ABI: the layout of every struct in the PE<->unix bridge headers
#     (bridge/*.h, airlib/d9mtair.h) on x86_64 and ARM64EC mingw (PE side) and
#     x86_64 and arm64 macOS (unix side); all four must be identical
#  2. native unit tests (test/native) on arm64 and x86_64 (Rosetta)
#  3. test/native/arm64_xarch_test.cpp: the driver's architecture-dependent
#     CPU routines, x86_64 (SSE/asm paths) vs arm64 (ARM64EC's paths), digests
#     must be identical
#  4. scripts/run-air-xarch.sh: IR, metallibs and reflection from the arm64
#     and x86_64 compile paths byte-identical over a generated corpus
#  5. MT=1: scripts/test-compile-mt.sh with ARCH=arm64 (heavy, ThreadSanitizer)
#     BENCH=1: test/native/arm64_port_bench.cpp: x86_64 SSE (Rosetta) vs the
#              arm64 portable code of BENCH_BASE (default main) vs this tree
#     PE=1: [3] built by the real PE compilers (GCC x64, clang ARM64EC), run
#           under Wine when RUN_X64 / RUN_ARM64EC are set
#
# Needs: both bridge builds (scripts/build-bridge.sh and
# ARCH=arm64ec scripts/build-bridge.sh), llvm-mingw (scripts/llvm-mingw.sh),
# DXVK's native Windows headers including its include/native/directx
# submodule (DXVK_NATIVE=<dxvk>/include/native; unset: fetched, pinned, by
# scripts/fetch-dxvk-native.sh), Rosetta.
# The x86 builds' byte identity is scripts/check-x86-identity.sh.
set -euo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$V2/build/arm64-port-test"
NATIVE="${DXVK_NATIVE:-$(bash "$V2/scripts/fetch-dxvk-native.sh")}"
mkdir -p "$OUT"
. "$V2/scripts/llvm-mingw.sh"
llvm_mingw_ensure
LMB="$LLVM_MINGW/bin"
[ -f "$NATIVE/windows/windows.h" ] && [ -f "$NATIVE/directx/d3d9.h" ] \
  || { echo "FATAL: no DXVK native headers at $NATIVE (windows/ and the directx/ submodule)" >&2; exit 1; }
cd "$V2"

echo "[1/4] bridge ABI layouts"
printf '#include "winemetal_thunks.h"\n#include "airconv_thunks.h"\n#include "d9mtair.h"\nint probe_anchor;\n' > "$OUT/layout.c"
dump() { # <name> <compiler...>
  local n=$1; shift
  "$@" -fsyntax-only -Xclang -fdump-record-layouts-complete -Ibridge -Iairlib "$OUT/layout.c" > "$OUT/layout-$n.txt"
}
dump pe-x86_64 "$LMB/x86_64-w64-mingw32-clang"
dump pe-arm64ec "$LMB/arm64ec-w64-mingw32-clang"
dump unix-x86_64 clang -target x86_64-apple-macos26.0 -x objective-c
dump unix-arm64 clang -target arm64-apple-macos26.0 -x objective-c
for p in pe-x86_64:pe-arm64ec unix-x86_64:unix-arm64 pe-arm64ec:unix-arm64; do
  printf '  %-26s ' "${p/:/ vs }:"
  python3 -I scripts/layout_cmp.py bridge airlib -- "$OUT/layout-${p%%:*}.txt" "$OUT/layout-${p##*:}.txt" | tail -1
done

echo "[2/4] native unit tests (arm64, x86_64)"
for a in arm64 x86_64; do
  t="$OUT/$a"; mkdir -p "$t"
  clang++ -arch $a -std=c++17 -I driver/d3d9 -I driver/mtl test/native/buffer_tracking_test.cpp -o "$t/btt"
  clang++ -arch $a -std=c++17 -I driver/util test/native/fenced_pool_test.cpp -o "$t/fpt"
  clang++ -arch $a -std=c++17 -I driver/d3d9 test/native/hitch_test.cpp -o "$t/ht"
  clang++ -arch $a -std=c++17 -I driver/mtl test/native/memory_profile_test.cpp -o "$t/mpt"
  clang++ -arch $a -std=c++17 -I test/native/stubs -I driver/backend test/native/upload_ring_test.cpp driver/backend/mtl_ring.cpp -o "$t/urt"
  clang++ -arch $a -std=c++17 -I driver/backend test/native/attachment_opt_test.cpp -o "$t/aot"
  clang++ -arch $a -std=c++17 -I driver/d3d9 test/native/stats_test.cpp -o "$t/st"
  for x in btt fpt ht mpt urt aot st; do
    arch -$a "$t/$x" > "$t/$x.log" 2>&1 || { echo "FAIL: $a $x"; tail -5 "$t/$x.log"; exit 1; }
  done
  echo "  $a: 7/7 passed"
done

echo "[3/4] driver CPU routines, x86_64 vs arm64"
xbuild() { # <arch> <src> <out> [extra...]
  local a=$1 src=$2 out=$3; shift 3
  local fl=(); [ "$a" = arm64 ] && fl=(-ffp-contract=off)
  clang -arch "$a" -O2 -c driver/util/sha1/sha1.c -o "$OUT/sha1-$a.o"
  clang++ -std=c++17 -O2 -arch "$a" ${fl[@]+"${fl[@]}"} -I"$NATIVE/windows" -I"$NATIVE/directx" \
    -Idriver/d3d9 -Ithird_party/dxbc-spirv -Iairlib "$src" driver/d3d9/d3d9_constant_copy.cpp "$@" -o "$out"
}
for a in x86_64 arm64; do
  xbuild $a test/native/arm64_xarch_test.cpp "$OUT/xarch-$a" driver/util/sha1/sha1_util.cpp "$OUT/sha1-$a.o"
  arch -$a "$OUT/xarch-$a" > "$OUT/xarch-$a.txt"
done
if cmp -s "$OUT/xarch-x86_64.txt" "$OUT/xarch-arm64.txt"; then
  echo "  identical: $(grep -c ' [0-9a-f]\{16\}$' "$OUT/xarch-arm64.txt") digests"
else
  echo "FAIL:"; paste "$OUT/xarch-x86_64.txt" "$OUT/xarch-arm64.txt"; exit 1
fi

echo "[4/4] AIR compile path, x86_64 vs arm64"
bash scripts/run-air-xarch.sh "${AIR_COUNT:-3000}" 1 2>"$OUT/air-xarch.err" | tail -2

if [ "${PE:-0}" = 1 ]; then
  # The same test built by the real PE compilers, with the driver's flags:
  # mingw GCC for x64 (the shipping d3d9.dll's), llvm-mingw clang for ARM64EC.
  # Running them needs Wine: RUN_X64 / RUN_ARM64EC = the command to run a
  # Windows program with (e.g. "<x86_64 wine> " and "<arm64 wine> "); each
  # result must match the native digests of [3].
  echo "[pe] driver CPU routines as built for x64 (GCC) and ARM64EC (clang)"
  PF=(-std=c++17 -O2 -fno-strict-aliasing -DDXVK_WSI_WIN32 -Wno-unused-parameter
      -Idriver/d3d9 -Ithird_party/dxbc-spirv -Iairlib)
  PSRC=(test/native/arm64_xarch_test.cpp driver/d3d9/d3d9_constant_copy.cpp driver/util/sha1/sha1_util.cpp)
  x86_64-w64-mingw32-gcc -O2 -c driver/util/sha1/sha1.c -o "$OUT/sha1-x64.o"
  x86_64-w64-mingw32-g++ "${PF[@]}" -msse -msse2 -mfpmath=sse "${PSRC[@]}" "$OUT/sha1-x64.o" \
    -static -o "$OUT/xarch-x64.exe"
  "$LMB/arm64ec-w64-mingw32-clang" -O2 -c driver/util/sha1/sha1.c -o "$OUT/sha1-arm64ec.o"
  "$LMB/arm64ec-w64-mingw32-clang++" "${PF[@]}" -ffp-contract=off -Wno-microsoft-exception-spec \
    "${PSRC[@]}" "$OUT/sha1-arm64ec.o" -static -o "$OUT/xarch-arm64ec.exe"
  for t in x64:"${RUN_X64:-}" arm64ec:"${RUN_ARM64EC:-}"; do
    a="${t%%:*}"; run="${t#*:}"
    if [ -z "$run" ]; then
      echo "  $a: built ($OUT/xarch-$a.exe); not run (set RUN_$(echo "$a" | tr a-z A-Z))"
      continue
    fi
    $run "$OUT/xarch-$a.exe" | tr -d '\r' > "$OUT/xarch-$a-pe.txt"
    cmp -s "$OUT/xarch-arm64.txt" "$OUT/xarch-$a-pe.txt" \
      && echo "  $a: identical to the native digests" \
      || { echo "FAIL: $a"; paste "$OUT/xarch-arm64.txt" "$OUT/xarch-$a-pe.txt"; exit 1; }
  done
fi

if [ "${BENCH:-0}" = 1 ]; then
  # three builds of the same benchmark: x86_64 (the SSE paths, as shipped,
  # under Rosetta), arm64 with BENCH_BASE's driver sources (default main: the
  # portable code ARM64EC would compile without this port), arm64 with this
  # tree's (NEON)
  BASE_DRV="$OUT/bench-base"
  rm -rf "$BASE_DRV"; mkdir -p "$BASE_DRV"
  git archive "${BENCH_BASE:-main}" driver | tar -x -C "$BASE_DRV"
  xbuild x86_64 test/native/arm64_port_bench.cpp "$OUT/bench-x86_64"
  xbuild arm64 test/native/arm64_port_bench.cpp "$OUT/bench-arm64"
  clang++ -std=c++17 -O2 -arch arm64 -ffp-contract=off -I"$NATIVE/windows" -I"$NATIVE/directx" \
    -I"$BASE_DRV/driver/d3d9" -Ithird_party/dxbc-spirv -Iairlib test/native/arm64_port_bench.cpp \
    "$BASE_DRV/driver/d3d9/d3d9_constant_copy.cpp" -o "$OUT/bench-arm64-base"
  arch -x86_64 "$OUT/bench-x86_64" > "$OUT/bench-x86_64.txt"
  arch -arm64 "$OUT/bench-arm64-base" > "$OUT/bench-arm64-base.txt"
  arch -arm64 "$OUT/bench-arm64" > "$OUT/bench-arm64.txt"
  echo "[bench] hot paths, median ns per call: x86_64 SSE (Rosetta) | arm64 ${BENCH_BASE:-main} (portable) | arm64 this tree"
  paste "$OUT/bench-x86_64.txt" "$OUT/bench-arm64-base.txt" "$OUT/bench-arm64.txt" | awk -F'\t' 'NR > 1 {
    n = split($1, a, "  +"); nx = split($1, x, " "); ny = split($2, y, " "); nz = split($3, z, " ")
    printf "  %-44s %7s %7s %7s\n", a[2], x[nx - 1], y[ny - 1], z[nz - 1] }'
fi
if [ "${MT:-0}" = 1 ]; then
  echo "[mt] compile path concurrency, arm64 (ThreadSanitizer)"
  DXVK_NATIVE="$NATIVE" ARCH=arm64 bash scripts/test-compile-mt.sh > "$OUT/compile-mt-arm64.log" 2>&1
  grep -E '^(---|psos|compared|OK)' "$OUT/compile-mt-arm64.log"
  echo "  ThreadSanitizer reports: $(grep -c 'WARNING: ThreadSanitizer' "$OUT/compile-mt-arm64.log" || true)"
fi
echo "[arm64-port] all checks passed"
