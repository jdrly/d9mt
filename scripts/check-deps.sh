#!/usr/bin/env bash
# Fail unless third_party/dxbc-spirv has every patch of patches/dxbc-spirv/
# applied (scripts/fetch-deps.sh applies them), so no build ships the
# converter without its fixes. Works on git checkouts and plain copies.
set -euo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
DST="$V2/third_party/dxbc-spirv"
[ -d "$DST/sm3" ] || { echo "FATAL: $DST missing; run scripts/fetch-deps.sh" >&2; exit 1; }
for p in "$V2"/patches/dxbc-spirv/*.patch; do
  # A reverse dry run succeeds only if the patch is in the tree. patch(1),
  # not git apply: a plain copy inside another repository confuses git.
  if ! patch -p1 -R --dry-run -s -f -d "$DST" < "$p" >/dev/null 2>&1; then
    echo "FATAL: $DST lacks $(basename "$p"); run scripts/fetch-deps.sh" >&2
    exit 1
  fi
done
