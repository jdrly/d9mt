#!/usr/bin/env bash
# Build the forked winemetal bridge (v2/bridge): DXMT's winemetal + our
# additive entries (vertex-descriptor PSO, d9mtair AIR-compile thunks).
#
#   unix side : winemetal.so   (x86_64 Mach-O; links the d9mtair compile core
#               + lean LLVM; wine unix ABI via CrossOver's ntdll.so/winemac.so)
#   PE side   : winemetal.dll (x86_64) + winemetal32.dll (i686), builtin-marked
#   gate      : tests/bridge_smoke.cpp (vertex-descriptor PSO + AIR parity)
#
#   ARCH=arm64ec  arm64 Wine instead: winemetal.so for arm64 and winemetal.dll
#                 as ARM64EC (llvm-mingw, scripts/llvm-mingw.sh), in
#                 build/bridge-arm64ec. No i386 dll: 32-bit games run under
#                 FEX's WoW64 and keep the i386 build.
#
# Mirrors scripts/build-d9mtair.sh conventions; meson flags derived from
# upstream bridge/meson.build + bridge/unix/meson.build.
set -euo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
# Oldest macOS the unix side runs on (README: Requirements). Without it clang targets the
# SDK's version, and dyld refuses the library on older systems.
export MACOSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-26.0}"
BR="$V2/bridge"
DX="$V2/third_party/dxbc-spirv"
case "${ARCH:-x86}" in
  x86|x86_64|x64)
    PE_ARCH=x86; UNIX_ARCH=x86_64; ARCH_FLAGS=()
    OUT="$V2/build/bridge"; AIRLIB_OUT="$V2/build/airlib" ;;
  arm64ec)
    # -ffp-contract=off: clang fuses a*b+c into fmadd on arm64, never on
    # x86_64, so float results could differ (docs/arm64ec-port.md).
    PE_ARCH=arm64ec; UNIX_ARCH=arm64; ARCH_FLAGS=(-ffp-contract=off)
    OUT="$V2/build/bridge-arm64ec"; AIRLIB_OUT="$V2/build/airlib-arm64" ;;
  *) echo "FATAL: ARCH must be x86 (default) or arm64ec" >&2; exit 1 ;;
esac
OBJ="$OUT/obj"
mkdir -p "$OBJ"

# No Wine engine is needed at build time: the unix side resolves ntdll/winemac
# symbols at load time (-undefined dynamic_lookup below).

LLVM="$V2/toolchains/llvm-$UNIX_ARCH"
[ -f "$LLVM/lib/libLLVMBitWriter.a" ] || { echo "FATAL: run scripts/build-llvm-unixside.sh first" >&2; exit 1; }

# The airlib compile-core objects (AirBuilder + metallib writer + ir core).
# Always run build-d9mtair.sh (incremental by mtime) so edits under air/ never
# link a stale air_builder.o into winemetal.so.
ARCH="$UNIX_ARCH" bash "$V2/scripts/build-d9mtair.sh"

CORE_OBJS=("$OBJ"/../../airlib/obj/third_party_dxbc-spirv_ir_*.o \
           "$OBJ"/../../airlib/obj/third_party_dxbc-spirv_util_*.o \
           "$OBJ"/../../airlib/obj/air_air_builder.cpp.o \
           "$OBJ"/../../airlib/obj/air_air_metallib.cpp.o)

CXXFLAGS_UNIX=(-std=c++17 -O2 -arch "$UNIX_ARCH" -fvisibility=hidden -ffile-prefix-map="$V2"=.
               -I"$DX" -I"$V2" -I"$LLVM/include" -I"$BR" -I"$V2/airlib"
               -D__STDC_CONSTANT_MACROS -D__STDC_FORMAT_MACROS -D__STDC_LIMIT_MACROS
               ${ARCH_FLAGS[@]+"${ARCH_FLAGS[@]}"})
CFLAGS_UNIX=(-ObjC -O2 -arch "$UNIX_ARCH" -ffile-prefix-map="$V2"=. -I"$BR" -I"$BR/unix" -I"$V2/airlib"
             ${ARCH_FLAGS[@]+"${ARCH_FLAGS[@]}"})
LLVM_LIBS=("$LLVM"/lib/libLLVM*.a)

