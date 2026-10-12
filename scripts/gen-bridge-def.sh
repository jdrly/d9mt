#!/usr/bin/env bash
# Print winemetal.def (the export list of the forked winemetal bridge) to
# stdout. It is derived from the WINEMETAL_API declarations in the bridge
# headers, so the driver's import lib always matches bridge/ without a
# prebuilt DLL or a DXMT checkout.
#
#   scripts/gen-bridge-def.sh > build/bridge/winemetal.def
#
# Declarations may span lines ("WINEMETAL_API obj_handle_t\nName(...)");
# comments are stripped first. The #define lines are skipped.
set -euo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
BR="$V2/bridge"

echo "LIBRARY winemetal.dll"
echo "EXPORTS"
perl -0777 -ne '
  s{/\*.*?\*/}{}gs;
  s{//[^\n]*}{}g;
  while (/^\s*WINEMETAL_API\b(?!\s+WINEMETAL_IMPORT)([^;{(]*?)\b([A-Za-z_]\w*)\s*\(/gm) {
    print "  $2\n";
  }
' "$BR/winemetal.h" "$BR/d9mtair_thunks.h" | sort -u
