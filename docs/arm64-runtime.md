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

(in progress)
