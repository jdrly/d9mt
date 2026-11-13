#!/usr/bin/env bash
# The hash suite's runner: builds d9mt's D3D9 test programs (test/code_*.c) with the hash
# harness (test/harness/d9h.h: a hash of every frame's backbuffer), runs them under Wine with
# this tree's d3d9.dll and bridge, and writes each program's frame hashes. Two runs are compared
# with scripts/cmp-hash-runs.py: the Definition of Done's "hash suite equals base".
#
#   ENGINE=<wine root> [VARIANT=<name>] [WORK=<dir>] [CACHE=<dir>] scripts/run-hash-tests.sh [--build-only] [code_xxx...]
#
# ENGINE    an x86_64 Wine install (bin/wine, lib/wine/x86_64-unix; it runs under Rosetta).
#           It is cloned into WORK; the original is never written.
# SRC       the tree under test (default: the tree this script is in): its test/code_*.c and
#           its builds. The harness (test/harness) is always this script's own, so a base
#           commit from before the runner existed runs through the same runner as head.
# DLL       the d3d9.dll under test (default SRC/build/driver-x64/d3d9.dll)
# BRIDGE    build (the default when SRC/build/bridge/winemetal.so exists): that bridge goes
#           into the engine clone. engine: keep the engine's own winemetal.
# VARIANT   default | debug (MTL_DEBUG_LAYER=1) | noattach (D9MT_NO_ATTACHMENT_OPT=1) |
#           repro (d3d9.reproducibleCommandStream = True) | lifetime (D9MT_LIFETIME_CHECK=1) |
#           shaderval (Metal shader validation, PSO archives off) | early (D9MT_EARLY_COMMIT=always)
#           Any other D9MT_* variable of the caller reaches the programs, D9MT_AIR_MACOS too.
# WORK      a scratch folder, removed and made again (default ${TMPDIR:-/tmp}/d9mt-hash). The
#           engine clone, a new prefix, d9mt's caches and every log live there, nothing else is
#           written.
# CACHE     a folder of the caller's for d9mt's three caches (the AIR cache, the PSO archives,
#           the state cache) instead of WORK/cache. It is used as it is found and left behind,
#           so a second run starts on what the first one wrote: a run of base, then head on
#           base's caches. Neither CACHE nor WORK may lie inside the other. After the run
#           WORK/cache.txt lists its PSO folders and the AIR cache's tables with their row
#           counts.
# VD=1      every window on a temporary virtual display: VD_DIR must name a folder with
#           with-vd.sh and d9h-vd.h (a tool outside this repository). Without it the windows
#           open on the main display.
# --build-only  build the programs and the tools, make and check the prefix, run no program
#           (what a machine without Metal 4 can do).
#
# Without program names every test/code_*.c runs except code_sm3_ref (scripts/sm3_ref_test.py)
# and code_statecache (scripts/run-statecache-bench.sh).
#
# Output, in WORK:
#   hashes.txt   one line per program: "<name> rc=<exit> fails=<its FAIL lines> <hash per frame>…"
#                (the input of scripts/cmp-hash-runs.py)
#   stats.txt    one line: programs, programs that aren't rc=0 fails=0, Metal validation lines,
#                "Invalid Metal usage" lines, lifetime violations, programs without proof that
#                d9mt drew them, GPU-lost and GPU-hung lines (scripts/cmp-hash-runs.py --stats
#                judges the counts of head against base), the programs that ran a second time
#                after an exit kill, and those whose second run drew other frames
#   logs/        per program .out, .err, and .exc when a process crashed; logs/killed/ has the
#                logs of a run that was killed at exit and run again
#
# A program's own exit status and FAIL lines are data: the comparison with base judges them.
# This script's exit status is about the run itself:
#   0  every program was built, ran, and its log proves d9mt was the driver ("D9MT: compile
#      workers"), and no Wine process was left behind
#   1  a program didn't build or has no such proof, the prefix wasn't made or its setup wasn't
#      verified, processes were left over (they are killed), or a program's second run after an
#      exit kill drew other frames than its first: the hashes don't count
#   3  no verdict: a log has D9MT-GPU-LOST or D9MT-GPU-HUNG. The GPU was lost or hung; run it
#      again
# The exit kill of issue #164: Wine sometimes ends a process with SIGKILL (rc=137) during its
# teardown, after the program printed every frame and "D9H done". That is not the program's
# doing, so such a program runs once more, and the second run is the one that counts. Its frames
# must equal the killed run's: if they differ, the program isn't deterministic, its line gets
# a FAIL and the run's exit status is 1. (code_mtlock's frames never repeat, docs/testing.md.)
# A second kill in a row stays rc=137.
#
# Before each Wine run it waits while SWTOR or its launcher runs on this Mac.
set -euo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
if [ "${VD:-0}" = 1 ] && [ "${VD_ACTIVE:-0}" != 1 ]; then
  : "${VD_DIR:?VD=1 needs VD_DIR (with-vd.sh, d9h-vd.h)}"
  exec "$VD_DIR/with-vd.sh" "${VD_SECONDS:-3600}" env VD_ACTIVE=1 bash "$0" "$@"
