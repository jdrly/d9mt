# Running d9mt on arm64 Wine + FEX (macOS)

Step 5 of the arm64/ARM64EC port (issue #6, `docs/arm64ec-port.md`): a Wine
whose unix side is native arm64, whose x64 processes run as ARM64EC, and
which runs the game's own x86-64 code through FEX. This file assesses the
free, source-built routes and records the prototype.

Researched 2026-10-18 on an M5 Max with macOS 27.0.1. The prototype's build
tooling lives in `~/Library/Application Support/swtor-mac-proper/experiments/arm64/`
for now (it will belong to the app repos, shared with SWGL on Mac); only what
d9mt itself needs goes here.

## What any such Wine has to solve on macOS

| Problem | Why | Ways around it |
|---|---|---|
| **x18** | Windows ARM64 (and ARM64EC) keeps the TEB in x18; macOS clears x18 on every return to user space for current binaries | (a) the restricted `cross-architecture-support` entitlement plus the self-granted `custom-x18-abi-toggle`; (b) mark the Wine loader as built with an old SDK: **checked here, macOS 27.0.1 keeps x18 when the main executable's `LC_BUILD_VERSION` records SDK 12.0 or lower, whatever its minimum OS** (0 of 20000 checks lost, with syscalls, `sched_yield`, `usleep` and 16 spinning threads forcing preemption; SDK 13.0, 14.0, 15.0, 26.0 or 27.0: x18 lost on every syscall, 13333 of 20000; minos 26.0 with SDK 12.0 keeps it; also after the fact with `vtool -set-build-version` and an ad-hoc re-sign). The catch: Apple's frameworks then treat the process as linked against the macOS 12 SDK, and some of their behaviour switches on that; (c) change the ABI: keep the TEB in a TSD slot read through `TPIDRRO_EL0`, rebuild the CRT startup code, and repair the remaining x18 accesses in a SIGSEGV handler (neutron) |
| **`__PAGEZERO`** | arm64 macOS rejects executables whose page zero ends below 4 GB, so nothing maps there: no `KUSER_SHARED_DATA` at 0x7ffe0000, no i386 address space | move `KUSER_SHARED_DATA` above 4 GB (all routes); for i386: the entitlement (the kernel leaves page zero replaceable) or a translated guest window (FEX-darwin) |
| **16K pages** | Windows code assumes 4K pages (protections, guard pages, self-modifying code tracking) | the entitlement (4K-page processes), or emulate 4K on 16K (Wine 11, with 16K guard pages in FEX) |
| **W^X JIT** | FEX generates code at run time | `MAP_JIT` + per-thread `pthread_jit_write_protect_np` (not restricted; hardened runtime needs `com.apple.security.cs.allow-jit`) |
| **TSO** | x86 code relies on total store order | hardware TSO per thread (`thread_set_x86_64_compat`, entitlement), else FEX emits ordered loads/stores (software TSO) |
| **Metal layer** | d9mt's bridge gets a window's `CAMetalLayer` through CrossOver's `macdrv_functions` | a winemac patch exporting that table |

## Routes

| | neutron | FEX-darwin | upstream Wine | athei/wine-build |
|---|---|---|---|---|
| What | complete stack: Wine 11.19 (17 patches) + FEX-2610 (5 patches) + DXMT + Proton's Steam bridge, built from source by one script | FEX-2609.1 with Darwin patches: `xtajit64.dll` (ARM64EC) and `xtajit.dll` (WoW64) | ARM64EC since 10.0; the macOS arm64 enablement is MR !11638 (third-party) plus preparatory MRs by CodeWeavers | Wine (athei/wine `arm64-patched`, CrossOver's msync, Hadron commits) + FEX (athei/FEX) + DXMT + mtld3d (D3D9 on Metal), bundled with a signed loader app |
| Source | github.com/lixsldotnet/neutron 0ac2574d (2026-10-10), release v0.1.0 | github.com/dbc-hbin/FEX-darwin 370c630 (2026-10-10) | gitlab.winehq.org MR !11638 (opened 2026-08-11, last update 2026-08-14, 1 note, merge conflict) | github.com/athei/wine-build (2026-10-17), athei/wine `arm64-patched` e2e42653 (2026-10-17) |
| License | LGPL-2.1+ (Valve's Steamworks SDK license for the downloaded Steam bridge) | MIT (FEX upstream MIT) | LGPL-2.1+ | wine-build: no license file; Wine fork LGPL; mtld3d zlib |
| x18 | (c) TSD slot + fault repair | relies on the Wine side | (b) loader built for macOS 12 | (a) entitlement + x18 toggle |
| Entitlement / signing | none, ad-hoc, SIP on | none of its own | none | **`cross-architecture-support` required**: provisioning profile + signature of the profile's team; "Wine does not run without it" |
| Pages | 16K (4K emulated, 16K FEX guard pages) | 16K guard pages | 16K (64K section alignment) | 4K processes (entitlement) |
| TSO | software (FEX barriers), `strict`/`fast` per game | software | n/a (no emulator integration) | hardware TSO via FEX (`FEX_HARDWARE_TSO`, entitlement) |
| i386 (SWG Legends) | no, by design | yes: WoW64 guest window | no | yes (page zero via entitlement) |
| Wine side for FEX | in the patches | **not published**: needs Wine hooks (shared data, JIT write protection, native JIT allocation, guest window); the author's only Wine trees are D3DMetal (Apple GPTK) runtimes | not in the MR | in athei/wine |
| Metal layer for our bridge | `macdrv_functions` with CrossOver's layout (wine 0004): compatible with `bridge/unix/winemetal_unix.c:3961` field for field; `on_main_thread` is NULL, which the bridge doesn't use | — | none (winemetal MRs !10935/!11257 are a different API) | athei's Wine (CrossOver-derived) |
| Known working | Abiotic Factor (UE5, D3D11), Gamble With Your Friends (Unity, D3D11/12, ~100 FPS 3440×1440), Ready or Not (UE5, D3D11) on M5 Max / macOS 27 | — | native ARM64 Windows programs | its own compatdb (recipes per game) |
| Build effort | ~10 min on an M5 Max, ~25 GB; Homebrew bison, gnutls, freetype, cmake, ninja (+ meson for DXMT/SDL) | CMake + llvm-mingw | Wine build | needs a paid team's identity + Rust + Mesa |
| Maturity | 56 commits in 9 days, AI-assisted (stated), 2 stars | 1 week old, 2 stars | stalled | active (daily), 11 stars |

Not used: CrossOver Preview (needs a download Jan has not approved; proprietary
FEX build), Apple's GPTK/D3DMetal (proprietary), Madeira (iOS only).

### Consequences for d9mt

- **Our ARM64EC binaries use x18.** `d3d9.dll` reads the TEB through x18 at 18
  places, `winemetal.dll` at 3 (llvm-objdump, symbolized):
  - CRT startup (`_CRT_INIT`, once per DLL);
  - libc++'s `__cxa_get_globals` (only when an exception is thrown);
  - native TLS (`thread_local`, `[x18, #0x58]`): dxbc-spirv's logger (per
    shader conversion) and `AttTimer::s_depth` (`mtl_context_record.cpp:748`),
    which is only touched with `D9MT_PROFILE` (the load sits behind the
    profiling branch, checked in the disassembly).

  Under (a) and (b) these are plain loads. Under neutron's (c) every one is a
  SIGSEGV that the handler emulates: correct, but a signal per access, so
  none may be in a hot path. Building our DLLs with neutron's patched
  llvm-mingw CRT removes the startup ones, not the compiler-generated TLS
  accesses.
