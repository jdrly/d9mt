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
# The patches stay uncommitted changes in the checkout. The script records
# what it applied (copies of the patches in .git/d9mt-patches/), so a re-run
# undoes exactly that, whatever patches/dxbc-spirv holds by then (patches
# added, edited or removed), checks out the pin and applies the current set.
# Before it touches anything it proves that the checkout is exactly HEAD plus
# those patches (comparing git trees in a scratch index); a checkout with any
# other local change is left as it is and the script fails. A checkout
# patched without a record (an older version of this script) is recognized
# when it holds the current patches or a leading part of them.
set -euo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
URL="${DXBC_SPIRV_URL:-https://github.com/doitsujin/dxbc-spirv.git}"
REV="${DXBC_SPIRV_REV:-64f078d685725b6bd5e1a045615705f47b6aa9de}"
DST="$V2/third_party/dxbc-spirv"
PATCHES=("$V2"/patches/dxbc-spirv/*.patch)
STAMP="$DST/.git/d9mt-patches"

if [ -L "$DST" ]; then
  echo "[deps] $DST is a symlink; leaving it alone"
  bash "$V2/scripts/check-deps.sh"
  exit 0
fi

SCRATCH="$(mktemp -d)"
trap 'rm -rf "$SCRATCH"' EXIT

# Tree of the checkout's tracked files as they are (untracked files ignored).
worktree_tree() {
  GIT_INDEX_FILE="$SCRATCH/wt" git -C "$DST" read-tree HEAD
  GIT_INDEX_FILE="$SCRATCH/wt" git -C "$DST" add -u
  GIT_INDEX_FILE="$SCRATCH/wt" git -C "$DST" write-tree
}
# Tree of <commit> with the given patches applied, in order; fails if one doesn't apply.
patched_tree() {
  local base=$1; shift
  rm -f "$SCRATCH/pt"
  GIT_INDEX_FILE="$SCRATCH/pt" git -C "$DST" read-tree "$base"
  GIT_INDEX_FILE="$SCRATCH/pt" git -C "$DST" apply --cached "$@" 2>/dev/null || return 1
  GIT_INDEX_FILE="$SCRATCH/pt" git -C "$DST" write-tree
}

FRESH=0
if [ ! -d "$DST/.git" ]; then
  echo "[deps] cloning dxbc-spirv -> $DST"
  rm -rf "$DST"
  git clone --no-checkout "$URL" "$DST"
  FRESH=1
fi
# Neither fetching nor these checks touch the checkout's files.
if ! git -C "$DST" cat-file -e "$REV^{commit}" 2>/dev/null; then
  echo "[deps] fetching $REV"
  git -C "$DST" fetch origin
fi
if ! patched_tree "$REV" "${PATCHES[@]}" >/dev/null; then
  echo "FATAL: patches/dxbc-spirv doesn't apply to $REV; nothing done" >&2
  exit 1
fi

if [ "$FRESH" = 0 ]; then
  # Find which patches the checkout holds: it must be exactly HEAD plus
  # them, nothing else, before anything is touched. First the recorded
  # set, else (older script, no record) the current set or a leading part.
  WT="$(worktree_tree)"
  APPLIED=()
  if [ "$WT" != "$(git -C "$DST" rev-parse 'HEAD^{tree}')" ]; then
    if [ -f "$STAMP/order" ]; then
      while IFS= read -r name; do APPLIED+=("$STAMP/$name"); done < "$STAMP/order"
      [ "$(patched_tree HEAD "${APPLIED[@]}" || true)" = "$WT" ] || APPLIED=()
    fi
    if [ "${#APPLIED[@]}" -eq 0 ]; then
      for ((n = ${#PATCHES[@]}; n > 0; n--)); do
        if [ "$(patched_tree HEAD "${PATCHES[@]:0:n}" || true)" = "$WT" ]; then
          APPLIED=("${PATCHES[@]:0:n}")
          break
        fi
      done
    fi
    if [ "${#APPLIED[@]}" -eq 0 ]; then
      echo "FATAL: $DST has local changes that aren't the patches this script applied" >&2
      echo "       (or patches/dxbc-spirv); nothing done. Inspect: git -C $DST diff;" >&2
      echo "       start over: git -C $DST checkout -- ." >&2
      exit 1
    fi
    UNDO=()
    for ((i = ${#APPLIED[@]} - 1; i >= 0; i--)); do UNDO+=("${APPLIED[i]}"); done
    echo "[deps] undoing ${#UNDO[@]} applied patches"
    git -C "$DST" apply --reverse "${UNDO[@]}"
  fi
  rm -rf "$STAMP"
fi

git -C "$DST" -c advice.detachedHead=false checkout --detach "$REV"
git -C "$DST" apply "${PATCHES[@]}"
mkdir -p "$STAMP"
for p in "${PATCHES[@]}"; do
  cp "$p" "$STAMP/"
  basename "$p" >> "$STAMP/order"
  echo "[deps] applied $(basename "$p")"
done
echo "[deps] dxbc-spirv at $(git -C "$DST" rev-parse --short HEAD) + ${#PATCHES[@]} patches"