fi
BUILD_ONLY=0
if [ "${1:-}" = --build-only ]; then BUILD_ONLY=1; shift; fi
SRC="$(cd "${SRC:-$V2}" && pwd)"
HARNESS=(-include "$V2/test/harness/d9h.h")
[ "${VD:-0}" = 1 ] && HARNESS=(-include "$VD_DIR/d9h-vd.h" -I "$V2/test/harness")
SRC_ENGINE="${ENGINE:?set ENGINE to a Wine root}"
[ -d "$SRC_ENGINE/lib/wine/x86_64-unix" ] || { echo "FATAL: $SRC_ENGINE is no x86_64 Wine install" >&2; exit 1; }
WORK="${WORK:-${TMPDIR:-/tmp}/d9mt-hash}"
ENG="$WORK/engine"
KEEP_CACHE=0
if [ -n "${CACHE:-}" ]; then
  KEEP_CACHE=1
  mkdir -p "$CACHE" "$WORK"
  CACHE="$(cd "$CACHE" && pwd -P)"
  WORK_REAL="$(cd "$WORK" && pwd -P)"
  # neither folder may hold the other: WORK is removed first, and that must not touch CACHE
  case "$CACHE/" in "$WORK_REAL"/*) echo "FATAL: CACHE ($CACHE) lies inside WORK, which is removed first" >&2; exit 1 ;; esac
  case "$WORK_REAL/" in "$CACHE"/*) echo "FATAL: WORK ($WORK_REAL) lies inside CACHE ($CACHE): removing WORK would remove part of the cache" >&2; exit 1 ;; esac
else
  CACHE="$WORK/cache"
fi
DLL="${DLL:-$SRC/build/driver-x64/d3d9.dll}"
[ -f "$DLL" ] || { echo "FATAL: $DLL missing (ARCH=x64 scripts/build-driver.sh)" >&2; exit 1; }
BRIDGE="${BRIDGE:-$([ -f "$SRC/build/bridge/winemetal.so" ] && echo build || echo engine)}"
VARIANT="${VARIANT:-default}"
VARIANT_ENV=()
case "$VARIANT" in
  default) ;;
  debug) VARIANT_ENV=(MTL_DEBUG_LAYER=1) ;;
  noattach) VARIANT_ENV=(D9MT_NO_ATTACHMENT_OPT=1) ;;
  repro) VARIANT_ENV=("DXVK_CONFIG=d3d9.reproducibleCommandStream = True") ;;
  lifetime) VARIANT_ENV=(D9MT_LIFETIME_CHECK=1) ;;
  shaderval) VARIANT_ENV=(MTL_SHADER_VALIDATION=1 MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1 D9MT_PSO_ARCHIVE=0) ;;
  early) VARIANT_ENV=(D9MT_EARLY_COMMIT=always) ;;
  *) echo "FATAL: unknown VARIANT '$VARIANT'" >&2; exit 1 ;;
esac

TESTS=("$@")
if [ ${#TESTS[@]} -eq 0 ]; then
  for f in "$SRC"/test/code_*.c; do
    t="$(basename "$f" .c)"
    case "$t" in code_sm3_ref|code_statecache) continue ;; esac
    TESTS+=("$t")
  done
fi

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

echo "[hash] tree $SRC, engine $SRC_ENGINE, dll $DLL, bridge $BRIDGE, variant $VARIANT, work $WORK" >&2
[ "$KEEP_CACHE" = 0 ] || echo "[hash] caches in $CACHE, kept" >&2
rm -rf "$WORK"
mkdir -p "$WORK/bin" "$WORK/logs" "$CACHE"
cp -cR "$SRC_ENGINE" "$ENG" 2>/dev/null || cp -R "$SRC_ENGINE" "$ENG"   # an APFS clone where it can
if [ "$BRIDGE" = build ]; then
  cp "$SRC/build/bridge/winemetal64.dll" "$ENG/lib/wine/x86_64-windows/winemetal.dll"
  cp "$SRC/build/bridge/winemetal.so" "$ENG/lib/wine/x86_64-unix/winemetal.so"
fi

for t in "${TESTS[@]}"; do
  [ -f "$SRC/test/$t.c" ] || { echo "FATAL: no test/$t.c in $SRC" >&2; exit 1; }
  x86_64-w64-mingw32-gcc -O1 "${HARNESS[@]}" -I "$SRC/test" "$SRC/test/$t.c" \
    -o "$WORK/bin/$t.exe" -ld3d9 -lgdi32 -luser32 2> "$WORK/logs/$t.build.log" \
    || { echo "FATAL: $t does not build, see $WORK/logs/$t.build.log" >&2; tail -5 "$WORK/logs/$t.build.log" >&2; exit 1; }
done
echo "[hash] ${#TESTS[@]} programs built" >&2
cp "$DLL" "$WORK/bin/d3d9.dll"
# Wine rewrites argv[0] to the Windows path: find leftovers by their real executable.
printf '#include <libproc.h>\n#include <stdio.h>\n#include <stdlib.h>\nint main(int c,char**v){char p[PROC_PIDPATHINFO_MAXSIZE];for(int i=1;i<c;i++)if(proc_pidpath(atoi(v[i]),p,sizeof p)>0)printf("%%s %%s\\n",v[i],p);return 0;}\n' > "$WORK/pidpath.c"
clang -O1 "$WORK/pidpath.c" -o "$WORK/pidpath"

export WINEPREFIX="$WORK/prefix" WINEDEBUG="${WINEDEBUG:--all}" WINEMSYNC="${WINEMSYNC:-1}"
export WINEDLLOVERRIDES="mscoree,mshtml=;d3d9=n,b"
export NEUTRON_MSYNC="${NEUTRON_MSYNC:-0}"
export D9MT_AIR_CACHE="$CACHE/air-cache.sqlite3" D9MT_PSO_ARCHIVE_DIR="$CACHE/pso" D9MT_STATE_CACHE="$CACHE/state"
unset WINELOADER WINESERVER WINEDLLPATH
# CrossOver 26-style engines carry compatdb: keep its DXMT tree from shadowing our bridge.
[ -f "$ENG/lib/wine/x86_64-unix/compatdb.so" ] && export WINE_COMPATDB='v=5
name=d9mt-tests;exe=*;dxgi=wined3d'
# Without FreeType the system font metrics are garbage and every window lands off screen.
[ -d "$ENG/lib/external" ] && export DYLD_FALLBACK_LIBRARY_PATH="$ENG/lib/external:/usr/lib"
# Wine runs go through test/harness/exc_guard.c: a timeout, and a crash of any process of the
# run is logged to logs/<name>.exc instead of becoming a crash report or a dialog.
xcrun mig -DMACH_EXC_SERVER_TASKIDTOKEN -sheader "$WORK/mach_excServer.h" -server "$WORK/mach_excServer.c" -user /dev/null -header /dev/null \
  "$(xcrun --show-sdk-path)/usr/include/mach/mach_exc.defs"
clang -O1 -I "$WORK" "$V2/test/harness/exc_guard.c" "$WORK/mach_excServer.c" -o "$WORK/exc_guard"
WINE="$ENG/bin/wine"
run() { # <timeout s> <log name> <args...>: wine with a timeout, crashes logged
  local t=$1 name=$2; shift 2
  "$WORK/exc_guard" "$t" "$WORK/logs/$name.exc" "$WINE" "$@"
}
stop_wine() {
  "$ENG/bin/wineserver" -k 2>/dev/null || true
  "$ENG/bin/wineserver" -w 2>/dev/null || true
}

# A new prefix that never opens a crash window: ShowCrashDialog=0 and a debugger that starts
# without asking, read back from the registry files before any program runs.
wait_for_game
echo "[hash] new prefix" >&2
if ! WINEDLLOVERRIDES="$WINEDLLOVERRIDES;winemac.drv=d" run 300 wineboot wineboot -i > "$WORK/logs/wineboot.log" 2>&1; then
  echo "FATAL: wineboot failed, no prefix: see $WORK/logs/wineboot.log" >&2
  tail -20 "$WORK/logs/wineboot.log" >&2 || true
  stop_wine
  exit 1
fi
{
  run 60 reg-dialog reg add 'HKCU\Software\Wine\WineDbg' /v ShowCrashDialog /t REG_DWORD /d 0 /f
  for k in 'HKLM\Software\Microsoft\Windows NT\CurrentVersion\AeDebug' 'HKLM\Software\Wow6432Node\Microsoft\Windows NT\CurrentVersion\AeDebug'; do
    run 60 reg-auto reg add "$k" /v Auto /t REG_SZ /d 1 /f
    run 60 reg-debugger reg add "$k" /v Debugger /t REG_SZ /d 'winedbg --auto %ld %ld' /f
  done
} >> "$WORK/logs/wineboot.log" 2>&1 || true
stop_wine
prefix_ok=1
grep -A4 -F '[Software\\Wine\\WineDbg]' "$WINEPREFIX/user.reg" 2>/dev/null | grep -q '"ShowCrashDialog"=dword:00000000' || prefix_ok=0
[ "$(grep -A3 -F 'CurrentVersion\\AeDebug]' "$WINEPREFIX/system.reg" 2>/dev/null | grep -c '"Auto"="1"')" = 2 ] || prefix_ok=0
[ "$(grep -A3 -F 'CurrentVersion\\AeDebug]' "$WINEPREFIX/system.reg" 2>/dev/null | grep -c '"Debugger"="winedbg --auto %ld %ld"')" = 2 ] || prefix_ok=0
if [ "$prefix_ok" != 1 ]; then
  echo "FATAL: the prefix setup isn't verified (ShowCrashDialog=0, AeDebug): no program runs. See $WORK/logs/wineboot.log" >&2
  tail -20 "$WORK/logs/wineboot.log" >&2 || true
  exit 1
fi
echo "[hash] prefix: ShowCrashDialog=0, AeDebug Auto=1 winedbg --auto: verified" >&2

leftovers() { # kills what is left of the engine clone's processes; status 1 if there were any
  local left=""
  for _ in $(seq 30); do
    # shellcheck disable=SC2046 # one pid per word
    left="$("$WORK/pidpath" $(ps -axo pid=) | grep -F "$ENG/" || true)"
    [ -z "$left" ] && return 0
    sleep 1
  done
  echo "[hash] LEFTOVER processes:" >&2; echo "$left" >&2
  echo "$left" | awk '{print $1}' | xargs kill -9 2>/dev/null || true
  return 1
}

if [ "$BUILD_ONLY" = 1 ]; then
  leftovers || exit 1
  echo "[hash] --build-only: ${#TESTS[@]} programs built, the prefix made and verified, no program run" >&2
  exit 0
fi

: > "$WORK/hashes.txt"
noproof="" notclean="" reruns="" rerundiffers=""
# run_program <name>: one run of it; sets rc, hashes and fails
run_program() {
  local t=$1
  wait_for_game
  rc=0
  ( cd "$WORK/bin" && env ${VARIANT_ENV[@]+"${VARIANT_ENV[@]}"} "$WORK/exc_guard" 60 "$WORK/logs/$t.exc" "$WINE" "$WORK/bin/$t.exe" \
      > "$WORK/logs/$t.out" 2> "$WORK/logs/$t.err" ) || rc=$?
  hashes="$(tr -d '\r' < "$WORK/logs/$t.out" | sed -n 's/^D9H frame=.*hash=\([0-9a-f]*\).*/\1/p' | tr '\n' ' ')"
  # the program's own FAIL lines, the harness' timeout, and a frame the harness couldn't read back
  fails="$(tr -d '\r' < "$WORK/logs/$t.out" | grep -c -E '\bFAIL\b|^D9H TIMEOUT|^D9H frame=[0-9]+ (GetBackBuffer|readback) hr=' || true)"
}
for t in "${TESTS[@]}"; do
  run_program "$t"
  # the exit kill (issue #164): everything was drawn, then Wine killed the process
  if [ "$rc" = 137 ] && [ "$fails" = 0 ] && [ -n "$hashes" ] && tr -d '\r' < "$WORK/logs/$t.out" | grep -q '^D9H done'; then
    killed="$hashes"
    mkdir -p "$WORK/logs/killed"
    for f in out err exc; do mv "$WORK/logs/$t.$f" "$WORK/logs/killed/" 2>/dev/null || true; done
    echo "[hash] $t: killed at exit (rc=137) after all its frames, running it again" >&2
    run_program "$t"
    reruns+="$t "
    if [ "$hashes" != "$killed" ] && [ "$t" != code_mtlock ]; then
      echo "FAIL: this run's frames differ from the frames of the run that was killed at exit: $killed" >> "$WORK/logs/$t.out"
      fails=$((fails + 1))
      rerundiffers+="$t "
    fi
  fi
  echo "$t rc=$rc fails=$fails $hashes" | tee -a "$WORK/hashes.txt"
  grep -q 'D9MT: compile workers' "$WORK/logs/$t.err" "$WORK/logs/$t.out" 2>/dev/null || noproof+="$t "
  { [ "$rc" = 0 ] && [ "$fails" = 0 ]; } || notclean+="$t(rc=$rc,fails=$fails) "