- **The bridge's Metal hook** works with neutron's winemac as is.
- **i386** (SWG Legends): only FEX-darwin's guest window or the entitlement.
  Neither has a free, published Wine side today.

## Recommendation

**Prototype on neutron's Wine and FEX patches, built by our own pinned
script, without its Steam bridge, SDL, Mono and DXMT** (we bring d9mt).
It is the only route that is complete, free, entitlement-free, built from
source and tested with real games on this Mac model and OS, and SWTOR is
64-bit only.

Then, as a second variant from the same sources: **the loader linked for
macOS 12 and the standard x18 ABI** (neutron's `WINE_TEB_IN_TSD` off), which
the x18 test above makes possible without an entitlement (the loader's
`LC_BUILD_VERSION` SDK set to 12.0). It would run stock ARM64EC binaries
(ours, and later anyone's) without signal repairs and keep the ABI CrossOver
and upstream use. Risk: macOS behaviour keyed to the linked SDK (AppKit,
Metal) in the Wine processes, which then look like macOS 12 SDK apps.

The entitled route (athei, CrossOver) stays the long-term best case
(hardware TSO, 4K pages, i386) for whoever has a paid developer account.

## Prototype

### Build

`experiments/arm64/tools/build-wine-fex.sh` (outside the repo for now):
Wine 11.19 (tarball SHA-256 `4820066e…95f1`, GPG-signed by Alexandre
Julliard) with neutron 0ac2574d's 17 Wine patches, FEX-2610 (14c92681) with
its 5 FEX patches, llvm-mingw 20260908 (SHA-256 `d1dc5d1e…a149`) with the
`NtCurrentTeb` patch and the CRT startup objects rebuilt from mingw-w64
9f55f4a2. Not built: neutron's Steam bridge (its two `WINE_CONFIG_MAKEFILE`
lines are dropped from `configure.ac`), SDL, Mono, DXMT. Three things the
script had to do beyond neutron's recipe:
- build through a path without spaces (`/tmp/arm64-work` → the work dir):
  Wine's build breaks on "Application Support";
- pass the PE cross compilers to configure by absolute path: with
  llvm-mingw appended to `PATH`, Homebrew's `x86_64-w64-mingw32-gcc` built
  the x86_64 part of the ARM64EC ntdll (`signal_x86_64.c`) and failed;
- `--without-sdl`.
About 3 minutes for Wine and FEX on an M5 Max from a warm source tree. No
system changes; Homebrew's bison, gnutls, freetype, pkgconf, cmake, ninja,
autoconf were already there.

### Results

`scripts/run-hash-tests.sh` (20 x64 test programs, fresh `/tmp` prefix,
FEX registered under `HKLM\Software\Microsoft\Wow64\amd64`):

| Engine | d3d9.dll | winemetal | Result |
|---|---|---|---|
| CrossOver 26.3 (x86_64, Rosetta) | x64 | x64 dll + x86_64 .so | 20/20, the reference hashes |
| arm64 Wine 11.19 + FEX (neutron patches, TSD) | **x64, unmodified, under FEX** | ARM64EC dll + arm64 .so | **20/20, all hashes identical** |
| ″ | **ARM64EC** | ARM64EC dll + arm64 .so | **20/20, all hashes identical** |

- `+loaddll`: `libarm64ecfex.dll` and our builtin `winemetal.dll` load,
  `d3d9.dll` from the test directory is the ARM64EC one (machine 0xA641).
- The x64 `winemetal.dll` can't be used there: it imports CrossOver's
  `__wine_unix_call`, which upstream Wine doesn't export. The x64 `d3d9.dll`
  with the ARM64EC `winemetal.dll` is the mixed setup that works.
- `PE=1`: `arm64_xarch_test` built by mingw GCC for x64 (run under FEX) and
  by llvm-mingw for ARM64EC: both digests identical to the native macOS run.
  This closes the "PE compilers" gap of `docs/arm64ec-port.md`. Its
  single-threaded integer loops: 39 s under FEX, 36.7 s under Rosetta, 27 s
  as ARM64EC.
- `wineboot` reports "Wine cannot find the FreeType font library": the
  engine needs Homebrew's or a bundled FreeType for fonts (neutron bundles
  it); the tests don't draw text.

