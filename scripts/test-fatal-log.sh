#!/usr/bin/env bash
# test-fatal-log.sh — the fatal-exception log's test (test/native/fatal_log_test.cpp, with
# test/native/fatal_log_dll.cpp): builds both with mingw (x64) and runs the test under Wine:
#   1. the checks with a capture sink (real divide by zero and access violation, C++ throws, thread
#      naming and a raised stack overflow not logged, a nested fault, a fault in a DLL loaded
#      after install also with its headers unreadable, a fault outside every image, a fault
#      while every heap is locked, the cap, the thread's last error kept);
#   2. --wine-output: the line reaches Wine's debug output, i.e. the game's log (under the heap
#      lock too);
#   3. --unhandled: a program that catches its own crash ends with Unix exit status 148
#      (0xC0000094's low byte), with the line in its log and no "starting debugger" line;
#   4. --stack-overflow: a real stack overflow ends the process with no D9MT-FATAL line;
#   5. --tls-exhausted: with every TLS slot taken, the log still installs and logs;
#   6. --dll-unload: the log installed from a DLL that is then freed: pinned, still logs;
#   7. D9MT_FATAL_LOG=0: install() returns false.
#
#   ENGINE=<Wine engine root, bin/wine inside> WORK=<durable dir> scripts/test-fatal-log.sh
#
# Wine runs in $WORK/prefix (made on first use) and its wineserver is stopped at the end.
# Writes only under $WORK. Exit status 1 if any check fails.
set -uo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
: "${ENGINE:?set ENGINE to a Wine engine root (bin/wine, bin/wineserver)}"
: "${WORK:?set WORK to a durable directory for the build, the prefix and the logs}"
mkdir -p "$WORK"
WORK="$(cd "$WORK" && pwd)"
CXX="${CXX:-x86_64-w64-mingw32-g++}"
WINE="$ENGINE/bin/wine" WINESERVER="$ENGINE/bin/wineserver"
export WINEPREFIX="$WORK/prefix" WINEDEBUG=-all WINEMSYNC=1

finish() {
  "$WINESERVER" -k >/dev/null 2>&1
  "$WINESERVER" -w >/dev/null 2>&1
}
trap finish EXIT

EXE="$WORK/fatal_log_test.exe" DLL="$WORK/fatal_log_dll.dll"
if ! { "$CXX" -std=c++17 -O1 -Wall -Wextra -static \
         "$V2/test/native/fatal_log_test.cpp" "$V2/driver/util/util_fatal_log.cpp" -o "$EXE" \
    && "$CXX" -std=c++17 -O1 -Wall -Wextra -static -shared \
         "$V2/test/native/fatal_log_dll.cpp" "$V2/driver/util/util_fatal_log.cpp" -o "$DLL"; } 2>"$WORK/build.log"; then
  echo "fatal_log_test: FAIL (build, $WORK/build.log)"; cat "$WORK/build.log"; exit 1
fi
[ -s "$WORK/build.log" ] && { echo "fatal_log_test: build warnings:"; cat "$WORK/build.log"; }

if [ ! -f "$WINEPREFIX/system.reg" ]; then
  echo "[fatal-log] new prefix in $WINEPREFIX"
  "$WINE" wineboot -i >"$WORK/wineboot.log" 2>&1
  "$WINESERVER" -w
fi
# No crash dialog in the test prefix
"$WINE" reg add 'HKCU\Software\Wine\WineDbg' /v ShowCrashDialog /t REG_DWORD /d 0 /f >>"$WORK/wineboot.log" 2>&1
"$WINESERVER" -w

fails=0
: >"$WORK/status.log"
run() { # <name> <expected status, or "nonzero"> <args...>
  local name=$1 want=$2; shift 2
  local log="$WORK/$name.log"
  "$WINE" "$EXE" "$@" >"$log" 2>&1
  local rc=$?
  "$WINESERVER" -w >/dev/null 2>&1
  echo "$name: exit $rc" >>"$WORK/status.log"
  if [ "$want" = nonzero ] && [ "$rc" != 0 ]; then
    return 0
  elif [ "$rc" != "$want" ]; then
    echo "$name: FAIL (exit $rc, expected $want; $log)"; tail -20 "$log"; fails=$((fails + 1)); return 1
  fi
  return 0
}

if run checks 0; then
  grep -q '^fatal_log_test: ok' "$WORK/checks.log" && echo "checks: ok" \
    || { echo "checks: FAIL ($WORK/checks.log)"; cat "$WORK/checks.log"; fails=$((fails + 1)); }
fi

if run wine-output 0 --wine-output; then
  if grep -q 'D9MT-FATAL: c0000094 integer divide by zero at 0x.*(fatal_log_test.exe+0x.*last d9mt step: test step: wine output;' "$WORK/wine-output.log"; then
    echo "wine-output: ok"
  else
    echo "wine-output: FAIL (no line in Wine's debug output, $WORK/wine-output.log)"; cat "$WORK/wine-output.log"; fails=$((fails + 1))
  fi
fi

if run unhandled 148 --unhandled; then
  if grep -q 'D9MT-FATAL: c0000094 integer divide by zero.*last d9mt step: test step: unhandled divide;' "$WORK/unhandled.log" \
     && ! grep -q 'starting debugger' "$WORK/unhandled.log"; then
    echo "unhandled: ok (exit 148, the line, no debugger)"
  else
    echo "unhandled: FAIL ($WORK/unhandled.log)"; cat "$WORK/unhandled.log"; fails=$((fails + 1))
  fi
fi

if run stack-overflow nonzero --stack-overflow; then
  if grep -q 'recursing' "$WORK/stack-overflow.log" && ! grep -q 'D9MT-FATAL' "$WORK/stack-overflow.log" \
     && ! grep -q 'still running' "$WORK/stack-overflow.log"; then
    echo "stack-overflow: ok (ended, no line; $(tail -1 "$WORK/status.log"))"
  else
    echo "stack-overflow: FAIL ($WORK/stack-overflow.log)"; cat "$WORK/stack-overflow.log"; fails=$((fails + 1))
  fi
fi

if run tls-exhausted 0 --tls-exhausted; then
  grep -q '^fatal_log_test --tls-exhausted: ok' "$WORK/tls-exhausted.log" && echo "tls-exhausted: ok" \
    || { echo "tls-exhausted: FAIL ($WORK/tls-exhausted.log)"; cat "$WORK/tls-exhausted.log"; fails=$((fails + 1)); }
fi

if run dll-unload 0 --dll-unload; then
  grep -q '^fatal_log_test --dll-unload: ok' "$WORK/dll-unload.log" && echo "dll-unload: ok" \
    || { echo "dll-unload: FAIL ($WORK/dll-unload.log)"; cat "$WORK/dll-unload.log"; fails=$((fails + 1)); }
fi

if D9MT_FATAL_LOG=0 run off 0 --expect-off; then
  echo "off: ok"
fi

[ "$fails" = 0 ] && echo "[fatal-log] PASS" || echo "[fatal-log] FAIL ($fails)"
[ "$fails" = 0 ]