done
stop_wine
left_rc=0
leftovers || left_rc=1
if [ "$KEEP_CACHE" = 1 ]; then
  {
    echo "# PSO folders of $CACHE/pso, with the files in each"
    for d in "$CACHE"/pso/*/; do
      [ -d "$d" ] || continue
      echo "$(basename "$d") files=$(find "$d" -type f | wc -l | tr -d ' ')"
    done
    echo "# AIR cache tables"
    if [ -f "$CACHE/air-cache.sqlite3" ] && command -v sqlite3 >/dev/null; then
      for tb in $(sqlite3 "$CACHE/air-cache.sqlite3" "select name from sqlite_master where type='table' order by name"); do
        echo "$tb rows=$(sqlite3 "$CACHE/air-cache.sqlite3" "select count(*) from \"$tb\"")"
      done
    fi
  } > "$WORK/cache.txt" 2>&1 || true
fi

count() { cat "$WORK"/logs/*.err "$WORK"/logs/*.out 2>/dev/null | grep "$@" || true; }
validation="$(cat "$WORK"/logs/*.err 2>/dev/null | grep -v 'Validation Enabled' | grep -c -i -E 'validation|failed assertion|MTLDebug' || true)"
invalid="$(count -c 'Invalid Metal usage')"
lifetime="$(count 'D9MT-LIFETIME' | grep -c -v 'D9MT-LIFETIME: check on' || true)"
gpulost="$(count -c -E 'D9MT-GPU-(LOST|HUNG)')"
printf 'STATS variant=%s programs=%s notclean=%s validation=%s invalid-usage=%s lifetime=%s no-d9mt-proof=%s gpu-lost=%s exit-kill-reruns=%s rerun-differs=%s tree=%s\n' \
  "$VARIANT" "${#TESTS[@]}" "${notclean// /,}" "$validation" "$invalid" "$lifetime" "${noproof// /,}" "$gpulost" \
  "${reruns// /,}" "${rerundiffers// /,}" \
  "$(git -C "$SRC" rev-parse --short HEAD 2>/dev/null || echo unknown)" | sed 's/=,*\( \)/=none\1/g; s/,\( \)/\1/g' | tee "$WORK/stats.txt"

if [ "$gpulost" != 0 ]; then
  echo "[hash] NO VERDICT: $gpulost D9MT-GPU-LOST or D9MT-GPU-HUNG lines. The GPU was lost or hung; run it again." >&2
  exit 3
fi
if [ -n "$noproof" ]; then
  echo "[hash] INVALID: no 'D9MT: compile workers' line for: $noproof(d9mt didn't draw them, so their hashes say nothing)" >&2
  exit 1
fi
if [ -n "$rerundiffers" ]; then
  echo "[hash] FAIL: after an exit kill the second run drew other frames than the first: $rerundiffers(not deterministic, so its hashes say nothing)" >&2
  exit 1
fi
[ "$left_rc" = 0 ] || exit 1
echo "[hash] done: $WORK/hashes.txt" >&2
