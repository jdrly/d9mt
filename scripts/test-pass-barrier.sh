#!/usr/bin/env bash
# test-pass-barrier.sh — the producer barrier's stage masks (#64; test/native/pass_barrier_test.cpp).
# The test compiles the driver's own MtlContextImpl::emitProducerBarrier:
# test/native/pass_barrier_extract.py cuts it out of mtl_context_transfer.cpp by its source
# anchors, and fails (exit 2) if an anchor is missing or not unique.
#
#   scripts/test-pass-barrier.sh [mtl_context_transfer.cpp]   # default: this checkout's
#   scripts/test-pass-barrier.sh --witness <base's mtl_context_transfer.cpp>
#
# --witness also runs the base file (git show <rev>:driver/backend/mtl_context_transfer.cpp > <file>)
# and four mutants of this checkout's method; each must fail (no_vertex, all_consumers,
# all_producers, no_epoch). PASS_BARRIER_OUT=<dir> keeps the builds and logs (default: a mktemp dir).
set -uo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
if [ -n "${PASS_BARRIER_OUT:-}" ]; then OUT=$PASS_BARRIER_OUT; mkdir -p "$OUT"
else OUT="$(mktemp -d "${TMPDIR:-/tmp}/d9mt-pass-barrier.XXXXXX")"; trap 'rm -rf "$OUT"' EXIT; fi
HEAD_SRC="$V2/driver/backend/mtl_context_transfer.cpp"
WITNESS=""
case "${1:-}" in
  --witness) WITNESS="${2:?base file}" ;;
  "") ;;
  *) HEAD_SRC=$1 ;;
esac

case_run() { # <name> <source file> [<mutant>]: prints the result, returns the test's status
  local n=$1 src=$2 m=${3:-}
  python3 -I "$V2/test/native/pass_barrier_extract.py" "$src" "$OUT/$n" $m || { echo "$n: extract FAILED"; return 2; }
  clang++ -std=c++17 -g -O1 -Wall -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I "$OUT/$n" "$V2/test/native/pass_barrier_test.cpp" -o "$OUT/$n/pbt" > "$OUT/$n/build.log" 2>&1 \
    || { echo "$n: build FAILED ($OUT/$n/build.log)"; return 2; }
  "$OUT/$n/pbt" > "$OUT/$n/test.log" 2>&1; local r=$?
  echo "$n: rc $r, $(grep -c '^FAIL ' "$OUT/$n/test.log") failed cases"
  return $r
}

case_run head "$HEAD_SRC"; rc=$?
[ -z "$WITNESS" ] && exit $rc
[ $rc -eq 0 ] || { echo "control failed: base and mutants not evaluated"; exit 1; }

bad=0
case_run base "$WITNESS"; r=$?
if [ $r -eq 1 ] && ! grep -q '^FAIL blit' "$OUT/base/test.log"; then echo "  base fails as it must (Fragment cases only)"
else echo "  base: NOT the expected failure"; bad=1; fi
for m in no_vertex all_consumers all_producers no_epoch; do
  case_run "$m" "$HEAD_SRC" "$m"; r=$?
  if [ $r -eq 1 ]; then echo "  $m fails as it must"; else echo "  $m: NOT the expected failure (rc $r)"; bad=1; fi
done
exit $bad
