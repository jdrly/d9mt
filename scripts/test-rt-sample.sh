#!/usr/bin/env bash
# test-rt-sample.sh — the render-thread sampler's test under Wine (test/native/rt_sample_wine_test.cpp
# with test/native/rt_sample_dll.cpp and driver/util/util_rt_sample.cpp): builds them with mingw
# (x64) and runs each mode in a fresh process.
#
#   ENGINE=<Wine engine root, bin/wine inside> WORK=<durable dir> scripts/test-rt-sample.sh
#       the checks: ratio, swapped (its control), caller, caller-exe (its control), wait, syscall,
#       fault, fault-early
#   ENGINE=... WORK=... scripts/test-rt-sample.sh storm <runs> <hz> <faults per s> <s> <guard 0|1>
#       the measurement behind the fault guard: <runs> runs of a thread that faults at the rate
#       while it is sampled at <hz>; counts the runs in which Wine ended the thread. Not a check:
#       the outcome is a chance, and the CPU and Rosetta decide it. Exit 0 whatever it counts.
#
# The checks need no GPU and no window. The faults are part of the test: on Wine + Rosetta a
# thread suspended while Wine handles its fault can die, which is what the guard keeps the sampler
# away from, and the fault check can (rarely) hit the case the guard can't avoid: a fault while the
# one suspend before the stop is in flight. Run on an Apple Silicon Mac; Wine's own debug output
# (WINEDEBUG=err+virtual) names a thread it ended ("nested exception on signal stack").
#
# Wine runs in $WORK/prefix (made on first use) and its wineserver is stopped at the end.
# Writes only under $WORK. Exit status 1 if a check fails.
set -uo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
: "${ENGINE:?set ENGINE to a Wine engine root (bin/wine, bin/wineserver)}"
: "${WORK:?set WORK to a durable directory for the build, the prefix and the logs}"
mkdir -p "$WORK"
WORK="$(cd "$WORK" && pwd)"
CXX="${CXX:-x86_64-w64-mingw32-g++}"
WINE="$ENGINE/bin/wine" WINESERVER="$ENGINE/bin/wineserver"
export WINEPREFIX="$WORK/prefix" WINEDEBUG=err+virtual WINEMSYNC=1

finish() {
  "$WINESERVER" -k >/dev/null 2>&1
  "$WINESERVER" -w >/dev/null 2>&1
}
trap finish EXIT

EXE="$WORK/rt_sample_wine_test.exe" DLL="$WORK/rt_sample_dll.dll"
if ! { "$CXX" -std=c++17 -O1 -Wall -Wextra -static -shared \
         "$V2/test/native/rt_sample_dll.cpp" -o "$DLL" -Wl,--out-implib,"$WORK/librt_sample_dll.a" \
    && "$CXX" -std=c++17 -O1 -Wall -Wextra -static \
         "$V2/test/native/rt_sample_wine_test.cpp" "$V2/driver/util/util_rt_sample.cpp" \
         "$WORK/librt_sample_dll.a" -o "$EXE"; } 2>"$WORK/build.log"; then
  echo "rt_sample_wine_test: FAIL (build, $WORK/build.log)"; cat "$WORK/build.log"; exit 1
fi
[ -s "$WORK/build.log" ] && { echo "rt_sample_wine_test: build warnings:"; cat "$WORK/build.log"; }

if [ ! -f "$WINEPREFIX/system.reg" ]; then
  echo "[rt-sample] new prefix in $WINEPREFIX"
  "$WINE" wineboot -i >"$WORK/wineboot.log" 2>&1
  "$WINESERVER" -w
fi
# No crash dialog in the test prefix
"$WINE" reg add 'HKCU\Software\Wine\WineDbg' /v ShowCrashDialog /t REG_DWORD /d 0 /f >>"$WORK/wineboot.log" 2>&1
"$WINESERVER" -w

# one <log> <args...>: one run of the program, its exit status in $rc
one() {
  local log=$1; shift
  rc=0
  "$WINE" "$EXE" "$@" >"$log" 2>&1 || rc=$?
  "$WINESERVER" -w >/dev/null 2>&1
}

if [ "${1:-}" = storm ]; then
  [ $# = 6 ] || { sed -n '8,11p' "$0" >&2; exit 64; }
  runs=$2; shift 2
  died=0 hung=0 other=0 nested=0
  for i in $(seq 1 "$runs"); do
    log="$WORK/storm-$i.log"
    one "$log" storm "$@"
    n=$(grep -c 'nested exception on signal stack' "$log")
    nested=$((nested + n))
    # the program's own verdict: a thread that ended without finishing died; one still running
    # at the limit hung; any other non-zero exit (a crash, a launch failure) is neither
    if grep -q '^rt_sample_wine_test storm: .*thread DIED' "$log"; then died=$((died + 1))
    elif grep -q '^rt_sample_wine_test storm: .*thread still running' "$log"; then hung=$((hung + 1))
    elif [ "$rc" != 0 ]; then other=$((other + 1))
    fi
    echo "storm $i/$runs: exit $rc, nested-exception lines $n: $(grep '^rt_sample_wine_test storm:' "$log" | head -1)"
  done
  echo "[rt-sample] storm $* : $died of $runs runs lost the thread, $hung hung, $other ended otherwise ($nested nested-exception lines)"
  exit 0
fi

fails=0
for mode in ratio swapped caller caller-exe wait syscall fault fault-early; do
  log="$WORK/$mode.log"
  one "$log" "$mode"
  if [ "$rc" = 0 ] && grep -q "^rt_sample_wine_test $mode: ok" "$log"; then
    echo "$mode: ok ($(grep "^rt_sample_wine_test $mode: [0-9]" "$log" | head -1 | sed "s/^rt_sample_wine_test $mode: //"))"
  else
    echo "$mode: FAIL (exit $rc, $log)"; tail -20 "$log"; fails=$((fails + 1))
  fi
done

[ "$fails" = 0 ] && echo "[rt-sample] PASS" || echo "[rt-sample] FAIL ($fails)"
[ "$fails" = 0 ]
