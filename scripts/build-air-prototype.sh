#!/usr/bin/env bash
# Build + run the AIR emission prototype (host, not mingw; built for the LLVM libraries' architecture).
#
# 1. air_prototype    — hand-builds an AIR LLVM module (llvm@15) and wraps it
#                       in our own metallib container.
# 2. metallib_load_test — loads the result with newLibraryWithData, builds a
#                       PSO, renders offscreen, verifies pixels.
#
# Requires LLVM 15.0.7 (AIR's LLVM era): d9mt's own toolchain or Homebrew's llvm@15.
set -euo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$V2/build/air-proto"
mkdir -p "$BUILD"

# LLVM 15: $LLVM_CONFIG, else d9mt's own toolchain (toolchains/llvm-build, x86_64:
# `ninja -C toolchains/llvm-build llvm-config llvm-dis`), else Homebrew's llvm@15.
if [ -z "${LLVM_CONFIG:-}" ]; then
  for c in "$V2/toolchains/llvm-build/bin/llvm-config" /opt/homebrew/opt/llvm@15/bin/llvm-config; do
    [ -x "$c" ] && { LLVM_CONFIG=$c; break; }
  done
fi
[ -n "${LLVM_CONFIG:-}" ] && [ -x "$LLVM_CONFIG" ] \
  || { echo "FATAL: need LLVM 15's llvm-config (ninja -C toolchains/llvm-build llvm-config, brew llvm@15, or LLVM_CONFIG)" >&2; exit 1; }
# AIR's LLVM era is exactly 15.0.7: no other version, and no falling back to another candidate.
LLVM_VERSION="$("$LLVM_CONFIG" --version 2>/dev/null || true)"
[ "$LLVM_VERSION" = 15.0.7 ] \
  || { echo "FATAL: $LLVM_CONFIG is LLVM '${LLVM_VERSION:-unknown}', not 15.0.7 (AIR's LLVM)" >&2; exit 1; }
# Build for the architecture of the LLVM libraries: d9mt's toolchain is x86_64 (the tools then run
# under Rosetta); Homebrew's is arm64.
LLVM_ARCH=$(lipo -archs "$("$LLVM_CONFIG" --libdir)/libLLVMCore.a" 2>/dev/null | awk '{print $1}')
LLVM_ARCH=${LLVM_ARCH:-$(uname -m)}

echo "[air-proto] llvm $LLVM_VERSION"

CXXFLAGS="-arch $LLVM_ARCH $("$LLVM_CONFIG" --cxxflags) -std=c++17 -O1 -g"
LDFLAGS="$("$LLVM_CONFIG" --ldflags) $("$LLVM_CONFIG" --libs bitwriter core support) $("$LLVM_CONFIG" --system-libs)"

echo "[air-proto] building air_prototype"
clang++ $CXXFLAGS \
  "$V2/air/tools/air_prototype.cpp" \
  "$V2/air/air_metallib.cpp" \
  $LDFLAGS \
  -o "$BUILD/air_prototype"

echo "[air-proto] building metallib_load_test"
clang++ -std=c++17 -O1 -g -fobjc-arc \
  "$V2/air/tools/metallib_load_test.mm" \
  -framework Metal -framework Foundation \
  -o "$BUILD/metallib_load_test"

echo "[air-proto] emitting metallib"
"$BUILD/air_prototype" "$BUILD/proto.metallib" "$@"

echo "[air-proto] load + render test"
"$BUILD/metallib_load_test" "$BUILD/proto.metallib"