echo "[bridge] airlib compile core for the bridge (table symbols renamed:"
echo "         airlib/unix.cpp also defines __wine_unix_call_funcs; the bridge"
echo "         has its own tables, so airlib's are renamed out of the way)"
clang++ "${CXXFLAGS_UNIX[@]}" \
  -D__wine_unix_call_funcs=d9mtair_private_call_funcs \
  -D__wine_unix_call_wow64_funcs=d9mtair_private_wow64_funcs \
  -c "$V2/airlib/unix.cpp" -o "$OBJ/airlib_unix_renamed.o"

echo "[bridge] unix winemetal.so"
for src in "$BR/unix/winemetal_unix.c" "$BR/unix/cache.c" "$BR/unix/dxso_stubs.c"; do
  obj="$OBJ/$(basename "$src").o"
  if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ]; then
    echo "[cc] ${src#$BR/}"
    clang "${CFLAGS_UNIX[@]}" -c "$src" -o "$obj"
  fi
done

# exports: upstream's two tables + the d9mtair direct symbols (host testing)
cat > "$OUT/exports.list" <<'EOF'
___wine_unix_call_funcs
___wine_unix_call_wow64_funcs
_d9mtair_version
_d9mtair_compile
_d9mtair_free
EOF

clang++ -arch "$UNIX_ARCH" -dynamiclib \
  -o "$OUT/winemetal.so" \
  "$OBJ/winemetal_unix.c.o" "$OBJ/cache.c.o" "$OBJ/dxso_stubs.c.o" \
  "$OBJ/airlib_unix_renamed.o" \
  $(ls $AIRLIB_OUT/obj/third_party_dxbc-spirv_ir_*.o) \
  $(ls $AIRLIB_OUT/obj/third_party_dxbc-spirv_util_*.o 2>/dev/null || true) \
  "$AIRLIB_OUT/obj/air_air_builder.cpp.o" "$AIRLIB_OUT/obj/air_air_metallib.cpp.o" \
  "${LLVM_LIBS[@]}" \
  -install_name @rpath/winemetal.so \
  -Wl,-exported_symbols_list,"$OUT/exports.list" \
  -Wl,-U,_NtSetEvent -Wl,-U,_macdrv_get_metal_layer \
  -undefined dynamic_lookup \
  -weak_framework CoreFoundation -weak_framework Metal \
  -weak_framework Foundation -weak_framework CoreGraphics \
  -weak_framework QuartzCore -weak_framework ColorSync -weak_framework Cocoa \
  -framework MetalFX \
  -lsqlite3 -lm -lpthread
echo "  -> $OUT/winemetal.so ($(wc -c < "$OUT/winemetal.so") bytes)"

PE_SRC=("$BR/main.c" "$BR/winecrt0_compat.c" "$BR/winemetal_thunks.c" "$BR/airconv_thunks.c" "$BR/d9mtair_thunks.c")
if [ "$PE_ARCH" = arm64ec ]; then
  . "$V2/scripts/llvm-mingw.sh"
  llvm_mingw_ensure
  LMB="$LLVM_MINGW/bin"
  echo "[bridge] ntdll import lib (arm64ec)"
  # winecrt0_compat.c's ARM64EC path finds ntdll's unix-call dispatcher itself
  cat > "$OUT/ntdll-arm64ec.def" <<'EOF'
LIBRARY ntdll.dll
EXPORTS
  NtQueryVirtualMemory
  LdrGetDllHandle
  RtlFindExportedRoutineByName
EOF
  "$LMB/arm64ec-w64-mingw32-dlltool" -d "$OUT/ntdll-arm64ec.def" \
    -l "$OUT/libntdll-arm64ec.a" --dllname ntdll.dll
  echo "[bridge] PE winemetal dll (arm64ec)"
  mkdir -p "$OUT/arm64ec"
  "$LMB/arm64ec-w64-mingw32-gcc" -shared -O2 -s -ffile-prefix-map="$V2"=. "${ARCH_FLAGS[@]}" \
    -o "$OUT/arm64ec/winemetal.dll" \
    "${PE_SRC[@]}" -I"$BR" -I"$V2/airlib" \
    "$OUT/libntdll-arm64ec.a" -Wl,--file-alignment,0x1000
  cp "$OUT/arm64ec/winemetal.dll" "$OUT/winemetal-arm64ec.dll"
  python3 "$V2/scripts/make-builtin.py" "$OUT/winemetal-arm64ec.dll"
  echo "[bridge] import lib for the driver"
  bash "$V2/scripts/gen-bridge-def.sh" > "$OUT/winemetal.def"
  echo "  $(grep -c '^  ' "$OUT/winemetal.def") exports -> $OUT/winemetal.def"
