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

Timings: pending, measured on an idle machine.
