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
#     BENCH=1: test/native/arm64_port_bench.cpp on both architectures
#
# Needs: both bridge builds (scripts/build-bridge.sh and
# ARCH=arm64ec scripts/build-bridge.sh), llvm-mingw (scripts/llvm-mingw.sh),
# DXVK's native Windows headers (DXVK_NATIVE=<dxvk>/include/native, default
# DXVK-3.0-uptsream/include/native as for test-compile-mt.sh), Rosetta.
# The x86 builds' byte identity is scripts/check-x86-identity.sh.
set -euo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$V2/build/arm64-port-test"
NATIVE="${DXVK_NATIVE:-$V2/DXVK-3.0-uptsream/include/native}"
mkdir -p "$OUT"
. "$V2/scripts/llvm-mingw.sh"
llvm_mingw_ensure
LMB="$LLVM_MINGW/bin"
[ -d "$NATIVE/windows" ] || { echo "FATAL: no DXVK native headers at $NATIVE (set DXVK_NATIVE)" >&2; exit 1; }
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

if [ "${BENCH:-0}" = 1 ]; then
  echo "[bench] hot paths (median ns per call)"
  for a in x86_64 arm64; do
    xbuild $a test/native/arm64_port_bench.cpp "$OUT/bench-$a"
    arch -$a "$OUT/bench-$a"
  done
fi
if [ "${MT:-0}" = 1 ]; then
  echo "[mt] compile path concurrency, arm64 (ThreadSanitizer)"
  DXVK_NATIVE="$NATIVE" ARCH=arm64 bash scripts/test-compile-mt.sh > "$OUT/compile-mt-arm64.log" 2>&1
  grep -E '^(---|psos|compared|OK)' "$OUT/compile-mt-arm64.log"
  echo "  ThreadSanitizer reports: $(grep -c 'WARNING: ThreadSanitizer' "$OUT/compile-mt-arm64.log" || true)"
fi
echo "[arm64-port] all checks passed"