### Crashes at process exit (a d9mt bug, both architectures)

The first prototype runs ended some tests with a crash *after* their last
frame: macOS crash reports and, Wine being a GUI app, "wine quit
unexpectedly" dialogs. The hashes are not affected (the crash comes after the
last readback). The runner now starts every Wine process through
`test/harness/exc_guard.c`, which takes the corpse of a crashing process on
a task exception port (`EXC_CORPSE_NOTIFY`, inherited by Wine's children)
before ReportCrash gets it on the host port, logs all threads to
`logs/<test>.exc` and reports `CRASH`. No report, no dialog, no system
setting changed.

What happens, from the guard's logs and a `pthread_exit` backtrace probe
(`DYLD_INSERT_LIBRARIES`, test runs only):
1. `ExitProcess` (or returning from `main`): wineserver has Wine SIGQUIT every
   other thread, which `pthread_exit`s from the signal handler wherever it is.
   d9mt's drawable acquirer (`d9mt-drawable`) is caught inside
   `-[CAMetalLayer nextDrawable]` → `CAMetalLayerPrivateNextDrawableLocked`
   allocating a drawable in every probed run: on arm64 Wine 8 of 8, **on
   CX26 under Rosetta 10 of 10**. It dies holding the layer's lock; other
   threads can die in `getenv` (libc's environment lock) or objc.
2. Then libc's `exit()` runs d9mt's `pso_flush_atexit`
   (`bridge/unix/winemetal_unix.c`), which serializes the PSO archive: Metal,
   objc, `getenv`, XPC to the compiler service, tens to hundreds of ms.
3. During it, either the flush itself takes a lock a dead thread owns
   (`getenv`, `objc_lookUpImpOrForward`), or CoreAnimation's presented
   callback for the last frame (`layer_presented`) takes the layer's lock:
   `os_unfair_lock is corrupt, or owner thread exited without unlocking`.
   The guard checks the lock's owner port: gone, every time. A lock that
   blocks instead of aborting hangs the exit (one `code_index` run timed out
   after all its frames).

| arm64 Wine + FEX, 20 tests | PSO archive on (default) | `D9MT_PSO_ARCHIVE=0` |
|---|---|---|
| ARM64EC d3d9 | 20/20 hashes identical; `code_updatetex` crashes at exit 7 of 10 alone | 20/20, no crash |
| x64 d3d9 under FEX | 20/20 identical; 3 crashes at exit (`code_alpha`, `code_rtt3d`, `code_updatetex`), 1 exit hang | 20/20, no crash |
| CX26, `code_updatetex` ×20 | no crash (acquirer killed in `nextDrawable` all the same) | |

`code_updatetex` hits it most: it never releases its shaders, declaration and
vertex buffer, so its `IDirect3DDevice9_Release` leaves the device alive, and
it exits two frames in, while the layer is still allocating drawables (the
tests that exit at frame 5 have their drawables). A crashed flush also leaves
`*.mtl4archive.tmp` and loose `<UUID>.metallib` files in the PSO directory.

So the latent bug is d9mt's, on every architecture: heavy work in an
`atexit` handler after Wine has killed the threads, plus a background thread
that is often inside CoreAnimation. On Rosetta the window is apparently too
short or the timing different (no crash in 20 runs), not absent. The fix
changes the x86_64 `winemetal.so`, so it is not made on this branch without
Jan's decision. Proposed: drop the `atexit` flush and flush at points where
every thread still runs (device destruction, and a few seconds after the last
new PSO on a worker thread, next to the existing every-256 flush); sweep
stale `.tmp`/`.metallib` files from the PSO directory at startup.

### Floating point, libm and buffer pages (`PROBES=1`, `code_vbsizes`)

`scripts/run-hash-tests.sh` with `PROBES=1` runs `test/fpenv/` on each
engine: an x64 caller sets MXCSR and calls a DLL built for x64 and, on arm64
Wine, for ARM64EC; and the x87 control word and MXCSR around `CreateDevice`.

- **MXCSR → FPCR at the x64 → ARM64EC call: exact.** For round to nearest,
  down, up, toward zero, FTZ, DAZ and FTZ+DAZ, the ARM64EC callee sees FPCR
  with the matching RMode, FZ (bit 24) and, for DAZ, FIZ (bit 0, FEAT_AFP),
  and its results (1/3 and -1/3 bits, `lrintf(±2.5)`, a denormal input, a
  denormal result) are bit-identical to the x64 callee's under Rosetta (CX26)
  and under FEX.
- **Back the other way:** a rounding-mode change made by the ARM64EC callee
  does not reach the caller's MXCSR (an x64 callee's does, on both engines).
  MXCSR's control bits are nonvolatile in the Windows x64 ABI, so a callee
  must not leave them changed anyway; d9mt sets no FP mode.
- **x87 control word: a real difference.** `CreateDevice` without
  `D3DCREATE_FPU_PRESERVE` sets the caller's x87 FPU to 24-bit precision
  (`D3D9DeviceEx::SetupFPU`, as Windows' d3d9 does): `037f` → `007f` with the
  x64 d3d9 on CX26 and under FEX. The ARM64EC d3d9 can't execute `fldcw`
  (the x87 state belongs to the emulator; the code is `#if`'d out on
  `main` too, from DXVK), so it stays `037f`: the game's x87 code, if any,
  computes with 64-bit mantissas instead of 24. x64 code normally uses SSE
  for float math (the MXCSR side matches), so this only matters for an x64
  game that still uses x87 and depends on single precision; SWTOR's x87 use
  is unknown. A fix would need an x64 helper the ARM64EC d3d9 calls through
  the emulator (or FEX's x87 state) — not done.
- **MXCSR exception flags:** after `CreateDevice` CX26 shows the precision
  flag set (`1fa0`), FEX doesn't (`1f80`); sticky flags only, no effect on
  results.
- **`cosf` (light cone angles, `d3d9_state.h`):** both d3d9 builds import it
  from ucrtbase. Every 64th float in [0, 2π] (16,983,104 values) gives
  bit-identical results from CX26's x64 ucrtbase (Rosetta), Wine 11.19's x64
  ucrtbase under FEX and its ARM64EC ucrtbase: 0 differences
  (`test/fpenv/cosf_*.c`, `ulpcmp.c`). Per call: 2.7 ns Rosetta, 5.4 ns FEX,
  2.3 ns ARM64EC.
- **`newBufferWithBytesNoCopy` on 16K pages:** CPU-visible buffers are
  VirtualAlloc'd memory (64K-aligned) handed to Metal at their exact size,
  so their lengths are arbitrary. `test/code_vbsizes.c` draws the last
  triangle of vertex buffers of 60 B to 1 MiB + 4116 B (around 4K, 16K and
  64K boundaries) through the default write-only, default read-write,
  dynamic and managed paths: 52 of 52 correct on CX26 and on arm64 Wine with
  either d3d9, same final hash.

### Timings

`scripts/run-hash-tests.sh` `BENCH=1` (`test/bench_draws.c`,
`test/bench_shaders.c`), M5 Max, macOS 27.0.1, 2026-10-18 between 05:00
and 08:00: five configurations interleaved round by round, 4 rounds each,
median (min-max). A run was repeated when the 1-minute load reached 6 or a
compiler ran during it (sampled every 5 s); load during the kept runs 1.1
to 4.7. Other agents' Wine processes were running during part of the time
(not avoidable; the ranges stay tight). The display was asleep: frame times
are CPU + GPU per frame (each frame waits for an event query, no Present),
not presented frames. Configurations: CX26 under Rosetta (the shipping
setup, x64 d3d9); arm64 Wine + FEX with the ARM64EC d3d9; with the x64 d3d9
under FEX, FEX's default ("fast") TSO, TSO off (`FEX_TSOENABLED=0`, an
upper bound, unsafe for real use) and neutron's strict mode (vector and
memcpy TSO, no half-barriers). The test programs themselves are x64 (under
Rosetta or FEX) in every configuration, like a game.

Draw loop: what the game thread pays per draw (one VS constant update, one
PS constant update, one render state change, one draw), ns:

| draws/frame | CX26 | ARM64EC | x64 FEX | x64 FEX TSO off | x64 FEX strict |
|---|---|---|---|---|---|
| 100 | 98 (95-104) | 97 (94-101) | 139 (138-145) | 116 (110-118) | 162 (156-173) |
| 1000 | 81 (80-85) | 86 (81-91) | 115 (112-120) | 96 (90-101) | 140 (132-144) |
| 3000 | 77 (76-78) | 79 (78-81) | 110 (108-112) | 86 (86-87) | 131 (130-131) |

Whole process CPU per draw (every thread: d9mt's CS thread, Metal,
the emulator), and frame time at 3000 draws:

| 3000 draws/frame | CX26 | ARM64EC | x64 FEX | x64 FEX TSO off | x64 FEX strict |
|---|---|---|---|---|---|
| CPU ns/draw | 776 (661-842) | 489 (483-497) | 613 (572-697) | 516 (511-531) | 766 (636-797) |
| frame median ms | 1.25 (0.97-1.49) | 0.80 (0.79-0.80) | 0.84 (0.84-1.46) | 0.81 (0.80-0.81) | 1.47 (0.95-1.49) |
| frame p99 ms | 1.52 (1.10-1.57) | 0.82 (0.81-0.82) | 1.51 (1.02-1.52) | 0.83 (0.83-0.89) | 1.53 (1.48-1.53) |

Shader compile, 313 generated SM3 pixel shaders, each compiled
synchronously (no workers, AIR cache or PSO archives) and drawn once,
ms per shader:

| | CX26 | ARM64EC | x64 FEX | x64 FEX TSO off | x64 FEX strict |
|---|---|---|---|---|---|
| median | 2.59 (2.47-2.64) | 1.15 (1.11-1.19) | 1.49 (1.41-1.55) | 1.33 (1.27-1.35) | 2.23 (2.17-2.27) |
| p90 | 3.37 (3.32-3.40) | 1.74 (1.57-1.95) | 2.57 (2.49-2.65) | 2.28 (2.23-2.33) | 2.98 (2.93-3.16) |
| total | 886 | 390 | 609 | 543 | 777 |

- **The ARM64EC d3d9 is the fastest configuration**, not just faster than
  the x64 d3d9 under FEX: draw-loop cost equal to Rosetta's (within 2-4
  ns), 37% less process CPU per draw than Rosetta, frames at 3000 draws 36%
  shorter, shader compiles 56% faster than Rosetta, 23% faster than the x64
  d3d9 under FEX.
