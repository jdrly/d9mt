#!/usr/bin/env bash
# Fetch DXVK's native Windows/DirectX headers (include/native/windows and the
# mingw-directx-headers submodule include/native/directx), pinned. Only the
# native tests need them (scripts/test-arm64-port.sh, test-compile-mt.sh): they
# compile driver code such as d3d9_constant_copy.cpp on macOS. Lands in
# third_party/dxvk-native (gitignored); prints the include/native path.
#
#   scripts/fetch-dxvk-native.sh
#
# The pins: DXVK 3a4c6fa3 (v3.0.2-21, the commit driver/d3d9/d3d9.def matches,
# driver/UPSTREAM) and the submodule revision that commit records.
set -euo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
DXVK_REV=3a4c6fa3cb1548d56a90a38dd8f526b6c13e63fd
DIRECTX_REV=9df86f2341616ef1888ae59919feaa6d4fad693d
DST="$V2/third_party/dxvk-native"

if [ ! -f "$DST/.done-$DXVK_REV" ]; then
  echo "[dxvk-native] fetching DXVK ${DXVK_REV:0:8} headers -> $DST" >&2
  rm -rf "$DST"
  git clone -q --filter=blob:none --no-checkout https://github.com/doitsujin/dxvk.git "$DST"
  git -C "$DST" -c advice.detachedHead=false checkout -q "$DXVK_REV"
  git clone -q --filter=blob:none --no-checkout https://github.com/Joshua-Ashton/mingw-directx-headers.git "$DST/include/native/directx"
  git -C "$DST/include/native/directx" -c advice.detachedHead=false checkout -q "$DIRECTX_REV"
  [ "$(git -C "$DST/include/native/directx" rev-parse HEAD)" = "$DIRECTX_REV" ] \
    || { echo "FATAL: mingw-directx-headers not at $DIRECTX_REV" >&2; exit 1; }
  [ -f "$DST/include/native/windows/windows.h" ] || { echo "FATAL: no include/native/windows" >&2; exit 1; }
  touch "$DST/.done-$DXVK_REV"
fi
echo "$DST/include/native"
