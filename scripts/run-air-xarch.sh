#!/usr/bin/env bash
# Cross-architecture determinism gate for the shader compile path: the
# arm64 build (build/bridge-arm64ec/winemetal.so + the arm64 front end) must
# produce byte-identical IR, metallibs and reflection to the x86_64 build
# (build/bridge/winemetal.so + the x86_64 front end, run under Rosetta) for
# every shader and variant. The AIR cache key covers the bytes, so this is
# what keeps arm64 and x86_64 users' rendering identical.
#
#   scripts/run-air-xarch.sh [count] [seed]
#
# Corpus: air/tests/bin/*.bin, the test programs' shaders (test/*.inc) and
# <count> (default 3000) random shaders from scripts/sm3_corpus.py (seed 1).
# Needs both bridge builds: scripts/build-bridge.sh and
# ARCH=arm64ec scripts/build-bridge.sh. KEEP=1 keeps every artifact under
# build/air-xarch/<arch>/ for diffing.
set -euo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
COUNT="${1:-3000}"
SEED="${2:-1}"
OUT="$V2/build/air-xarch"
CORPUS="$OUT/corpus"
DX="$V2/third_party/dxbc-spirv"

# Never touch a user's caches: the dumped compile entry point has no cache,
# but keep everything in /tmp in case that ever changes.
export D9MT_AIR_CACHE=/tmp/d9mt-xarch-air-cache.sqlite3
export D9MT_PSO_ARCHIVE_DIR=/tmp/d9mt-xarch-pso

for f in build/bridge/winemetal.so build/bridge-arm64ec/winemetal.so; do
  [ -f "$V2/$f" ] || { echo "FATAL: $f missing (run the bridge builds first)" >&2; exit 1; }
done

rm -rf "$CORPUS"
mkdir -p "$CORPUS"
cp "$V2"/air/tests/bin/*.bin "$CORPUS/"
for inc in "$V2"/test/*_bytecode.inc; do
  python3 -I -c 'import sys; open(sys.argv[2], "wb").write(bytes(int(x, 0) for x in open(sys.argv[1]).read().replace(",", " ").split()))' \
    "$inc" "$CORPUS/$(basename "$inc" .inc).bin"
done
python3 -I "$V2/scripts/sm3_corpus.py" gen "$CORPUS/gen" "$COUNT" "$SEED"
mv "$CORPUS"/gen/*.bin "$CORPUS/"
BINS=("$CORPUS"/*.bin)
echo "[xarch] corpus: ${#BINS[@]} shaders (seed $SEED)"

build() { # <arch> <airlib obj dir>
  # the arm64 objects are built with -ffp-contract=off (build-d9mtair.sh); so is this
  local fl=(); [ "$1" = arm64 ] && fl=(-ffp-contract=off)
  clang++ -std=c++17 -O2 -arch "$1" ${fl[@]+"${fl[@]}"} -I"$DX" -I"$V2/airlib" \
    "$V2/airlib/xarch_dump.cpp" "$2"/third_party_dxbc-spirv_*.o \
    -o "$OUT/xarch_dump-$1"
}
build x86_64 "$V2/build/airlib/obj"
build arm64 "$V2/build/airlib-arm64/obj"

run() { # <arch> <winemetal.so>
  local dir=-
  if [ "${KEEP:-0}" = 1 ]; then dir="$OUT/$1"; rm -rf "$dir"; mkdir -p "$dir"; fi
  arch "-$1" "$OUT/xarch_dump-$1" "$2" "$dir" "${BINS[@]}" > "$OUT/manifest-$1.txt"
}
time run x86_64 "$V2/build/bridge/winemetal.so"
time run arm64 "$V2/build/bridge-arm64ec/winemetal.so"

lines="$(wc -l < "$OUT/manifest-arm64.txt" | tr -d ' ')"
ml="$(grep -c ' metallib ' "$OUT/manifest-arm64.txt" || true)"
rej="$(grep -c ' reject$' "$OUT/manifest-arm64.txt" || true)"
ab="$(grep -cE " (abort|exit) [0-9]+$" "$OUT/manifest-arm64.txt" || true)"
if cmp -s "$OUT/manifest-x86_64.txt" "$OUT/manifest-arm64.txt"; then
  echo "[xarch] PASS: $lines manifest lines identical ($ml metallibs, $rej front-end rejects, $ab front-end aborts)"
else
  echo "[xarch] FAIL: manifests differ:"
  diff "$OUT/manifest-x86_64.txt" "$OUT/manifest-arm64.txt" | head -40
  exit 1
fi
