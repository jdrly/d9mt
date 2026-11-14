# Configuration

Every `dxvk.conf` option and environment variable d9mt reads. The [README](../README.md) has the
short version.

d9mt reads DXVK-style configuration: `dxvk.conf` next to the game's `.exe`
(or the file in `DXVK_CONFIG_FILE`), with environment variables taking
priority where both exist. Under CrossOver, environment variables reach the
game only through the bottle's `cxbottle.conf` (`[EnvironmentVariables]`),
not through a shell `export`.

## `dxvk.conf` options

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
| `d9mt.stateCache` | `True` | pipeline state cache: record the pipelines the game uses, rebuild them in the background in later sessions ([state-cache.md](state-cache.md)); `D9MT_STATE_CACHE=0` turns it off too |

## Memory profiles

`auto` picks by installed RAM: up to 8 GB `low`, up
to 16 GB `medium`, up to 24 GB `high`, below 64 GB `ultra`, otherwise
`unlimited`. The tiers set the reported video memory (`low` 2 GB, `medium`
3 GB, otherwise 4 GB) and how much the driver keeps pooled
(staging blocks, staging bytes in flight, spare buffer backings, idle
upload-ring chunks, command allocators). `unlimited` behaves like the driver
before profiles existed and is meant for A/B comparisons.

## Pipeline wait budget

When a draw needs a shader or pipeline that is
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

## Attachment load/store optimization

A Metal render pass loads each
attachment from memory when it starts and stores it when it ends. d9mt
creates a pass's encoder only when the pass ends, so it knows what the pass
did, and it tracks for each single-subresource attachment image whether its
contents are a known clear value:

- An attachment that holds a known clear (from creation or a clear) is
  Clear-loaded instead of read from memory.
- If no draw of the pass writes it and nothing samples it during the pass,
  its store is skipped. Its contents are still the clear value: the next pass
  Clear-loads it, and any other reader (sampling, copies, blits, resolves,
  readbacks, Present) gets it by a re-clear first (`restore` in `STATS`).
  Sampling reads only the view's aspects: a depth-stencil texture sampled as a
  shadow map or INTZ (depth) doesn't need its unstored stencil clear.
- Once a reader has needed such a clear (a target cleared every frame and
  sampled or copied later in the frame), the passes of the next 8 frames store
  it as before instead of leaving it to a re-clear; the hint lapses when the
  reads stop.
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
tests; `att_trace` logs the decisions and the restores. With `D9MT_PROFILE`, a
`RESTORE` line follows each second's `STATS` line when there were restores:
the most frequent images (Metal format, size, aspects re-cleared), why (`sample`,
`copy`, `blit`, `resolve`, `readback`, `partial-clear`, ...) and the D3D9 call
they came from, e.g. `120x 2048x2048 Depth32Float_Stencil8 s sample@Draw`;
`RESTORE-SUMMARY` has the session's totals when the device is destroyed. `D9MT_DEFERRED` turns it off (deferred
passes have their own load/store rules).

## Environment variables

