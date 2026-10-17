# d9mt on arm64 / ARM64EC

Port of d9mt to arm64 Wine (issue #6): `winemetal.so` and the AIR compile core
for **aarch64** (Wine's unix side), `d3d9.dll` and `winemetal.dll` as
**ARM64EC** (native code in an x86-64 process; only the game's own x64 code is
emulated, by FEX). The x86 and x64 builds stay as they are, byte for byte.

This file is the record of what was checked: every architecture-specific spot
in `driver/`, `bridge/`, `air/`, `airlib/` and `scripts/`, what it does on x86,
what ARM64EC and arm64 do instead, and how the result was verified. Keep it
updated with every change on the `arm64ec` branch.

## Status

| Part | State |
|---|---|
| Toolchain: llvm-mingw 20260616 (LLVM 22.1.8), LLVM 15.0.7 for arm64 | done |
| Unix side: `winemetal.so`, `d9mtair.so`, loopback for arm64 | builds, tests pass |
| PE side: `winemetal.dll`, `d3d9.dll` as ARM64EC | builds, no unjustified warnings |
| x86 builds byte-identical to `main` | verified (`scripts/check-x86-identity.sh`) |
| Running the PE side under arm64 Wine + FEX | not yet (§ Runtime) |

## Building

```bash
export PATH="/opt/homebrew/bin:$PATH"
ARCH=arm64 scripts/build-llvm-unixside.sh     # once: toolchains/llvm-arm64
scripts/llvm-mingw.sh                         # once: pinned, checksummed llvm-mingw
ARCH=arm64ec scripts/build-bridge.sh          # build/bridge-arm64ec/{winemetal.so,winemetal-arm64ec.dll}
ARCH=arm64ec scripts/build-driver.sh          # build/driver-arm64ec/d3d9.dll
```

- **llvm-mingw** `20260616` (LLVM 22.1.8, the version FEX-darwin builds its
  ARM64EC DLLs with), `ucrt-macos-universal`, SHA-256
  `2cab02a2e964bd4aae981150a45985d07c657cfa8d244959eb9e2dcc5eedd7b1`
  (the GitHub release digest). `scripts/llvm-mingw.sh` downloads it into a
  directory of its own, verifies it and unpacks it under
  `~/Library/Application Support/swtor-mac-proper/toolchains/`; `LLVM_MINGW`
  overrides the location. DXMT's CI uses 20260908 (LLVM 23.1.1) and adds
  `-marm64x`; we build plain ARM64EC (see Open points).
- **LLVM 15.0.7 for arm64**: the same recipe as x86_64 (no targets, no tools),
  `-DCMAKE_OSX_ARCHITECTURES=arm64 -DLLVM_HOST_TRIPLE=arm64-apple-darwin`, plus
  `-ffp-contract=off`. About 2 minutes on an M5 Max.
- **The x86 commands are unchanged**: every script keeps its default path's
  command lines argument for argument (`ARCH` unset, `x86`, `x64`).

### Compiler flags that differ from x86

| Flag | Where | Why |
|---|---|---|
| `-ffp-contract=off` | arm64 unix side, LLVM, ARM64EC PE side | clang fuses `a*b+c` into `fmadd` on arm64 and ARM64EC by default (checked: `fmadd s0, s0, s1, s2` for `a*b+c`); x86-64 without `-mfma` never fuses (`mulss`+`addss`). With the flag the arm64 code is `fmul`+`fadd`, bit-identical to SSE scalar math. Measured cost on the compile path: none (19.06 s vs 19.24 s over the AIR corpus, 3 runs each, noise level). |
| `-msse -msse2 -mfpmath=sse` | x86 builds only | x86 options; ARM64EC doesn't take them. |
| `-Wno-microsoft-exception-spec` | ARM64EC driver only | 5251 warnings, one kind: llvm-mingw's COM headers declare the interface methods nothrow, DXVK's overrides don't. Benign (DXVK's COM methods catch everything), GCC doesn't diagnose it, DXMT's `meson.build` disables it too. |

### Remaining warnings (justified)

- ARM64EC driver: `third_party/dxbc-spirv/ir/ir_utils.cpp:166` "captured
  structured bindings are a C++20 extension" (2×). Third-party; clang accepts
  it in C++17 mode; Apple clang reports the same lines on the unix side.
- arm64 bridge: the same 3 compiler warnings and 1 linker note as the x86_64
  build (the two above, the known MetalFX selector bug at
  `winemetal_unix.c:3717`, `-U option is redundant`). The x86_64 build also
  shows 973 `ld: object file ... built for newer macOS version (27.0)` lines:
  its LLVM was built before the 26.0 default (README); not a port issue.
- ARM64EC `winemetal.dll`: none.

## Verification

| What | How | Result |
|---|---|---|
| x86 builds unchanged | `scripts/check-x86-identity.sh main`: builds `main` and this tree with the repo's scripts in the same directory, compares `winemetal.so`, `d9mtair.so`, `winemetal32/64.dll`, both `d3d9.dll` (debug info included) | all 6 byte-identical; `winemetal.so` LC_UUID `2BBB585C-5076-3157-9A6A-BAE494B00ACC` |
| Bridge ABI | `scripts/test-arm64-port.sh` [1]: clang record layouts of every struct in `bridge/*.h` and `airlib/d9mtair.h` for x86_64 mingw, ARM64EC mingw, x86_64 macOS, arm64 macOS | 264 records identical on all four (negative test: a `long` field is caught) |
| Unit tests | [2]: the 7 `test/native` tests on arm64 and x86_64 | 7/7 on both |
| Driver CPU routines | [3]: `test/native/arm64_xarch_test.cpp`, real driver code, x86_64 (SSE/asm paths, Rosetta) vs arm64 (the paths ARM64EC compiles) | 9 digests identical (below) |
| Shader compile path | [4]: `scripts/run-air-xarch.sh`: x86_64 front end + `build/bridge/winemetal.so` under Rosetta vs arm64 front end + `build/bridge-arm64ec/winemetal.so`, 3047 shaders (42 committed, 5 from the test programs, 3000 generated) × front-end variants × 4 compile flag sets | 50059 manifest lines identical: 39884 metallibs (30212 distinct), IR and reflection blobs; the same 204 front-end aborts on both |
| Compile path concurrency | `ARCH=arm64 scripts/test-compile-mt.sh` (also `MT=1` in [5]): the harness and a ThreadSanitizer build of the unix side, natively on arm64 | OK, 0 ThreadSanitizer reports (x86_64: the same) |
| Speed | `BENCH=1` [5]: `test/native/arm64_port_bench.cpp` | § Benchmarks |

`arm64_xarch_test.cpp` covers: `bit::tzcnt/lzcnt/bsf/popcnt` (all 2^32 32-bit
values, 2^27 64-bit values plus edge patterns, each against an integer
reference), `bit::interleave` (all 2^32), `D9MT_U32` (all 2^32 float bit
patterns), the point-size encoding (all 2^32 DWORDs), FNV-1a,
`DxvkHashState` and SHA-1 (with a known answer), `D3D9ConstantBufferCopy`
(20000 random layouts with NaN payloads, infinities, ±0, denormals),
`UpdateStateConstants` (200000 calls) and `D3D9InputSignature::find`.

`scripts/sm3_corpus.py` is an SM3 assembler (its `check` mode reassembles all
43 committed `.asm` files byte-identical to D3DX's output) and a seeded random
shader generator: every ALU and texture opcode d9mt handles, swizzles, write
masks, source modifiers, saturate/pp, `if`/`rep`/`loop aL`/`break`, relative
addressing, and constants drawn from NaN payloads (quiet and signalling),
±inf, ±0, denormals, FLT_MAX and the half-float edges (65504, 65520, 2^-14,
2^-24).

Why "byte-identical" needs care for the x86 builds:
- **Line numbers.** `winemetal_unix.c`'s `assert`s embed `__LINE__`, so a line
  inserted above one changes the x86_64 `.so` and its LC_UUID (every user's
  shader cache key). The driver is built with `-g`, so a line inserted in any
  file it compiles moves its DWARF line table. Edits in shared files are
  therefore line-neutral: new code goes after all code in a file, or replaces
  a non-x86 branch with the same number of lines.
- **Columns.** GCC's DWARF also records columns: replacing `uint32_t(` with a
  longer macro name changed `.debug_line` (`mtl_context_transfer.cpp:571`),
  hence the 8-letter `D9MT_U32`.
- **PE timestamps.** Two builds of one commit differ in the PE timestamp and
  checksum; `SOURCE_DATE_EPOCH` is pinned for the comparison.
- **Build directory.** ld's automatic image base for a mingw DLL is a hash of
  the output path, so the same commit built in two directories gives different
  DLLs (0x309f51000 vs 0x288521000 for `winemetal64.dll`). The identity check
  builds both sides in one directory.

## Audit: architecture-specific code

Predefined macros of llvm-mingw's ARM64EC target (checked with `-dM -E`):
`__arm64ec__`, `__x86_64__`, `__amd64__`, `_WIN64`, `__ARM_NEON`; **not**
`__aarch64__` and none of MSVC's `_M_*`. `long` is 4 bytes (as x64),
`long double` 8 (x64: 16), `char` signed. Every bare `__x86_64__` test is
therefore true on ARM64EC.

### driver/

| Where | x86 | ARM64EC / arm64 | Verified |
|---|---|---|---|
| `util/util_bit.h:3-11` arch detection | `DXVK_ARCH_X86(_64)` | `DXVK_ARCH_X86` off (DXVK excludes `__arm64ec__`). `DXVK_ARCH_ARM64` needed `_M_ARM64EC`, which mingw doesn't define, so it was never set: **added `__arm64ec__`** (line 9; an `#elif` after the x86 branch) | identity; preprocessed both |
| `util/util_bit.h:13-29` `x86intrin.h`, AVX check | included | not included | builds |
| `util/util_bit.h:67-200` `tzcnt`, `bsf`, `lzcnt` | `tzcnt` asm (no BMI), `__builtin_clz` | `__builtin_ctz/clz` with the zero case, `lzcnt(uint64)` split (no callers) | exhaustive [3] |
| `util/util_bit.h:221-325` `bclear`, `bcmpeq`, `bcndcpy` | SSE | `memset`, `memcmp`, scalar | no callers in the driver |
| `util/util_bit.h:614-646` `encodeFixed`, `decodeFixed` | float, arch-independent | same | no callers |
| `util/util_bit.h:649-737` interleave, FNV-1a | integer | same | [3] |
| `util/util_bit.h:741-` (new) `D9MT_U32`, NEON helpers | not compiled (`!DXVK_ARCH_X86`) | `cvtF32U32X86`, `copyVec4`, `copyDwords`, `zeroDwords`, `cndcpyVec4`, `findByte32` | [3], [5] |
| `util/sync/sync_spinlock.h:16-20` spin pause | `_mm_pause` | `yield` (before the fix above: nothing) | disassembly: `yield` in the ARM64EC dll |
| `d3d9/d3d9_constant_copy.cpp:285-390` constant upload (per draw) | SSE loads, NaN flush `and(v, cmpeq(v,v))`, streaming stores, `pad16` | **NEON** via `bit::copyVec4/copyDwords/zeroDwords`: same lanes (NaN → +0, every other lane's bits kept, also under FTZ/DAZ, since `fcmeq`/`cmpeq` keep a denormal equal to itself), no CRT calls | [3]: identical over the bytes shaders read; [5] |
| ″ bool padding | SSE copies whole groups of 4 bools and `pad16` whole 16-byte blocks: bytes after the last used bool can hold other source bools or stale data | exact copy, zero to the end | the differing bytes lie past every index a shader reads (the layout packs used bools); `arm64_xarch_test.cpp` documents it |
| `d3d9/d3d9_state.h:587-627` `UpdateStateConstants` dirty test (every `Set*ShaderConstant*`) | SSE compare to the first difference, copy from there | **NEON** `bit::cndcpyVec4`: same return value, same final memory | [3], [5] |
| `d3d9/d3d9_shader_analysis.h:100-115` `D3D9InputSignature::find` | SSE: first match of 32 bytes (the count byte at 31 included), 32 if none | **NEON** `bit::findByte32`: the same result in every case (the old loop gave 32 where SSE gives 31 when the semantic packs to the count; callers only use indices below the count) | [3]: exact equality |
| `d3d9/d3d9_hitch.h:25,60-66,265` profiler ticks (`D9MT_PROFILE`) | `__rdtsc` | `__x86_64__` is set on ARM64EC, so it would have used `__rdtsc`: **`mrs cntvct_el0`** (no syscall; the steady clock goes through QueryPerformanceCounter under Wine); the per-frame calibration handles its frequency | `hitch_test` on arm64 (calibrated ms checks); 262 `CNTVCT_EL0` reads, no `rdtsc` in the ARM64EC dll |
| `d3d9/d3d9_device.cpp:5714-5741` `SetupFPU` | x87 control word: exceptions masked, 24-bit precision, round to nearest | skipped (upstream DXVK excludes `__arm64ec__`), logs "not supported". The x87 state is FEX's (emulated); x64 MSVC code uses SSE | accepted; affects only x87 code in a 64-bit game |
| `d3d9/d3d9_adapter.cpp:21` vendor DLL names | `aticfx64.dll` etc. on x64 | the same (`__x86_64__`): right, a 64-bit process | reading |
| `backend/mtl_context_transfer.cpp:571` `uint32_t(m_viewport.width)` when a pass has no attachment and a zero scissor | `cvttss2si` (64-bit): ≥ 2^32 → 0 | ARM saturates → `D9MT_U32` emulates x86 | [3]: all 2^32 floats identical to the x86 cast |
| `d3d9/d3d9_device.h:1404` `EncodePointSize` | NaN → `cvttss2si` → low 16 bits 0 | NaN → 0 | [3]: all 2^32 values |
| `backend/mtl_instrument.cpp:52` (`PROFILE_INSTRUMENT=1` builds) | `__builtin_ia32_rdtsc`, GCC's exclude list | not ported; `build-driver.sh` refuses `PROFILE_INSTRUMENT=1` with `ARCH=arm64ec` | — |
| `util/util_unmap.h:12` | 32-bit only | not compiled | — |

### bridge/

| Where | x86 | ARM64EC / arm64 | Verified |
|---|---|---|---|
| `winecrt0_compat.c` unix call dispatch | `WINE_UNIX_CALL` → `__wine_unix_call_dispatcher` → ntdll's `__wine_unix_call` (CrossOver 24 exports it) | Upstream Wine (11.0, master) exports no `__wine_unix_call` function, only dispatcher pointers. In an ARM64EC process the unix loader sets ntdll's `__wine_unix_call_dispatcher` to ntdll's own ARM64EC stub (`dlls/ntdll/unix/loader.c`), and `__wine_unix_call_dispatcher_arm64ec` to the native unix dispatcher, which may only be reached by a branch. **Mirrors winecrt0** (`dlls/winecrt0/unix_lib.c`, identical in 11.0 and master): `LdrGetDllHandle` + `RtlFindExportedRoutineByName("__wine_unix_call_dispatcher")` once in `DllMain`, and a naked `__wine_unix_call_arm64ec` that `br`s to it (no ARM64EC indirect-call check). A missing export fails the DLL load. | ARM64EC dll imports only `NtQueryVirtualMemory`, `LdrGetDllHandle`, `RtlFindExportedRoutineByName` from ntdll; runtime: § Runtime |
| `wineunixlib.h:38-58` `__wine_jmp_buf` | unix side only (`WINE_UNIX_LIB`) | aarch64 branch, Wine's layout | — |
| `wineunixlib.h` `WINE_UNIX_CALL` | dispatcher pointer | `__wine_unix_call_arm64ec` | — |
| `main.c:17-48` Forza Horizon 4 `gs:0x20` fix (DXMT) | x64 asm | excluded by DXMT for ARM64EC | — |
| `winemetal.h:151-208` `WMTMemoryPointer` high part | i386 only | not compiled | layouts [1] |
| `airconv_public.h:126`, `airconv_thunks.h:166` | `__LP64__ \|\| _WIN64` | true on both sides | layouts [1] |
| `unix/winemetal_unix.c:5223-5227` spin (DXMT) | `_mm_pause` | `yield` | — |
| `unix/winemetal_unix.c:1725,1746,2064,5509` GPU completion | Rosetta's TSO kept the later loads of GPU-written memory (query results, readbacks, locked buffers) behind the completion check | **`D9MT_GPU_ACQUIRE()`** (`winemetal_thunks.h`): `dmb ishld` after `signaledValue`, `waitUntilSignaledValue` (2×), `waitUntilCompleted`; `((void)0)` on x86_64 (memory-ordering audit R1) | disassembly: 4 `dmb ishld` after the 4 calls; none in x86_64 |
| `unix/winemetal_unix.c:5897` AIR cache file | `d9mt/air-cache.sqlite3` | **`d9mt/air-cache-arm64.sqlite3`**: each build drops all other builds' tables when it opens the cache, so an arm64 and an x86_64 engine on one Mac would wipe each other's cache every launch | strings in both `.so` |
| PSO archives (`unix/winemetal_unix.c:6206-`) | `pso/os-<build>-<gpu>/`, last 8 sessions | shared with x86_64: keyed by OS and GPU only, and the metallibs are identical [4]; Metal pipeline archives hold GPU code | runtime check pending |
| everything else in `bridge/unix` | — | no architecture-specific code: `long` only in printf casts and Objective-C APIs; `__builtin_bswap32` is integer | grep, layouts |

The unix side (`air/`, `airlib/`) has no architecture-specific code. dxbc-spirv
folds no float→int conversion on the host, and LLVM folds constants in
software (APFloat), so the AIR depends only on the source; [4] proves it for
the corpus, and with and without `-ffp-contract=off` alike.

### scripts/

| Script | Change |
|---|---|
| `build-llvm-unixside.sh` | `ARCH=arm64` → `toolchains/llvm-arm64` (build dir `llvm-build-arm64`) |
| `build-d9mtair.sh` | `ARCH=arm64` → `build/airlib-arm64` (unix `.so` and loopback; the legacy d9mtair PE dlls are not built) |
| `build-bridge.sh` | `ARCH=arm64ec` → `build/bridge-arm64ec`: arm64 `winemetal.so`, ARM64EC `winemetal-arm64ec.dll` (builtin-marked), `winemetal.def`. Passes its arch to `build-d9mtair.sh` explicitly |
| `build-driver.sh` | `ARCH=arm64ec` → `build/driver-arm64ec/d3d9.dll`; exports counted with `llvm-readobj` |
| `llvm-mingw.sh` (new) | pinned, checksummed llvm-mingw |
| `test-compile-mt.sh` | `ARCH=arm64` |
| `check-x86-identity.sh`, `test-arm64-port.sh`, `run-air-xarch.sh`, `sm3_corpus.py`, `layout_cmp.py` (new) | verification above |
| `install-bridge-builtins.sh`, `run-*.sh`, `dump-verify.sh` | x86_64 Wine layout only; arm64 Wine needs `lib/wine/aarch64-unix/winemetal.so` and `lib/wine/aarch64-windows/winemetal.dll` (to do with the runtime work) |

Exports: the ARM64EC `d3d9.dll` has the x64 build's export table (20 exports,
the same ordinals and names, `d3d9.def`). Its CRT is UCRT
(`api-ms-win-crt-*`).

## Memory ordering

Under Rosetta both the PE side and the unix side ran with x86's TSO. Natively
they don't. Audit of every atomic, `volatile`, lock-free handoff and
cross-thread variable in scope (all of `driver/`, `bridge/`, `airlib/`, `air/`):

- **R1 (fixed)**: loads of GPU-written memory after observing a shared
  event's value or a finished wait were ordered only by TSO
  (`mtl_submission.cpp:47-56` → `mtl_context_transfer.cpp:59-70`,
  `d3d9_device.cpp:4674-4697`). `D9MT_GPU_ACQUIRE()` on the unix side, which
  runs on the calling thread.
- Everything else is correct on a weak memory model: the CS thread queues,
  compile tasks (`MtlCompileTask` acq_rel/release/acquire), the submission
  tracker, queries (seq_cst counters), residency (mutex), presenter
  acquisition (mutex), the spinlocks (`exchange(acquire)`/`store(release)`),
  reference counts (seq_cst), and the relaxed atomics, which only feed
  statistics. No `volatile` is used as a cross-thread flag in production code.
- Not port-specific, the same on x86, left alone (changing them changes the x86
  builds):
  - R2 `MtlPresenterImpl::m_desc`, written by `Present` on the app thread,
    read by the CS thread without a lock (`mtl_presenter_impl.cpp:75,84,140-174`);
  - R6 per-process state used as per-device (`winemetal_unix.c:2310`
    `g_dxmt_pass_rt_w/h` and others), a race only with two devices;
  - R3–R5, R7: debug-only, apitrace-only or unused code (`D9MT_DUMP_TRIGGER`'s
    sleep instead of a fence, `mtl_instrument.cpp` calibration, the apitrace
    init, `util_lazy.h`).
- `ARCH=arm64 scripts/test-compile-mt.sh` under ThreadSanitizer: 0 reports.

## Float results

- **FMA contraction**: `-ffp-contract=off` everywhere on arm64/ARM64EC (above).
- **float → int conversions** (38 sites, found with clang's AST, scanned as
  x86_64 and as ARM64EC: the same set): one could differ,
  `mtl_context_transfer.cpp:571` (`D9MT_U32`); point size is identical;
  everything else is clamped first, log-only, or runs on the GPU.
- **Not under our control, to check under Wine**:
  - the game thread's FP state: x64 code can set MXCSR (FTZ, DAZ, rounding)
    and d3d9 runs on that thread. Under ARM64EC it depends on how FEX maps
    MXCSR onto FPCR when the game calls into ARM64EC code. ARM has one
    flush-to-zero bit for inputs and outputs; x86 has two.
  - libm: `cosf` (light cone angles, `d3d9_state.h:301`) comes from Wine's
    x64 msvcrt vs its ARM64EC ucrtbase, possibly an ulp apart.
  - the front end runs in `d3d9.dll`, built by GCC for x64 and by clang for
    ARM64EC; [3] and [4] build both architectures with clang. A PE test
    program under Wine for both closes that gap.
- **No dependence**: rounding-mode-sensitive functions (`lrint` etc.): none.
  `long double`: none in shared code. Hash keys: FNV/SHA-1 on integers;
  `std::hash` (libstdc++ vs libc++ on the PE side) is used only for in-memory
  maps, and nothing depends on their iteration order. The AIR cache key is
  SHA-256 over the IR on the unix side.

## Benchmarks

M5 Max, macOS 27, `BENCH=1 scripts/test-arm64-port.sh`
(`test/native/arm64_port_bench.cpp`): median ns per call of 15 runs, range
over 4 interleaved rounds per architecture. "x86_64" is today's x64 build's
code (the SSE paths) translated by Rosetta, "arm64" the code ARM64EC compiles
(NEON), native. The benchmark warms the core up first and moves the
constant-copy destination through 16 offsets over 4 KB, as the constant ring
does: a single placement varies by up to 3 ns per call on its own.

| Hot path | x86_64 (Rosetta) | arm64 NEON |
|---|---|---|
| `copyConstantData` 32 float + 8 int + 16 bool | 11.5–11.8 | 10.0–10.5 |
| ″ with NaN flush | 14.8–15.6 | 12.2–12.8 |
| ″ 128 float | 25.4–26.3 | 21.2–21.7 |
| ″ 128 float, NaN flush | 37.1–38.8 | 26.5–27.4 |
| ″ 256 float | 44.5–46.6 | 35.7–36.2 |
| ″ 256 float, NaN flush | 70.8–71.2 | 45.4–46.3 |
| `UpdateStateConstants` ×1 unchanged / changed | 0.7–0.8 / 6.5–7.1 | 0.5 / 2.3–2.4 |
| ″ ×4 | 1.7–1.8 / 6.8–7.7 | 1.2 / 1.7–2.0 |
| ″ ×16 | 5.8–6.0 / 8.6–9.4 | 4.3–4.4 / 3.2 |
| ″ ×64 | 22.6–23.1 / 17.6–18.3 | 15.9–16.2 / 9.0–9.4 |
| ″ ×256 | 98.3–99.6 / 71.7–72.9 | 78.5–79.4 / 34.3–39.9 |
| `D3D9InputSignature::find` (12 entries) | 1.9 | 0.8 |

- The portable code these replace (DXVK's non-x86 branches, compiled for
  arm64) was slower in places: `find` 4.3 ns (2.2× SSE), `copyConstantData`
  32 float 12.6 ns, `UpdateStateConstants` ×1 unchanged 1.1 ns. It also
  called `memcpy`/`memcmp`/`memset`, which under arm64 Wine are Wine's
  generic C (`dlls/msvcrt/string.c`; its SSE2 `memmove` is x86-only), so its
  native numbers above are optimistic. The NEON versions make no CRT calls.
- Shader compile path: the whole AIR corpus (3047 shaders, 50059 compiles and
  front-end runs) takes 39.7 s for x86_64 under Rosetta and 19.7 s natively:
  **2.0× faster** shader compiles.

## Found on the way (not port issues, the same on x86)

- **dxbc-spirv asserts are live in all builds** (no `NDEBUG`): a pixel shader
  reading `vPos.xyxy`, an input with an undeclared component, or a
  `rep`/`if`/partial `crs` combination aborts in `ir_utils.cpp:103`
  (`normalizeTypeForConsume`) or `sm3_io_map.cpp:269`, which on Windows ends
  the game. 204 of 3047 corpus shaders abort, on both architectures alike.
  Unknown whether real game shaders hit it (fxc doesn't emit the first two).
- **`pad16`'s SSE path** (`d3d9_constant_copy.cpp:378-385`) uses
  `_mm_stream_si128` at `start = count * 4`, which isn't always 16-byte
  aligned (bool constants); real x86 hardware faults on that. Upstream DXVK
  code; not reached on ARM.
- **Build directory in the PE binaries**: ld's automatic image base depends
  on the output path, so release DLLs differ by where they were built.
- R2 and R6 above.

## Runtime (arm64 Wine + FEX)

To do: assess and prototype a free, source-built arm64 Wine for macOS with
FEX (FEX-darwin, neutron, Wine MR !11638, athei/wine-build), install layout
(`aarch64-unix`, `aarch64-windows`, `HKLM\Software\Microsoft\Wow64\amd64`),
and run d9mt's test programs there.

## Open points

- **ARM64X** (`-marm64x`, as DXMT's CI) would make `d3d9.dll`/`winemetal.dll`
  loadable by native arm64 processes too; SWTOR's process is x64, so plain
  ARM64EC suffices for now.
- **To verify at runtime**: the unix-call path (`winecrt0_compat.c`), MXCSR
  handling across the FEX boundary, Wine's CRT `cosf`,
  `newBufferWithBytesNoCopy` alignment in a 4K-page process, PSO archive
  sharing between the two engines, and the occlusion buffer staying below
  4 GB for i386.
- **i386** (SWG Legends): unchanged builds, run under FEX's WoW64.
- `install-bridge-builtins.sh` and the `run-*` test scripts for an arm64 Wine
  layout.
