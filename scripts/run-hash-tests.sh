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
#
# BENCH=1 then runs the benchmarks BENCH_RUNS (3) times each into
# $WORK/bench.txt: test/bench_draws.c (100, 1000 and 3000 draws per frame,
# BENCH_FRAMES (1200) frames, no vsync, BENCH_MODE=flush (each frame waits
# for the GPU, no Present: independent of the compositor), spinflush or
# present; see the program) and
# test/bench_shaders.c
# (BENCH_SHADERS=<list of pixel shader .bin files>, compiled one by one: no
# compile workers, AIR cache or PSO archives). BENCH_ONLY=1 skips the tests.
# Timing numbers mean something only on an otherwise idle machine; the load
# average is logged with each line.
#
# PROBES=1 then runs test/fpenv: the FP environment an x64 caller hands to
# an x64 and (arm64 Wine) an ARM64EC DLL, the x87 control word and MXCSR
# around CreateDevice with this run's d3d9.dll, and cosf dumps of each CRT
# ($WORK/probes/cosf-*.bin, compared with test/fpenv/ulpcmp.c).
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
# neutron-patched Wine: its msync is on unless NEUTRON_MSYNC=0, and neutron
# runs with it off (with it on, a test's exit got SIGKILLed 6 of 10 times)
export NEUTRON_MSYNC="${NEUTRON_MSYNC:-0}"
export D9MT_AIR_CACHE="$WORK/air-cache.sqlite3" D9MT_PSO_ARCHIVE_DIR="$WORK/pso"
unset WINELOADER WINESERVER WINEDLLPATH
# CrossOver 26-style engines carry compatdb: keep its DXMT tree from shadowing our bridge
[ -f "$ENG/lib/wine/$UNIX_DIR/compatdb.so" ] && export WINE_COMPATDB='v=5
name=d9mt-tests;exe=*;dxgi=wined3d'
# Wine dlopens FreeType (and GnuTLS) by soname: without FreeType the system
# font metrics are garbage (SM_CYCAPTION 32770), every window gets a huge
# minimum size, lands off screen, and d9mt sees an invisible window (30 fps
# background cap). Engines that bundle them use lib/external; for an arm64
# engine without that (the prototype build) use Homebrew's.
if [ -d "$ENG/lib/external" ]; then
  export DYLD_FALLBACK_LIBRARY_PATH="$ENG/lib/external:/usr/lib"
elif [ "$KIND" = arm64 ] && command -v brew >/dev/null; then
  export DYLD_FALLBACK_LIBRARY_PATH="$(brew --prefix)/lib:/usr/lib"
fi
# Wine runs go through test/harness/exc_guard.c: a timeout, and a process of
# the run that crashes is logged to $WORK/logs/<name>.exc instead of becoming
# a crash report and, for Wine's GUI processes, a "quit unexpectedly" dialog
# (status CRASH). It must not be a system binary such as /usr/bin/perl: SIP
# strips DYLD_* from its environment.
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
run 300 wineboot wineboot -i > "$WORK/logs/wineboot.log" 2>&1
if [ "$KIND" = arm64 ]; then
  run 120 reg reg add 'HKLM\Software\Microsoft\Wow64\amd64' /ve /d libarm64ecfex.dll /f > "$WORK/logs/reg.log" 2>&1
  "$ENG/bin/wineserver" -w
fi
now() { perl -MTime::HiRes=time -e 'printf "%.0f", time * 1000'; }

wait_for_game
out="$(run 60 hello "$WORK/bin/hello.exe" 2> "$WORK/logs/hello.err" | tr -d '\r')"
printf '%-16s %s\n' hello "${out:-FAIL}" | tee "$WORK/results.txt"
[ "${BENCH_ONLY:-0}" = 1 ] && TESTS=()
for t in ${TESTS[@]+"${TESTS[@]}"}; do
  wait_for_game
  t0=$(now)
  rc=0
  ( cd "$WORK/bin" && run 60 "$t" "$WORK/bin/$t.exe" > "$WORK/logs/$t.out" 2> "$WORK/logs/$t.err" ) || rc=$?
  t1=$(now)
  tr -d '\r' < "$WORK/logs/$t.out" > "$WORK/logs/$t.txt"
  last="$(grep '^D9H frame=' "$WORK/logs/$t.txt" | tail -1)"
  frames="$(echo "$last" | sed -n 's/^D9H frame=\([0-9]*\).*/\1/p')"
  hash="$(echo "$last" | sed -n 's/.*hash=\([0-9a-f]*\).*/\1/p')"
  center="$(echo "$last" | sed -n 's/.*center=\([0-9a-f]*\).*/\1/p')"
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
  printf '%-16s %-2s %-16s %-6s %6s ms %s\n' "$t" "${frames:--}" "${hash:--}" "${center:--}" "$((t1 - t0))" "$status" \
    | tee -a "$WORK/results.txt"
done