fi

if [ "$PE_ARCH" = x86 ]; then
echo "[bridge] ntdll import libs"
cat > "$OUT/ntdll-cx32.def" <<'EOF'
LIBRARY ntdll.dll
EXPORTS
  __wine_unix_call@16
  NtQueryVirtualMemory@24
EOF
i686-w64-mingw32-dlltool -k -d "$OUT/ntdll-cx32.def" \
  -l "$OUT/libntdll-cx32.a" --dllname ntdll.dll
cat > "$OUT/ntdll-cx64.def" <<'EOF'
LIBRARY ntdll.dll
EXPORTS
  __wine_unix_call
  NtQueryVirtualMemory
EOF
x86_64-w64-mingw32-dlltool -d "$OUT/ntdll-cx64.def" \
  -l "$OUT/libntdll-cx64.a" --dllname ntdll.dll

echo "[bridge] PE winemetal dlls"
mkdir -p "$OUT/i386" "$OUT/x86_64"
i686-w64-mingw32-gcc -shared -O2 -s -ffile-prefix-map="$V2"=. -o "$OUT/i386/winemetal.dll" \
  "${PE_SRC[@]}" -I"$BR" -I"$V2/airlib" \
  "$OUT/libntdll-cx32.a" -Wl,--file-alignment,0x1000
x86_64-w64-mingw32-gcc -shared -O2 -s -ffile-prefix-map="$V2"=. -o "$OUT/x86_64/winemetal.dll" \
  "${PE_SRC[@]}" -I"$BR" -I"$V2/airlib" \
  "$OUT/libntdll-cx64.a" -Wl,--file-alignment,0x1000
cp "$OUT/i386/winemetal.dll" "$OUT/winemetal32.dll"
cp "$OUT/x86_64/winemetal.dll" "$OUT/winemetal64.dll"

python3 "$V2/scripts/make-builtin.py" "$OUT/winemetal32.dll" "$OUT/winemetal64.dll"

echo "[bridge] import lib for the driver"
bash "$V2/scripts/gen-bridge-def.sh" > "$OUT/winemetal.def"
echo "  $(grep -c '^  ' "$OUT/winemetal.def") exports -> $OUT/winemetal.def"
i686-w64-mingw32-dlltool -d "$OUT/winemetal.def" \
  -l "$OUT/libwinemetal32.a" --dllname winemetal.dll
fi

# Smoke test (unix-side arch on this host, needs a Metal GPU): vertex-descriptor PSO, MTL4
# path and AIR parity against reference metallibs from the host AIR compiler
# (scripts/build-air-compiler.sh, which needs Homebrew llvm@15 + a
# SPIRV-Cross checkout). Skipped when those references can't be built;
# SMOKE=0 skips it explicitly, SMOKE=1 makes a missing reference fatal.
SMOKE="${SMOKE:-auto}"
if [ "$SMOKE" != 0 ] && [ ! -f "$V2/build/air-compiler/vs.metallib" ]; then
  bash "$V2/scripts/build-air-compiler.sh" >/dev/null 2>&1 || {
    [ "$SMOKE" = 1 ] && { echo "FATAL: build-air-compiler.sh failed (smoke test references)" >&2; exit 1; }
    echo "[bridge] smoke test skipped: no reference metallibs (scripts/build-air-compiler.sh failed)"
    SMOKE=0
  }
fi
if [ "$SMOKE" != 0 ]; then
  echo "[bridge] smoke test ($UNIX_ARCH host)"
  clang++ "${CXXFLAGS_UNIX[@]}" \
    "$BR/tests/bridge_smoke.cpp" \
    $(ls $AIRLIB_OUT/obj/third_party_dxbc-spirv_*.o) \
    "$AIRLIB_OUT/obj/air_air_builder.cpp.o" "$AIRLIB_OUT/obj/air_air_metallib.cpp.o" \
    "${LLVM_LIBS[@]}" -lm -lpthread \
    -o "$OUT/bridge_smoke"
  (cd "$V2" && "$OUT/bridge_smoke" \
    "$OUT/winemetal.so" "$AIRLIB_OUT/d9mtair.so" \
    build/air-compiler/vs.metallib build/air-compiler/ps.metallib \
    air/tests/bin/vs_mvp.bin)
fi

echo "[bridge] done"
