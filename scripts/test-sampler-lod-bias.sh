#!/usr/bin/env bash
# test-sampler-lod-bias.sh — the sampler LOD bias (driver/backend/mtl_sampler_bias.h) and the bias
# createSampler hands the bridge (test/native/sampler_lod_bias_test.cpp), under AddressSanitizer
# and UndefinedBehaviorSanitizer. sampler_fill_extract.py cuts createSampler's fill of
# WMTSamplerInfo out of mtl_device_impl.cpp by its anchors (exit 2 if one is missing or not
# unique), so the test compiles the driver's own lines.
#
#   scripts/test-sampler-lod-bias.sh [<rev>]     # default: this checkout's mtl_device_impl.cpp
#
# <rev> tests that commit's mtl_device_impl.cpp against this checkout's headers: a commit before
# the fix fails the fill cases. Writes only under build/native-tests/sampler-lod-bias.
set -euo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$V2/build/native-tests/sampler-lod-bias"
rm -rf "$OUT"; mkdir -p "$OUT"
SRC=$V2/driver/backend/mtl_device_impl.cpp
if [ $# -gt 0 ]; then
  git -C "$V2" show "$1:driver/backend/mtl_device_impl.cpp" > "$OUT/mtl_device_impl.cpp"
  SRC=$OUT/mtl_device_impl.cpp
fi
python3 -I "$V2/test/native/sampler_fill_extract.py" "$SRC" "$OUT"
clang++ -std=c++17 -g -O1 -Wall -fsanitize=address,undefined -fno-sanitize-recover=all \
  -I "$OUT" -I "$V2/bridge" -I "$V2/driver/backend" "$V2/test/native/sampler_lod_bias_test.cpp" -o "$OUT/slbt"
"$OUT/slbt"
