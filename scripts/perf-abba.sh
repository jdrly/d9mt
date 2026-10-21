#!/usr/bin/env bash
# perf-abba.sh — compares d3d9.dll variants on one bench under Wine, in rotated rounds: every
# round runs each variant once, and the order shifts by one per round, so each variant takes
# each position equally often (ABBA). One line per run into $WORK/results/<tag>.txt, then the
# comparison (scripts/perf-compare.py): per variant the median, the per-round paired difference
# to the reference, "slower n/N", and the control's difference as the noise floor.
# PERFORMANCE.md has the method and its pitfalls.
#
#   ENGINE=<Wine engine root> WORK=<scratch dir> scripts/perf-abba.sh [options] <bench> [bench args]
#     --variant <name>=<d3d9.dll>   repeat, 2 or more; the first is the reference unless --ref
#     --native                      the variants are native executables (<name>=<program>) run
#                                   directly with the bench args, without Wine; e.g. one
#                                   state_cache_test per tree: --native ... -- bench 50
#     --env <name>:<VAR>=<value>    extra environment for one variant (repeat), e.g. the same DLL
#                                   with a switch off
#     --rounds <n>                  (10)
#     --timeout <s>                 (300) per run
#     --tag <tag>                   (the bench name) the results file
#     --ref <name>                  the reference (default: the first variant)
#     --control <name>              a variant whose difference to the reference is noise only,
#                                   e.g. a byte copy of the reference DLL
#   <bench>: an exe in BENCH_DIR (default build/bench/x64, scripts/build-benches.sh) or a path.
#   RUN_WRAPPER="<cmd> [args]"      optional prefix for each Wine run (e.g. CPU pinning).
#   To run the benches' windows on a virtual display, wrap the whole call, e.g.
#     <virtual-display tool> scripts/perf-abba.sh ...
#   and build them with BENCH_INCLUDE (scripts/build-benches.sh).
#
# Nothing outside WORK is read or written except the given DLLs and the bench; the engine is only
# executed. Each variant gets its own AIR cache and PSO archives (warm after its first run); the
# state cache is off. A run is marked DIRTY when another engine's wineserver is running during it.
# Benchmarks belong in a quiet machine: no other GPU or CPU load, the display asleep unless the
# bench needs it awake.
set -uo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
: "${WORK:?set WORK to a scratch dir}"

NAMES=() DLLS=() ENVS=() ROUNDS=10 TIMEOUT=300 TAG="" REF="" CONTROL="" NATIVE=0
while [ $# -gt 0 ]; do
  case "$1" in
    --variant) NAMES+=("${2%%=*}"); DLLS+=("${2#*=}"); shift 2 ;;
    --env) ENVS+=("$2"); shift 2 ;;
    --native) NATIVE=1; shift ;;
    --rounds) ROUNDS="$2"; shift 2 ;;
    --timeout) TIMEOUT="$2"; shift 2 ;;
    --tag) TAG="$2"; shift 2 ;;
    --ref) REF="$2"; shift 2 ;;
    --control) CONTROL="$2"; shift 2 ;;
    --) shift; break ;;
    -*) echo "unknown option $1" >&2; exit 2 ;;
    *) break ;;
  esac
