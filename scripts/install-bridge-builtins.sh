#!/usr/bin/env bash
# install-bridge-builtins.sh: install OUR winemetal (the bridge fork with the
# vertex-descriptor PSO, sampler/VTF, Metal 4 and AIR-thunk additions) into a
# Wine engine's tree. Originals are backed up once as *.cx-orig.
#
#   CX=<engine root> scripts/install-bridge-builtins.sh
#   CX=<engine root> scripts/install-bridge-builtins.sh --restore
#
# Use a dedicated engine copy, not /Applications/CrossOver.app itself: this
# replaces the engine's builtin winemetal, which DXMT (D3D10/11) also uses,
# so DXMT games stop working on that engine (see README, Known issues).
set -euo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
CX="${CX:?set CX to the Wine engine root (a dedicated copy, see README)}"

pairs=(
  "$V2/build/bridge/winemetal32.dll|$CX/lib/wine/i386-windows/winemetal.dll"
  "$V2/build/bridge/winemetal64.dll|$CX/lib/wine/x86_64-windows/winemetal.dll"
  "$V2/build/bridge/winemetal.so|$CX/lib/wine/x86_64-unix/winemetal.so"
)

if [ "${1:-}" = "--restore" ]; then
  for p in "${pairs[@]}"; do
    dst="${p#*|}"
    [ -f "$dst.cx-orig" ] && cp "$dst.cx-orig" "$dst" && echo "restored $dst"
  done
  exit 0
fi

for p in "${pairs[@]}"; do
  src="${p%%|*}"; dst="${p#*|}"
  [ -f "$src" ] || { echo "FATAL: $src missing — run scripts/build-bridge.sh" >&2; exit 1; }
  [ -f "$dst" ] && [ ! -f "$dst.cx-orig" ] && cp "$dst" "$dst.cx-orig"
  cp "$src" "$dst"
  echo "installed $dst"
done
echo "done (restore: --restore)"
