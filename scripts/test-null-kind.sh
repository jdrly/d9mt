#!/usr/bin/env bash
# test-null-kind.sh — #62's null-texture selection for unbound argument-table slots
# (test/native/null_kind_test.cpp) on the production statements: null_kind_extract.py cuts
# nullTextureId() out of mtl_context_impl.h and the shader-change slot mask out of
# mtl_context_transfer.cpp by their anchors (exit 2 if an anchor is missing or not unique).
#
#   scripts/test-null-kind.sh [<rev>]     # default: this checkout's files; <rev>: that commit's
#
# A commit without #62 fails (its anchors don't exist). Writes only under build/native-tests/null-kind.
set -euo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$V2/build/native-tests/null-kind"
rm -rf "$OUT"; mkdir -p "$OUT"
IMPL=$V2/driver/backend/mtl_context_impl.h XFER=$V2/driver/backend/mtl_context_transfer.cpp
if [ $# -gt 0 ]; then
  git -C "$V2" show "$1:driver/backend/mtl_context_impl.h" > "$OUT/impl.h"
  git -C "$V2" show "$1:driver/backend/mtl_context_transfer.cpp" > "$OUT/xfer.cpp"
  IMPL=$OUT/impl.h XFER=$OUT/xfer.cpp
fi
python3 -I "$V2/test/native/null_kind_extract.py" "$IMPL" "$XFER" "$OUT"
clang++ -std=c++17 -g -O1 -Wall -fsanitize=address,undefined -fno-sanitize-recover=all \
  -I "$OUT" "$V2/test/native/null_kind_test.cpp" -o "$OUT/nkt"
"$OUT/nkt"