| Variable | Default | Effect |
|---|---|---|
| `D9MT_AIR_CACHE` | on | persistent compiled-shader cache (`<user cache dir>/d9mt/air-cache.sqlite3`); `0` turns it off, a path relocates it |
| `D9MT_PSO_ARCHIVE` / `D9MT_PSO_ARCHIVE_DIR` | on | Metal 4 pipeline archives in `<user cache dir>/d9mt/pso/` (last 8 sessions); `0` turns them off, `_DIR` relocates them |
| `D9MT_STATE_CACHE` | `~/Library/Caches/d9mt/state/<exe>.d9mtstate` | pipeline state cache file; `0` turns the cache off, a Unix directory or `.d9mtstate` file path relocates it. Launchers should set it to `"$(getconf DARWIN_USER_CACHE_DIR)d9mt/state"`, next to the AIR cache and PSO archives. Without it, d9mt uses that dir if Wine passes `TMPDIR` (CrossOver's doesn't), else `~/Library/Caches/d9mt/state` (home from `WINEHOMEDIR`), else the cache stays off (logged) |
| `D9MT_STATE_CACHE_MAX` | `1024` | most pipelines kept per game (least recently used dropped first), `64`–`16384`; every session's PSO archive holds every prewarmed pipeline, ≈ 45 KB each |
| `D9MT_STATE_CACHE_THREADS` | `2` | background prewarm threads (lowest priority, the second one only for long queues), `0`–`4`; `0` records and loads but never prewarms |
| `D9MT_COMMIT_FEEDBACK` | on | `0` commits without a feedback handler. By default each commit carries one (a new `MTL4CommitOptions` per commit), and a commit the GPU failed is a `D9MT-GPU-ERROR` line on stderr: the error's name and code (`PageFault`, `Timeout`, `InvalidResource`, …; Metal 4's queue errors as `queue Timeout` etc.), its description, the GPU start and end times, the host time and the commit count, the encoders that didn't complete when Metal lists them in the error (index in recorded order, state, label), and the error's other userInfo keys. At most 30 lines per 10 s, then a count of the suppressed. macOS's unified log has these errors only as `IOGPUMetalError: <private>` |
| `D9MT_COMPILE_THREADS` | `min(4, cores/2)` | shader/pipeline compile worker threads; `0` compiles on first use |
| `D9MT_COMPILE_MAXCONC` | `0` | `1` asks Metal at device creation to maximize concurrent pipeline compilation (`MTLDevice.shouldMaximizeConcurrentCompilation`): more of Metal's own compiler threads, which can compete with the game on Macs with few performance cores. A scheduling request, not guaranteed parallelism; an A/B switch. Independent of `D9MT_COMPILE_THREADS` |
| `D9MT_PS_PREFETCH` | `1` | compile pixel shaders at creation; `0` waits until a draw needs them |
| `D9MT_ASYNC_SKIP` | off | `1` skips draws whose pipeline isn't ready yet: less stutter, brief pop-in; overrides `D9MT_PSO_WAIT_MS` |
| `D9MT_PSO_WAIT_MS` | off | wait at most this many ms per frame for pending pipelines, then skip (see `d9mt.pipelineWaitBudgetMs`); `off` or `-1` overrides a `dxvk.conf` value |
| `D9MT_IMAGE_MAP` | on | `0` turns off the image map: `d9mt: image <when> <name> <start>-<end>` lines (their `__TEXT` range) for Metal, each device's driver (AGXMetal…), IOGPU, MetalTools and GPUCompiler (those loaded), and the dyld shared cache's range (`d9mt: image <when> dyld-shared-cache …`), so a `D9MT-FATAL` address that no PE module holds (`no module known at install`) maps to an image and offset offline (`atos` on the same macOS build). Logged by the bridge (`bridge/unix/winemetal_unix.c`) twice per process: at the first device enumeration (`devices`) and before the first render pipeline build (`first-pso`). Never from a dyld callback or an exception handler, and no walk of dyld's image list: each image is found from an address it holds, logged only if it lies in the shared cache (mapped for the life of the process), and held with `dlopen(RTLD_NOLOAD)` while its header is read. With it, once, a `d9mt: device "<name>" …` line per Metal device: its limits (threads and memory per threadgroup, buffer length, working set, argument-buffer samplers, sparse tile size, concurrent compilation tasks) and families (Apple7–Apple10, Metal3, Metal4) |
| `D9MT_FATAL_LOG` | on | `0` turns off the fatal-exception log: a vectored exception handler in d3d9.dll writes a `D9MT-FATAL` line for each fatal exception (an NTSTATUS of the 0xC0000000 class: access violation, integer divide by zero, illegal instruction, …) with the code, the address as module+RVA (a module d3d9.dll knew at install, its range read then: the exe, d3d9.dll, winemetal.dll, ntdll, kernelbase, kernel32, user32, the C runtimes …; any other address, a DLL loaded later or one outside every PE image such as Metal, is the raw address with `no module known at install`, see `D9MT_IMAGE_MAP`), the thread's id, and the d9mt step entered last on any thread (render PSO build, MSAA texture creation, Present, …): the first 8, then one line that says more were left out. C++ throws, thread naming and debug output are not of that class and are never logged; a stack overflow is never logged either. It never handles anything (always `EXCEPTION_CONTINUE_SEARCH`), so a game that catches its own crash still does, and still ends with the exception code as its exit code (0xC0000094 → Unix status 148). In the handler it allocates nothing, takes no lock (no `VirtualQuery`, which takes Wine's virtual-memory lock), reads no image; the line goes out in one `write` to the log (Wine's `__wine_dbg_write`). Installing it pins d3d9.dll for the life of the process. At most 64 threads at once are inside it (a lock-free table of thread ids, its recursion guard); a fault on a 65th at the same time isn't logged, and goes on as without the log. It runs only when an exception is dispatched; the steps are one relaxed pointer store each at a few places, none per draw (`driver/util/util_fatal_log.h`) |
| `D9MT_GPU_CANARY` | on | `0` turns off the GPU canary: three cases mark a command buffer's slot: a present copy into the drawable marks it in its own encoder; a present that isn't a copy (a scaled blit, MetalFX) gets a marking encoder of its own every frame; a command buffer that doesn't present gets one on every 4th, and only if no canary ran for 250 ms (`driver/backend/mtl_canary_gate.h`). The mark goes into the command buffer's slot in a 16 KiB buffer; a command buffer that completed without its mark was discarded by the GPU (after a page fault: always the command buffers queued after it, the faulting one if the fault came before the mark); `D9MT-GPU-CANARY: on` once the first mark arrived (or `off` and why), so a log shows the canary worked and is a `D9MT-GPU-LOST` line in the log (the first 20, then a summary at most every 10 s, and the session's total). macOS itself only logs `IOGPUMetalError` in its unified log |
| `D9MT_NO_ATTACHMENT_OPT` | off | `1` turns the attachment load/store optimization off (see `d9mt.attachmentOpt`) |
| `D9MT_FLUSH_CAP` | `0` | `1` caps the heuristic flushes at strong hints, as upstream DXVK does on tiling GPUs: a full CS chunk and the other weak hints no longer submit a command buffer mid-frame. An A/B switch, read at device creation; `d3d9.reproducibleCommandStream` takes precedence ([`g1-early-commit.md`](g1-early-commit.md) §7) |
| `D9MT_FPS_CAP` | off | frame rate cap with even frame times (`20`–`1000`), for heat and battery: with vsync and a cap that divides the refresh rate (30 at 60 Hz; 30, 40, 60 at 120 Hz) every frame stays on screen for the same number of refreshes (`presentAfterMinimumDuration`); otherwise (40, 45 at 60 Hz, or vsync off) an exact timer, frames then shown for uneven numbers of refreshes at 60 Hz; at or above the refresh rate with vsync, nothing (vsync caps already). Logs a `frame cap` line. Takes over from `d3d9.maxFrameRate` while it caps ([`driver/d3d9/d3d9_frame_cap.h`](../driver/d3d9/d3d9_frame_cap.h)) |
| `D9MT_FPS_CAP_ALIGN` / `D9MT_FPS_CAP_SPIN_US` | on / `200` | `0`: the cap's timer only, no vsync alignment; the least time the cap's and the background throttle's sleep spin before the deadline (more while the system's sleep oversleeps, at most 4 ms) |
| `D9MT_BG_FPS` / `D9MT_BG_FPS_MINIMIZED` | `30` / `10` | the background throttle: the rate Present sleeps to while the window is hidden / minimized; `0` turns it off |
| `D9MT_DRAWABLE_TIMEOUT_MS` | max(3 frames, 50 ms) | longest wait for a drawable before a present is skipped |
| `D9MT_DEPTH_BIAS_VS` | off | `1` (opt-in, for A/B and testers) moves the constant depth bias (`D3DRS_DEPTHBIAS`) into the vertex shader on float depth formats (D24S8, D24X8, INTZ, D24FS8 and D32F_LOCKABLE, which are all Depth32Float(_Stencil8) on Apple GPUs). D3D9's bias is absolute in depth-buffer units, but Metal's constant bias on a float format scales with the depth's exponent, so the hardware path gives only half the bias below z = 0.5, a quarter below 0.25, and so on (#163). The vertex shader adds k·w to clip-space z (k = DEPTHBIAS / the viewport's depth range) and writes the original near and far planes as clip distances 6 and 7, so clipping stays the original geometry's (for finite positions with normal, not subnormal, z and w; no claim beyond that); the hardware keeps the slope term, and Metal's depth clip mode is clamp while k ≠ 0, so a fragment the bias moves past the near or far plane is kept. **The biased depth is not clamped to the viewport's depth range:** with MinZ and MaxZ narrower than [0, 1] it can leave [MinZ, MaxZ] by up to the bias, as it does on the hardware path. D3D9's documentation defines no clamp of the biased depth (its viewport page says MinZ and MaxZ "are not used for clipping"); D3D10 and later clamp the final depth to the viewport's range (D3D11.3 §15.6), and d9mt doesn't follow that rule here. Apple documents no range for Metal's clamp mode, on the M5 Max it isn't the viewport's (`test/code_depth_bias.c`, rows K), and Metal's render encoder offers no clamp to a chosen range (its depth bounds test discards fragments), so one per sample would need a depth write in every pixel shader of a biased draw. Only triangle draws with a programmable vertex shader and a pixel shader that doesn't write depth; not with \|k\| > 2^-6 (a 0.001-wide depth range, say), a subnormal k, or the `depth_clamp` debug flag; a vertex with \|z\| > 2^126 or \|w\| outside [2^-100, 2^126] keeps its unbiased z (a triangle mixing such vertices gets a partial bias, and that bias's own gradient is an artificial slope: with a slope-scaled bias it moves the whole triangle); D16, fixed-function and POSITIONT draws, lines and points keep the hardware path. Off: today's behaviour; no vertex shader is rewritten, so every shader's code and AIR cache entry are the same as without the feature. On: every vertex shader is compiled in its rewritten form (its own AIR cache entries, compiled once; switching back uses the unrewritten entries again), and without a bias the extra code leaves z and the clip distances unchanged; a vertex shader the rewrite can't handle logs `D9MT: depth bias: <vs>: not in the expected form` and keeps the hardware path. Read once per process |
| `D9MT_AIR_FASTMATH` | off | `1` restores full fast-math in compiled shaders (faster, but can produce NaN specks) |
| `D9MT_METALFX` | off | MetalFX spatial upscaling at present. **Currently broken** (selector bug; fix pending) |
| `D9MT_MEMORY_PROFILE`, `D9MT_WRITEONLY_DIRECT`, `D9MT_WRITEONLY_DEMOTE`, `D9MT_MEMSTATS`, `D9MT_LABELS` | | see the `dxvk.conf` table |
| `DXVK_LOG_PATH`, `DXVK_LOG_LEVEL` | none, `info` | log directory and verbosity, as in DXVK |

