#!/usr/bin/env bash
# llvm-mingw for the ARM64EC PE side (d3d9.dll, winemetal.dll), pinned and
# checksummed. Sourced by build-driver.sh and build-bridge.sh for ARCH=arm64ec;
# run it directly to fetch the toolchain ahead of time.
#
#   scripts/llvm-mingw.sh            # download + verify if missing, print its root
#
# Env: LLVM_MINGW  toolchain root to use instead (skips the download)
#
# 20260616 ships LLVM 22.1.8, the version FEX-darwin builds its ARM64EC DLLs
# with. Bump deliberately: the ARM64EC driver's code generation depends on it.
# Installed outside the repo (it is ~600 MB) under
# ~/Library/Application Support/swtor-mac-proper/toolchains/.

LLVM_MINGW_RELEASE=20260616
LLVM_MINGW_NAME="llvm-mingw-$LLVM_MINGW_RELEASE-ucrt-macos-universal"
LLVM_MINGW_SHA256=2cab02a2e964bd4aae981150a45985d07c657cfa8d244959eb9e2dcc5eedd7b1
LLVM_MINGW_URL="https://github.com/mstorsjo/llvm-mingw/releases/download/$LLVM_MINGW_RELEASE/$LLVM_MINGW_NAME.tar.xz"
LLVM_MINGW_HOME="${LLVM_MINGW_HOME:-$HOME/Library/Application Support/swtor-mac-proper/toolchains}"

llvm_mingw_ensure() {
  if [ -n "${LLVM_MINGW:-}" ]; then
    [ -x "$LLVM_MINGW/bin/arm64ec-w64-mingw32-clang" ] \
      || { echo "FATAL: LLVM_MINGW=$LLVM_MINGW has no bin/arm64ec-w64-mingw32-clang" >&2; return 1; }
    return 0
  fi
  LLVM_MINGW="$LLVM_MINGW_HOME/$LLVM_MINGW_NAME"
  [ -x "$LLVM_MINGW/bin/arm64ec-w64-mingw32-clang" ] && return 0

  # Download into a fresh directory of its own, verify, then unpack.
  local dl="$LLVM_MINGW_HOME/dl-$LLVM_MINGW_NAME"
  mkdir -p "$LLVM_MINGW_HOME"
  rm -rf "$dl"
  mkdir "$dl"
  echo "[llvm-mingw] downloading $LLVM_MINGW_URL" >&2
  curl -fSL -o "$dl/$LLVM_MINGW_NAME.tar.xz" "$LLVM_MINGW_URL" || return 1
  echo "$LLVM_MINGW_SHA256  $dl/$LLVM_MINGW_NAME.tar.xz" | shasum -a 256 -c >&2 \
    || { echo "FATAL: llvm-mingw checksum mismatch" >&2; return 1; }
  tar -xf "$dl/$LLVM_MINGW_NAME.tar.xz" -C "$LLVM_MINGW_HOME" || return 1
  rm -rf "$dl"
  [ -x "$LLVM_MINGW/bin/arm64ec-w64-mingw32-clang" ] \
    || { echo "FATAL: unpacked llvm-mingw has no arm64ec-w64-mingw32-clang" >&2; return 1; }
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
  set -euo pipefail
  llvm_mingw_ensure
  echo "$LLVM_MINGW"
fi
