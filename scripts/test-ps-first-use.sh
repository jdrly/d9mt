#!/usr/bin/env bash
# test-ps-first-use.sh — the app profile's psPrefetch and psIrPrefetch (C5) under Wine: runs
# test/code_ps_first_use.c through scripts/run-hash-tests.sh once per arm of the two fields, with
# D9MT_PROFILE=1 and fresh caches, and checks from each log:
#   - the fields line names the arm's values;
#   - the Phase A runs (COMPILE phaseA=) and the libraries made (COMPILE shaders=): the program
#     creates 1 vertex shader and 17 pixel shaders and draws with 3 of the pixel shaders, so an
#     arm that queues Phase A at creation runs it 18 times and one that defers it runs it 4 times;
#     an arm that queues the pixel shaders' functions at creation makes at least 18 libraries and
#     one that defers them makes 4 (the vertex shader's and the 3 drawn pixel shaders');
#   - the program's own colour checks pass and every arm draws the same frames.
#
#   ENGINE=<Wine engine root> WORK=<durable dir> scripts/test-ps-first-use.sh
#
# Needs this tree's build (build/driver-x64/d3d9.dll and the bridge), as run-hash-tests.sh does,
# and a Mac with Metal 4. Writes only under $WORK (one run-hash-tests.sh work folder per arm).
# Exit status 1 if a check fails.
set -uo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
: "${ENGINE:?set ENGINE to a Wine engine root}"
: "${WORK:?set WORK to a durable directory}"
mkdir -p "$WORK"
WORK="$(cd "$WORK" && pwd)"
fail=0 first_hashes=
# The swtor app profile's own values (mtl_app_profile.h) and what they give.
SWTOR_PS=off SWTOR_IR=on SWTOR_PHASEA=18 SWTOR_LIBS='=4'

# <name> <want psPrefetch> <want psIrPrefetch> <want Phase A runs> <libraries: =4 or >=18> <env...>
arm() {
  local name=$1 wantPs=$2 wantIr=$3 wantPhaseA=$4 wantLibs=$5; shift 5
  local W="$WORK/$name" log out hashes phaseA libs rc=0
  echo "== $name: ${*:-(no variable)}"
  # A fresh work folder (no log of an earlier run can count), and d9mt's caches inside it: the
  # caller's D9MT_* and CACHE would reach the run, so the fields', the profile's and the caches'
  # are cleared first.
  rm -rf "$W"
  env -u D9MT_PS_PREFETCH -u D9MT_PS_IR_PREFETCH -u D9MT_APP_PROFILE -u D9MT_STATE_CACHE -u CACHE \
    "$@" D9MT_PROFILE=1 VARIANT=default WORK="$W" ENGINE="$ENGINE" \
    bash "$V2/scripts/run-hash-tests.sh" code_ps_first_use > "$WORK/$name.run.txt" 2>&1 || rc=$?
  log="$W/logs/code_ps_first_use.err" out="$W/logs/code_ps_first_use.out"
  # The runner's verdict on the run itself (0: it ran and d9mt drew it; 3: the GPU was lost), and
  # the program's line in hashes.txt: exit 0, no FAIL line, every frame hashed.
  if [ "$rc" != 0 ]; then echo "  FAIL: run-hash-tests.sh exit $rc (see $WORK/$name.run.txt)"; fail=1; return; fi
  local hl; hl="$(grep '^code_ps_first_use ' "$W/hashes.txt" 2>/dev/null)"
  case "$hl" in
    "code_ps_first_use rc=0 fails=0 "*) ;;
    *) echo "  FAIL: hashes.txt: '${hl:-no line}'"; fail=1; return ;;
  esac
  if [ ! -s "$log" ]; then echo "  FAIL: no log (see $WORK/$name.run.txt)"; fail=1; return; fi
  local line; line="$(tr -d '\r' < "$log" | grep -m1 'd9mt: app profile fields:')"
  case "$line" in
    *"psPrefetch $wantPs"*"psIrPrefetch $wantIr"*) echo "  fields: ${line#*fields: }" ;;
    *) echo "  FAIL: fields line '${line#*fields: }', want psPrefetch $wantPs, psIrPrefetch $wantIr"; fail=1 ;;
  esac
  phaseA="$(tr -d '\r' < "$log" | LC_ALL=C awk '/COMPILE frame=/ { for (i = 1; i <= NF; i++) if ($i ~ /^phaseA=/) {
      split(substr($i, 8), a, "/"); n += a[1]; seen = 1 } } END { print seen ? n : "none" }')"
  libs="$(tr -d '\r' < "$log" | LC_ALL=C awk '/COMPILE frame=/ { for (i = 1; i <= NF; i++) if ($i ~ /^shaders=/) {
      split(substr($i, 9), a, "/"); n += a[1] } } END { print n + 0 }')"
  if [ "$phaseA" = "$wantPhaseA" ]; then echo "  Phase A runs: $phaseA"
  else echo "  FAIL: Phase A runs $phaseA, want $wantPhaseA"; fail=1; fi
  case "$wantLibs" in
    =*)  [ "$libs" = "${wantLibs#=}" ] ;;
    '>='*) [ "$libs" -ge "${wantLibs#>=}" ] ;;
  esac && echo "  libraries: $libs" || { echo "  FAIL: libraries $libs, want $wantLibs"; fail=1; }
  if tr -d '\r' < "$out" | grep -q '^code_ps_first_use: [0-9]* checks, 0 failed'; then
    echo "  colour checks: $(tr -d '\r' < "$out" | grep '^code_ps_first_use:')"
  else echo "  FAIL: colour checks: $(tr -d '\r' < "$out" | grep -E '^code_ps_first_use:|FAIL' | head -5)"; fail=1; fi
  hashes="$(tr -d '\r' < "$out" | sed -n 's/^D9H frame=.*hash=\([0-9a-f]*\).*/\1/p' | tr '\n' ' ')"
  if [ "$(echo $hashes | wc -w)" != 5 ]; then echo "  FAIL: $(echo $hashes | wc -w) frame hashes, want 5"; fail=1
  elif [ -z "$first_hashes" ]; then first_hashes=$hashes
  elif [ "$hashes" != "$first_hashes" ]; then echo "  FAIL: frames differ from the first arm's"; fail=1; fi
}

