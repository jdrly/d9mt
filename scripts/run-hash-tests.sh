#!/usr/bin/env bash
# Run d9mt's x64 test programs on a Wine engine and print the hash of each
# one's last backbuffer (test/harness/d9h.h): the A/B check between engines
# and builds, e.g. today's x86_64 Wine under Rosetta against arm64 Wine + FEX
# (docs/arm64-runtime.md).
#
#   ENGINE=<wine root> [DRIVER=x64|arm64ec] scripts/run-hash-tests.sh [code_xxx...]
#
# ENGINE   a Wine install: x86_64 (lib/wine/x86_64-unix, Rosetta) or arm64
#          (lib/wine/aarch64-unix, x64 processes as ARM64EC, FEX)
# DRIVER   the d3d9.dll next to the programs: x64 (build/driver-x64, the
#          shipping one; on arm64 Wine it runs under FEX) or arm64ec
#          (build/driver-arm64ec). Default: x64
# The bridge always matches the engine: x86_64 Wine gets build/bridge
# (winemetal64.dll + x86_64 winemetal.so), arm64 Wine build/bridge-arm64ec
# (ARM64EC winemetal.dll + arm64 winemetal.so; the x64 winemetal.dll needs
# CrossOver's __wine_unix_call, which upstream Wine doesn't export).
#
# Everything happens in a throwaway work dir (WORK, default
# /tmp/d9mt-hash-<engine>-<driver>): an APFS clone of the engine with the
# bridge installed into it, a fresh prefix, d9mt's caches. Nothing outside it
# is written. Before each Wine run it waits while SWTOR runs. Prints one line
# per test (frames, hash of the last frame, center pixel, wall time) to stdout
# and $WORK/results.txt; leftover Wine processes of the clone fail the run.
set -euo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
SRC_ENGINE="${ENGINE:?set ENGINE to a Wine root}"
DRIVER="${DRIVER:-x64}"
if [ -d "$SRC_ENGINE/lib/wine/aarch64-unix" ]; then KIND=arm64
elif [ -d "$SRC_ENGINE/lib/wine/x86_64-unix" ]; then KIND=x86_64
else echo "FATAL: $SRC_ENGINE is no Wine install" >&2; exit 1; fi
WORK="${WORK:-/tmp/d9mt-hash-$KIND-$DRIVER}"
ENG="$WORK/engine"
DEFAULT_TESTS=(code code_alpha code_blend code_cube3d code_cull code_depth code_dxt code_ff code_index
  code_mip code_readback code_rtt code_rtt3d code_shader code_stencil code_strip code_texcube3d
  code_texture code_twodraw code_updatetex)
TESTS=("$@"); [ ${#TESTS[@]} -gt 0 ] || TESTS=("${DEFAULT_TESTS[@]}")

case "$DRIVER" in
  x64) DLL="$V2/build/driver-x64/d3d9.dll" ;;
  arm64ec) DLL="$V2/build/driver-arm64ec/d3d9.dll" ;;
  *) echo "FATAL: DRIVER must be x64 or arm64ec" >&2; exit 1 ;;
esac
if [ "$KIND" = arm64 ]; then
  BR_DLL="$V2/build/bridge-arm64ec/winemetal-arm64ec.dll"; BR_SO="$V2/build/bridge-arm64ec/winemetal.so"
  PE_DIR=aarch64-windows; UNIX_DIR=aarch64-unix
else
  [ "$DRIVER" = x64 ] || { echo "FATAL: an ARM64EC driver needs arm64 Wine" >&2; exit 1; }
  BR_DLL="$V2/build/bridge/winemetal64.dll"; BR_SO="$V2/build/bridge/winemetal.so"
  PE_DIR=x86_64-windows; UNIX_DIR=x86_64-unix
fi
for f in "$DLL" "$BR_DLL" "$BR_SO"; do [ -f "$f" ] || { echo "FATAL: $f missing" >&2; exit 1; }; done

swtor_running() { # a real swtor.exe process, not a shell whose command line mentions it
  ps -axo pid=,comm=,args= | grep -i 'swtor\.exe' \
    | grep -vE ' (/bin/)?(zsh|bash|sh|grep|pgrep|ps|sed|awk)( |$)|/(zsh|bash|sh|grep|pgrep) |pgrep|grep ' | grep -q .
}
wait_for_game() {
  while swtor_running; do echo "[hash] SWTOR is running, waiting" >&2; sleep 30; done
}

echo "[hash] engine $SRC_ENGINE ($KIND), driver $DRIVER, work $WORK" >&2
rm -rf "$WORK"
mkdir -p "$WORK/bin" "$WORK/logs"
cp -cR "$SRC_ENGINE" "$ENG"
cp "$BR_DLL" "$ENG/lib/wine/$PE_DIR/winemetal.dll"
cp "$BR_SO" "$ENG/lib/wine/$UNIX_DIR/winemetal.so"

