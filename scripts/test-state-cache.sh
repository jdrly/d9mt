#!/usr/bin/env bash
# test-state-cache.sh — the pipeline state cache's native tests (test/native/state_cache_test.cpp,
# docs/state-cache.md): file format, corruption, versions, cap, the prewarm index, the record
# ring, and record -> write -> load -> prewarm with a fake backend, once plain (-O2) and once
# under ThreadSanitizer; then the render-thread cost of recording one PSO miss.
#
#   scripts/test-state-cache.sh
#
# Needs DXVK's native Windows headers (include/native/{windows,directx} of a DXVK checkout),
# as scripts/test-compile-mt.sh: DXVK_NATIVE=<dir> or $V2/DXVK-3.0-uptsream/include/native.
# Writes only under a mktemp dir (the tests' cache files live in /tmp/d9mt-sct.*).
set -euo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
NATIVE="${DXVK_NATIVE:-$V2/DXVK-3.0-uptsream/include/native}"
[ -d "$NATIVE/windows" ] && [ -d "$NATIVE/directx" ] || {
  echo "[state-cache] no DXVK native headers at $NATIVE (set DXVK_NATIVE)" >&2; exit 2; }

OUT="$(mktemp -d /tmp/d9mt-statecache-test.XXXXXX)"
SRC=("$V2/test/native/state_cache_test.cpp" "$V2/driver/mtl/mtl_state_cache_core.cpp"
     "$V2/driver/mtl/mtl_state_cache.cpp")
INC=(-I"$V2/driver" -I"$NATIVE/windows" -I"$NATIVE/directx")

echo "[state-cache] plain -O2"
clang++ -std=c++17 -O2 -g -Wall -Wno-unused-function "${INC[@]}" "${SRC[@]}" -o "$OUT/sct"
"$OUT/sct"

echo "[state-cache] ThreadSanitizer"
clang++ -std=c++17 -O1 -g -fsanitize=thread "${INC[@]}" "${SRC[@]}" -o "$OUT/sct-tsan"
TSAN_OPTIONS="halt_on_error=1" "$OUT/sct-tsan"

echo "[state-cache] render-thread cost of recording"
"$OUT/sct" bench

rm -rf "$OUT" /tmp/d9mt-sct.*
