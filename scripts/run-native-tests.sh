#!/usr/bin/env bash
# Build and run the native unit tests (test/native/*.cpp), as README's Tests section lists
# them: host clang++ (no Wine, no GPU), and submission_retire_test as a Windows program
# under Wine (dxvk::thread is a Windows thread) when ENGINE is set.
#
#   [ENGINE=<wine root>] scripts/run-native-tests.sh
#
# state_cache_test needs DXVK's native headers: scripts/test-state-cache.sh runs it.
# Exit status 1 if any test fails or doesn't build.
set -uo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$V2/build/native-tests"
mkdir -p "$OUT"
cd "$V2"
fails=0
host() { # <name> <clang++ args...>
  local n=$1; shift
  if ! clang++ -std=c++17 "$@" -o "$OUT/$n" 2> "$OUT/$n.build.log"; then
    echo "$n: FAIL (build, $OUT/$n.build.log)"; fails=$((fails + 1)); return
  fi
  if "$OUT/$n" > "$OUT/$n.log" 2>&1; then echo "$n: ok"; else echo "$n: FAIL ($OUT/$n.log)"; fails=$((fails + 1)); fi
}
host attachment_opt_test -I driver/backend test/native/attachment_opt_test.cpp
host block_fill_test -I driver test/native/block_fill_test.cpp
host buffer_tracking_test -I driver/d3d9 -I driver/mtl test/native/buffer_tracking_test.cpp
host fenced_pool_test -I driver/util test/native/fenced_pool_test.cpp
host hitch_test -I driver/d3d9 test/native/hitch_test.cpp
host memory_profile_test -I driver/mtl test/native/memory_profile_test.cpp
host pace_test -I driver/d3d9 test/native/pace_test.cpp
host stats_test -I driver/d3d9 test/native/stats_test.cpp
host upload_ring_test -I test/native/stubs -I driver/backend test/native/upload_ring_test.cpp driver/backend/mtl_ring.cpp
host xfer_stats_test -I driver/d3d9 test/native/xfer_stats_test.cpp

if [ -n "${ENGINE:-}" ]; then
  n=submission_retire_test
  if x86_64-w64-mingw32-g++ -std=c++17 -O1 '-DWINEMETAL_API=extern "C"' -I driver/backend \
       test/native/$n.cpp driver/backend/mtl_submission.cpp driver/util/thread.cpp \
       -o "$OUT/$n.exe" -static 2> "$OUT/$n.build.log"; then
    if WINEPREFIX="${WINEPREFIX:-$OUT/prefix}" WINEDEBUG=-all "$ENGINE/bin/wine" "$OUT/$n.exe" > "$OUT/$n.log" 2>&1 \
       && ! grep -q '^FAIL' "$OUT/$n.log"; then
      echo "$n: ok"
    else
      echo "$n: FAIL ($OUT/$n.log)"; fails=$((fails + 1))
    fi
    "$ENGINE/bin/wineserver" -k 2>/dev/null || true
  else
    echo "$n: FAIL (build, $OUT/$n.build.log)"; fails=$((fails + 1))
  fi
else
  echo "submission_retire_test: skipped (needs ENGINE, runs under Wine)"
fi
echo "state_cache_test: run by scripts/test-state-cache.sh (needs DXVK's native headers)"
[ "$fails" -eq 0 ]
