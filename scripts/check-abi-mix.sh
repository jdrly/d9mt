#!/usr/bin/env bash
# Static check of the d3d9.dll <-> winemetal mixes around the bundle's new bridge entry
# D9MTPsoArchiveCoverage (unix call 213):
#   - d3d9.dll must not import it (it is looked up with GetProcAddress), so a new d3d9.dll
#     still loads next to an older bridge, which has no such export: no signal, and the
#     bridge never consolidates;
#   - this tree's winemetal.dll exports it and its winemetal.so has the call in both tables;
#   - with OLD_BRIDGE=<an older winemetal.dll> (e.g. the shipping one): it lacks the export,
#     i.e. the mix this guards against is real.
# An older d3d9.dll next to this bridge never calls 213: no signal, no consolidation.
#   scripts/check-abi-mix.sh [OLD_BRIDGE=...]
set -uo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
DLL="$V2/build/driver-x64/d3d9.dll"; BRDLL="$V2/build/bridge/winemetal64.dll"
fails=0
# captured once: grep -q on a live pipe would SIGPIPE objdump, which pipefail reports
IMP=$(x86_64-w64-mingw32-objdump -p "$DLL" | awk '/DLL Name: winemetal.dll/{f=1;next} /DLL Name:/{f=0} f')
EXP=$(x86_64-w64-mingw32-objdump -p "$BRDLL")
ok() { echo "  $1: ok"; }; bad() { echo "  $1: FAIL"; fails=$((fails + 1)); }
imports() { x86_64-w64-mingw32-objdump -p "$1" | awk '/DLL Name: winemetal.dll/{f=1;next} /DLL Name:/{f=0} f'; }
exports() { x86_64-w64-mingw32-objdump -p "$1" | awk '/\[Ordinal\/Name Pointer\] Table/{f=1;next} f&&NF==0{f=0} f'; }
echo "$IMP" | grep -q D9MTPsoArchiveCoverage && bad "d3d9.dll doesn't import D9MTPsoArchiveCoverage" \
  || ok "d3d9.dll doesn't import D9MTPsoArchiveCoverage"
echo "$IMP" | grep -q D9MTAirCompile && ok "(control) d3d9.dll imports other winemetal entries" \
  || bad "(control) d3d9.dll imports other winemetal entries"
echo "$EXP" | grep -q D9MTPsoArchiveCoverage && ok "this winemetal.dll exports it" || bad "this winemetal.dll exports it"
grep -c '&_D9MTPsoArchiveCoverage_pooled' "$V2/bridge/unix/winemetal_unix.c" | grep -qx 2 \
  && ok "winemetal.so has it in both call tables" || bad "winemetal.so has it in both call tables"
if [ -n "${OLD_BRIDGE:-}" ]; then
  OLD=$(x86_64-w64-mingw32-objdump -p "$OLD_BRIDGE"); echo "$OLD" | grep -q D9MTPsoArchiveCoverage && bad "the old winemetal.dll lacks it" \
    || ok "the old winemetal.dll ($(basename "$OLD_BRIDGE")) lacks it"
fi
echo "[abi-mix] $fails failure(s)"
[ "$fails" -eq 0 ]
