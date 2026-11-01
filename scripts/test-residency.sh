#!/usr/bin/env bash
# test-residency.sh — the residency tracker's pipeline test (test/native/residency_pso_test.cpp,
# #63): pipeline states join the residency set, are committed before use, and leave it (removal
# committed) before they are released. Builds driver/backend/mtl_residency.cpp as is, plain and
# with ASan/UBSan. Needs DXVK's native headers as scripts/test-state-cache.sh (DXVK_NATIVE=<dir>).
#   scripts/test-residency.sh
set -euo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
NATIVE="${DXVK_NATIVE:-$V2/DXVK-3.0-uptsream/include/native}"
[ -d "$NATIVE/windows" ] && [ -d "$NATIVE/directx" ] || {
  echo "[residency] no DXVK native headers at $NATIVE (set DXVK_NATIVE)" >&2; exit 2; }
OUT="$V2/build/native-tests"
mkdir -p "$OUT"
build() { # <name> <extra flags...>
  local n=$1; shift
  clang++ -std=c++17 -O1 -g -Wall "$@" -I"$V2/driver/backend" -I"$NATIVE/windows" -I"$NATIVE/directx" \
    -include "$V2/test/native/stubs/virtual_free.h" \
    "$V2/test/native/residency_pso_test.cpp" "$V2/driver/backend/mtl_residency.cpp" -o "$OUT/$n"
  "$OUT/$n"
}
build residency_pso_test
build residency_pso_test_asan -fsanitize=address,undefined -fno-sanitize-recover=undefined
build residency_pso_test_tsan -fsanitize=thread
