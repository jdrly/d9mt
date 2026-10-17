#!/usr/bin/env bash
# Fetch the external source dependency: dxbc-spirv (SM1-3 bytecode parser +
# shader IR, MIT), pinned to the commit the build is tested with, and apply
# d9mt's fixes from patches/dxbc-spirv/. It lands in third_party/dxbc-spirv
# (gitignored) and is compiled into d3d9.dll, winemetal.so and d9mtair.so by
# the build scripts.
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
#
# The patches stay uncommitted changes in the checkout. Running the script
# again reverts them, checks out the pin and applies them again; a checkout
# with other local changes is left alone and the script fails.
set -euo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
URL="${DXBC_SPIRV_URL:-https://github.com/doitsujin/dxbc-spirv.git}"
REV="${DXBC_SPIRV_REV:-c5c1a5b9b61773d59592f628faa39be9add26282}"
DST="$V2/third_party/dxbc-spirv"
PATCHES=("$V2"/patches/dxbc-spirv/*.patch)

if [ -L "$DST" ]; then
  echo "[deps] $DST is a symlink; leaving it alone"
  bash "$V2/scripts/check-deps.sh"
  exit 0
fi

if [ ! -d "$DST/.git" ]; then
  echo "[deps] cloning dxbc-spirv -> $DST"
  rm -rf "$DST"
  git clone --no-checkout "$URL" "$DST"
elif [ -n "$(git -C "$DST" status --porcelain --untracked-files=no)" ]; then
  # Our patches from a previous run: revert them, newest first.
  for ((i = ${#PATCHES[@]} - 1; i >= 0; i--)); do
    if ! git -C "$DST" apply --reverse --check "${PATCHES[i]}" 2>/dev/null; then
      echo "FATAL: $DST has local changes other than patches/dxbc-spirv; commit or discard them" >&2
      exit 1
    fi
    git -C "$DST" apply --reverse "${PATCHES[i]}"
  done
  if [ -n "$(git -C "$DST" status --porcelain --untracked-files=no)" ]; then
    echo "FATAL: $DST has local changes other than patches/dxbc-spirv; commit or discard them" >&2
    exit 1
  fi
fi

if ! git -C "$DST" cat-file -e "$REV^{commit}" 2>/dev/null; then
  echo "[deps] fetching $REV"
  git -C "$DST" fetch origin
fi
git -C "$DST" -c advice.detachedHead=false checkout --detach "$REV"
for p in "${PATCHES[@]}"; do
  echo "[deps] applying $(basename "$p")"
  git -C "$DST" apply "$p"
done
echo "[deps] dxbc-spirv at $(git -C "$DST" rev-parse --short HEAD) + ${#PATCHES[@]} patches"
