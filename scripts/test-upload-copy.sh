#!/usr/bin/env bash
# test-upload-copy.sh — UploadPerDrawData's vertex clamp and copy (#56) under AddressSanitizer and
# UndefinedBehaviorSanitizer (test/native/upload_copy_test.cpp). The test compiles the driver's own
# statements: test/native/upload_copy_extract.py cuts the VBOCopy struct, one stream's clamp and
# its copy out of d3d9_device.cpp by their source anchors, and fails (exit 2) if an anchor is
# missing or not unique.
#
#   scripts/test-upload-copy.sh [d3d9_device.cpp]     # default: this checkout's
#
# Another commit's file tests that commit (git show <rev>:driver/d3d9/d3d9_device.cpp > <file>).
# Writes only under a mktemp dir.
set -euo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
SRC="${1:-$V2/driver/d3d9/d3d9_device.cpp}"
OUT="$(mktemp -d "${TMPDIR:-/tmp}/d9mt-upload-copy.XXXXXX")"
trap 'rm -rf "$OUT"' EXIT

python3 -I "$V2/test/native/upload_copy_extract.py" "$SRC" "$OUT"
clang++ -std=c++17 -g -O1 -Wall -fsanitize=address,undefined -fno-sanitize-recover=all \
  -I "$OUT" "$V2/test/native/upload_copy_test.cpp" -o "$OUT/uct"
"$OUT/uct"
