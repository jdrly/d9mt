# d9mt — D3D9 on Metal 4 for Apple Silicon

A **Direct3D 9 → Metal 4** driver for Wine/CrossOver on Apple Silicon.
**No Vulkan, no MoltenVK**: D3D9 calls are translated to Metal directly, for
32-bit and 64-bit games.

It pairs DXVK's D3D9 front-end (state tracking, resources, the whole D3D9 API
surface) with a Metal 4 backend and its own shader compiler, which turns D3D9
shader bytecode straight into Apple's AIR, so no Metal toolchain or MSL is
involved at runtime. Metal is reached through a fork of DXMT's `winemetal`
bridge.

> **Fork.** This is a fork of [neo773/d9mt-v2](https://github.com/neo773/d9mt-v2),
> the successor of [neo773/d9mt](https://github.com/neo773/d9mt). neo773 wrote
> d9mt and d9mt-v2, archived the original and permitted this fork. The fork
> adds 64-bit (x64) builds, macOS 27 fixes, rendering fixes, persistent shader
> and pipeline caches, asynchronous shader compilation and memory profiles.
> Its full history, neo773's included, is in this repository.

> **Tested with a handful of games.** It has been run with:
>
> - **GTA IV** (32-bit): neo773's target while developing d9mt and d9mt-v2
> - **Star Wars: The Old Republic** (64-bit): this fork's main test target
> - **Star Wars Galaxies: Legends** (32-bit, community server)
>
> Nothing else has been tried yet, so expect unimplemented paths on other
> games. d9mt is not affiliated with or endorsed by the publishers or
> operators of these games; all trademarks belong to their owners.

```
YourGame.exe  (32- or 64-bit Windows PE, x86 code under Rosetta 2)
   │  loads
   ▼
d3d9.dll      ← THIS PROJECT (mingw PE): DXVK D3D9 front-end + d9mt Metal 4 backend
   │            DXSO bytecode → dxbc-spirv IR (sm3 parser) → serialized to the bridge
   ▼
winemetal.dll ← THIS PROJECT (fork of DXMT's bridge, PE side, Wine builtin)
   │  Wine unix call
   ▼
winemetal.so  ← THIS PROJECT (x86_64 Mach-O, Wine's unix side):
                IR → AIR (LLVM 15 bitcode) → metallib, AIR + pipeline disk caches,
                Metal 4 command path  ──►  Metal  ──►  CAMetalLayer
```

## Status

- **Experimental, but playable** in the games above. In our testing, the
  64-bit MMO renders correctly and holds 60 fps with vsync at 4K on an
  M5 Max.
- **Metal 4 only**: MTL4 command queues, allocators, argument tables,
  residency sets and the MTL4 compiler. There is no Metal 3 path.
- **Shader compiler**: SM1–3 bytecode is parsed with dxbc-spirv and compiled
  to AIR by d9mt's own LLVM-based writer (`air/`) on Wine's unix side. The
  fixed-function pipeline uses DXVK's ubershaders, precompiled into an
  embedded metallib.
- **Caches**: compiled shaders go to a persistent AIR cache and pipelines to
  Metal 4 pipeline archives, so a second visit to an area is much smoother
  than the first. Shaders and pipelines compile on worker threads.
- **Memory profiles** pick pool sizes and reported video memory from the
  Mac's RAM (8/16/24 GB Macs get smaller pools).
- This is research-grade software for one platform (Apple Silicon + a
  CrossOver-based Wine). Expect rough edges.

## Requirements

- **Apple Silicon** Mac on **macOS 26 or later** (Metal 4). Only macOS 27
  has been tested; see Known issues.
- **Rosetta 2**: Wine's unix side, and with it `winemetal.so`, runs as
  x86_64.
- **A Wine engine with an x86_64 unix side and the winemac hooks DXMT's
  bridge uses** (`macdrv_get_metal_layer`). Tested with the CrossOver 24.0.7
  engine; engines of CrossOver-based wrappers should work. Vanilla upstream
  Wine is untested. 32-bit games need 32-on-64 (wow64) support, as in
  CrossOver.

To build (see below): Homebrew `bash` (≥ 4.3), `mingw-w64` (i686 and
x86_64), `cmake`, `ninja`, `python3`, Xcode Command Line Tools; optionally
Xcode with the Metal Toolchain plus `glslang` and `spirv-cross` to regenerate
the embedded metallibs.

## Build

From the repo root. The scripts need bash ≥ 4.3 (`/bin/bash` 3.2 fails), so
put Homebrew's first in `PATH`.

LLVM and `winemetal.so` target macOS 26.0, whatever the SDK; set
`MACOSX_DEPLOYMENT_TARGET` to change it. An LLVM built before this default
(with the SDK's version) gives linker warnings: rebuild it. The embedded
metallibs come in two builds: the SDK's newest Metal language (MSL 4.1 with
Xcode 27) and a Compat build for that minimum. The driver loads the newest
and falls back to Compat where Metal rejects it (MSL 4.1 on macOS 26).

```bash
export PATH="/opt/homebrew/bin:$PATH"

# 1. Fetch dxbc-spirv, pinned to c5c1a5b9 (-> third_party/dxbc-spirv).
scripts/fetch-deps.sh

# 2. Build LLVM 15.0.7 as static x86_64 libs (-> toolchains/llvm-x86_64).
#    Once; a large clone and a long build.
scripts/build-llvm-unixside.sh

# 3. The bridge: winemetal.so (x86_64 Mach-O, includes the AIR compiler) and
#    winemetal.dll for i386 and x86_64, marked as Wine builtins.
scripts/build-bridge.sh

# 4. The driver: build/driver-x64/d3d9.dll (64-bit games) and/or
#    build/driver/d3d9.dll (32-bit games).
ARCH=x64 scripts/build-driver.sh
ARCH=x86 scripts/build-driver.sh
```

- No DXVK or DXMT checkout and no Wine engine are needed to build;
  `driver/` and `bridge/` contain everything.
- `build-driver.sh` regenerates `driver/d3d9/d3d9_ff_metallib.h` and
  `driver/backend/mtl_blit_metallib.h` when their sources (or the script)
  are newer and `xcrun metal`, `glslangValidator` and `spirv-cross` are
  available; otherwise it keeps the committed headers. On Xcode 27 run
  `xcodebuild -downloadComponent MetalToolchain` once. If `xcode-select`
  points at the Command Line Tools, the script falls back to
  `/Applications/Xcode.app`.
- `build-bridge.sh` ends with a smoke test that needs reference metallibs
  from `scripts/build-air-compiler.sh` (Homebrew `llvm@15` and a SPIRV-Cross
  checkout in `third_party/SPIRV-Cross`). Without them the test is skipped;
  `SMOKE=0` skips it explicitly.
- The driver and the bridge share an ABI: always rebuild and install them
  together.

### Release binaries

Absolute build paths are mapped out of the binaries (`-ffile-prefix-map`),
but the driver is built with `-g` and is about 80 MB. Strip before
installing or shipping:

```bash
x86_64-w64-mingw32-strip build/driver-x64/d3d9.dll
i686-w64-mingw32-strip   build/driver/d3d9.dll
strip -x build/bridge/winemetal.so
```

A binary release must carry the notices listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

### Tests

Native unit tests (no Wine or GPU needed), from the repo root:

```bash
clang++ -std=c++17 -I driver/d3d9 -I driver/mtl test/native/buffer_tracking_test.cpp -o /tmp/btt && /tmp/btt
clang++ -std=c++17 -I driver/util test/native/fenced_pool_test.cpp -o /tmp/fpt && /tmp/fpt
clang++ -std=c++17 -I driver/d3d9 test/native/hitch_test.cpp -o /tmp/ht && /tmp/ht
clang++ -std=c++17 -I driver/mtl test/native/memory_profile_test.cpp -o /tmp/mpt && /tmp/mpt
clang++ -std=c++17 -I test/native/stubs -I driver/backend test/native/upload_ring_test.cpp driver/backend/mtl_ring.cpp -o /tmp/urt && /tmp/urt
clang++ -std=c++17 -I driver/backend test/native/attachment_opt_test.cpp -o /tmp/aot && /tmp/aot
```

`scripts/test-compile-mt.sh` runs the compile-path concurrency tests (with
ThreadSanitizer) against the built bridge. The scripts under `scripts/run-*`
and `scripts/dump-verify.sh` drive the D3D9 test programs in `test/` under a
CrossOver bottle (`CX` and `BOTTLE` must be set).

## Install & run

> **Use a dedicated Wine engine copy.** d9mt's `winemetal` replaces the
> engine's builtin `winemetal`, which DXMT (D3D10/11) also uses. **DXMT
> games stop working on that engine.** Don't modify
> `/Applications/CrossOver.app` itself: updates overwrite it and it is
> code-signed.

1. **Copy a Wine engine**, e.g. CrossOver's
   `Contents/SharedSupport/CrossOver` or your wrapper's engine folder. Call
   it `$ENGINE`.

2. **Install the bridge into the engine** (originals are kept as
   `*.cx-orig`; `--restore` puts them back):

   ```bash
   CX="$ENGINE" scripts/install-bridge-builtins.sh
   ```

   which copies `build/bridge/winemetal.so` to `lib/wine/x86_64-unix/`,
   `build/bridge/winemetal64.dll` to `lib/wine/x86_64-windows/winemetal.dll`
   and `build/bridge/winemetal32.dll` to `lib/wine/i386-windows/winemetal.dll`.

3. **Deploy the driver as `d3d9.dll`**: next to the game's `.exe`, or into
   the prefix (`drive_c/windows/system32/` for a 64-bit game,
   `drive_c/windows/syswow64/` for a 32-bit one).

   ```bash
   cp build/driver-x64/d3d9.dll "$GAME_DIR/d3d9.dll"   # 64-bit game
   cp build/driver/d3d9.dll     "$GAME_DIR/d3d9.dll"   # 32-bit game
   ```

4. **Launch** with `d3d9` overridden to the native (our) DLL, using the
   engine from step 1:

   ```bash
   WINEDLLOVERRIDES="d3d9=n,b" "$ENGINE/bin/wine" --bottle "My Game" \
     start /unix "$GAME_DIR/YourGame.exe"
   ```

   `--bottle` is CrossOver's; with plain Wine set `WINEPREFIX` instead. The
   override can also go in `winecfg` → Libraries or your wrapper's DLL
   override setting.

To check that it's active, set `DXVK_LOG_PATH=<dir>` and look for
`<game>_d3d9.log`, or enable the Metal HUD (`MTL_HUD_ENABLED=1`).

## Configuration

d9mt reads DXVK-style configuration: `dxvk.conf` next to the game's `.exe`
(or the file in `DXVK_CONFIG_FILE`), with environment variables taking
priority where both exist. Under CrossOver, environment variables reach the
game only through the bottle's `cxbottle.conf` (`[EnvironmentVariables]`),
not through a shell `export`.

### `dxvk.conf` options

The D3D9 options of DXVK's front-end apply (`d3d9.*`, `dxvk.maxFrameRate`,
...). Options added or worth knowing:

| Option | Default | Effect |
|---|---|---|
| `d3d9.presentInterval` | `-1` (the game decides) | `1` forces vsync, e.g. for games that request immediate present in borderless mode and tear |
| `d3d9.maxFrameRate` / `dxvk.maxFrameRate` | `0` (off) | frame-rate cap |
| `d3d9.maxAvailableMemory` | `-1` (= the memory profile's value) | video memory (MB) reported to the game |
| `d3d9.allowOutOfRangeVsDefs` | `False` | accept hardware vertex shaders whose only float constants ≥ 256 are `def` immediates (advanced) |
| `d9mt.memoryProfile` | `auto` | `auto`, `low`, `medium`, `high`, `ultra` or `unlimited` (see below); env `D9MT_MEMORY_PROFILE` |
| `d9mt.reportedVramMB` | per profile | video memory reported when `d3d9.maxAvailableMemory` is unset |
| `d9mt.memoryBudgetMB` | per profile | soft Metal budget; currently only logged |
| `d9mt.writeOnlyDirect` | `True` (`False` in `unlimited`) | map DEFAULT-pool WRITEONLY buffers directly, like upstream DXVK; env `D9MT_WRITEONLY_DIRECT` |
| `d9mt.writeOnlyDemoteLocks` | `2` (`0` in `unlimited`) | a directly mapped buffer locked in place more often than this goes back to a staging copy; `0` = never; env `D9MT_WRITEONLY_DEMOTE` |
| `d9mt.memoryStats` | `False` | log a `DIAG MEM` line every 256 frames; env `D9MT_MEMSTATS` (`1`, or a frame interval ≥ 16) |
| `d9mt.resourceLabels` | `False` | label Metal resources for Xcode's GPU tools; env `D9MT_LABELS` |
| `d9mt.pipelineWaitBudgetMs` | `-1` (off: wait as long as it takes) | most the render thread waits per frame for shaders/pipelines still compiling before it skips the draws that need them (see below); `0` = never wait (= `D9MT_ASYNC_SKIP=1`); max `10000`; env `D9MT_PSO_WAIT_MS` |
| `d9mt.attachmentOpt` | `True` | attachment load/store optimization (see below); `D9MT_NO_ATTACHMENT_OPT=1` turns it off |

**Memory profiles.** `auto` picks by installed RAM: up to 8 GB `low`, up
to 16 GB `medium`, up to 24 GB `high`, below 64 GB `ultra`, otherwise
`unlimited`. The tiers set the reported video memory (`low` 2 GB, `medium`
3 GB, otherwise 4 GB) and how much the driver keeps pooled
(staging blocks, staging bytes in flight, spare buffer backings, idle
upload-ring chunks, command allocators). `unlimited` behaves like the driver
before profiles existed and is meant for A/B comparisons.

**Pipeline wait budget.** When a draw needs a shader or pipeline that is
still compiling on the worker threads, the render thread waits for it by
default: no draw is ever missing, but a slow compile is a hitch.
`D9MT_ASYNC_SKIP=1` never waits and skips such draws instead: no hitch, but
objects pop in a few frames late. `d9mt.pipelineWaitBudgetMs = N` sits in
between: the render thread waits for pending compiles, but at most N ms per
frame in total (each wait gets what is left of the frame's N ms, so many
pending pipelines can't stack up their waits). Draws whose compile isn't done
when the budget runs out are skipped for that frame, as with
`D9MT_ASYNC_SKIP`; the compile carries on and the draws come back once it is
done. Most compiles finish within a few ms and are still waited for; only
the rare slow one is skipped, so a budget of e.g. `30` caps compile hitches
at about 30 ms in exchange for occasional brief pop-in. While a draw is
skipped, occlusion queries that cover it count fewer samples. Clears and
blits are never skipped (their pipelines are built on the spot). `D9MT_ASYNC_SKIP=1` takes
precedence and acts as a budget of `0`; without compile workers
(`D9MT_COMPILE_THREADS=0`) the budget is ignored. With `D9MT_PROFILE`, the
`COMPILE` and `HITCH` lines count skipped draws (`skipped=`) and waits cut
short by the budget (`budgetHit=`).

**Attachment load/store optimization.** A Metal render pass loads each
attachment from memory when it starts and stores it when it ends. d9mt
creates a pass's encoder only when the pass ends, so it knows what the pass
did, and it tracks for each single-subresource attachment image whether its
contents are a known clear value:

- An attachment that holds a known clear (from creation or a clear) is
  Clear-loaded instead of read from memory.
- If no draw of the pass writes it and nothing samples it during the pass,
  its store is skipped. Its contents are still the clear value: the next pass
  Clear-loads it, and any other reader (sampling, copies, blits, resolves,
  readbacks, Present) gets it by a re-clear first.
- A pass without draws whose clears needn't be stored gets no encoder at all;
  the clear moves into the next pass's load action.
- An MSAA color resolve (StretchRect) right after the pass that rendered the
  source becomes that pass's `StoreAndMultisampleResolve`, instead of a pass
  of its own that loads every sample again.
- With `D3DPRESENTFLAG_DISCARD_DEPTHSTENCIL`, the pass still rendering into
  the auto depth-stencil at Present doesn't store it, and the next pass
  doesn't load it (wined3d discards it too).

Everything else is loaded and stored as before. Written contents are never
dropped: when a pass ends, nothing tells whether a later pass needs them
(only `D9MT_DEFERRED`'s whole-frame rules can). In SWTOR this removes the
load and store of the 2048² R32F color target that its shadow passes bind with
color writes off, about 32 MB per shadow pass. Memoryless textures aren't
used: D3D9 surfaces outlive the pass. `no_attach_opt` in the
`D9MT_DEBUG_FILE` flags switches back to the previous actions live, for A/B
tests; `att_trace` logs the decisions. `D9MT_DEFERRED` turns it off (deferred
passes have their own load/store rules).

### Environment variables

| Variable | Default | Effect |
|---|---|---|
| `D9MT_AIR_CACHE` | on | persistent compiled-shader cache (`<user cache dir>/d9mt/air-cache.sqlite3`); `0` turns it off, a path relocates it |
| `D9MT_PSO_ARCHIVE` / `D9MT_PSO_ARCHIVE_DIR` | on | Metal 4 pipeline archives in `<user cache dir>/d9mt/pso/` (last 8 sessions); `0` turns them off, `_DIR` relocates them |
| `D9MT_COMPILE_THREADS` | `min(4, cores/2)` | shader/pipeline compile worker threads; `0` compiles on first use |
| `D9MT_PS_PREFETCH` | `1` | compile pixel shaders at creation; `0` waits until a draw needs them |
| `D9MT_ASYNC_SKIP` | off | `1` skips draws whose pipeline isn't ready yet: less stutter, brief pop-in; overrides `D9MT_PSO_WAIT_MS` |
| `D9MT_PSO_WAIT_MS` | off | wait at most this many ms per frame for pending pipelines, then skip (see `d9mt.pipelineWaitBudgetMs`); `off` or `-1` overrides a `dxvk.conf` value |
| `D9MT_NO_ATTACHMENT_OPT` | off | `1` turns the attachment load/store optimization off (see `d9mt.attachmentOpt`) |
| `D9MT_BG_FPS` / `D9MT_BG_FPS_MINIMIZED` | `30` / `10` | frame cap while the window is hidden / minimized; `0` turns it off |
| `D9MT_DRAWABLE_TIMEOUT_MS` | max(3 frames, 50 ms) | longest wait for a drawable before a present is skipped |
| `D9MT_AIR_FASTMATH` | off | `1` restores full fast-math in compiled shaders (faster, but can produce NaN specks) |
| `D9MT_METALFX` | off | MetalFX spatial upscaling at present. **Currently broken** (selector bug; fix pending) |
| `D9MT_MEMORY_PROFILE`, `D9MT_WRITEONLY_DIRECT`, `D9MT_WRITEONLY_DEMOTE`, `D9MT_MEMSTATS`, `D9MT_LABELS` | | see the `dxvk.conf` table |
| `DXVK_LOG_PATH`, `DXVK_LOG_LEVEL` | none, `info` | log directory and verbosity, as in DXVK |

<details><summary>Developer and diagnostic switches</summary>

| Variable | Effect |
|---|---|
| `D9MT_PROFILE` | per-frame timing lines (`PSOBUILD`, `AIRCOMPILE`, compile counters) and `HITCH` lines that attribute frames longer than `D9MT_HITCH_MS` (default 50) to d9mt or to the game |
| `D9MT_TRACE` | verbose trace lines |
| `D9MT_WSILOG` | window-system / presenter log (bridge, stderr) |
| `D9MT_DEBUG_FILE=<Windows path>` | live debug flags file, polled once a second: `occ_visible`, `occ_trace`, `occ_sync`, `no_scissor`, `no_depthbias`, `cull_none`, `depth_clamp`, `no_ztest_blended`, `no_sameview_skip`, `no_clipplanes`, `bools_off`, `no_attach_opt` (previous attachment load/store actions), `att_trace` (log up to 400 render-pass load/store decisions each time it's set), and `dump` to dump the next frame's draws. Off unless set |
| `D9MT_SHADER_DUMP_PATH=<Windows dir>` | disassembly of vertex shaders rejected (or accepted via `d3d9.allowOutOfRangeVsDefs`) for using more than 256 float constants. Off unless set |
| `D9MT_AIR_ERRLOG=<host path>` | append the AIR compiler's reason for rejecting a shader. Off unless set |
| `D9MT_DUMP_TRIGGER=1` | backbuffer dump for the test scripts (`C:/d9mt-test/DUMP` trigger) |
| `D9MT_GPUCAPTURE=<n>\|trigger`, `D9MT_GPUCAPTURE_COMMITS` | Xcode GPU trace to `/tmp/d9mt.gputrace` |
| `D9MT_PSO_FLUSH_EVERY` | pipelines between archive flushes (default 256) |
| `D9MT_DEFERRED`, `D9MT_XFER_HOIST` | experimental: deferred render passes; hoisting vertex/index uploads into an earlier command buffer |
| `D9MT_NO_SAMEVIEW_SKIP`, `D9MT_NO_PSOMRU`, `D9MT_NOPSOCACHE`, `D9MT_NO_PASSMERGE`, `D9MT_NO_RULE_{B,C,E,F}`, `D9MT_NOCOLORCLEAR`, `D9MT_NO_AB`, `D9MT_RTSKIP`, `D9MT_STALL_PACE=0` | A/B escape hatches that turn individual optimizations off |
| `PROFILE_INSTRUMENT=1` (build) + `D9MT_PROFILE_INSTRUMENT=1` | per-function CPU profiler build |

</details>

## Known issues

- **Conflicts with DXMT on the same engine.** The bridge replaces the
  engine's `winemetal`; DXMT's D3D10/11 entry points are stubbed out and
  the render-pass struct has grown, so D3D10/11 games using DXMT break or
  crash on that engine. Use a dedicated engine copy until the bridge gets
  its own module name.
- **Driver and bridge must come from the same build.** There is no version
  handshake; mixing them can crash.
- **MetalFX upscaling** (`D9MT_METALFX`) is broken (selector bug); a fix is
  pending.
- **First-time stutter** in new areas while shaders and pipelines compile.
  Compiles run on worker threads and are cached on disk, so later visits are
  smoother. `D9MT_ASYNC_SKIP=1` trades stutter for pop-in;
  `d9mt.pipelineWaitBudgetMs` caps the stutter and only skips what takes
  longer.
- **The disk caches have no size cap.** The AIR cache is one sqlite file
  shared by all games (entries from other driver builds are dropped); the
  pipeline archives keep the last 8 sessions.
- **macOS 26 is barely tested.** Before the Compat metallibs (see Build),
  builds with Xcode 27 embedded only MSL 4.1 metallibs, which Metal on
  macOS 26 rejects ("language version 4.1 … not supported"): a black
  screen. Checked on 26.6.2 in a VM, whose paravirtual GPU has no Metal 4,
  so d9mt itself can't run there; no in-game test on 26 yet.
- **Gamma ramps** (`SetGammaRamp`, used by some brightness sliders) are
  ignored.
- **Not implemented or unverified**, from code review: instance divisors
  > 1, indexed triangle fans, stride-0 vertex streams, and hardware vertex
  shaders that address more than 256 float constants (rejected, as in
  DXVK; see `d3d9.allowOutOfRangeVsDefs`).

## How it works (deeper)

- The D3D9 front-end in `driver/d3d9` is DXVK's, extracted and with Vulkan
  removed (`driver/EXTRACTION-LOG.md`, `driver/UPSTREAM`). It tracks all
  D3D9 state and records work through DXVK's command-stream thread into a
  Metal seam (`driver/mtl`).
- The backend (`driver/backend`) implements that seam on Metal 4: argument
  tables, residency sets, MTL4 command buffers and allocators, render and
  compute encoders, MTL4 pipeline compilation, the upload ring and
  presentation to the window's `CAMetalLayer`.
- Shaders: the game's DXSO bytecode is parsed by dxbc-spirv's `sm3`
  front-end into dxbc-spirv's IR in `d3d9.dll`, serialized and passed
  through the bridge. On Wine's unix side, `airlib`/`air` lower the IR to
  AIR (LLVM 15 bitcode) and wrap it in a metallib, which Metal loads
  directly. Results are cached in the AIR cache.
- The bridge (`bridge/`) is DXMT's `winemetal` with additions
  (`bridge/UPSTREAM`): vertex-descriptor pipelines, per-slot samplers and
  vertex textures, the AIR compile calls and the full Metal 4 command
  surface, plus persistent pipeline archives.
- `docs/dev-notes/` has neo773's design notes and research from v2's
  development (historical, not maintained).

## Credits

- **[neo773](https://github.com/neo773)**: author of
  [d9mt](https://github.com/neo773/d9mt) and
  [d9mt-v2](https://github.com/neo773/d9mt-v2): the Metal 4 backend, the
  AIR shader compiler, the bridge extensions and the DXVK extraction this
  fork is built on.
- **[DXVK](https://github.com/doitsujin/dxvk)** (Philip Rebohle, Joshua
  Ashton and contributors): the D3D9 front-end and its fixed-function
  shaders.
- **[DXMT](https://github.com/3Shain/dxmt)** (Feifan He / CodeWeavers): the
  `winemetal` bridge that carries Metal calls across Wine's PE/unix
  boundary.
- **[dxbc-spirv](https://github.com/doitsujin/dxbc-spirv)** (Philip
  Rebohle): the shader bytecode parser and IR.
- **[LLVM](https://llvm.org)**: bitcode writing for AIR.
- **[SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross)** and
  **[glslang](https://github.com/KhronosGroup/glslang)**: build-time
  conversion of the fixed-function shaders.
- **Wine** and **CodeWeavers**: the platform all of this runs on.
- Fork maintenance, x64, memory and rendering work: Jan Drlý.

## License

See the licenses of the third-party components in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and
[`LICENSES/`](LICENSES/). d9mt's own code follows suit:

- d9mt's own code and the DXVK-derived driver are under the **zlib/libpng
  license** ([`LICENSE`](LICENSE)), DXVK's license, with DXVK's copyright
  notice kept. `driver/` is an altered version of DXVK, not the original.
- The **winemetal bridge** in `bridge/` follows DXMT and is under the **GNU
  LGPL 2.1 or later** ([`bridge/LICENSE`](bridge/LICENSE),
  [`bridge/COPYING.LIB`](bridge/COPYING.LIB)).
