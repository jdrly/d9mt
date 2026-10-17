#!/usr/bin/env bash
# Prove that the shipping x86 builds of this tree are byte-identical to a base
# commit's: winemetal.so (its LC_UUID keys every user's AIR cache),
# d9mtair.so, winemetal32/64.dll and both d3d9.dll (debug info included).
#
#   scripts/check-x86-identity.sh [base-ref [head-ref]]
#
# base-ref defaults to main; without head-ref the working tree is checked
# (uncommitted changes included), with it that commit.
#
# Both sides are built with the repo's scripts in the same directory,
# build/x86-identity/tree: commits exported with `git archive` (no
# worktree), the working tree as a copy of its tracked and untracked files.
# Both share this tree's toolchains/ and
# third_party/dxbc-spirv. Two things make PE builds differ that are not code:
#  - ld's automatic image base for a mingw DLL is a hash of the output path,
#    so the same commit built in two directories differs; hence one directory;
#  - the PE timestamps (and the checksums over them): SOURCE_DATE_EPOCH is
#    pinned.
# The base's artifacts are kept per commit in build/x86-identity/<commit>.
set -euo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
BASE_REF="${1:-main}"
BASE_SHA="$(git -C "$V2" rev-parse --verify "$BASE_REF^{commit}")"
export SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-1760000000}"
ARTIFACTS=(build/bridge/winemetal.so build/bridge/winemetal32.dll build/bridge/winemetal64.dll
           build/airlib/d9mtair.so build/driver-x64/d3d9.dll build/driver/d3d9.dll)
LOG="$V2/build/x86-identity"
mkdir -p "$LOG"

TREE="$LOG/tree"
link_deps() { # <tree>
  ln -s "$V2/toolchains" "$1/toolchains"
  mkdir -p "$1/third_party"
  ln -s "$V2/third_party/dxbc-spirv" "$1/third_party/dxbc-spirv"
}
build_tree() { # <log prefix>
  ( cd "$TREE"
    SMOKE=0 ARCH=x86 bash scripts/build-bridge.sh > "$1-bridge.log" 2>&1
    ARCH=x64 bash scripts/build-driver.sh > "$1-driver-x64.log" 2>&1
    ARCH=x86 bash scripts/build-driver.sh > "$1-driver-x86.log" 2>&1 )
}
keep() { # <dir>
  mkdir -p "$1"
  for f in "${ARTIFACTS[@]}"; do cp "$TREE/$f" "$1/$(echo "$f" | tr / _)"; done
}

REF="$LOG/$BASE_SHA"
if [ ! -f "$REF/done" ]; then
  echo "[identity] building base $BASE_REF (${BASE_SHA:0:12})"
  rm -rf "$TREE" "$REF"
  mkdir -p "$TREE"
  git -C "$V2" archive "$BASE_SHA" | tar -x -C "$TREE"
  link_deps "$TREE"
  build_tree "$LOG/base" || { echo "FATAL: base build failed, see $LOG/base-*.log" >&2; exit 1; }
  keep "$REF"
  touch "$REF/done"
fi

rm -rf "$TREE"
mkdir -p "$TREE"
if [ -n "${2:-}" ]; then
  HEAD_SHA="$(git -C "$V2" rev-parse --verify "$2^{commit}")"
  echo "[identity] building $2 (${HEAD_SHA:0:12})"
  git -C "$V2" archive "$HEAD_SHA" | tar -x -C "$TREE"
else
  echo "[identity] building the working tree (with uncommitted changes)"
  git -C "$V2" ls-files -z --cached --others --exclude-standard \
    | (cd "$V2" && tar -cf - --null --no-recursion -T -) | tar -x -C "$TREE"
fi
link_deps "$TREE"
build_tree "$LOG/head" || { echo "FATAL: build failed, see $LOG/head-*.log" >&2; exit 1; }
keep "$LOG/head"

rc=0
for f in "${ARTIFACTS[@]}"; do
  a="$REF/$(echo "$f" | tr / _)"; b="$LOG/head/$(echo "$f" | tr / _)"
  if cmp -s "$a" "$b"; then
    echo "  IDENTICAL $f"
  else
    echo "  DIFFERENT $f ($(cmp -l "$a" "$b" 2>/dev/null | wc -l | tr -d ' ') bytes differ, sizes $(wc -c < "$a") $(wc -c < "$b"))"
    rc=1
  fi
done
for t in "$REF" "$LOG/head"; do
  dwarfdump --uuid "$t/build_bridge_winemetal.so" | sed -e 's/^/  /' -e "s|$LOG/||"
done
[ "$rc" = 0 ] && echo "[identity] PASS: x86 builds of ${2:-the working tree} identical to $BASE_REF" || echo "[identity] FAIL"
exit "$rc"
