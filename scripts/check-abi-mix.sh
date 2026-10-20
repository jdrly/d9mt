#!/usr/bin/env bash
# Static check of the d3d9.dll <-> winemetal mixes around the bundle's new bridge entries
# D9MTPsoArchiveCoverage (unix call 213) and D9MTPsoPauseHint (214), for each:
#   - d3d9.dll must not import it (it is looked up with GetProcAddress), so a new d3d9.dll
#     still loads next to an older bridge, which has no such export: no signal, and the
#     bridge never consolidates;
#   - this tree's winemetal.dll exports it and its winemetal.so has the call in both tables;
#   - with OLD_BRIDGE=<an older winemetal.dll> (e.g. the shipping one): it lacks the export,
#     i.e. the mix this guards against is real.
# An older d3d9.dll next to this bridge never calls them: no signal, no consolidation, no Reset
# flush (the idle and backstop flushes still run).
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
for e in D9MTPsoArchiveCoverage D9MTPsoPauseHint; do
  grep -q $e <<< "$IMP" && bad "d3d9.dll doesn't import $e" || ok "d3d9.dll doesn't import $e"
  grep -q $e <<< "$EXP" && ok "this winemetal.dll exports $e" || bad "this winemetal.dll exports $e"
  grep -c "&_${e}_pooled" "$V2/bridge/unix/winemetal_unix.c" | grep -qx 2 \
    && ok "winemetal.so has $e in both call tables" || bad "winemetal.so has $e in both call tables"
  if [ -n "${OLD_BRIDGE:-}" ]; then
    OLD=$(x86_64-w64-mingw32-objdump -p "$OLD_BRIDGE"); grep -q $e <<< "$OLD" && bad "the old winemetal.dll lacks $e" \
      || ok "the old winemetal.dll ($(basename "$OLD_BRIDGE")) lacks $e"
  fi
done
grep -q D9MTAirCompile <<< "$IMP" && ok "(control) d3d9.dll imports other winemetal entries" \
  || bad "(control) d3d9.dll imports other winemetal entries"
# both tables: 213 and 214 are the last two entries, in this order
for t in __wine_unix_call_funcs __wine_unix_call_wow64_funcs; do
  tail2=$(awk -v t="$t" '$0 ~ "const void \\*"t"\\[\\] = \\{" {f=1; next} f && /^};/ {f=0} f && /_pooled,/ {print}' "$V2/bridge/unix/winemetal_unix.c" | tail -2 | tr -d ' ,&' | paste -sd' ' -)
  [ "$tail2" = "_D9MTPsoArchiveCoverage_pooled _D9MTPsoPauseHint_pooled" ] && ok "$t ends with 213, 214" || bad "$t ends with 213, 214 (got: $tail2)"
done
# WMTSamplerInfo (sampler LOD bias, M-1): lod_bias_present in the old padding byte (23), the
# id stays at 24, lod_bias at 32, 40 bytes, for the unix side and both PE targets; the bridge
# reads lod_bias only behind the flag, and every caller zeroes the struct (an older d3d9.dll's
# 32 bytes: 0 at 23, never read past)
L=$(mktemp -d)
cat > "$L/l.c" <<'C'
#include <stddef.h>
#include "winemetal.h"
_Static_assert(offsetof(struct WMTSamplerInfo, lod_bias_present) == 23, "flag at 23");
_Static_assert(offsetof(struct WMTSamplerInfo, gpu_resource_id) == 24, "id at 24");
_Static_assert(offsetof(struct WMTSamplerInfo, lod_bias) == 32, "bias at 32");
_Static_assert(sizeof(struct WMTSamplerInfo) == 40, "40 bytes");
C
for cc in "clang -arch x86_64" x86_64-w64-mingw32-gcc i686-w64-mingw32-gcc; do
  $cc -fsyntax-only -I"$V2/bridge" "$L/l.c" 2> "$L/err" && ok "WMTSamplerInfo layout ($cc)" || { bad "WMTSamplerInfo layout ($cc)"; cat "$L/err"; }
done
rm -rf "$L"
# every read of info->lod_bias: the flag on the same line (a ternary) or the line before (an if)
unguarded=$(awk '/info->lod_bias([^_]|$)/ && !/lod_bias_present/ && prev !~ /if \(info->lod_bias_present\)/ {print FILENAME":"NR": "$0} {prev=$0}' "$V2/bridge/unix/winemetal_unix.c")
nreads=$(grep -cE 'info->lod_bias([^_]|$)' "$V2/bridge/unix/winemetal_unix.c")
[ -z "$unguarded" ] && [ "$nreads" -gt 0 ] && ok "the bridge reads lod_bias only behind lod_bias_present ($nreads reads)" \
  || { bad "the bridge reads lod_bias only behind lod_bias_present"; echo "$unguarded"; }
unzeroed=$(grep -rn 'WMTSamplerInfo [a-zA-Z_]* *;\|WMTSamplerInfo [a-zA-Z_]* *= *[^{ ]' "$V2/driver" "$V2/bridge/tests" 2>/dev/null)
[ -z "$unzeroed" ] && ok "every WMTSamplerInfo caller zeroes it" || { bad "every WMTSamplerInfo caller zeroes it"; echo "$unzeroed"; }

echo "[abi-mix] $fails failure(s)"
[ "$fails" -eq 0 ]
