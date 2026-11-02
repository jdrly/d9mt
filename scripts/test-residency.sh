#!/usr/bin/env bash
# test-residency.sh — pipeline states in the residency set (#63), two native tests, each plain,
# with ASan/UBSan and with TSan:
#  - test/native/residency_pso_test.cpp: the tracker (driver/backend/mtl_residency.cpp as is):
#    commit before use, the removal committed before release, no leak, concurrent adds;
#  - test/native/pso_residency_test.cpp: the PSO cache (driver/backend/mtl_pso_cache.cpp as is)
#    with a stub compiler: every PSO built for a draw, an internal pipeline, the prewarm and a
#    failed prewarm's replacement is in the set once done, and committed by the next submit. It
#    watches bridge calls only, so it also builds against a tree without #63 (and fails there);
#  - test/native/commit_guard_test.cpp: the context's commit path (commitRecorded to submit, cut
#    out of mtl_context_record.cpp by test/native/commit_guard_extract.py): after a residency
#    commit that couldn't be made (process detach), no queue commit, serial, retirement, wait or
#    canary check, for submit and commitPrefix. Plain and ASan/UBSan (no threads);
#  - test/native/submission_abandon_test.cpp: SubmissionTracker::abandon (mtl_submission.cpp as
#    is): waits unchanged (waitFor and the drain still wait for submitted serials); what is
#    retired past the last submitted serial is leaked, the rest still runs. Plain and ASan/UBSan;
#  - test/native/device_teardown_test.cpp: MtlDeviceImpl's destructor (cut out of mtl_device_impl.cpp
#    by test/native/device_teardown_extract.py) releases the sampler and depth-stencil caches
#    normally and leaks them after a dropped submit. Plain and ASan/UBSan.
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
# submission_abandon_test: the tracker's abandonment (it calls #63's API: not on a tree without it)
n=submission_abandon_test
if ! grep -q 'void abandon()' "$SRC/driver/backend/mtl_submission.h"; then
  echo "$n: skipped ($SRC has no SubmissionTracker::abandon)"
else
  for v in plain asan; do
    f=(); nn=$n; [ $v = asan ] && { f=(-fsanitize=address,undefined -fno-sanitize-recover=all); nn=${n}_asan; }
    if ! clang++ -std=c++17 -O1 -g -Wall "${f[@]}" -I"$SRC/driver/backend" -I"$SRC/driver" -I"$NATIVE/windows" -I"$NATIVE/directx" \
         -include "$V2/test/native/stubs/win_submission.h" "$V2/test/native/submission_abandon_test.cpp" \
         "$SRC/driver/backend/mtl_submission.cpp" -o "$OUT/$nn" > "$OUT/$nn.build.log" 2>&1; then
      echo "$nn: FAIL (build, $OUT/$nn.build.log)"; fails=$((fails + 1)); continue
    fi
    if "$OUT/$nn" > "$OUT/$nn.log" 2>&1; then echo "$nn: ok"
    else echo "$nn: FAIL ($OUT/$nn.log: $(grep -c '^FAIL' "$OUT/$nn.log") failed checks)"; fails=$((fails + 1)); fi
    [ -n "${RESIDENCY_PLAIN_ONLY:-}" ] && break
  done
fi

# device_teardown_test: the driver's own device destructor
n=device_teardown_test
if ! python3 -I "$V2/test/native/device_teardown_extract.py" "$SRC/driver/backend/mtl_device_impl.cpp" "$OUT/$n.src" > "$OUT/$n.extract.log" 2>&1; then
  echo "$n: FAIL (extract, $OUT/$n.extract.log)"; fails=$((fails + 1))
else
  for v in plain asan; do
    f=(); nn=$n; [ $v = asan ] && { f=(-fsanitize=address,undefined -fno-sanitize-recover=all); nn=${n}_asan; }
    if ! clang++ -std=c++17 -O1 -g -Wall "${f[@]}" -I "$OUT/$n.src" "$V2/test/native/device_teardown_test.cpp" -o "$OUT/$nn" > "$OUT/$nn.build.log" 2>&1; then
      echo "$nn: FAIL (build, $OUT/$nn.build.log)"; fails=$((fails + 1)); continue
    fi
    if "$OUT/$nn" > "$OUT/$nn.log" 2>&1; then echo "$nn: ok"
    else echo "$nn: FAIL ($OUT/$nn.log: $(grep -c '^FAIL' "$OUT/$nn.log") failed checks)"; fails=$((fails + 1)); fi
    [ -n "${RESIDENCY_PLAIN_ONLY:-}" ] && break
  done
fi

# commit_guard_test: the driver's own commit path
n=commit_guard_test
if ! python3 -I "$V2/test/native/commit_guard_extract.py" "$SRC/driver/backend/mtl_context_record.cpp" "$OUT/$n.src" > "$OUT/$n.extract.log" 2>&1; then
  echo "$n: FAIL (extract, $OUT/$n.extract.log)"; fails=$((fails + 1))
else
  for v in plain asan; do
    f=(); nn=$n; [ $v = asan ] && { f=(-fsanitize=address,undefined -fno-sanitize-recover=all); nn=${n}_asan; }
    if ! clang++ -std=c++17 -O1 -g -Wall "${f[@]}" -I "$OUT/$n.src" -I "$V2/test/native" "$V2/test/native/commit_guard_test.cpp" -o "$OUT/$nn" > "$OUT/$nn.build.log" 2>&1; then
      echo "$nn: FAIL (build, $OUT/$nn.build.log)"; fails=$((fails + 1)); continue
    fi
    if "$OUT/$nn" > "$OUT/$nn.log" 2>&1; then echo "$nn: ok"
    else echo "$nn: FAIL ($OUT/$nn.log: $(grep -c '^FAIL' "$OUT/$nn.log") failed checks)"; fails=$((fails + 1)); fi
    [ -n "${RESIDENCY_PLAIN_ONLY:-}" ] && break
  done
fi
[ "$fails" -eq 0 ]
