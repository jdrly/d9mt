# d9mt domain context

The glossary of d9mt, the Direct3D 9 → Metal 4 driver for Wine on Apple Silicon. Use these terms
in issues, pull requests, code, comments, log lines and design documents.

This glossary defines **language, not implementation**. ADRs in [`docs/adr/`](docs/adr/index.md)
own the decisions; the README and the design documents in `docs/` describe the behaviour in
detail. If a term you need is missing, add it here in the same pull request and say so in the PR
description. Every definition names its source; what is not decided is marked _Open_.

## Architecture

**Driver**
`d3d9.dll`, the PE side the game loads: DXVK's D3D9 front-end (`driver/d3d9`) with d9mt's Metal 4
backend (`driver/backend`) (README, *How it works*).

**Front-end**
DXVK's D3D9 code in `driver/d3d9`, extracted with Vulkan removed: it tracks all D3D9 state and
records work through the CS thread into the seam (README, *How it works*).

**CS thread**
DXVK's command-stream thread: the game's D3D9 calls are recorded on the game's thread and
executed on the CS thread (README, *How it works*). The backend's single submitter.

**Seam**
The interface in `driver/mtl` between the front-end and the backend (README, *How it works*).

**Backend**
`driver/backend`: the seam on Metal 4: argument tables, residency sets, MTL4 command buffers and
allocators, encoders, pipeline compilation, the upload ring, presentation (README).

**Bridge**
`winemetal`: d9mt's fork of DXMT's bridge, which carries Metal calls across Wine's PE/unix
boundary. `winemetal.dll` (PE side) and `winemetal.so` (unix side, x86_64, with the AIR
compiler) (`bridge/`; README). Always built and shipped with the driver from the same build:
there is no version handshake.
_Avoid_: “DXMT” for the bridge (DXMT is the upstream project).

**AIR compiler**
d9mt's own shader compiler on the unix side (`air/`, `airlib/`): it lowers dxbc-spirv's IR of the
game's SM1–3 bytecode to Apple's AIR (LLVM 15 bitcode) and wraps it in a metallib, so no Metal
toolchain or MSL is involved at runtime (README).
_Avoid_: “MSL compiler”, “shader translator”.

**Compat metallib**
The second build of each embedded metallib (fixed-function shaders, blit), for the minimum macOS
version: the driver loads the newest Metal language and falls back to Compat where Metal rejects
it (MSL 4.1 on macOS 26) (README, *Build*).

## Submission and lifetime

**Serial**
The number of a submitted command buffer, signalled on one `MTLSharedEvent`. There is one queue
and one submitter (the CS thread), so serials complete in submission order
(`driver/backend/mtl_submission.h`).

**Retire**
To release what a command buffer used once the GPU is past its serial. Retirements run in serial
order: on the `d9mt-retire` pump thread, which observes GPU completion independently of the CS
thread, plus serialised drains on the calling thread (in `submit` and `endFrame` on the CS thread,
and in the recovery from a failed host allocation), never two at once. `retireAfter` releases at
once (inline) when the serial is already complete or the device has drained
(`driver/backend/mtl_submission.h`, `mtl_submission.cpp`, `mtl_context_record.cpp`,
`mtl_device_impl.cpp`). A **retired** serial is one the GPU has completed.
_Avoid_: “free” or “release” for this deferred step.

**Lifetime rule**
Metal 4 command buffers don't retain what they use, so nothing a recorded or committed command
buffer refers to may be released before its serial retires (ADR-0001;
`driver/backend/mtl_lifetime.h`).

**Lifetime check**
`D9MT_LIFETIME_CHECK`: stamps every texture handle put into a command buffer with that command
buffer's serial and reports (`D9MT-LIFETIME`) a release before the GPU is past it, on any Mac,
without needing the GPU to fault (`driver/backend/mtl_lifetime.h`; README).

**Drawable**
The `CAMetalLayer` image d9mt acquires for each present and copies the frame into. It is kept
until the present's command buffer retires (#26). Drawables never enter d9mt's own residency set;
they are in the layer's (`driver/backend/mtl_residency.h`).

**Residency set**
The Metal 4 set that makes resources resident for the GPU; removals wait until the GPU retires
the referencing serial (`driver/backend/mtl_residency.h`).

