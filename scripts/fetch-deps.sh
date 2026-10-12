#!/usr/bin/env bash
# Fetch the external source dependency: dxbc-spirv (SM1-3 bytecode parser +
# shader IR, MIT), pinned to the commit the build is tested with. It lands in
# third_party/dxbc-spirv (gitignored) and is compiled into d3d9.dll,
# winemetal.so and d9mtair.so by the build scripts.
#
#   scripts/fetch-deps.sh
#
# Env: DXBC_SPIRV_URL  clone source (default: upstream on GitHub; a local
#                      clone or mirror works too)
#      DXBC_SPIRV_REV  commit to check out (default: the pin below)
#
# The pin matches DXVK's subprojects/dxbc-spirv submodule at the time the
# driver was last synced. Bump it deliberately: the driver and the AIR
# compiler consume dxbc-spirv's IR directly, so IR changes can break them.
set -euo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
URL="${DXBC_SPIRV_URL:-https://github.com/doitsujin/dxbc-spirv.git}"
REV="${DXBC_SPIRV_REV:-c5c1a5b9b61773d59592f628faa39be9add26282}"
DST="$V2/third_party/dxbc-spirv"

if [ -L "$DST" ]; then
  echo "[deps] $DST is a symlink; leaving it alone"
  exit 0
fi

if [ ! -d "$DST/.git" ]; then
  echo "[deps] cloning dxbc-spirv -> $DST"
  rm -rf "$DST"
  git clone --no-checkout "$URL" "$DST"
fi

if ! git -C "$DST" cat-file -e "$REV^{commit}" 2>/dev/null; then
  echo "[deps] fetching $REV"
  git -C "$DST" fetch origin
fi
git -C "$DST" -c advice.detachedHead=false checkout --detach "$REV"
echo "[deps] dxbc-spirv at $(git -C "$DST" rev-parse --short HEAD)"