- **The x64 d3d9 under FEX** (the "drop in the unmodified DLL" route) works
  but costs 40-45% more on the game thread per draw than Rosetta.
- **TSO**: on the x64 d3d9 under FEX, FEX's default TSO costs 18-28% of
  the draw loop against no TSO and 12% of the shader compile; strict mode
  another 19-22% and 49%. With the ARM64EC d3d9 none of that applies to the
  driver: only the game's own code pays TSO (software TSO, no entitlement for
  the hardware one).
- These are microbenchmarks of the driver path. A game's own CPU work
  (all x64, all under FEX's TSO) is a separate cost: the `PE=1` integer
  loops ran 39 s under FEX, 36.7 s under Rosetta, 27 s as ARM64EC.
- Still pending: presented frame times (needs the display awake), and SWTOR
  itself (its own code under FEX).

Methodology problems found on the way (all fixed in the programs/runner):
- zsh runs background jobs at nice 5 (`BG_NICE`): launch with
  `setopt no_bg_nice`.
- With the display asleep, d9mt sees the window hidden and throttles
  Present to 30 fps, and nextDrawable can block: `bench_shaders` no longer
  presents, `bench_draws` waits for an event query instead.
- Wine on macOS has only process CPU times (from `times()`, 10 ms ticks):
  CPU is taken once over the whole run.
- **A hot `GetData(D3DGETDATA_FLUSH)` spin** — a pattern games use — showed
  a d9mt problem: with the ARM64EC d3d9 (native d3d9, emulated polling, so
  many more polls per second) d9mt's CS thread used 2.3 s of CPU per 3 s
  against 0.45 s with the x64 d3d9, process CPU per draw at 100 draws/frame
  was 4x higher and the p99 frame ~9 ms against 0.3-3 ms; in the shader
  benchmark it gave bimodal runs (1.1 or 2.4-7.9 ms median, p90 at a
  constant ~10.5 ms) that an extra environment variable switched on and
  off. Not the flush tracker (it doesn't submit without new chunks) and not
  `sched_yield` (one call per run); not found yet. `bench_draws ...
  spinflush` reproduces it. Both benchmarks now poll every 20 us. Worth a
  look on `main`: the same code runs on x86, only the polling rate differs.
