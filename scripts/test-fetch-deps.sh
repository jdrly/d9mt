#!/usr/bin/env bash
# Tests scripts/fetch-deps.sh re-runs in a sandbox (a copy of the script and
# patches/dxbc-spirv, cloning from a local dxbc-spirv clone, no network):
# fresh, fully patched (with and without its record), partially patched,
# patch set changed (edited, added, removed), and the cases it must refuse
# without touching the checkout (a foreign local change, an unrecorded edited
# patch, patches that don't apply to the pin).
#
#   scripts/test-fetch-deps.sh [<dxbc-spirv clone with the pin>]
#
# Default clone: third_party/dxbc-spirv. Exit 0 when every case passes.
set -uo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
SRC="${1:-$V2/third_party/dxbc-spirv}"
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
export DXBC_SPIRV_URL="$SRC"
P="$T/v2/patches/dxbc-spirv"
D="$T/v2/third_party/dxbc-spirv"
mkdir -p "$T/v2/scripts" "$P"
cp "$V2/scripts/fetch-deps.sh" "$V2/scripts/check-deps.sh" "$T/v2/scripts/"
mkdir "$T/orig-patches" && cp "$V2"/patches/dxbc-spirv/*.patch "$T/orig-patches/"
ALL=("$T"/orig-patches/*.patch)
[ "${#ALL[@]}" -ge 2 ] || { echo "needs at least two patches"; exit 2; }
FIRST="$(basename "${ALL[0]}")"
SECOND="$(basename "${ALL[1]}")"
fails=0

fetch() { bash "$T/v2/scripts/fetch-deps.sh" > "$T/out" 2>&1; }
state() { git -C "$D" rev-parse HEAD; git -C "$D" diff; ls "$D/.git/d9mt-patches" 2>/dev/null; }
use_patches() { rm -f "$P"/*.patch; for p in "$@"; do cp "$p" "$P/"; done; }
# the current patches, applied to the pin, with nothing else changed
check_patched() {
  local want
  want="$(cd "$T" && rm -rf ref && git clone -q --no-checkout "$SRC" ref && git -C ref checkout -q --detach 64f078d685725b6bd5e1a045615705f47b6aa9de \
          && git -C ref apply "$P"/*.patch && git -C ref diff)"
  [ "$(git -C "$D" diff)" = "$want" ] && bash "$T/v2/scripts/check-deps.sh" >/dev/null 2>&1
}
ok() { echo "  PASS $1"; }
bad() { echo "  FAIL $1"; sed 's/^/       /' "$T/out"; fails=$((fails + 1)); }
expect_ok() { if fetch && check_patched; then ok "$1"; else bad "$1"; fi; }
expect_refused() {
  local before after
  before="$(state)"
  if fetch; then bad "$1 (not refused)"; return; fi
  after="$(state)"
  if [ "$before" = "$after" ] && grep -q "nothing done" "$T/out"; then ok "$1"; else bad "$1 (checkout changed)"; fi
}

echo "[fetch-deps test] source $SRC, patches: ${ALL[*]##*/}"
use_patches "${ALL[@]}"
expect_ok "fresh clone"
expect_ok "re-run, fully patched"

rm -rf "$D/.git/d9mt-patches"
expect_ok "re-run, fully patched without a record (older script)"

git -C "$D" checkout -q -- . && rm -rf "$D/.git/d9mt-patches" && git -C "$D" apply "$P/$FIRST"
expect_ok "partially patched without a record (first patch only)"

git -C "$D" checkout -q -- . && rm -rf "$D/.git/d9mt-patches"
git -C "$D" apply "$P/$FIRST" && mkdir -p "$D/.git/d9mt-patches" && cp "$P/$FIRST" "$D/.git/d9mt-patches/" \
  && echo "$FIRST" > "$D/.git/d9mt-patches/order"
expect_ok "partially patched with a record (first patch only), patch added since"

# edited patch: the recorded old version is undone, the new one applied
sed 's/^\(+.*\)\*\/$/\1 (edited) *\//' "${ALL[1]}" > "$T/edited.patch"
cmp -s "$T/edited.patch" "${ALL[1]}" && { echo "could not edit $SECOND"; exit 2; }
use_patches "${ALL[0]}" && cp "$T/edited.patch" "$P/$SECOND"
expect_ok "patch edited (record holds the old version)"
use_patches "${ALL[@]}"
expect_ok "patch edited back"

use_patches "${ALL[0]}"
expect_ok "patch removed"
use_patches "${ALL[@]}"
expect_ok "patch added"

echo "// local change" >> "$D/README.md"
expect_refused "foreign local change on top of the patches"
git -C "$D" checkout -q -- README.md
use_patches "${ALL[@]}" && fetch

# unrecorded edited patch: old version applied without a record
git -C "$D" checkout -q -- . && rm -rf "$D/.git/d9mt-patches"
git -C "$D" apply "${ALL[0]}" "$T/edited.patch"
expect_refused "edited patch applied without a record"
git -C "$D" checkout -q -- .

fetch && git -C "$D" checkout -q -- .   # patches gone by hand, record left behind
expect_ok "clean checkout with a stale record"

fetch
printf -- '--- a/README.md\n+++ b/README.md\n@@ -1 +1 @@\n-no such line\n+x\n' > "$P/9999-broken.patch"
expect_refused "patch that doesn't apply to the pin"
rm -f "$P/9999-broken.patch"
expect_ok "after removing it"

if [ "$fails" = 0 ]; then echo "[fetch-deps test] PASS"; else echo "[fetch-deps test] FAIL ($fails)"; fi
exit "$fails"