done
[ ${#NAMES[@]} -ge 2 ] || { echo "need at least two --variant" >&2; exit 2; }
for d in "${DLLS[@]}"; do [ -f "$d" ] || { echo "no file $d" >&2; exit 2; }; done
REF="${REF:-${NAMES[0]}}"
A="$WORK/abba"
mkdir -p "$WORK/results" "$A/logs"

if [ "$NATIVE" = 1 ]; then
  # Native programs: the remaining arguments are theirs.
  TAG="${TAG:-$(basename "${DLLS[0]}")}"
  WHAT="native"
else
  : "${ENGINE:?set ENGINE to a Wine engine root (bin/wine, bin/wineserver)}"
  BENCH="${1:?usage: perf-abba.sh [options] <bench> [bench args] (see the header)}"; shift
  case "$BENCH" in
    */*) EXE="$BENCH" ;;
    *) EXE="${BENCH_DIR:-$V2/build/bench/x64}/${BENCH%.exe}.exe" ;;
  esac
  [ -f "$EXE" ] || { echo "no bench $EXE (scripts/build-benches.sh)" >&2; exit 2; }
  NAME_EXE="$(basename "$EXE")"
  TAG="${TAG:-${NAME_EXE%.exe}}" WHAT="$NAME_EXE"
  export WINEPREFIX="$WORK/prefix" WINEDEBUG=-all WINEMSYNC=1
  export DYLD_FALLBACK_LIBRARY_PATH="${FRAMEWORKS:+$FRAMEWORKS:}/usr/lib"
  WINE="$ENGINE/bin/wine" WINESERVER="$ENGINE/bin/wineserver"
  if [ ! -f "$WINEPREFIX/system.reg" ]; then
    "$WINE" wineboot -i >"$WORK/wineboot.log" 2>&1
    "$WINESERVER" -w
  fi
  for i in "${!NAMES[@]}"; do
    mkdir -p "$A/bin-${NAMES[$i]}" "$A/caches/${NAMES[$i]}/pso"
    cp "${DLLS[$i]}" "$A/bin-${NAMES[$i]}/d3d9.dll"
    cp "$EXE" "$A/bin-${NAMES[$i]}/"
  done
fi

# Another engine's wineserver (another program, another test run) running: the run is DIRTY.
foreign() { ps -axo comm= | grep -E '/wineserver$' | grep -vF "${ENGINE:-/nonexistent}/" | head -1; }

OUT="$WORK/results/$TAG.txt"
echo "# $WHAT $* rounds=$ROUNDS variants=${NAMES[*]} ref=$REF${CONTROL:+ control=$CONTROL} $(date '+%F %T')" >> "$OUT"
for i in "${!NAMES[@]}"; do echo "#   ${NAMES[$i]} = ${DLLS[$i]} ($(shasum -a 256 "${DLLS[$i]}" | cut -c1-16))" >> "$OUT"; done
n=${#NAMES[@]}
for r in $(seq "$ROUNDS"); do
  for k in $(seq 0 $((n - 1))); do
    i=$(( (k + r) % n )) v=${NAMES[$(( (k + r) % n ))]}
    log="$A/logs/$TAG-$v-$r.err"
    load=$(sysctl -n vm.loadavg 2>/dev/null | awk '{print $2}')
    dirty=$(foreign)
    (
      for e in ${ENVS[@]+"${ENVS[@]}"}; do [ "${e%%:*}" = "$v" ] && export "${e#*:}"; done
      if [ "$NATIVE" = 1 ]; then
        exec ${RUN_WRAPPER:-} "${DLLS[$i]}" "$@"
      fi
      export WINEDLLOVERRIDES="d3d9=n,b"
      export D9MT_AIR_CACHE="$A/caches/$v/air-cache.sqlite3" D9MT_PSO_ARCHIVE_DIR="$A/caches/$v/pso"
      export D9MT_STATE_CACHE=0 D9MT_BG_FPS=0 D9MT_STALL_PACE=0
      cd "$A/bin-$v"
      exec ${RUN_WRAPPER:-} "$WINE" "$NAME_EXE" "$@"
    ) > "$A/out" 2> "$log" &
    pid=$! t=0
    while kill -0 "$pid" 2>/dev/null && [ "$t" -lt "$TIMEOUT" ]; do
      sleep 1; t=$((t + 1))
      [ -z "$dirty" ] && dirty=$(foreign)
    done
    if kill -0 "$pid" 2>/dev/null; then
      if [ "$NATIVE" = 1 ]; then kill -9 "$pid" 2>/dev/null; else "$WINESERVER" -k >/dev/null 2>&1; fi
      wait "$pid" 2>/dev/null
      line="RESULT FAIL timeout ${TIMEOUT}s"
    else
      wait "$pid"; rc=$?
      line=$(tr -d '\r' < "$A/out" | grep -E '^(RESULT|BENCH)' | head -1)
      [ -n "$line" ] || line="RESULT FAIL rc=$rc"
    fi
    # A wait on a serial still unsubmitted after a whole 2 s slice: a hang hazard, not a time.
    grep -q 'not submitted yet' "$log" 2>/dev/null && line="RESULT FAIL unsubmitted-wait"
    [ "$NATIVE" = 1 ] || { "$WINESERVER" -k >/dev/null 2>&1; "$WINESERVER" -w >/dev/null 2>&1; }
    echo "${dirty:+DIRTY }round=$r v=$v load=$load $line" | tee -a "$OUT"
  done
done

python3 "$V2/scripts/perf-compare.py" --ref "$REF" ${CONTROL:+--control "$CONTROL"} "$OUT"
