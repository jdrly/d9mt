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
# A bench's `CLIENT <bench> <W>x<H>` line (test/perf/bench_window.h) becomes client=<W>x<H> on the
# run's line, client=<W>x<H>!=<back buffer> when the client area isn't the back buffer's size:
# then a visible window's presents need the presenter's scaling blit (perf-compare.py warns).
# visible=: the window's visibility over the timed interval, from the bridge's `D9MT-WSI ...
# visibility A -> B` lines (it starts at visible and logs every change, but nothing when it
# starts watching) and the bench's `TIMED <bench> <HH:MM:SS.mmm>` line (the timed start, the
# same wall clock):
#   0         occluded from before the timed start on
#   1>0, 0>1  the states in order when it changed (1> first: the first change came at or after
#             the timed start, so it began visible)
#   nolog     no change logged: visible throughout if the bench presented (a bench that never
#             presents, like bench_draws flush, never has its visibility read)
#   ?, ?>0    not established: lines may have been dropped (30 or more, or a suppression mark),
#             the clocks disagree by more than 10 min, or (?> first) the bench printed no TIMED
# An occluded window's presents are skipped, so perf-compare.py warns about runs that changed or
# are unknown, and about occluded runs mixed with others.
# A run is marked DIRTY (also left out) when a wineserver other than this run's own is running:
# before the launch, with this prefix's server stopped, any wineserver; during the run, more than
# one (none at all in --native mode). DIRTY_CHECK=0 turns that off, for functional smoke runs on a
# busy machine (not for measurements). Interrupted (INT/TERM), the script stops its own run (and,
# under Wine, only this WORK prefix's server) before it exits.
#
# Nothing outside WORK is written; the DLLs, the bench and the engine are only read and executed.
# Every variant runs from the same directory ($WORK/abba/run, WORK made absolute): its d3d9.dll
# (with --native, its program, as run/native-program) is copied in right before each of its runs,
# so the process's working directory and path don't depend on the variant's name. Under Wine, each
# variant gets its own AIR cache and PSO archives (warm after its first run) under caches/<8 hex
# digits of its name's sha256>, a fixed length, so the environment doesn't either; the state cache
# is off. --native sets no cache variables (only the --env ones).
# scripts/test-perf-abba.sh checks this without Wine. Benchmarks belong on a quiet machine: no other GPU or CPU load, and the display
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
    --rounds|--timeout|--tag|--ref|--control|--metric)
      [ $# -ge 2 ] || die "$1 needs a value"
      case "$1" in
        --rounds) ROUNDS="$2" ;; --timeout) TIMEOUT="$2" ;; --tag) TAG="$2" ;;
        --ref) REF="$2" ;; --control) CONTROL="$2" ;; --metric) METRIC="$2" ;;
      esac
      shift 2 ;;
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

mkdir -p "$WORK" || die "can't create $WORK"
WORK="$(cd "$WORK" && pwd)" || die "can't enter $WORK"   # absolute: the runs cd into $RUN
A="$WORK/abba"
mkdir -p "$WORK/results" "$A/logs" || die "can't create $WORK/results or $A/logs"
OUT="$WORK/results/$TAG-$(date +%Y%m%d-%H%M%S)-$$.txt"
[ -e "$OUT" ] && die "$OUT exists"

if [ "$NATIVE" = 0 ]; then
  export WINEPREFIX="$WORK/prefix" WINEDEBUG=-all WINEMSYNC=1
  export DYLD_FALLBACK_LIBRARY_PATH="${FRAMEWORKS:+$FRAMEWORKS:}/usr/lib"
fi

# Wine servers on this machine (any engine, any prefix).
servers() { [ "${DIRTY_CHECK:-1}" = 0 ] && { echo 0; return; }; ps -axo comm= | grep -cE '(^|/)wineserver$'; }
# This run's own server only: WINEPREFIX is bound on every call, so a signal that arrives before
# (or after) anything else can never stop another prefix's server.
stop_own() {
  [ "$NATIVE" = 1 ] && return 0
  WINEPREFIX="$WORK/prefix" "$WINESERVER" -k >/dev/null 2>&1
  WINEPREFIX="$WORK/prefix" "$WINESERVER" -w >/dev/null 2>&1
}

# Cleanup on INT/TERM, installed before the first launch (wineboot included): the owned child and
# every process below it (a RUN_WRAPPER may fork the program), then, under Wine, only this WORK
# prefix's server. Children run in the background and are waited for, so a signal is handled at once.
CHILD=""
subtree() { local c; echo "$1"; for c in $(pgrep -P "$1" 2>/dev/null); do subtree "$c"; done; }
cleanup() {
  if [ -n "$CHILD" ] && kill -0 "$CHILD" 2>/dev/null; then
    local pids; pids=$(subtree "$CHILD")
    kill -TERM $pids 2>/dev/null; sleep 1; kill -KILL $pids 2>/dev/null
    wait "$CHILD" 2>/dev/null
  fi
  CHILD=""
  stop_own
}
trap 'cleanup; echo "perf-abba: interrupted" >&2; exit 130' INT
trap 'cleanup; echo "perf-abba: terminated" >&2; exit 143' TERM

if [ "$NATIVE" = 0 ]; then
  if [ ! -f "$WINEPREFIX/system.reg" ]; then
    "$WINE" wineboot -i >"$WORK/wineboot.log" 2>&1 & CHILD=$!
    wait "$CHILD" || die "wineboot failed ($WORK/wineboot.log)"
    CHILD=""
    "$WINESERVER" -w
  fi
