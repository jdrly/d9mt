#!/usr/bin/env bash
# test-present-ring.sh — the bridge's presented-time ring (bridge/unix/present_ring.h, the SCREEN
# line's stream, d9mt #153) under ThreadSanitizer: test/native/present_ring_test.cpp, whose stress
# case runs writer threads against a draining reader. The plain build of the same test runs in
# scripts/run-native-tests.sh. No Wine, no GPU. Writes only under build/present-ring/.
#   scripts/test-present-ring.sh
set -euo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$V2/build/present-ring"
mkdir -p "$OUT"
cd "$V2"
clang++ -std=c++17 -O1 -g -fsanitize=thread -I bridge/unix -I driver/d3d9 test/native/present_ring_test.cpp \
  -o "$OUT/present_ring_test_tsan"
TSAN_OPTIONS="halt_on_error=1 ${TSAN_OPTIONS:-}" "$OUT/present_ring_test_tsan"
echo "present_ring_test (ThreadSanitizer): ok"
