#!/usr/bin/env bash
# perf-abba.sh — compares d3d9.dll variants on one bench under Wine, in rotated rounds: every
# round runs each variant once, and the order shifts by one per round, so each variant takes
# each position equally often (ABBA). One line per run into a fresh results file per call,
# $WORK/results/<tag>-<date>-<pid>.txt, then the comparison (scripts/perf-compare.py): per
# variant the median, the per-round paired difference to the reference, "slower n/N", and the
# control's difference as the noise floor. PERFORMANCE.md has the method and its pitfalls.
#
#   ENGINE=<Wine engine root> WORK=<scratch dir> scripts/perf-abba.sh [options] <bench> [bench args]
#     --variant <name>=<d3d9.dll>   repeat, 2 or more, unique names; the first is the reference
#                                   unless --ref
#     --native                      the variants are native executables (<name>=<program>) run
#                                   directly with the bench args, without Wine (ENGINE not
#                                   needed); e.g. one state_cache_test per tree: --native ... -- bench 50
#     --env <name>:<VAR>=<value>    extra environment for one variant (repeat), e.g. the same DLL
#                                   with a switch off
#     --rounds <n>                  (10)
#     --timeout <s>                 (300) per run
#     --tag <tag>                   (the bench name) the results file's name
#     --ref <name>                  the reference (default: the first variant)
#     --control <name>              a variant whose difference to the reference is noise only,
#                                   e.g. a byte copy of the reference DLL
#     --metric <key>                the RESULT field to compare (default: perf-compare.py's
#                                   first of avg_ms, ms/frame, frame_median_ms, median_ns);
#                                   e.g. p99 or sd for bench_fps_cap's pacing
#   <bench>: an exe in BENCH_DIR (default build/bench/x64, scripts/build-benches.sh) or a path.
#   RUN_WRAPPER="<cmd> [args]"      optional prefix for each run (e.g. CPU pinning).
#   To run the benches' windows on a virtual display, wrap the whole call, e.g.
#     <virtual-display tool> scripts/perf-abba.sh ...
#   and build them with BENCH_INCLUDE (scripts/build-benches.sh).
#
# A run counts only if the program exits 0 with a RESULT/BENCH line that isn't "RESULT FAIL";
# otherwise its line is RESULT FAIL, it's left out of the comparison, and the script exits 1.
# A run is marked DIRTY (also left out) when a wineserver other than this run's own is running:
# before the launch, with this prefix's server stopped, any wineserver; during the run, more than
# one (none at all in --native mode). DIRTY_CHECK=0 turns that off, for functional smoke runs on a
# busy machine (not for measurements). Interrupted (INT/TERM), the script stops its own run (and,
# under Wine, only this WORK prefix's server) before it exits.
#
# Nothing outside WORK is written; the DLLs, the bench and the engine are only read and executed.
# Each variant gets its own AIR cache and PSO archives (warm after its first run); the state
# cache is off. Benchmarks belong on a quiet machine: no other GPU or CPU load, and the display
# asleep unless the bench needs it awake.
set -uo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
die() { echo "perf-abba: $*" >&2; exit 2; }
: "${WORK:?set WORK to a scratch dir}"

NAMES=() DLLS=() ENVS=() ROUNDS=10 TIMEOUT=300 TAG="" REF="" CONTROL="" METRIC="" NATIVE=0
while [ $# -gt 0 ]; do
  case "$1" in
    --variant) [ $# -ge 2 ] && [ "${2#*=}" != "$2" ] || die "--variant needs <name>=<file>"
               NAMES+=("${2%%=*}"); DLLS+=("${2#*=}"); shift 2 ;;
    --env) [ $# -ge 2 ] || die "--env needs <name>:<VAR>=<value>"; ENVS+=("$2"); shift 2 ;;
    --native) NATIVE=1; shift ;;
    --rounds) ROUNDS="${2:-}"; shift 2 ;;
    --timeout) TIMEOUT="${2:-}"; shift 2 ;;
    --tag) TAG="${2:-}"; shift 2 ;;
    --ref) REF="${2:-}"; shift 2 ;;
    --control) CONTROL="${2:-}"; shift 2 ;;
    --metric) METRIC="${2:-}"; shift 2 ;;
    --) shift; break ;;
    -*) die "unknown option $1" ;;
    *) break ;;
  esac
done