fi
# Every variant runs from one directory, its DLL (or native program) copied in right before its run,
# and its caches live under a fixed-length id: a process's speed can depend on the length of its
# path and environment (#65: a byte-identical copy ran 6 % faster under a name one character longer).
RUN="$A/run"
mkdir -p "$RUN" || die "can't create $RUN"
[ "$NATIVE" = 1 ] || cp "$EXE" "$RUN/" || die "can't copy $EXE into $RUN"
cid() { printf '%s' "$1" | shasum -a 256 | cut -c1-8; }
CIDS=()
if [ "$NATIVE" = 0 ]; then
  for i in "${!NAMES[@]}"; do
    c=$(cid "${NAMES[$i]}")
    for o in ${CIDS[@]+"${CIDS[@]}"}; do [ "$o" != "$c" ] || die "variants ${NAMES[$i]} and another share cache id $c: rename one"; done
    CIDS+=("$c")
    mkdir -p "$A/caches/$c/pso" || die "can't create $A/caches/$c"
  done
fi
# put <variant index>'s binary into the run directory (a temporary name, then a rename)
place() {
  local i=$1
  if [ "$NATIVE" = 1 ]; then
    cp -p "${DLLS[$i]}" "$RUN/.prog.tmp" && mv -f "$RUN/.prog.tmp" "$RUN/native-program"
  else
    cp "${DLLS[$i]}" "$RUN/.d3d9.tmp" && mv -f "$RUN/.d3d9.tmp" "$RUN/d3d9.dll"
  fi
}

echo "# $WHAT $* rounds=$ROUNDS variants=${NAMES[*]} ref=$REF${CONTROL:+ control=$CONTROL} $(date '+%F %T')" > "$OUT" || die "can't write $OUT"
for i in "${!NAMES[@]}"; do echo "#   ${NAMES[$i]} = ${DLLS[$i]} ($(shasum -a 256 "${DLLS[$i]}" | cut -c1-16))${CIDS[$i]+ cache=${CIDS[$i]}}" >> "$OUT"; done
echo "#   run directory: $RUN (every variant)" >> "$OUT"
echo "perf-abba: results in $OUT"
OWN=$([ "$NATIVE" = 1 ] && echo 0 || echo 1)
n=${#NAMES[@]} fails=0
for r in $(seq "$ROUNDS"); do
  for k in $(seq 0 $((n - 1))); do
    i=$(( (k + r) % n )); v=${NAMES[$i]}
    log="$A/logs/$TAG-$$-$v-$r.err"
    load=$(sysctl -n vm.loadavg 2>/dev/null | awk '{print $2}')
    stop_own
    place "$i" || die "can't put ${NAMES[$i]}'s binary into $RUN"
    dirty=""; [ "$(servers)" -gt 0 ] && dirty=1   # before the launch: nothing of ours runs
    (
      for e in ${ENVS[@]+"${ENVS[@]}"}; do [ "${e%%:*}" = "$v" ] && export "${e#*:}"; done
      cd "$RUN" || exit 125
      if [ "$NATIVE" = 1 ]; then
        exec ${RUN_WRAPPER:-} "$RUN/native-program" "$@"
      fi
      export WINEDLLOVERRIDES="d3d9=n,b"
      export D9MT_AIR_CACHE="$A/caches/${CIDS[$i]}/air-cache.sqlite3" D9MT_PSO_ARCHIVE_DIR="$A/caches/${CIDS[$i]}/pso"
      export D9MT_STATE_CACHE=0 D9MT_BG_FPS=0 D9MT_STALL_PACE=0
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
    client=$(tr -d '\r' < "$A/out" 2>/dev/null | awk '$1 == "CLIENT" { c = $3
      if ($4 == "MISMATCH") { b = $5; sub(/^backbuffer=/, "", b); sub(/:$/, "", b); c = c "!=" b }
      print c; exit }')
    visible=""
    if [ "$NATIVE" = 0 ] && grep -q 'MtlPresenter' "$log" 2>/dev/null; then
      timed=$(tr -d '\r' < "$A/out" 2>/dev/null | awk '$1 == "TIMED" { print $3; exit }')
      visible=$(awk -v T="$timed" '
        function ms(t, a) { split(t, a, ":"); return (a[1] * 3600 + a[2] * 60 + a[3]) * 1000 }
        /^D9MT-WSI .* visibility 0x[0-9a-f]+ -> 0x[0-9a-f]+ on / {
          n++; if (/earlier lines suppressed/) lost = 1
          if (n == 1) first = ms($2)
          for (i = 1; i < NF; i++) if ($i == "->") s = ($(i + 1) == "0x3") ? "1" : "0"
          if (seq == "") seq = s; else if (s != last) seq = seq ">" s
          last = s }
        END {
          if (lost || n >= 30) { print "?"; exit }
          if (n == 0) { print "nolog"; exit }
          if (T == "") { print "?>" seq; exit }
          d = first - ms(T)
          if (d > 600000 || d < -600000) { print "?"; exit }
          print (d >= 0 ? "1>" : "") seq }' "$log")
    fi
    # A wait on a serial still unsubmitted after a whole 2 s slice: a hang hazard, not a time.
    grep -q 'not submitted yet' "$log" 2>/dev/null && line="RESULT FAIL unsubmitted-wait"
    stop_own
    case "$line" in "RESULT FAIL"*) fails=$((fails + 1)) ;; esac
    echo "${dirty:+DIRTY }round=$r v=$v load=$load${client:+ client=$client}${visible:+ visible=$visible} $line" | tee -a "$OUT"
  done
done
trap - INT TERM

python3 "$V2/scripts/perf-compare.py" --ref "$REF" ${CONTROL:+--control "$CONTROL"} ${METRIC:+--metric "$METRIC"} "$OUT"
cmp=$?
[ "$fails" -eq 0 ] || { echo "perf-abba: $fails failed run(s), see $OUT and $A/logs" >&2; exit 1; }
exit "$cmp"
