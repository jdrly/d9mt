#!/usr/bin/env bash
# Build the forked winemetal bridge (v2/bridge): DXMT's winemetal + our
# additive entries (vertex-descriptor PSO, d9mtair AIR-compile thunks).
#
#   unix side : winemetal.so   (x86_64 Mach-O; links the d9mtair compile core
#               + lean LLVM; wine unix ABI via CrossOver's ntdll.so/winemac.so)
#   PE side   : winemetal.dll (x86_64) + winemetal32.dll (i686), builtin-marked
#   gate      : tests/bridge_smoke.cpp (vertex-descriptor PSO + AIR parity)
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
OUT="$V2/build/bridge"
AIRLIB_OUT="$V2/build/airlib"
OBJ="$OUT/obj"
mkdir -p "$OBJ"

# No Wine engine is needed at build time: the unix side resolves ntdll/winemac
# symbols at load time (-undefined dynamic_lookup below).

LLVM="$V2/toolchains/llvm-x86_64"
[ -f "$LLVM/lib/libLLVMBitWriter.a" ] || { echo "FATAL: run scripts/build-llvm-unixside.sh first" >&2; exit 1; }

# The airlib compile-core objects (AirBuilder + metallib writer + ir core).
# Always run build-d9mtair.sh (incremental by mtime) so edits under air/ never
# link a stale air_builder.o into winemetal.so.
bash "$V2/scripts/build-d9mtair.sh"

CXXFLAGS_UNIX=(-std=c++17 -O2 -arch x86_64 -fvisibility=hidden -ffile-prefix-map="$V2"=.
               -I"$DX" -I"$V2" -I"$LLVM/include" -I"$BR" -I"$V2/airlib"
               -D__STDC_CONSTANT_MACROS -D__STDC_FORMAT_MACROS -D__STDC_LIMIT_MACROS)
CFLAGS_UNIX=(-ObjC -O2 -arch x86_64 -ffile-prefix-map="$V2"=. -I"$BR" -I"$BR/unix" -I"$V2/airlib")
LLVM_LIBS=("$LLVM"/lib/libLLVM*.a)

echo "[bridge] airlib compile core for the bridge (table symbols renamed:"
echo "         airlib/unix.cpp also defines __wine_unix_call_funcs; the bridge"
echo "         has its own tables, so airlib's are renamed out of the way)"
clang++ "${CXXFLAGS_UNIX[@]}" \
  -D__wine_unix_call_funcs=d9mtair_private_call_funcs \
  -D__wine_unix_call_wow64_funcs=d9mtair_private_wow64_funcs \
  -c "$V2/airlib/unix.cpp" -o "$OBJ/airlib_unix_renamed.o"

echo "[bridge] unix winemetal.so"
for src in "$BR/unix/winemetal_unix.c" "$BR/unix/air_cache.c" "$BR/unix/cache.c" "$BR/unix/dxso_stubs.c"; do
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

# The AIR cache's build identity (S1, docs/bridge-bundle.md): one list of everything that
# decides the bytes an AIR compile returns: the codegen (AirBuilder, metallib writer, IR
# core, the d9mtair compile thunk, LLVM) and the cache in front of it (air_cache.c, the
# sqlite store cache.c). The link takes exactly this list; the Metal thunks
# (winemetal_unix.c) aren't in it, so changing them keeps every user's AIR cache. The
# objects are path-free (-ffile-prefix-map) and reproducible, so the hash changes when
# their contents do (also with a new clang/Xcode or LLVM build: a cold cache, never a
# stale one). Release builds are clean builds: build-d9mtair.sh rebuilds by .cpp mtime,
# so an edit to an air/ header alone isn't compiled in (nor hashed).
shopt -s nullglob
CODEGEN_OBJS=("$AIRLIB_OUT"/obj/third_party_dxbc-spirv_ir_*.o "$AIRLIB_OUT"/obj/third_party_dxbc-spirv_util_*.o
              "$AIRLIB_OUT/obj/air_air_builder.cpp.o" "$AIRLIB_OUT/obj/air_air_metallib.cpp.o"
              "$OBJ/airlib_unix_renamed.o" "$OBJ/air_cache.c.o" "$OBJ/cache.c.o"
              "${LLVM_LIBS[@]}")
shopt -u nullglob
codegen_id="$(cat "${CODEGEN_OBJS[@]}" | shasum -a 256 | cut -c1-64)"
# Regenerated, compiled and linked on every run (never a stale object: M1).
printf 'const char d9mt_air_codegen_id[] = "D9MT-AIR-CODEGEN-ID %s";\n' "$codegen_id" > "$OBJ/air_codegen_id.c"
clang -O2 -arch x86_64 -c "$OBJ/air_codegen_id.c" -o "$OBJ/air_codegen_id.c.o"
echo "[bridge] AIR codegen identity ${codegen_id:0:32} (${#CODEGEN_OBJS[@]} inputs)"

clang++ -arch x86_64 -dynamiclib \
  -o "$OUT/winemetal.so" \
  "$OBJ/winemetal_unix.c.o" "$OBJ/dxso_stubs.c.o" "$OBJ/air_codegen_id.c.o" \
  "${CODEGEN_OBJS[@]}" \
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
# The identity in the .so must be the one of the objects just linked.
linked_id="$(strings -a "$OUT/winemetal.so" | sed -n 's/^D9MT-AIR-CODEGEN-ID \([0-9a-f]\{64\}\)$/\1/p')"
if [ "$linked_id" != "$codegen_id" ]; then
  echo "FATAL: winemetal.so carries AIR codegen identity '${linked_id:-none}', the objects hash to $codegen_id" >&2
  exit 1
fi

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
PE_SRC=("$BR/main.c" "$BR/winecrt0_compat.c" "$BR/winemetal_thunks.c" "$BR/airconv_thunks.c" "$BR/d9mtair_thunks.c")
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

# Smoke test (x86_64 host, needs a Metal GPU): vertex-descriptor PSO, MTL4
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
  echo "[bridge] smoke test (x86_64 host)"
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
