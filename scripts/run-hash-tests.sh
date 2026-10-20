#!/usr/bin/env bash
# Run d9mt's x64 test programs on a Wine engine and print the hash of each one's
# last backbuffer (test/harness/d9h.h) and its own checks: the regression suite
# for a d3d9.dll (compare two builds' results.txt), and the A/B check between
# engines. From arm64ec's scripts/run-hash-tests.sh, x86_64 Wine only.
#
#   ENGINE=<wine root> [VD=1] scripts/run-hash-tests.sh [code_xxx...]
#
# ENGINE   an x86_64 Wine install (lib/wine/x86_64-unix, Rosetta)
# DLL      the d3d9.dll next to the programs (default build/driver-x64/d3d9.dll)
# BRIDGE   build (default when build/bridge/winemetal.so exists): this tree's
#          bridge is installed into the engine clone; engine: keep the engine's
#          own winemetal (a d3d9.dll-only change against the shipping bridge)
# VD=1     every window on a temporary virtual display (tools/vd of the
#          swtor-mac-proper workspace, VD_DIR to override), none on the monitor
#
# Without arguments every test/code_*.c runs except code_sm3_ref (the harness of
# scripts/sm3_ref_test.py) and code_statecache (scripts/run-statecache-bench.sh).
# test/expected-failures.txt lists tests that fail for a known, open reason
# (one per line: name, then the issue): they report XFAIL when they fail and
# XPASS when they pass. The exit status is 1 if any other test fails or a test
# listed there passes, so the list stays current.
#
# Everything happens in a throwaway work dir (WORK, default /tmp/d9mt-hash):
# an APFS clone of the engine, a fresh prefix (the Mac driver off while it is
# created: no window), d9mt's caches. Nothing outside it is written. Before
# each Wine run it waits while SWTOR runs. One line per test (frames, hash of
# the last frame, center pixel, its checks, wall time, status) to stdout and
# $WORK/results.txt; leftover Wine processes of the clone fail the run.
set -euo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
VD_DIR="${VD_DIR:-$HOME/Development/personal/swtor-mac-proper/tools/vd}"
if [ "${VD:-0}" = 1 ] && [ "${VD_ACTIVE:-0}" != 1 ]; then
  exec "$VD_DIR/with-vd.sh" "${VD_SECONDS:-3600}" env VD_ACTIVE=1 bash "$0" "$@"
fi
HARNESS=(-include "$V2/test/harness/d9h.h")
[ "${VD:-0}" = 1 ] && HARNESS=(-include "$VD_DIR/d9h-vd.h" -I "$V2/test/harness")
SRC_ENGINE="${ENGINE:?set ENGINE to a Wine root}"
[ -d "$SRC_ENGINE/lib/wine/x86_64-unix" ] || { echo "FATAL: $SRC_ENGINE is no x86_64 Wine install" >&2; exit 1; }
WORK="${WORK:-/tmp/d9mt-hash}"
ENG="$WORK/engine"
DLL="${DLL:-$V2/build/driver-x64/d3d9.dll}"
[ -f "$DLL" ] || { echo "FATAL: $DLL missing (ARCH=x64 scripts/build-driver.sh)" >&2; exit 1; }
BRIDGE="${BRIDGE:-$([ -f "$V2/build/bridge/winemetal.so" ] && echo build || echo engine)}"