## Developer and diagnostic switches

| Variable | Effect |
|---|---|
| `D9MT_PROFILE` | per-frame timing lines (`PSOBUILD`, `AIRCOMPILE`, compile counters), `HITCH` lines that attribute frames longer than `D9MT_HITCH_MS` (default 50) to d9mt or to the game (ending in the local wall clock and the presenter's state: `| at=HH:MM:SS.mmm vis=0x3 stalled=0`, so a hitch at a Space switch, the window leaving the screen, tells itself apart from one on screen; `PACE` and `STATS` end in `| at=` too), and a `STATS` line every second: fps, Present-to-Present frame time (avg/p99/max, frames over 33 and 50 ms), per-frame render passes, skipped passes, command buffers, draws, attachment loads/stores and the estimated attachment traffic saved, the render thread's CPU time in the attachment optimization (`attMs`, ms per second) and clears folded into a pass or given one of their own, compiles and render-thread compile waits, Metal's allocated MiB, the front end's flush requests by origin (fields: `driver/d3d9/d3d9_stats.h`), plus a `RESTORE` line in seconds with attachment restores and `RESTORE-SUMMARY` at device destruction (`driver/mtl/mtl_restore_stats.h`), and a `PACE` line every second: the interval between the game's Present calls (min/avg/max, standard deviation), d9mt's share of each frame as per-device outer-entry time (the D3D9 entries on the device, from any thread, that began while no other entry was open on it; the Present thread's time only when the game calls d9mt from one thread), the device's blocked time from any thread (the cap's sleep included) and the rest of the frame, and `cpu` (`GetThreadTimes` on the Present thread, which on Wine for macOS returns the whole process's CPU time) with d9mt's busy wall time subtracted from it as `gameCpu` (`driver/d3d9/d3d9_pace.h`), and a `SCREEN` line every second: when the presented frames actually reached the screen, from each drawable's `presentedTime` (retired strictly in present order: frames shown, dropped (`presentedTime` 0) and lost, the presents still pending, the on-screen interval median/sd/max, never across a lost present, a window switch or skipped presents, the intervals by length in refreshes (about 1 / 2 / 3 or more), `late` frames that stayed on screen at least one refresh longer than the game's requested cadence (its swap interval, or the `D9MT_FPS_CAP` duration if longer; `req`), present-to-screen latency median/p90), preceded by a `SCREEN-INFO` line with the layer's displaySync and drawable count, the swap interval, the frame cap, the refresh rate, whether the window is in macOS fullscreen and `D9MT_FORCE_COMPOSITED` (`driver/d3d9/d3d9_screen.h`; only while profiling are drawables presented with a presented handler, into a stream per presenter, `bridge/unix/present_ring.h`). No GPU time: MTL4 command buffers don't report it |
| `D9MT_TRACE` | verbose trace lines |
| `D9MT_FORCE_COMPOSITED` | `1`: a diagnostic for #153's direct-vs-composited A/B, off by default. It requests composition: the bridge adds a 1x1-point opaque black sublayer at the game layer's bottom-left corner, overlapping content that should make macOS composite the window instead of presenting it directly. That isn't guaranteed: verify with the Metal HUD's present mode (Direct/Composited). The layer itself stays opaque. Read at start; `SCREEN-INFO` shows it as `forceComposited` (the request, not the measured mode) |
| `D9MT_LIFETIME_CHECK` | `1`: a texture d9mt releases before the GPU is past the last command buffer that uses it (sampled, attachment, copy) is a `D9MT-LIFETIME` line and an abort, on any Mac, without needing the GPU to fault; `log`: the lines only (the first 100, then every 1000th). A drawable's texture (Core Animation holds it too) is logged as `D9MT-LIFETIME drawable`, never aborts (`driver/backend/mtl_lifetime.h`) |
| `D9MT_UNWRITTEN_NULL` | `0`: a DEFAULT 2D texture nothing has written yet is sampled as it is (undefined memory, losslessly compressed) instead of as the null texture (zero) until its first write; for A/B. The first 8 such binds are `D9MT-UNWRITTEN` lines |
| `D9MT_GPU_CANARY_TEST_FAULT` | `<n>`: the n-th armed mark is written far outside the canary buffer, which Metal drops, so the mark stays unwritten: tests the `D9MT-GPU-LOST` detection (not a GPU fault). Tests only |
| `D9MT_COMMIT_FEEDBACK_TEST` | `<n>`: every commit's feedback reads as error `n` (`q<n>`: of `MTL4CommandQueueErrorDomain`), with one faulted and one affected encoder: tests the `D9MT-GPU-ERROR` line (`scripts/test-commit-feedback.sh`). Tests only |
| `D9MT_GPU_HUNG` | `continue`: a command buffer the GPU never finishes (a submitted serial not signalled for 10 s it spent submitted) only gives up the wait, and the game renders on, as before. By default that ends the game: the GPU stopped and its queue doesn't recover, so rendering on left the game frozen with its GPU context alive (and, once, the process running past a WindowServer crash and logout). A `D9MT-GPU-HUNG` line names the serial (on stderr even with `DXVK_LOG_LEVEL=none`), no further wait on an unfinished serial returns, and the process exits with code 0xD9D9 |
| `D9MT_WSILOG` | window-system / presenter log (bridge, stderr) |
| `D9MT_DEBUG_FILE=<Windows path>` | live debug flags file, polled once a second: `occ_visible`, `occ_trace`, `occ_sync`, `no_scissor`, `no_depthbias`, `cull_none`, `depth_clamp`, `no_ztest_blended`, `no_sameview_skip`, `no_clipplanes`, `bools_off`, `no_attach_opt` (previous attachment load/store actions), `att_trace` (log up to 400 render-pass load/store decisions and restores each time it's set), and `dump` to dump the next frame's draws. Off unless set |
| `D9MT_SHADER_DUMP_PATH=<Windows dir>` | disassembly of vertex shaders rejected (or accepted via `d3d9.allowOutOfRangeVsDefs`) for using more than 256 float constants. Off unless set |
| `D9MT_FLAT_CHECK=1` | tests: log the first 64 draws' flat choice (`D9MT-FLAT-DRAW topo=… indexed=… effective=… psflat=…`) and check every pixel shader function's final IR against the other flat (`D9MT-FLAT-CHECK <ps> flat|gouraud … ok|FAIL`): COLOR inputs exactly flat in the flat IR and not in the Gouraud one, every other input the same. Compiles each PS twice. Off unless set |
| `D9MT_DEPTH_BIAS_TEST_UNHOOK=1` | tests (`test/code_depth_bias.c`): a vertex shader that declares a `TEXCOORD15` output doesn't get the depth-bias rewrite, so the fail-closed path (the hardware constant for an unhooked shader) can be tested |
| `D9MT_AIR_ERRLOG=<host path>` | append the AIR compiler's reason for rejecting a shader. Off unless set |
| `D9MT_AIR_MACOS=<version>` | the macOS the AIR compiler targets instead of this Mac's: 27 and later give target 27 (AIR 2.9 / Metal 4.1, `air64_v29-apple-macosx27.0.0`), anything else, also a value that isn't a version, target 26 (AIR 2.8 / Metal 4.0). Both are what Apple's toolchain writes for that macOS, in the bitcode and the metallib container. The choice is logged once (`d9mt: AIR target macOS 26: AIR 2.8 / Metal 4.0`). Tests only; use it with its own `D9MT_AIR_CACHE` path, since the cache keeps one target's table |
| `D9MT_AIR_INVARIANT=0` | diagnostic: compile vertex shaders without position invariance. By default a vertex shader whose position output dxbc-spirv marks invariant (every SM3 one) gets Apple's `-fpreserve-invariance` form (`air.invariant` on the position; no reassociation or contraction before the backend's own invariance analysis), so passes that draw the same position expression with different vertex shaders get the same depth, as D3D9's EQUAL depth tests expect. It's a compile flag, so it's in the AIR cache key: on and off never share entries. Not a shipping switch |
| `D9MT_AIR_LEGACY=1` | the support switch: the AIR versions d9mt wrote before 0.1.8 (AIR 2.7 / Metal 3.0, `air64-apple-macosx15.0.0`, container 2.7; VERS 2.7 on macOS 26, 2.9 on 27). For a Mac where the per-macOS target fails; set it with `D9MT_AIR_CACHE=<separate path>`, since a cache file keeps one target's table and drops the other's |
| `D9MT_DUMP_TRIGGER=1` | backbuffer dump for the test scripts (`C:/d9mt-test/DUMP` trigger) |
| `D9MT_GPUCAPTURE=<n>\|trigger`, `D9MT_GPUCAPTURE_COMMITS` | Xcode GPU trace to `/tmp/d9mt.gputrace` |
| `D9MT_PSO_FLUSH_EVERY` | pipelines between archive flushes (default 256) |
| `D9MT_PSO_ASYNC_FLUSH` | `1`: the pipeline archive is flushed by its own thread at a quiet moment (`D9MT_PSO_FLUSH_EVERY` pipelines pending and no build for 2 s, anything pending and no build for 10 s, or between two builds once a flush is 60 s overdue: new builds then hold off until the flush has the lock), not by the build that made it due (#11), and not at exit; pipelines built in a session's last 10 s aren't saved. A build that arrives during a flush still waits for it. `0` (the default): the build that makes a flush due writes it, and once at exit, when the bridge's DLL is detached (on a thread of its own; exit waits for it at most 5 s, #191) |
| `D9MT_PSO_FLUSH_FORCE_MS=<n>` | with `D9MT_PSO_ASYNC_FLUSH=1`: the deadline after which an overdue archive flush goes between two builds, in ms (default 60000). Tests only (`scripts/test-compile-mt.sh`) |
| `D9MT_STATE_CACHE_TEST_NO_VALIDATE=1`, `D9MT_STATE_CACHE_TEST_FAIL_PREWARM=1` | test builds only (`STATE_CACHE_TESTING=1 scripts/build-driver.sh`): skip the state cache's content validation to test its crash-loop guard with a real Metal crash (`scripts/forge-statecache.py`); make prewarm builds fail after 150 ms |
| `STATECACHE` log lines (info) | `load` (file, entries, how many can be prewarmed, `invalid=` entries dropped by content validation, a warning if the last session ended during prewarm builds (crash, kill, or exit without releasing the device): only the entries in flight then are dropped), `prewarm` (each time the background queue drains: built, already built by a draw, failed, time paused for the render thread), `use` (every 60 s and at the end: PSO misses served by a finished prewarm `hits`, by a still-building one `late`, not prewarmed `misses`, new records, the build time the hits saved, `boosts`: prewarm threads raised to normal priority for a late hit), `write` |
| `D9MT_DEFERRED`, `D9MT_XFER_HOIST` | experimental: deferred render passes; hoisting vertex/index uploads into an earlier command buffer |
| `D9MT_EARLY_COMMIT`, `D9MT_EARLY_COMMIT_MIN_MIB` | G1 ([g1-early-commit.md](g1-early-commit.md)): a heuristic flush inside a pass commits the work before it and keeps the pass open, while the GPU is on an earlier frame and the pass's attachments sum to at least `_MIN_MIB` (default `8`). On with the attachment optimization and without `D9MT_DEFERRED`. Exactly `0` turns it off; exactly `always` drops the backlog gate (hang tests and the hash suite of a submission-path change, CONTRIBUTING); any other value, empty included, leaves the default. `_MIN_MIB` set and not empty is read with `strtoull` base 10: leading whitespace and an optional sign are accepted, the decimal integer prefix is consumed (`8junk` and ` +8` are 8, `1.5` is 1, `-1` wraps to the largest unsigned value before the shift to bytes), and text with no number is `0`; unset or empty keeps `8`. Read when the device's context is created; the choice is logged |
| `D9MT_PASS_BARRIER` | exactly `all`: every pass ends with the barrier before all stages, as before #18's narrowing; any other value leaves the narrow barrier (before Fragment\|Blit\|Dispatch, vertex fences for vertex textures). Without the attachment optimization the barrier is always before all stages. Read when the device's context is created; logged |
| `D9MT_CLEAR_DISCARD` | exactly `0`: a clear that ends a pass keeps the pass's stores; any other value, empty included, leaves the store-skip on. Only with the attachment optimization. Read when the device's context is created; logged |
| `D9MT_TEX_PFV` | which textures get `PixelFormatView`, which turns lossless compression off (block-compressed ones always do; `driver/mtl/mtl_pfv.h`). Unset or empty: the GPU's default: on GPUs before Apple10 (M1–M4) multisampled textures and the textures MSAA resolves write, nothing more on Apple10 (M5). A value starting with `1`: every texture. `0`: none, on every GPU. `msaa` (multisampled colour and depth-stencil textures), `resolve` (resolve images), `ds` (the depth-stencil ones of both), or a comma list such as `msaa,resolve` (the M1–M4 default on any Mac): those, on every GPU. Where resolve images are uncompressed, a `StretchRect` or RESZ resolve into a game's texture that a resolve can write (render-target usage) goes through the source's resolve image and a copy. Another value is the default, with a warning. Read once per device; the GPU-path line at start-up names the set and the mode (for example `D9MT: PixelFormatView on block-compressed textures and MSAA colour, MSAA depth-stencil, colour resolve, depth-stencil resolve images (default, GPU before Apple10): no lossless compression on those`) |
| `D9MT_NO_SAMEVIEW_SKIP`, `D9MT_NO_PSOMRU`, `D9MT_NOPSOCACHE`, `D9MT_NO_PASSMERGE`, `D9MT_NO_RULE_{B,C,E,F}`, `D9MT_NOCOLORCLEAR`, `D9MT_NO_AB`, `D9MT_RTSKIP`, `D9MT_STALL_PACE=0` | A/B escape hatches that turn individual optimizations off |
| `PROFILE_INSTRUMENT=1` (build) + `D9MT_PROFILE_INSTRUMENT=1` | per-function CPU profiler build: never a release or player build. `scripts/build-driver.sh` compiles the TUs named in `INSTRUMENT_TUS` (file names separated by spaces; default the backend's CS-thread draw loop: `mtl_context_record.cpp mtl_context_transfer.cpp mtl_pso_cache.cpp mtl_submission.cpp mtl_device_impl.cpp mtl_ring.cpp mtl_residency.cpp`) with `-finstrument-functions`, skipping the functions whose qualified name (without parameters) contains an entry of `INSTRUMENT_EXCLUDE` (comma-separated; it replaces the default, which leaves out the trivial helpers: `std::,dxvk::Rc,MtlBufferSlice,dxvk::Com<,dxvk::bit::,dxvk::Flags<,D3D9DeviceLock`). For the game thread's front end: `PROFILE_INSTRUMENT=1 INSTRUMENT_TUS="d3d9_device.cpp d3d9_constant_buffer.cpp d3d9_stateblock.cpp dxvk_cs.cpp"`. A name that is no TU of the DLL stops the build. Without `PROFILE_INSTRUMENT=1` neither variable changes the build. At run time each thread writes its table to stderr every 10 s (when its outermost instrumented call returns) and at exit: `D9MTINST BEGIN tid=… base=… ns_per_tick=… dropped=… overflow=… at=period\|exit name=<the thread's description, e.g. dxvk-cs; - without one; the rest of the line>`, then `D9MTINST F <offset from base> <calls> <self ns> <total ns>` per function, cumulative since the thread's first call; the offsets name functions through the same DLL's symbols (`x86_64-w64-mingw32-nm -C`) |
