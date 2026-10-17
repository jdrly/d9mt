#!/usr/bin/env bash
# run-statecache-bench.sh — one run of test/code_statecache.c (synthetic many-pipeline workload,
# docs/state-cache.md) under Wine with every cache in a scratch dir, then a one-line summary:
# the test's world-phase frame times, d9mt's render-thread compile waits (COMPILE lines) and
# the STATECACHE counters.
#
#   ENGINE=<Wine engine root> WORK=<scratch dir> scripts/run-statecache-bench.sh <label> [options]
#     --dll <d3d9.dll>       (default build/driver-x64/d3d9.dll)
#     --caches <dir>         AIR cache + PSO archives live here (default $WORK/caches/<label>)
#     --state <dir|0>        D9MT_STATE_CACHE (default $WORK/state)
#     --threads <n>          D9MT_STATE_CACHE_THREADS
#     --fastmath             D9MT_AIR_FASTMATH=1: different AIR bytes, so Metal's own shader
#                            cache misses (simulates a bridge update that changes the AIR)
#     --args "<test args>"   seed [pixelShaders pipelines loadMs perFrame] (default "1")
#     --timeout <s>          (default 180)
#
# Nothing outside WORK is read or written (Wine prefix, AIR cache, PSO archives, state cache);
# the engine is only executed. Prints: label, summary; keeps the log in $WORK/logs/<label>.log.
#   EXC_GUARD=<exc_guard binary>  run Wine through test/harness/exc_guard.c (arm64ec branch), so
#                                 a crash is logged to $WORK/logs/<label>.exc instead of a
#                                 "quit unexpectedly" dialog
set -uo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
: "${ENGINE:?set ENGINE to a Wine engine root (bin/wine, bin/wineserver)}"
: "${WORK:?set WORK to a scratch dir}"
LABEL="${1:?usage: run-statecache-bench.sh <label> [options]}"; shift
DLL="$V2/build/driver-x64/d3d9.dll" CACHES="" STATE="" THREADS="" FASTMATH=0 ARGS="1" TIMEOUT=180
while [ $# -gt 0 ]; do
  case "$1" in
    --dll) DLL="$2"; shift 2 ;;
    --caches) CACHES="$2"; shift 2 ;;
    --state) STATE="$2"; shift 2 ;;
    --threads) THREADS="$2"; shift 2 ;;
    --fastmath) FASTMATH=1; shift ;;
    --args) ARGS="$2"; shift 2 ;;
    --timeout) TIMEOUT="$2"; shift 2 ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
done
CACHES="${CACHES:-$WORK/caches/$LABEL}" STATE="${STATE:-$WORK/state}"
mkdir -p "$WORK/logs" "$CACHES"
[ "$STATE" = 0 ] || mkdir -p "$STATE"

export WINEPREFIX="$WORK/prefix" WINEDEBUG=-all WINEMSYNC=1
export DYLD_FALLBACK_LIBRARY_PATH="${FRAMEWORKS:+$FRAMEWORKS:}/usr/lib"
WINE="$ENGINE/bin/wine" WINESERVER="$ENGINE/bin/wineserver"
if [ ! -f "$WINEPREFIX/system.reg" ]; then
  "$WINE" wineboot -i >"$WORK/wineboot.log" 2>&1
  "$WINESERVER" -w
fi

D="$WINEPREFIX/drive_c/d9mt-statecache"
mkdir -p "$D"
if [ ! -f "$D/code_statecache.exe" ] || [ "$V2/test/code_statecache.c" -nt "$D/code_statecache.exe" ]; then
  x86_64-w64-mingw32-gcc -O2 -Wall "$V2/test/code_statecache.c" -o "$D/code_statecache.exe" \
    -ld3d9 -lgdi32 -luser32 || exit 2
fi
cp "$DLL" "$D/d3d9.dll"

LOG="$WORK/logs/$LABEL.log"
(
  export WINEDLLOVERRIDES="d3d9=n,b"
  export D9MT_PROFILE=1 DXVK_LOG_LEVEL=info DXVK_LOG_PATH=none
  export D9MT_AIR_CACHE="$CACHES/air-cache.sqlite3" D9MT_PSO_ARCHIVE_DIR="$CACHES/pso"
  export D9MT_STATE_CACHE="$STATE"
  [ -n "$THREADS" ] && export D9MT_STATE_CACHE_THREADS="$THREADS"
  [ "$FASTMATH" = 1 ] && export D9MT_AIR_FASTMATH=1
  cd "$D"
  if [ -n "${EXC_GUARD:-}" ]; then
    exec "$EXC_GUARD" "$TIMEOUT" "$WORK/logs/$LABEL.exc" "$WINE" code_statecache.exe $ARGS
  fi
  exec "$WINE" code_statecache.exe $ARGS
) >"$LOG" 2>&1 &
pid=$!
t=0
while kill -0 "$pid" 2>/dev/null && [ "$t" -lt "$TIMEOUT" ]; do sleep 1; t=$((t + 1)); done
if kill -0 "$pid" 2>/dev/null; then
  "$WINESERVER" -k >/dev/null 2>&1; wait "$pid" 2>/dev/null
  echo "$LABEL: TIMEOUT after ${TIMEOUT}s"; exit 1
fi
wait "$pid"
"$WINESERVER" -w >/dev/null 2>&1

app=$(grep -o 'code_statecache OK.*' "$LOG" | tr -d '\r' | sed 's/^code_statecache OK //')
[ -n "$app" ] || { echo "$LABEL: FAILED"; tail -5 "$LOG"; exit 1; }
waits=$(LC_ALL=C awk '/COMPILE frame=/ { for (i = 1; i <= NF; i++) if ($i ~ /^renderWait=/) {
          split(substr($i, 12), a, "/"); n += a[1]; ms += a[2] + 0 } }
        END { printf "renderWait=%d/%.1fms", n, ms }' "$LOG")
psos=$(LC_ALL=C awk '/COMPILE frame=/ { for (i = 1; i <= NF; i++) if ($i ~ /^psos=/) {
          split(substr($i, 6), a, "/"); n += a[1]; ms += a[2] + 0 } }
        END { printf "renderPathPsos=%d/%.1fms", n, ms }' "$LOG")
sc=$(grep 'STATECACHE use' "$LOG" | tail -1 | sed 's/.*STATECACHE use //')
echo "$LABEL: $app | $waits $psos | ${sc:-statecache off}"
