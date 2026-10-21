#!/usr/bin/env bash
# test-pso-key-hash.sh — the PSO key hash's quality test (test/native/pso_key_hash_test.cpp,
# docs/pso-key-hash.md §4): avalanche, and with state cache files as arguments, collisions and
# bucket spread on their real keys, plus native timing. Needs DXVK's native headers as
# scripts/test-state-cache.sh (DXVK_NATIVE=<dir>).
#   scripts/test-pso-key-hash.sh [state cache files...]
set -euo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
NATIVE="${DXVK_NATIVE:-$V2/DXVK-3.0-uptsream/include/native}"
[ -d "$NATIVE/windows" ] && [ -d "$NATIVE/directx" ] || {
  echo "[pso-key-hash] no DXVK native headers at $NATIVE (set DXVK_NATIVE)" >&2; exit 2; }
OUT="$(mktemp -d /tmp/d9mt-pkh-test.XXXXXX)"
clang++ -std=c++17 -O2 -g -Wall -I"$V2/driver" -I"$NATIVE/windows" -I"$NATIVE/directx" \
  "$V2/test/native/pso_key_hash_test.cpp" "$V2/driver/mtl/mtl_state_cache_core.cpp" -o "$OUT/pkh"
"$OUT/pkh" "$@"
rm -rf "$OUT"
