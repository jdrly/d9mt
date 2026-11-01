#!/usr/bin/env bash
# CS thread unit test (test/unit_cs_sync.cpp): several threads waiting in
# DxvkCsThread::synchronize for the same chunk must all return. Builds against the
# x64 driver's libdriver_core.a (run `ARCH=x64 scripts/build-driver.sh` first) and
# runs the console exe under Wine; no window, no GPU.
#
#   WINE=<engine>/bin/wine scripts/test-cs-sync.sh
set -euo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
WINE="${WINE:?set WINE to a Wine binary}"
BUILD="$V2/build/driver-x64"
OUT="$BUILD/unit"
mkdir -p "$OUT"
x86_64-w64-mingw32-g++ -std=c++17 -O2 -fno-strict-aliasing -DDXVK_WSI_WIN32 -msse -msse2 -mfpmath=sse \
  -I"$V2/driver" "$V2/test/unit_cs_sync.cpp" "$BUILD/libdriver_core.a" -static -o "$OUT/unit_cs_sync.exe"
"$WINE" "$OUT/unit_cs_sync.exe"