for t in "${TESTS[@]}"; do
  x86_64-w64-mingw32-gcc -O1 -include "$V2/test/harness/d9h.h" -I "$V2/test" "$V2/test/$t.c" \
    -o "$WORK/bin/$t.exe" -ld3d9 -lgdi32 -luser32 2> "$WORK/logs/$t.build.log" \
    || { echo "FATAL: $t does not build, see $WORK/logs/$t.build.log" >&2; exit 1; }
done
printf '#include <stdio.h>\nint main(void){printf("hello from x64\\n");return 0;}\n' > "$WORK/hello.c"
x86_64-w64-mingw32-gcc -O1 "$WORK/hello.c" -o "$WORK/bin/hello.exe"
cp "$DLL" "$WORK/bin/d3d9.dll"
# Wine 11 rewrites argv[0] to the Windows path: find leftovers by real executable
printf '#include <libproc.h>\n#include <stdio.h>\n#include <stdlib.h>\nint main(int c,char**v){char p[PROC_PIDPATHINFO_MAXSIZE];for(int i=1;i<c;i++)if(proc_pidpath(atoi(v[i]),p,sizeof p)>0)printf("%%s %%s\\n",v[i],p);return 0;}\n' > "$WORK/pidpath.c"
clang -O1 "$WORK/pidpath.c" -o "$WORK/pidpath"

export WINEPREFIX="$WORK/prefix" WINEDEBUG="${WINEDEBUG:--all}" WINEMSYNC="${WINEMSYNC:-1}"
export WINEDLLOVERRIDES="mscoree,mshtml=;d3d9=n,b"
export D9MT_AIR_CACHE="$WORK/air-cache.sqlite3" D9MT_PSO_ARCHIVE_DIR="$WORK/pso"
unset WINELOADER WINESERVER WINEDLLPATH
# CrossOver 26-style engines carry compatdb: keep its DXMT tree from shadowing our bridge
[ -f "$ENG/lib/wine/$UNIX_DIR/compatdb.so" ] && export WINE_COMPATDB='v=5
name=d9mt-tests;exe=*;dxgi=wined3d'
WINE="$ENG/bin/wine"
run() { # <timeout s> <args...>: wine with a timeout
  local t=$1; shift
  perl -e 'alarm shift; exec @ARGV' "$t" "$WINE" "$@"
}

wait_for_game
echo "[hash] new prefix" >&2
run 300 wineboot -i > "$WORK/logs/wineboot.log" 2>&1
if [ "$KIND" = arm64 ]; then
  run 120 reg add 'HKLM\Software\Microsoft\Wow64\amd64' /ve /d libarm64ecfex.dll /f > "$WORK/logs/reg.log" 2>&1
  "$ENG/bin/wineserver" -w
fi
now() { perl -MTime::HiRes=time -e 'printf "%.0f", time * 1000'; }

wait_for_game
out="$(run 60 "$WORK/bin/hello.exe" 2> "$WORK/logs/hello.err" | tr -d '\r')"
printf '%-16s %s\n' hello "${out:-FAIL}" | tee "$WORK/results.txt"
for t in "${TESTS[@]}"; do
  wait_for_game
  t0=$(now)
  rc=0
  ( cd "$WORK/bin" && run 60 "$WORK/bin/$t.exe" > "$WORK/logs/$t.out" 2> "$WORK/logs/$t.err" ) || rc=$?
  t1=$(now)
  tr -d '\r' < "$WORK/logs/$t.out" > "$WORK/logs/$t.txt"
  last="$(grep '^D9H frame=' "$WORK/logs/$t.txt" | tail -1)"
  frames="$(echo "$last" | sed -n 's/^D9H frame=\([0-9]*\).*/\1/p')"
  hash="$(echo "$last" | sed -n 's/.*hash=\([0-9a-f]*\).*/\1/p')"
  center="$(echo "$last" | sed -n 's/.*center=\([0-9a-f]*\).*/\1/p')"
  # a test ends after its frames (exit 0) or the harness' 5th; FAIL lines are its own checks
  status=ok
  [ "$rc" = 0 ] || status="FAIL(exit $rc)"
  grep -qE '\bFAIL\b|^D9H TIMEOUT' "$WORK/logs/$t.txt" && status=FAIL
  printf '%-16s %-2s %-16s %-6s %6s ms %s\n' "$t" "${frames:--}" "${hash:--}" "${center:--}" "$((t1 - t0))" "$status" \
    | tee -a "$WORK/results.txt"
done

"$ENG/bin/wineserver" -k 2>/dev/null || true
"$ENG/bin/wineserver" -w 2>/dev/null || true
sleep 1
left="$("$WORK/pidpath" $(ps -axo pid=) | grep -F "$ENG/" || true)"
if [ -n "$left" ]; then
  echo "[hash] LEFTOVER processes:" >&2; echo "$left" >&2
  echo "$left" | awk '{print $1}' | xargs kill -9 2>/dev/null || true
  exit 1
fi
echo "[hash] done: $WORK/results.txt" >&2
