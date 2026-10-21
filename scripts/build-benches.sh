#!/usr/bin/env bash
# build-benches.sh — builds the performance benches (PE executables) into build/bench/<arch>/.
# They're timing programs, not hash tests: each prints one RESULT/BENCH line that
# scripts/perf-abba.sh collects and scripts/perf-compare.py compares (PERFORMANCE.md).
#
#   scripts/build-benches.sh [bench ...]          (default: all)
#     ARCH=x64|i686     (default x64; SWTOR is x64)
#     BENCH_INCLUDE=<h> a header force-included into every bench (-include), e.g. one that moves
#                       the bench's window onto a virtual display. Optional; the benches build
#                       and run without it.
#
# Benches (sources in test/perf/, bench_4k in test/):
#   bindbench      5000 draws per frame, each rebinding one of 64 textures (texture binds)
#   churnbench     128 new textures per frame plus 3000 rebinding draws (texture churn)
#   psobench       3000 draws switching PSO state on A->B->A: topo|blend|blend2|blendop|blend4|none
#   bench_draws    many small draws with constant updates and a state change: present|flush|spinflush
#   bench_getdata  occlusion queries polled as games do, inside 4K passes: flush|noflush
#   bench_upload   dynamic VB renames and managed texture uploads inside 4K passes
#   bench_4k       a synthetic 4K multi-pass frame: [frames] [post] [clears] [wait | cpu ms]
#   bench_fps_cap  frame pacing under a frame rate cap (D9MT_FPS_CAP, d3d9.maxFrameRate, or a game cap)
# The state cache's render-thread record() cost is native: scripts/test-state-cache.sh
# (state_cache_test bench [rounds]).
set -euo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
ARCH="${ARCH:-x64}"
case "$ARCH" in
  x64)  CC=x86_64-w64-mingw32-gcc ;;
  i686) CC=i686-w64-mingw32-gcc ;;
  *) echo "[benches] ARCH must be x64 or i686" >&2; exit 2 ;;
esac
OUT="$V2/build/bench/$ARCH"
mkdir -p "$OUT"
INC=()
[ -n "${BENCH_INCLUDE:-}" ] && INC=(-include "$BENCH_INCLUDE")

# name -> source (relative to the repo root)
src() {
  case "$1" in
    bench_4k) echo test/bench_4k.c ;;
    bindbench|churnbench|psobench|bench_draws|bench_getdata|bench_upload|bench_fps_cap) echo "test/perf/$1.c" ;;
    *) return 1 ;;
  esac
}
ALL="bindbench churnbench psobench bench_draws bench_getdata bench_upload bench_4k bench_fps_cap"
BENCHES="${*:-$ALL}"

for b in $BENCHES; do
  s=$(src "$b") || { echo "[benches] unknown bench $b (known: $ALL)" >&2; exit 2; }
  "$CC" -O1 -Wall -ffile-prefix-map="$V2"=. ${INC[@]+"${INC[@]}"} -I "$V2/test" "$V2/$s" -o "$OUT/$b.exe" \
    -ld3d9 -lgdi32 -luser32 -lm
  echo "[benches] $OUT/$b.exe"
done