# Validate everything before anything is created.
[ ${#NAMES[@]} -ge 2 ] || die "need at least two --variant"
case "$ROUNDS" in ''|*[!0-9]*|0) die "--rounds must be a positive integer" ;; esac
case "$TIMEOUT" in ''|*[!0-9]*|0) die "--timeout must be a positive integer" ;; esac
has() { local x; for x in "${NAMES[@]}"; do [ "$x" = "$1" ] && return 0; done; return 1; }
for i in "${!NAMES[@]}"; do
  case "${NAMES[$i]}" in ''|*[!A-Za-z0-9._-]*) die "variant name '${NAMES[$i]}': letters, digits, . _ - only" ;; esac
  for j in "${!NAMES[@]}"; do
    [ "$i" -lt "$j" ] && [ "${NAMES[$i]}" = "${NAMES[$j]}" ] && die "variant name ${NAMES[$i]} given twice"
  done
  [ -f "${DLLS[$i]}" ] || die "no file ${DLLS[$i]}"
done
REF="${REF:-${NAMES[0]}}"
has "$REF" || die "--ref $REF is not a variant"
[ -z "$CONTROL" ] || has "$CONTROL" || die "--control $CONTROL is not a variant"
[ -z "$CONTROL" ] || [ "$CONTROL" != "$REF" ] || die "--control must differ from the reference"
for e in ${ENVS[@]+"${ENVS[@]}"}; do
  has "${e%%:*}" || die "--env $e: ${e%%:*} is not a variant"
  [ "${e#*:}" != "$e" ] && [ "${e#*:*=}" != "${e#*:}" ] || die "--env $e: expected <name>:<VAR>=<value>"
done

if [ "$NATIVE" = 1 ]; then
  WHAT="native"
  TAG="${TAG:-$(basename "${DLLS[0]}")}"