**GPU canary**
`D9MT_GPU_CANARY`, on by default, marks a command buffer's slot in a small buffer: a present copy
into the drawable in its own encoder; a present that isn't a copy (a scaled blit, MetalFX) in an
encoder of its own every frame; a command buffer that doesn't present in an encoder of its own on
every fourth, and only if no canary ran for 250 ms (`driver/backend/mtl_canary_gate.h`); a marked command
buffer that completes without its mark was discarded by the GPU (after a page fault), which logs
`D9MT-GPU-LOST` (the first 20, then a summary at most every 10 s) (README, *Environment
variables*; #27).

**GPU hang**
A submitted command buffer the GPU never finishes: no serial completes after it, the GPU's
timeout cuts it off with everything queued behind it, and the queue doesn't recover. Unlike a
page fault (see GPU canary), nothing completes afterwards. A wait that sees it for 10 s logs
`D9MT-GPU-HUNG` and ends the game (`D9MT_GPU_HUNG`; `driver/backend/mtl_submission.cpp`).

**Pass split**
A submit in the middle of a render pass: the pass ends, so its attachments are stored and loaded
again (`docs/gpu-4k.md`, `docs/g1-early-commit.md`).

**G1, carry**
G1 (“commit early, complete late”): a heuristic flush in the middle of a pass commits the work
recorded before the open pass, without splitting the pass; that is a **carry**. The serial
completes when the carried pass closes. Carries are gated and capped at two per serial;
`D9MT_EARLY_COMMIT` switches it (`docs/g1-early-commit.md`; #31).

**Upload ring**
Chunks of memory for per-draw uploads; a chunk retires by GPU-completed serial, and a slice is
valid only for the command buffer it was allocated in (`driver/backend/mtl_ring.h`).

## Pipelines and caches

**PSO**
A Metal render pipeline state, built from the shaders and the state fixed at pipeline creation;
its key is `RenderPsoKey` (`docs/state-cache.md` §2, `docs/pso-key-hash.md`).
_Avoid_: “shader” for the whole pipeline.

**PSO MRU**
The last four pipeline snapshots, checked before a full key build. Its state generations are
content-addressed, so returning to earlier state (A → B → A) hits again
(`driver/backend/mtl_pso_gen.h`; #40).

**AIR cache**
The persistent cache of compiled shaders, one sqlite file in the user cache folder
(`air-cache.sqlite3`), shared by all games. Its key includes `winemetal.so`'s LC_UUID, so every
bridge rebuild invalidates it (README, *Environment variables*; ADR-0004).

**PSO archive**
The Metal 4 pipeline archives in `<user cache>/d9mt/pso/`, one per session, the last eight kept
(README, *Environment variables*).

**Pipeline state cache**
The record of the pipelines a game used (shader identities plus the derived key), rebuilt in the
background in later sessions as soon as the game has created their shaders; it lives entirely in
`d3d9.dll` (`docs/state-cache.md`; ADR-0003).
_Avoid_: “shader cache” (that is the AIR cache).

**Prewarm**
The state cache's background rebuild, on `d9mt-prewarm` threads at the lowest priority
(`docs/state-cache.md` §5).

**State cache journal**
`<file>.journal`: an append-only file of the records since the last write of the state cache, so
they survive a kill or `ExitProcess` (`docs/state-cache-journal.md`).

**Key signature**
The state cache file's hash of `RenderPsoKey`'s layout and a mapping version; a mismatch drops the
file's entries, so a bump resets every player's state cache (`docs/state-cache.md` §2).

**Pipeline wait budget**
`d9mt.pipelineWaitBudgetMs`: the most the render thread waits per frame for compiles still
running before it skips the draws that need them (README, *Pipeline wait budget*).

## Memory and render passes

**Memory profile**
`auto`, `low`, `medium`, `high`, `ultra` or `unlimited`: the reported video memory and pool sizes,
picked from the Mac's RAM by `auto` (README, *Memory profiles*).

**Attachment optimization**
The load/store decisions per render pass: a known clear is Clear-loaded, an unwritten attachment's
store is skipped (README, *Attachment load/store optimization*).

**Restore**
The re-clear that gives a reader the contents of an attachment whose store was skipped (`restore`
in `STATS`, `RESTORE` lines) (README).

**Rules B, C, E, F**
The frame-pass rules of the experimental `D9MT_DEFERRED` mode (`flushFramePasses` in
`driver/backend/mtl_context_record.cpp`; `D9MT_NO_RULE_{B,C,E,F}` in the README). _Open:_ they are
defined only in code.

## Presentation and pacing

**Frame cap**
`D9MT_FPS_CAP`: Present returns on a fixed cadence for a set frame rate. With vsync on and a rate
that divides the refresh rate it is **vsync-aligned**: every frame stays on screen for the same
number of refreshes (`presentAfterMinimumDuration`); otherwise it is a plain timer
(`driver/d3d9/d3d9_frame_cap.h`; README, *Environment variables*; #44).
_Avoid_: “FPS limiter” for d9mt's cap (SWTOR on Mac's *Frame Rate Limit* is the setting that
turns it on); DXVK's `d3d9.maxFrameRate` is the **DXVK limiter**.

**Background throttle**
`D9MT_BG_FPS` / `D9MT_BG_FPS_MINIMIZED`: while the window is hidden or minimized, d9mt skips the
presents and Present sleeps to this rate (default 30 / 10 fps); for the first second after the
window disappears, or while there is no drawable, it paces to the display rate instead
(`driver/d3d9/d3d9_swapchain.cpp`; README).

**Sleeper**
The sleep of the frame cap and the background throttle: the system's sleep in halves down to a
last piece of at most 1 ms, then a spin as long as the last piece recently overslept (200 µs to
4 ms). Wine's sleep on macOS wakes late by about half its length, and DXVK's `Sleep` busy-waits
4 ms and more of every sleep (`driver/d3d9/d3d9_frame_cap.h`; #44).

## Diagnostics and measurement

**Hitch**
A frame longer than `D9MT_HITCH_MS` (default 50 ms), attributed to d9mt or to the game in a
`HITCH` line under `D9MT_PROFILE` (`driver/d3d9/d3d9_hitch.h`).

**Hash suite**
The D3D9 test programs `test/code_*.c` (except `code_sm3_ref` and `code_statecache`, which have
harnesses of their own), five frames each, whose frame hashes are compared with the base commit's; the correctness gate of every PR (CONTRIBUTING, *Definition of Done*).
_Open:_ its runner (`scripts/run-hash-tests.sh`) is on the branch `chore/test-runner`, not on `main` yet.

**Base, basecopy**
**Base** is the commit a change is measured and hashed against. **Basecopy** is a byte-identical
copy of base, run next to the variants as a control for the harness (CONTRIBUTING).

**ABBA**
Paired benchmark rounds in alternating order (base, branch, branch, base, …), so drift hits both
sides equally (CONTRIBUTING; ADR-0005).

**a64**
The layout control: base and branch both built with `-falign-functions=64`; a gap that
disappears under a64 is code layout, not the change (CONTRIBUTING; ADR-0005). It doesn't
control data layout, and one block under a64 isn't a pass (PERFORMANCE.md §2.4).

**Block**
One call of the benchmark harness: paired rounds of every variant under one admission, in one
setup. A bench's mode can change between blocks, so one block's interval doesn't carry to the
next (PERFORMANCE.md §2.3, §2.5).

**Mode, mode occupancy, within-mode contrast**
A **mode** is one of the few levels a bimodal bench's frame time settles at. A binary's **mode
occupancy** is how many of its runs fall in each mode. The **within-mode contrast** is the paired
contrast over the rounds in which both runs share a mode: diagnosis, not the change's cost
(PERFORMANCE.md §2.4, §2.5).

**Layout-matched**
A comparison whose two sides share their code and data layout as far as the disassembly shows:
the a64 pair, or a change and its identical-size placebo. Per PR, a cost is judged on it
(PERFORMANCE.md §2.4).

**Outcome**
What a comparison of two binaries concludes: cost established, cost bounded below +0.25 % under
stated conditions, or unresolved. A sign flip between blocks is unresolved. A fix needs no bound:
only an established cost holds it (PERFORMANCE.md §2.4).

**Council**
Independent agent reviewers on different models who work on an unresolved flag alone, debate it
and write one result for the maintainer (PERFORMANCE.md §2.4).

**Fix, speed-only change**
For the performance rule, a **fix** is a change that prevents a bug or a GPU fault; it merges
when no cost is established at the measurable resolution. A **speed-only change** is one whose
purpose is speed, not preventing a bug; it needs evidence of identical rendering on other Mac
generations, a slower and a fanless Mac among them, or it goes in behind a switch that is off by
default. That applies to new speed-only changes; existing speed optimisations stay as they are
(PERFORMANCE.md §1; ADR-0005).

**Measurable resolution**
The smallest cost the calibrated method actually resolves between two binaries on a benchmark:
about ±1 % on bindbench today. A fix is judged at it, not at the +0.25 % flag
(PERFORMANCE.md §1, §2.4; ADR-0005).

**Smoothness guard**
The release candidate's comparison with the last stable release on what the player feels: 1 %
lows, frames over 33 and 50 ms, frame-time p99 and the present interval's spread. A measurable
regression goes to the maintainer before the release (PERFORMANCE.md §6.1; ADR-0005).

**Quiet window**
A time slot in which a benchmark runs alone on the Mac: no game, no other build or benchmark
(ADR-0005).
