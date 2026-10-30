#!/usr/bin/env bash
# run-native-tests.sh — the native unit tests (test/native/*_test.cpp) that build and run with the
# host's clang++: no Wine, no GPU, no game. Every other test there gets a named skip with the
# script or environment that runs it; a test in neither list fails, so the inventory can't drift.
#
#   scripts/run-native-tests.sh
#
# Never starts Wine and never stops a wineserver: ENGINE and WINEPREFIX are not read. The mingw
# tests run under Wine by hand (their source comments give the command); the dedicated scripts
# stay the canonical runners of their tests.
# Writes only under build/native-tests/. Exit status 1 if any test fails or doesn't build.
set -uo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$V2/build/native-tests"
mkdir -p "$OUT"
cd "$V2"
fails=0
listed=" "
host() { # <name> <clang++ args...>
  local n=$1; shift
  listed+="$n "
  if ! clang++ -std=c++17 "$@" -o "$OUT/$n" 2> "$OUT/$n.build.log"; then
    echo "$n: FAIL (build, $OUT/$n.build.log)"; fails=$((fails + 1)); return
  fi
  if "$OUT/$n" > "$OUT/$n.log" 2>&1; then echo "$n: ok"; else echo "$n: FAIL ($OUT/$n.log)"; fails=$((fails + 1)); fi
}
skip() { # <name> <owner>
  listed+="$1 "
  echo "$1: skipped, run by $2"
}

host attachment_opt_test -I driver/backend test/native/attachment_opt_test.cpp
host block_fill_test -I driver test/native/block_fill_test.cpp
host buffer_tracking_test -I driver/d3d9 -I driver/mtl test/native/buffer_tracking_test.cpp
host fenced_pool_test -I driver/util test/native/fenced_pool_test.cpp
host frame_cap_test -I driver/d3d9 test/native/frame_cap_test.cpp
host hitch_test -I driver/d3d9 test/native/hitch_test.cpp
host memory_profile_test -I driver/mtl test/native/memory_profile_test.cpp
host pace_test -I driver/d3d9 test/native/pace_test.cpp
host pso_gen_test -I driver/backend test/native/pso_gen_test.cpp
host stats_test -I driver/d3d9 test/native/stats_test.cpp
host upload_ring_test -I test/native/stubs -I driver/backend test/native/upload_ring_test.cpp driver/backend/mtl_ring.cpp
host xfer_stats_test -I driver/d3d9 test/native/xfer_stats_test.cpp

skip lifetime_check_test "hand under Wine (a mingw exe; build line in its source)"
skip submission_retire_test "hand under Wine (a mingw exe; build line in its source)"
skip submission_tracker_test "hand under Wine (a mingw exe; build line in its source)"
skip state_cache_test "scripts/test-state-cache.sh (DXVK's native headers; plain and TSan)"
skip pso_key_hash_test "scripts/test-pso-key-hash.sh (DXVK's native headers)"
skip upload_copy_test "scripts/test-upload-copy.sh (extracts d3d9_device.cpp; ASan/UBSan)"

for f in test/native/*_test.cpp; do
  n=$(basename "$f" .cpp)
  case "$listed" in
    *" $n "*) ;;
    *) echo "$n: FAIL (not in this script's inventory)"; fails=$((fails + 1)) ;;
  esac
done
[ "$fails" -eq 0 ]
