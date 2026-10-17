#!/usr/bin/env bash
# Build LLVM 15.0.7 as Mach-O static libs for the AIR unixlib.
#
# The AIR emitter (LLVM-dependent) lives on Wine's unix side (a Mach-O
# dylib companion like d9mtmetal), NOT in the PE d3d9.dll — so LLVM is a
# plain macOS build for the unix side's architecture.
# Recipe mirrors DXMT docs/DEVELOPMENT.md; no codegen targets, no tools —
# we only need Core/BitWriter/Support.
#
#   scripts/build-llvm-unixside.sh             x86_64 (Rosetta Wine) -> toolchains/llvm-x86_64
#   ARCH=arm64 scripts/build-llvm-unixside.sh  arm64 (arm64 Wine)    -> toolchains/llvm-arm64
#
# arm64 adds -ffp-contract=off: clang fuses a*b+c into fmadd on arm64 but not on x86_64
# (no FMA in the x86-64 baseline), so without it float results could differ in the last
# bit between the two builds (docs/arm64ec-port.md).
set -euo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
# Oldest macOS the unix side runs on (README: Requirements). Without it clang targets the
# SDK's version, and dyld refuses the library on older systems.
export MACOSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-26.0}"
TC="$V2/toolchains"
mkdir -p "$TC"
grep -q toolchains "$V2/.gitignore" || printf '\n# local LLVM toolchain builds\n/toolchains/\n' >> "$V2/.gitignore"

if [ ! -d "$TC/llvm-project" ]; then
  echo "[llvm] cloning llvmorg-15.0.7 (shallow)"
  git clone --depth 1 --branch llvmorg-15.0.7 \
    https://github.com/llvm/llvm-project.git "$TC/llvm-project"
fi

ARCH="${ARCH:-x86_64}"
case "$ARCH" in
  x86_64) BUILD_DIR="$TC/llvm-build"; FPFLAGS="" ;;
  arm64)  BUILD_DIR="$TC/llvm-build-arm64"; FPFLAGS=" -ffp-contract=off" ;;
  *) echo "FATAL: ARCH must be x86_64 or arm64" >&2; exit 1 ;;
esac

cmake -B "$BUILD_DIR" -S "$TC/llvm-project/llvm" \
  -DCMAKE_INSTALL_PREFIX="$TC/llvm-$ARCH" \
  -DCMAKE_OSX_ARCHITECTURES="$ARCH" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET="$MACOSX_DEPLOYMENT_TARGET" \
  -DLLVM_HOST_TRIPLE="$ARCH-apple-darwin" \
  -DLLVM_ENABLE_ASSERTIONS=Off \
  -DLLVM_ENABLE_ZSTD=Off \
  -DLLVM_ENABLE_ZLIB=Off \
  -DLLVM_ENABLE_LIBXML2=Off \
  -DLLVM_ENABLE_TERMINFO=Off \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_FLAGS="-ffile-prefix-map=$TC/llvm-project=llvm-project$FPFLAGS" \
  -DCMAKE_CXX_FLAGS="-ffile-prefix-map=$TC/llvm-project=llvm-project$FPFLAGS" \
  -DLLVM_TARGETS_TO_BUILD="" \
  -DLLVM_BUILD_TOOLS=Off \
  -DLLVM_INCLUDE_TESTS=Off \
  -DLLVM_INCLUDE_BENCHMARKS=Off \
  -DLLVM_INCLUDE_EXAMPLES=Off \
  -G Ninja

cmake --build "$BUILD_DIR"
cmake --install "$BUILD_DIR"
echo "[llvm] done: $TC/llvm-$ARCH"
