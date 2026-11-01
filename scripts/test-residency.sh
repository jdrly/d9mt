#!/usr/bin/env bash
# test-residency.sh — pipeline states in the residency set (#63), two native tests, each plain,
# with ASan/UBSan and with TSan:
#  - test/native/residency_pso_test.cpp: the tracker (driver/backend/mtl_residency.cpp as is):
#    commit before use, the removal committed before release, no leak, concurrent adds;
#  - test/native/pso_residency_test.cpp: the PSO cache (driver/backend/mtl_pso_cache.cpp as is)
#    with a stub compiler: every PSO built for a draw, an internal pipeline, the prewarm and a
#    failed prewarm's replacement is in the set once done, and committed by the next submit. It
#    watches bridge calls only, so it also builds against a tree without #63 (and fails there).
# Needs DXVK's native headers as scripts/test-state-cache.sh (DXVK_NATIVE=<dir>).
#   scripts/test-residency.sh [<driver tree>]   (default: this checkout's driver/; another tree's
#                                                driver/ with this checkout's tests, for witnesses)
# RESIDENCY_PLAIN_ONLY=1: the plain builds only; RESIDENCY_OUT=<dir>: where binaries and logs go.
# Every variant runs; exit status 1 if any of them fails or doesn't build.
set -uo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
SRC="${1:-$V2}"
NATIVE="${DXVK_NATIVE:-$V2/DXVK-3.0-uptsream/include/native}"
[ -d "$NATIVE/windows" ] && [ -d "$NATIVE/directx" ] || {
  echo "[residency] no DXVK native headers at $NATIVE (set DXVK_NATIVE)" >&2; exit 2; }
OUT="${RESIDENCY_OUT:-$V2/build/native-tests}"
mkdir -p "$OUT"
INC=(-I"$SRC/driver/backend" -I"$SRC/driver" -I"$NATIVE/windows" -I"$NATIVE/directx"
     -include "$V2/test/native/stubs/virtual_free.h")
TRACKER=("$V2/test/native/residency_pso_test.cpp" "$SRC/driver/backend/mtl_residency.cpp")
CACHE=("$V2/test/native/pso_residency_test.cpp" "$SRC/driver/backend/mtl_pso_cache.cpp"
       "$SRC/driver/backend/mtl_residency.cpp" "$SRC/driver/mtl/mtl_state_cache.cpp"
       "$SRC/driver/mtl/mtl_state_cache_core.cpp")
fails=0
run() { # <name> <test: TRACKER|CACHE> <flags...>
  local n=$1 t=$2; shift 2
  local srcs
  if [ "$t" = TRACKER ]; then srcs=("${TRACKER[@]}"); else srcs=("${CACHE[@]}"); fi
  if ! clang++ -std=c++17 -O1 -g -Wall "$@" "${INC[@]}" "${srcs[@]}" -o "$OUT/$n" > "$OUT/$n.build.log" 2>&1; then
    echo "$n: FAIL (build, $OUT/$n.build.log)"; fails=$((fails + 1)); return
  fi
  if TSAN_OPTIONS=halt_on_error=1 "$OUT/$n" > "$OUT/$n.log" 2>&1; then echo "$n: ok ($(tail -1 "$OUT/$n.log"))"
  else echo "$n: FAIL ($OUT/$n.log: $(grep -c '^FAIL' "$OUT/$n.log") failed checks)"; fails=$((fails + 1)); fi
}
for t in TRACKER CACHE; do
  case $t in TRACKER) n=residency_pso_test ;; CACHE) n=pso_residency_test ;; esac
  # the tracker test calls #63's API: it can't build against a tree without it
  if [ $t = TRACKER ] && ! grep -q 'pipelineCreated' "$SRC/driver/backend/mtl_residency.h"; then
    echo "$n: skipped ($SRC has no MtlResidencyTracker::pipelineCreated)"; continue
  fi
  run "$n" $t
  [ -n "${RESIDENCY_PLAIN_ONLY:-}" ] && continue
  run "${n}_asan" $t -fsanitize=address,undefined -fno-sanitize-recover=undefined
  run "${n}_tsan" $t -fsanitize=thread
done
[ "$fails" -eq 0 ]