TESTS=("$@")
if [ ${#TESTS[@]} -eq 0 ]; then
  for f in "$V2"/test/code_*.c; do
    t="$(basename "$f" .c)"
    case "$t" in code_sm3_ref|code_statecache) continue ;; esac
    TESTS+=("$t")
  done
fi
XFAIL_LIST="$V2/test/expected-failures.txt"
xfail_reason() { [ -f "$XFAIL_LIST" ] && sed -n "s/^$1[[:space:]]\{1,\}//p" "$XFAIL_LIST" | head -1; }

# A real SWTOR or launcher process, not a shell whose command line mentions one.
swtor_running() {
  ps -Ao command= | awk '
    /\/retailclient\/swtor\.exe/ && !/awk/ { f = 1 }
    /Star Wars - The Old Republic Launcher\.app\/|\/Contents\/MacOS\/Star Wars - The Old Republic Launcher$/ && !/awk/ { f = 1 }
    /winetemp-[^ ]*\/swtor\.exe/ && !/awk/ { f = 1 }
    END { exit !f }'
}
wait_for_game() {
  while swtor_running; do echo "[hash] SWTOR is running, waiting" >&2; sleep 30; done
}

echo "[hash] engine $SRC_ENGINE, dll $DLL, bridge $BRIDGE, work $WORK" >&2
rm -rf "$WORK"
mkdir -p "$WORK/bin" "$WORK/logs"
cp -cR "$SRC_ENGINE" "$ENG"
if [ "$BRIDGE" = build ]; then
  cp "$V2/build/bridge/winemetal64.dll" "$ENG/lib/wine/x86_64-windows/winemetal.dll"
  cp "$V2/build/bridge/winemetal.so" "$ENG/lib/wine/x86_64-unix/winemetal.so"
fi

for t in "${TESTS[@]}"; do
  x86_64-w64-mingw32-gcc -O1 "${HARNESS[@]}" -I "$V2/test" "$V2/test/$t.c" \
    -o "$WORK/bin/$t.exe" -ld3d9 -lgdi32 -luser32 2> "$WORK/logs/$t.build.log" \
    || { echo "FATAL: $t does not build, see $WORK/logs/$t.build.log" >&2; exit 1; }
done
cp "$DLL" "$WORK/bin/d3d9.dll"
# Wine rewrites argv[0] to the Windows path: find leftovers by real executable
printf '#include <libproc.h>\n#include <stdio.h>\n#include <stdlib.h>\nint main(int c,char**v){char p[PROC_PIDPATHINFO_MAXSIZE];for(int i=1;i<c;i++)if(proc_pidpath(atoi(v[i]),p,sizeof p)>0)printf("%%s %%s\\n",v[i],p);return 0;}\n' > "$WORK/pidpath.c"
clang -O1 "$WORK/pidpath.c" -o "$WORK/pidpath"

export WINEPREFIX="$WORK/prefix" WINEDEBUG="${WINEDEBUG:--all}" WINEMSYNC="${WINEMSYNC:-1}"
export WINEDLLOVERRIDES="mscoree,mshtml=;d3d9=n,b"
export NEUTRON_MSYNC="${NEUTRON_MSYNC:-0}"
export D9MT_AIR_CACHE="$WORK/air-cache.sqlite3" D9MT_PSO_ARCHIVE_DIR="$WORK/pso" D9MT_STATE_CACHE=0
unset WINELOADER WINESERVER WINEDLLPATH
# CrossOver 26-style engines carry compatdb: keep its DXMT tree from shadowing our bridge
[ -f "$ENG/lib/wine/x86_64-unix/compatdb.so" ] && export WINE_COMPATDB='v=5
name=d9mt-tests;exe=*;dxgi=wined3d'
# Without FreeType the system font metrics are garbage and every window lands off screen.
[ -d "$ENG/lib/external" ] && export DYLD_FALLBACK_LIBRARY_PATH="$ENG/lib/external:/usr/lib"
# Wine runs go through test/harness/exc_guard.c: a timeout, and a crash of any process of
# the run is logged to $WORK/logs/<name>.exc instead of becoming a crash report or dialog.
xcrun mig -DMACH_EXC_SERVER_TASKIDTOKEN -sheader "$WORK/mach_excServer.h" -server "$WORK/mach_excServer.c" -user /dev/null -header /dev/null \
  "$(xcrun --show-sdk-path)/usr/include/mach/mach_exc.defs"
clang -O1 -I "$WORK" "$V2/test/harness/exc_guard.c" "$WORK/mach_excServer.c" -o "$WORK/exc_guard"
WINE="$ENG/bin/wine"
run() { # <timeout s> <log name> <args...>: wine with a timeout, crashes logged
  local t=$1 name=$2; shift 2
  "$WORK/exc_guard" "$t" "$WORK/logs/$name.exc" "$WINE" "$@"
}

wait_for_game
echo "[hash] new prefix" >&2
WINEDLLOVERRIDES="$WINEDLLOVERRIDES;winemac.drv=d" run 300 wineboot wineboot -i > "$WORK/logs/wineboot.log" 2>&1
"$ENG/bin/wineserver" -k 2>/dev/null || true
"$ENG/bin/wineserver" -w 2>/dev/null || true
now() { perl -MTime::HiRes=time -e 'printf "%.0f", time * 1000'; }

: > "$WORK/results.txt"
unexpected=0
for t in "${TESTS[@]}"; do
  wait_for_game
  t0=$(now)
  rc=0
  ( cd "$WORK/bin" && run 60 "$t" "$WORK/bin/$t.exe" > "$WORK/logs/$t.out" 2> "$WORK/logs/$t.err" ) || rc=$?
  t1=$(now)
  tr -d '\r' < "$WORK/logs/$t.out" > "$WORK/logs/$t.txt"
  last="$(grep '^D9H frame=' "$WORK/logs/$t.txt" | tail -1 || true)"
  frames="$(echo "$last" | sed -n 's/^D9H frame=\([0-9]*\).*/\1/p')"
  hash="$(echo "$last" | sed -n 's/.*hash=\([0-9a-f]*\).*/\1/p')"
  center="$(echo "$last" | sed -n 's/.*center=\([0-9a-f]*\).*/\1/p')"
  checks="$(sed -n 's/.*: \([0-9]*\) checks, \([0-9]*\) failed.*/\2\/\1/p' "$WORK/logs/$t.txt" | tail -1)"
  # a test ends after its frames (exit 0) or the harness' 5th; FAIL lines are its own checks
  status=ok
  case "$rc" in
    0) ;;
    125) status="CRASH(logs/$t.exc)" ;;
    124) status="FAIL(timeout)" ;;
    *) status="FAIL(exit $rc)" ;;
  esac
  if grep -qE '\bFAIL\b|^D9H TIMEOUT' "$WORK/logs/$t.txt"; then
    if [ "$status" = ok ]; then status=FAIL; else status="FAIL,$status"; fi
  fi
  reason="$(xfail_reason "$t")"
  if [ -n "$reason" ]; then
    if [ "$status" = ok ]; then status="XPASS($reason)"; unexpected=$((unexpected + 1)); else status="XFAIL($reason)"; fi
  elif [ "$status" != ok ]; then
    unexpected=$((unexpected + 1))
  fi
  printf '%-20s %-2s %-16s %-6s %-9s %6s ms %s\n' "$t" "${frames:--}" "${hash:--}" "${center:--}" \
    "${checks:+failed $checks}" "$((t1 - t0))" "$status" | tee -a "$WORK/results.txt"
done

"$ENG/bin/wineserver" -k 2>/dev/null || true
"$ENG/bin/wineserver" -w 2>/dev/null || true
left=""
for i in $(seq 30); do
  left="$("$WORK/pidpath" $(ps -axo pid=) | grep -F "$ENG/" || true)"
  [ -z "$left" ] && break
  sleep 1
done
if [ -n "$left" ]; then
  echo "[hash] LEFTOVER processes:" >&2; echo "$left" >&2
  echo "$left" | awk '{print $1}' | xargs kill -9 2>/dev/null || true
  exit 1
fi
echo "[hash] done: $WORK/results.txt, $unexpected unexpected" >&2
[ "$unexpected" -eq 0 ]