else
  : "${ENGINE:?set ENGINE to a Wine engine root (bin/wine, bin/wineserver)}"
  [ $# -ge 1 ] || die "usage: perf-abba.sh [options] <bench> [bench args] (see the header)"
  BENCH="$1"; shift
  case "$BENCH" in
    */*) EXE="$BENCH" ;;
    *) EXE="${BENCH_DIR:-$V2/build/bench/x64}/${BENCH%.exe}.exe" ;;
  esac
  [ -f "$EXE" ] || die "no bench $EXE (scripts/build-benches.sh)"
  WINE="$ENGINE/bin/wine" WINESERVER="$ENGINE/bin/wineserver"
  [ -x "$WINE" ] && [ -x "$WINESERVER" ] || die "no bin/wine or bin/wineserver under $ENGINE"
  NAME_EXE="$(basename "$EXE")"
  WHAT="$NAME_EXE"
  TAG="${TAG:-${NAME_EXE%.exe}}"
fi
case "$TAG" in ''|*[!A-Za-z0-9._-]*) die "tag '$TAG': letters, digits, . _ - only" ;; esac

A="$WORK/abba"
mkdir -p "$WORK/results" "$A/logs" || die "can't create $WORK/results or $A/logs"
OUT="$WORK/results/$TAG-$(date +%Y%m%d-%H%M%S)-$$.txt"
[ -e "$OUT" ] && die "$OUT exists"

if [ "$NATIVE" = 0 ]; then
  export WINEPREFIX="$WORK/prefix" WINEDEBUG=-all WINEMSYNC=1
  export DYLD_FALLBACK_LIBRARY_PATH="${FRAMEWORKS:+$FRAMEWORKS:}/usr/lib"
  if [ ! -f "$WINEPREFIX/system.reg" ]; then
    "$WINE" wineboot -i >"$WORK/wineboot.log" 2>&1 || die "wineboot failed ($WORK/wineboot.log)"
    "$WINESERVER" -w
  fi
  for i in "${!NAMES[@]}"; do
    d="$A/bin-${NAMES[$i]}"
    mkdir -p "$d" "$A/caches/${NAMES[$i]}/pso" || die "can't create $d"
    cp "${DLLS[$i]}" "$d/d3d9.dll" && cp "$EXE" "$d/" || die "can't copy into $d"
  done
fi

# Wine servers on this machine (any engine, any prefix).
servers() { [ "${DIRTY_CHECK:-1}" = 0 ] && { echo 0; return; }; ps -axo comm= | grep -cE '(^|/)wineserver$'; }
stop_own() { [ "$NATIVE" = 1 ] || { "$WINESERVER" -k >/dev/null 2>&1; "$WINESERVER" -w >/dev/null 2>&1; }; }

CHILD=""
cleanup() {
  if [ -n "$CHILD" ] && kill -0 "$CHILD" 2>/dev/null; then
    if [ "$NATIVE" = 1 ]; then
      kill -TERM "$CHILD" 2>/dev/null; sleep 1; kill -KILL "$CHILD" 2>/dev/null
    else
      "$WINESERVER" -k >/dev/null 2>&1   # this WORK prefix's server and its processes only
    fi
    wait "$CHILD" 2>/dev/null
  fi
  CHILD=""
  stop_own
}
trap 'cleanup; echo "perf-abba: interrupted" >&2; exit 130' INT
trap 'cleanup; echo "perf-abba: terminated" >&2; exit 143' TERM

echo "# $WHAT $* rounds=$ROUNDS variants=${NAMES[*]} ref=$REF${CONTROL:+ control=$CONTROL} $(date '+%F %T')" > "$OUT" || die "can't write $OUT"
for i in "${!NAMES[@]}"; do echo "#   ${NAMES[$i]} = ${DLLS[$i]} ($(shasum -a 256 "${DLLS[$i]}" | cut -c1-16))" >> "$OUT"; done
echo "perf-abba: results in $OUT"
OWN=$([ "$NATIVE" = 1 ] && echo 0 || echo 1)
n=${#NAMES[@]} fails=0
for r in $(seq "$ROUNDS"); do
  for k in $(seq 0 $((n - 1))); do
    i=$(( (k + r) % n )); v=${NAMES[$i]}
    log="$A/logs/$TAG-$$-$v-$r.err"
    load=$(sysctl -n vm.loadavg 2>/dev/null | awk '{print $2}')
    stop_own
    dirty=""; [ "$(servers)" -gt 0 ] && dirty=1   # before the launch: nothing of ours runs
    (
      for e in ${ENVS[@]+"${ENVS[@]}"}; do [ "${e%%:*}" = "$v" ] && export "${e#*:}"; done
      if [ "$NATIVE" = 1 ]; then
        exec ${RUN_WRAPPER:-} "${DLLS[$i]}" "$@"
      fi
      export WINEDLLOVERRIDES="d3d9=n,b"
      export D9MT_AIR_CACHE="$A/caches/$v/air-cache.sqlite3" D9MT_PSO_ARCHIVE_DIR="$A/caches/$v/pso"
      export D9MT_STATE_CACHE=0 D9MT_BG_FPS=0 D9MT_STALL_PACE=0
      cd "$A/bin-$v" || exit 125
      exec ${RUN_WRAPPER:-} "$WINE" "$NAME_EXE" "$@"
    ) > "$A/out" 2> "$log" &
    CHILD=$! t=0
    while kill -0 "$CHILD" 2>/dev/null && [ "$t" -lt "$TIMEOUT" ]; do
      sleep 1; t=$((t + 1))
      [ -z "$dirty" ] && [ "$(servers)" -gt "$OWN" ] && dirty=1   # during it: more than its own
    done
    if kill -0 "$CHILD" 2>/dev/null; then
      cleanup
      line="RESULT FAIL timeout ${TIMEOUT}s"
    else
      wait "$CHILD"; rc=$?; CHILD=""
      line=$(tr -d '\r' < "$A/out" | grep -E '^(RESULT|BENCH)' | head -1)
      if [ "$rc" -ne 0 ]; then
        line="RESULT FAIL rc=$rc${line:+ ($line)}"
      elif [ -z "$line" ]; then
        line="RESULT FAIL rc=0, no RESULT line"
      fi
    fi
    # A wait on a serial still unsubmitted after a whole 2 s slice: a hang hazard, not a time.
    grep -q 'not submitted yet' "$log" 2>/dev/null && line="RESULT FAIL unsubmitted-wait"
    stop_own
    case "$line" in "RESULT FAIL"*) fails=$((fails + 1)) ;; esac
    echo "${dirty:+DIRTY }round=$r v=$v load=$load $line" | tee -a "$OUT"
  done
done
trap - INT TERM

python3 "$V2/scripts/perf-compare.py" --ref "$REF" ${CONTROL:+--control "$CONTROL"} ${METRIC:+--metric "$METRIC"} "$OUT"
cmp=$?
[ "$fails" -eq 0 ] || { echo "perf-abba: $fails failed run(s), see $OUT and $A/logs" >&2; exit 1; }
exit "$cmp"
