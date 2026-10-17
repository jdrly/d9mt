#!/usr/bin/env bash
# run-hang-test.sh — the Debug Logging regression test: test/code_mtlock.c (a render thread
# presenting frames while a second thread streams managed textures, on a device without
# D3DCREATE_MULTITHREADED, as SWTOR) under the profiling/debug environment SWTOR on Mac's
# Debug Logging sets: D9MT_PROFILE=1, D9MT_MEMSTATS=1, DXVK_LOG_LEVEL=info and a
# D9MT_DEBUG_FILE flags file (every flag commented out). It must present all its frames and
# exit within the timeout; a hang (0.0.6-dev.2: SWTOR stuck at a black screen at startup)
# fails it.
#
#   ENGINE=<Wine engine root, bin/wine inside> scripts/run-hang-test.sh [options]
#     --arch x64|x86    (default x64)
#     --dll <d3d9.dll>  (default build/driver-x64/d3d9.dll or build/driver/d3d9.dll)
#     --frames <n>      (default 600)
#     --timeout <s>     (default 90)
#     --runs <n>        (default 3; the race it guards against is timing-dependent)
#   FRAMEWORKS=<dir>    extra DYLD_FALLBACK_LIBRARY_PATH entry the engine needs (optional)
#   WORK=<dir>          scratch dir (default: a new mktemp -d, removed afterwards unless
#                       KEEP=1); holds the Wine prefix, the AIR cache and the PSO archives,
#                       so nothing outside it is read or written
#   SAMPLE=1            on a timeout, `sample` the hung process into WORK first
#
# Uses the engine's own winemetal bridge. Exit 0: every run finished; 1: a run hung,
# crashed or presented too few frames.
set -uo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
: "${ENGINE:?set ENGINE to a Wine engine root (bin/wine, bin/wineserver)}"
ARCH=x64 DLL= FRAMES=600 TIMEOUT=90 RUNS=3
while [ $# -gt 0 ]; do
  case "$1" in
    --arch) ARCH="$2"; shift 2 ;;
    --dll) DLL="$2"; shift 2 ;;
    --frames) FRAMES="$2"; shift 2 ;;
    --timeout) TIMEOUT="$2"; shift 2 ;;
    --runs) RUNS="$2"; shift 2 ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
done
if [ "$ARCH" = x64 ]; then
  CC=x86_64-w64-mingw32-gcc; DLL="${DLL:-$V2/build/driver-x64/d3d9.dll}"
else
  CC=i686-w64-mingw32-gcc; DLL="${DLL:-$V2/build/driver/d3d9.dll}"
fi
[ -f "$DLL" ] || { echo "no $DLL" >&2; exit 2; }

CLEAN=0
if [ -z "${WORK:-}" ]; then WORK="$(mktemp -d /tmp/d9mt-hangtest.XXXXXX)"; CLEAN=1; fi
mkdir -p "$WORK"
export WINEPREFIX="$WORK/prefix" WINEDEBUG=-all WINEMSYNC=1
export DYLD_FALLBACK_LIBRARY_PATH="${FRAMEWORKS:+$FRAMEWORKS:}/usr/lib"
WINE="$ENGINE/bin/wine" WINESERVER="$ENGINE/bin/wineserver"

finish() {
  "$WINESERVER" -k >/dev/null 2>&1
  "$WINESERVER" -w >/dev/null 2>&1
  if [ "$CLEAN" = 1 ] && [ "${KEEP:-0}" != 1 ]; then rm -rf "$WORK"; else echo "[hang-test] kept $WORK"; fi
}
trap finish EXIT

if [ ! -f "$WINEPREFIX/system.reg" ]; then
  echo "[hang-test] new prefix in $WINEPREFIX"
  "$WINE" wineboot -i >"$WORK/wineboot.log" 2>&1
  "$WINESERVER" -w
fi

D="$WINEPREFIX/drive_c/d9mt-test"
mkdir -p "$D"
"$CC" -O1 "$V2/test/code_mtlock.c" -o "$D/code_mtlock.exe" -I "$V2/test" -ld3d9 -lgdi32 -luser32 || exit 2
cp "$DLL" "$D/d3d9.dll"
# As SWTOR on Mac writes it with Debug Logging on: every flag commented out.
cat >"$D/flags.txt" <<'EOF'
# d9mt live debug flags (hang test). d9mt reads this file once a second while the game runs.
#
# no_attach_opt   attachment load/store optimization off
# att_trace       log up to 400 render-pass load/store decisions and re-clears
# dump            dump the next frame's draws
EOF

export WINEDLLOVERRIDES="d3d9=n,b"
export D9MT_PROFILE=1 D9MT_MEMSTATS=1 DXVK_LOG_LEVEL=info D9MT_MEMORY_PROFILE=auto DXVK_LOG_PATH=none
export D9MT_DEBUG_FILE='C:\d9mt-test\flags.txt'
export D9MT_AIR_CACHE="$WORK/air-cache.sqlite3" D9MT_PSO_ARCHIVE_DIR="$WORK/pso"

fail=0
for run in $(seq 1 "$RUNS"); do
  LOG="$WORK/run$run.log"
  ( cd "$D" && exec "$WINE" code_mtlock.exe "$FRAMES" ) >"$LOG" 2>&1 &
  pid=$!
  t=0
  while kill -0 "$pid" 2>/dev/null && [ "$t" -lt "$TIMEOUT" ]; do sleep 1; t=$((t + 1)); done
  if kill -0 "$pid" 2>/dev/null; then
    echo "[hang-test] run $run: HUNG after ${TIMEOUT}s ($(grep -c 'code_mtlock:' "$LOG") progress lines)"
    grep -m3 'Unhandled\|page fault\|exception' "$LOG"
    if [ "${SAMPLE:-0}" = 1 ]; then
      exe_pid=$(ps -axo pid=,command= | awk '/code_mtlock\.exe/ && !/start\.exe/ && !/awk/ { print $1; exit }')
      [ -n "$exe_pid" ] && sample "$exe_pid" 3 -file "$WORK/run$run.sample.txt" >/dev/null 2>&1
    fi
    "$WINESERVER" -k >/dev/null 2>&1
    wait "$pid" 2>/dev/null
    fail=1
    continue
  fi
  wait "$pid"; rc=$?
  if [ "$rc" != 0 ] || ! grep -q "code_mtlock OK frames=$FRAMES" "$LOG"; then
    echo "[hang-test] run $run: FAILED (exit $rc)"; tail -5 "$LOG"; fail=1; continue
  fi
  # The environment was really the Debug Logging one.
  dbg=$(grep -c 'D9MT-DEBUG flags file' "$LOG"); stats=$(grep -c 'STATS t=' "$LOG")
  echo "[hang-test] run $run: ok in ${t}s ($(grep -o 'locks=[0-9]*' "$LOG"), debug-file lines $dbg, STATS lines $stats)"
  [ "$dbg" -ge 1 ] || { echo "[hang-test] run $run: no D9MT-DEBUG flags file line"; fail=1; }
  "$WINESERVER" -w >/dev/null 2>&1
done
[ "$fail" = 0 ] && echo "[hang-test] PASS ($ARCH, $RUNS runs)" || echo "[hang-test] FAIL ($ARCH)"
exit "$fail"