"$ENG/bin/wineserver" -k 2>/dev/null || true
"$ENG/bin/wineserver" -w 2>/dev/null || true
# killed processes can take a while to go away on a loaded machine (seen:
# CX26's helper processes over 10 s)
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
if [ "${BENCH:-0}" = 1 ]; then
  for b in bench_draws bench_shaders; do
    x86_64-w64-mingw32-gcc -O2 -I "$V2/test" "$V2/test/$b.c" -o "$WORK/bin/$b.exe" -ld3d9 -lgdi32 -luser32
  done
  : > "$WORK/bench.txt"
  # no Present throttle for a window d9mt thinks hidden (e.g. the display
  # asleep): it would land in the timed waits through the CS queue
  export D9MT_BG_FPS=0 D9MT_STALL_PACE=0
  load() { sysctl -n vm.loadavg | awk '{print "load=" $2}'; }
  for r in $(seq "${BENCH_RUNS:-3}"); do
    for d in 100 1000 3000; do
      wait_for_game
      line="$(cd "$WORK/bin" && run 300 bench_draws "$WORK/bin/bench_draws.exe" "$d" "${BENCH_FRAMES:-1200}" "${BENCH_MODE:-flush}" 2> "$WORK/logs/bench_draws.err" | tr -d '\r' | grep '^BENCH' || echo "BENCH draws=$d FAIL")"
      echo "run=$r $(load) $line" | tee -a "$WORK/bench.txt"
    done
    if [ -n "${BENCH_SHADERS:-}" ]; then
      sed 's|^/|Z:/|' "$BENCH_SHADERS" > "$WORK/shaders.txt"
      wait_for_game
      line="$(cd "$WORK/bin" && D9MT_COMPILE_THREADS=0 D9MT_AIR_CACHE=0 D9MT_PSO_ARCHIVE=0 \
        run 1800 bench_shaders "$WORK/bin/bench_shaders.exe" 'Z:'"$WORK/shaders.txt" 2> "$WORK/logs/bench_shaders.err" \
        | tr -d '\r' | grep '^BENCH' || echo "BENCH shaders FAIL")"
      echo "run=$r $(load) $line" | tee -a "$WORK/bench.txt"
    fi
  done
  "$ENG/bin/wineserver" -k 2>/dev/null || true
  "$ENG/bin/wineserver" -w 2>/dev/null || true
fi
if [ "${PROBES:-0}" = 1 ]; then
  # FP environment and libm across the x64/ARM64EC boundary (test/fpenv): the
  # x64 caller with an x64 callee DLL (both under the emulator on arm64 Wine)
  # and, on arm64 Wine, with the ARM64EC callee; cosf dumps of each CRT.
  . "$V2/scripts/llvm-mingw.sh"
  llvm_mingw_ensure
  LMB="$LLVM_MINGW/bin"
  P="$WORK/probes"
  callees=(x64); [ "$KIND" = arm64 ] && callees+=(arm64ec)
  for c in "${callees[@]}"; do
    mkdir -p "$P/$c"
    cc="$LMB/x86_64-w64-mingw32-clang"; fl=()
    [ "$c" = arm64ec ] && { cc="$LMB/arm64ec-w64-mingw32-clang"; fl=(-ffp-contract=off); }
    "$cc" -O2 ${fl[@]+"${fl[@]}"} -shared "$V2/test/fpenv/fpenv_dll.c" -o "$P/$c/fpenv_dll.dll"
    "$cc" -O2 ${fl[@]+"${fl[@]}"} -shared "$V2/test/fpenv/cosf_dll.c" -o "$P/$c/cosf_dll.dll"
    x86_64-w64-mingw32-gcc -O2 "$V2/test/fpenv/fpenv_main.c" -o "$P/$c/fpenv_main.exe" -ld3d9 -lgdi32 -luser32
    "$LMB/x86_64-w64-mingw32-clang" -O2 "$V2/test/fpenv/cosf_main.c" -o "$P/$c/cosf_main.exe"
    wait_for_game
    ( cd "$P/$c" && run 60 "fpenv-$c" "$P/$c/fpenv_main.exe" 2> "$WORK/logs/fpenv-$c.err" ) \
      | tr -d '\r' > "$WORK/logs/fpenv-$c.txt" || true
    ( cd "$P/$c" && run 300 "cosf-$c" "$P/$c/cosf_main.exe" cosf_dll.dll 'Z:'"$P/cosf-$c.bin" 2> "$WORK/logs/cosf-$c.err" ) \
      | tr -d '\r' > "$WORK/logs/cosf-$c.txt" || true
    sed "s/^/[$c] /" "$WORK/logs/fpenv-$c.txt" "$WORK/logs/cosf-$c.txt"
  done
  # the x87 control word and MXCSR around CreateDevice, with this run's d3d9.dll
  cp "$WORK/bin/d3d9.dll" "$P/x64/d3d9.dll"
  ( cd "$P/x64" && run 60 fpenv-d3d9 "$P/x64/fpenv_main.exe" d3d9 2> "$WORK/logs/fpenv-d3d9.err" ) \
    | tr -d '\r' | tee "$WORK/logs/fpenv-d3d9.txt" | sed "s/^/[d3d9 $DRIVER] /" || true
  "$ENG/bin/wineserver" -k 2>/dev/null || true
  "$ENG/bin/wineserver" -w 2>/dev/null || true
fi
echo "[hash] done: $WORK/results.txt" >&2
