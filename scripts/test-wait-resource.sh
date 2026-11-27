#!/usr/bin/env bash
# test-wait-resource.sh — D3D9DeviceEx::WaitForResource's contract (test/native/wait_resource_test.cpp)
# under AddressSanitizer and UndefinedBehaviorSanitizer. The test compiles the driver's own function:
# test/native/wait_resource_extract.py cuts it out of d3d9_device.cpp, and fails (exit 2) if its
# signature is missing or not unique.
#
#   scripts/test-wait-resource.sh [d3d9_device.cpp]     # default: this checkout's
#
# Another commit's file tests that commit (git show <rev>:driver/d3d9/d3d9_device.cpp > <file>).
# CXX names the compiler (default clang++).
# Writes only under a mktemp dir.
set -euo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
SRC="${1:-$V2/driver/d3d9/d3d9_device.cpp}"
OUT="$(mktemp -d "${TMPDIR:-/tmp}/d9mt-wait-resource.XXXXXX")"
trap 'rm -rf "$OUT"' EXIT

CXX="${CXX:-clang++}"
python3 -I "$V2/test/native/wait_resource_extract.py" "$SRC" "$OUT"
"$CXX" -std=c++17 -g -O1 -Wall -fsanitize=address,undefined -fno-sanitize-recover=all \
  -I "$OUT" "$V2/test/native/wait_resource_test.cpp" -o "$OUT/wrt"
"$OUT/wrt"