# The profiles' own values first, then every combination of the two overrides on each profile.
arm generic                 on  on  18 '>=18'
arm swtor                   $SWTOR_PS $SWTOR_IR $SWTOR_PHASEA $SWTOR_LIBS D9MT_APP_PROFILE=swtor
arm swtor-ps1-ir1           on  on  18 '>=18' D9MT_APP_PROFILE=swtor D9MT_PS_PREFETCH=1 D9MT_PS_IR_PREFETCH=1
arm swtor-ps0-ir1           off on  18 '=4'   D9MT_APP_PROFILE=swtor D9MT_PS_PREFETCH=0 D9MT_PS_IR_PREFETCH=1
arm swtor-ps0-ir0           off off 4  '=4'   D9MT_APP_PROFILE=swtor D9MT_PS_PREFETCH=0 D9MT_PS_IR_PREFETCH=0
arm swtor-ps1-ir0           on  off 18 '>=18' D9MT_APP_PROFILE=swtor D9MT_PS_PREFETCH=1 D9MT_PS_IR_PREFETCH=0
arm generic-ps0             off on  18 '=4'   D9MT_PS_PREFETCH=0
arm generic-ps0-ir0         off off 4  '=4'   D9MT_PS_PREFETCH=0 D9MT_PS_IR_PREFETCH=0
arm swtor-ps-legacy         on  off 18 '>=18' D9MT_APP_PROFILE=swtor D9MT_PS_PREFETCH=yes D9MT_PS_IR_PREFETCH=0
if tr -d '\r' < "$WORK/swtor-ps-legacy/logs/code_ps_first_use.err" 2>/dev/null \
     | grep -q 'D9MT_PS_PREFETCH is neither 0 nor 1, read as 1 (as before)'; then
  echo "  the warning for a value other than 0 or 1: logged"
else echo "  FAIL: no warning for D9MT_PS_PREFETCH=yes"; fail=1; fi

if [ "$fail" = 0 ]; then echo "test-ps-first-use: PASS"; else echo "test-ps-first-use: FAIL"; fi
exit "$fail"
