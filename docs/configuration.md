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
| `D9MT_COMPILE_THREADS` | `min(4, cores/2)` | shader/pipeline compile worker threads; `0` compiles on first use |
| `D9MT_COMPILE_MAXCONC` | `0` | `1` asks Metal at device creation to maximize concurrent pipeline compilation (`MTLDevice.shouldMaximizeConcurrentCompilation`): more of Metal's own compiler threads, which can compete with the game on Macs with few performance cores. A scheduling request, not guaranteed parallelism; an A/B switch. Independent of `D9MT_COMPILE_THREADS` |
| `D9MT_PS_PREFETCH` | `1` | compile pixel shaders at creation; `0` waits until a draw needs them |
| `D9MT_ASYNC_SKIP` | off | `1` skips draws whose pipeline isn't ready yet: less stutter, brief pop-in; overrides `D9MT_PSO_WAIT_MS` |
| `D9MT_PSO_WAIT_MS` | off | wait at most this many ms per frame for pending pipelines, then skip (see `d9mt.pipelineWaitBudgetMs`); `off` or `-1` overrides a `dxvk.conf` value |
| `D9MT_GPU_CANARY` | on | `0` turns off the GPU canary: three cases mark a command buffer's slot: a present copy into the drawable marks it in its own encoder; a present that isn't a copy (a scaled blit, MetalFX) gets a marking encoder of its own every frame; a command buffer that doesn't present gets one on every 4th, and only if no canary ran for 250 ms (`driver/backend/mtl_canary_gate.h`). The mark goes into the command buffer's slot in a 16 KiB buffer; a command buffer that completed without its mark was discarded by the GPU (after a page fault: always the command buffers queued after it, the faulting one if the fault came before the mark); `D9MT-GPU-CANARY: on` once the first mark arrived (or `off` and why), so a log shows the canary worked and is a `D9MT-GPU-LOST` line in the log (the first 20, then a summary at most every 10 s, and the session's total). macOS itself only logs `IOGPUMetalError` in its unified log |
| `D9MT_NO_ATTACHMENT_OPT` | off | `1` turns the attachment load/store optimization off (see `d9mt.attachmentOpt`) |
| `D9MT_FLUSH_CAP` | `0` | `1` caps the heuristic flushes at strong hints, as upstream DXVK does on tiling GPUs: a full CS chunk and the other weak hints no longer submit a command buffer mid-frame. An A/B switch, read at device creation; `d3d9.reproducibleCommandStream` takes precedence ([`g1-early-commit.md`](g1-early-commit.md) §7) |
| `D9MT_FPS_CAP` | off | frame rate cap with even frame times (`20`–`1000`), for heat and battery: with vsync and a cap that divides the refresh rate (30 at 60 Hz; 30, 40, 60 at 120 Hz) every frame stays on screen for the same number of refreshes (`presentAfterMinimumDuration`); otherwise (40, 45 at 60 Hz, or vsync off) an exact timer, frames then shown for uneven numbers of refreshes at 60 Hz; at or above the refresh rate with vsync, nothing (vsync caps already). Logs a `frame cap` line. Takes over from `d3d9.maxFrameRate` while it caps ([`driver/d3d9/d3d9_frame_cap.h`](../driver/d3d9/d3d9_frame_cap.h)) |
| `D9MT_FPS_CAP_ALIGN` / `D9MT_FPS_CAP_SPIN_US` | on / `200` | `0`: the cap's timer only, no vsync alignment; the least time the cap's and the background throttle's sleep spin before the deadline (more while the system's sleep oversleeps, at most 4 ms) |
| `D9MT_BG_FPS` / `D9MT_BG_FPS_MINIMIZED` | `30` / `10` | the background throttle: the rate Present sleeps to while the window is hidden / minimized; `0` turns it off |
| `D9MT_DRAWABLE_TIMEOUT_MS` | max(3 frames, 50 ms) | longest wait for a drawable before a present is skipped |
| `D9MT_AIR_FASTMATH` | off | `1` restores full fast-math in compiled shaders (faster, but can produce NaN specks) |
| `D9MT_METALFX` | off | MetalFX spatial upscaling at present. **Currently broken** (selector bug; fix pending) |
| `D9MT_MEMORY_PROFILE`, `D9MT_WRITEONLY_DIRECT`, `D9MT_WRITEONLY_DEMOTE`, `D9MT_MEMSTATS`, `D9MT_LABELS` | | see the `dxvk.conf` table |
| `DXVK_LOG_PATH`, `DXVK_LOG_LEVEL` | none, `info` | log directory and verbosity, as in DXVK |

## Developer and diagnostic switches

| Variable | Effect |
|---|---|
| `D9MT_PROFILE` | per-frame timing lines (`PSOBUILD`, `AIRCOMPILE`, compile counters), `HITCH` lines that attribute frames longer than `D9MT_HITCH_MS` (default 50) to d9mt or to the game, and a `STATS` line every second: fps, Present-to-Present frame time (avg/p99/max, frames over 33 and 50 ms), per-frame render passes, skipped passes, command buffers, draws, attachment loads/stores and the estimated attachment traffic saved, the render thread's CPU time in the attachment optimization (`attMs`, ms per second) and clears folded into a pass or given one of their own, compiles and render-thread compile waits, Metal's allocated MiB (fields: `driver/d3d9/d3d9_stats.h`), plus a `RESTORE` line in seconds with attachment restores and `RESTORE-SUMMARY` at device destruction (`driver/mtl/mtl_restore_stats.h`), and a `PACE` line every second: the interval between the game's Present calls (min/avg/max, standard deviation) and the Present thread's time per frame inside d9mt (and how much of it is waiting, the cap's sleep included) and outside it, with its CPU time per second split into d9mt's and the game's own (`driver/d3d9/d3d9_pace.h`). No GPU time: MTL4 command buffers don't report it |
| `D9MT_TRACE` | verbose trace lines |
| `D9MT_LIFETIME_CHECK` | `1`: a texture d9mt releases before the GPU is past the last command buffer that uses it (sampled, attachment, copy) is a `D9MT-LIFETIME` line and an abort, on any Mac, without needing the GPU to fault; `log`: the lines only (the first 100, then every 1000th). A drawable's texture (Core Animation holds it too) is logged as `D9MT-LIFETIME drawable`, never aborts (`driver/backend/mtl_lifetime.h`) |
| `D9MT_UNWRITTEN_NULL` | `0`: a DEFAULT 2D texture nothing has written yet is sampled as it is (undefined memory, losslessly compressed) instead of as the null texture (zero) until its first write; for A/B. The first 8 such binds are `D9MT-UNWRITTEN` lines |
| `D9MT_GPU_CANARY_TEST_FAULT` | `<n>`: the n-th armed mark is written far outside the canary buffer, which Metal drops, so the mark stays unwritten: tests the `D9MT-GPU-LOST` detection (not a GPU fault). Tests only |
| `D9MT_GPU_HUNG` | `continue`: a command buffer the GPU never finishes (a submitted serial not signalled for 10 s it spent submitted) only gives up the wait, and the game renders on, as before. By default that ends the game: the GPU stopped and its queue doesn't recover, so rendering on left the game frozen with its GPU context alive (and, once, the process running past a WindowServer crash and logout). A `D9MT-GPU-HUNG` line names the serial (on stderr even with `DXVK_LOG_LEVEL=none`), no further wait on an unfinished serial returns, and the process exits with code 0xD9D9 |
| `D9MT_WSILOG` | window-system / presenter log (bridge, stderr) |
| `D9MT_DEBUG_FILE=<Windows path>` | live debug flags file, polled once a second: `occ_visible`, `occ_trace`, `occ_sync`, `no_scissor`, `no_depthbias`, `cull_none`, `depth_clamp`, `no_ztest_blended`, `no_sameview_skip`, `no_clipplanes`, `bools_off`, `no_attach_opt` (previous attachment load/store actions), `att_trace` (log up to 400 render-pass load/store decisions and restores each time it's set), and `dump` to dump the next frame's draws. Off unless set |
| `D9MT_SHADER_DUMP_PATH=<Windows dir>` | disassembly of vertex shaders rejected (or accepted via `d3d9.allowOutOfRangeVsDefs`) for using more than 256 float constants. Off unless set |
| `D9MT_AIR_ERRLOG=<host path>` | append the AIR compiler's reason for rejecting a shader. Off unless set |
| `D9MT_AIR_MACOS=<major>` | the macOS version the AIR compiler tags its metallibs for (below 27: the bitcode's AIR version; 27 and later: AIR 2.9), instead of this Mac's (a value that isn't a version gives the bitcode's AIR version). The choice is logged once (`d9mt: AIR metallibs tagged for macOS …`). Tests only |
| `D9MT_DUMP_TRIGGER=1` | backbuffer dump for the test scripts (`C:/d9mt-test/DUMP` trigger) |
| `D9MT_GPUCAPTURE=<n>\|trigger`, `D9MT_GPUCAPTURE_COMMITS` | Xcode GPU trace to `/tmp/d9mt.gputrace` |
| `D9MT_PSO_FLUSH_EVERY` | pipelines between archive flushes (default 256) |
| `D9MT_PSO_ASYNC_FLUSH` | `1`: the pipeline archive is flushed by its own thread at a quiet moment (`D9MT_PSO_FLUSH_EVERY` pipelines pending and no build for 2 s, anything pending and no build for 10 s, or between two builds once a flush is 60 s overdue: new builds then hold off until the flush has the lock), not by the build that made it due (#11), and not at exit; pipelines built in a session's last 10 s aren't saved. A build that arrives during a flush still waits for it. `0` (the default): the build that makes a flush due writes it, and once at exit |
| `D9MT_PSO_FLUSH_FORCE_MS=<n>` | with `D9MT_PSO_ASYNC_FLUSH=1`: the deadline after which an overdue archive flush goes between two builds, in ms (default 60000). Tests only (`scripts/test-compile-mt.sh`) |
| `D9MT_STATE_CACHE_TEST_NO_VALIDATE=1`, `D9MT_STATE_CACHE_TEST_FAIL_PREWARM=1` | test builds only (`STATE_CACHE_TESTING=1 scripts/build-driver.sh`): skip the state cache's content validation to test its crash-loop guard with a real Metal crash (`scripts/forge-statecache.py`); make prewarm builds fail after 150 ms |
| `STATECACHE` log lines (info) | `load` (file, entries, how many can be prewarmed, `invalid=` entries dropped by content validation, a warning if the last session ended during prewarm builds (crash, kill, or exit without releasing the device): only the entries in flight then are dropped), `prewarm` (each time the background queue drains: built, already built by a draw, failed, time paused for the render thread), `use` (every 60 s and at the end: PSO misses served by a finished prewarm `hits`, by a still-building one `late`, not prewarmed `misses`, new records, the build time the hits saved, `boosts`: prewarm threads raised to normal priority for a late hit), `write` |
| `D9MT_DEFERRED`, `D9MT_XFER_HOIST` | experimental: deferred render passes; hoisting vertex/index uploads into an earlier command buffer |
| `D9MT_EARLY_COMMIT`, `D9MT_EARLY_COMMIT_MIN_MIB` | G1 ([g1-early-commit.md](g1-early-commit.md)): a heuristic flush inside a pass commits the work before it and keeps the pass open, while the GPU is on an earlier frame and the pass's attachments sum to at least `_MIN_MIB` (default `8`). On with the attachment optimization and without `D9MT_DEFERRED`. Exactly `0` turns it off; exactly `always` drops the backlog gate (hang tests and the hash suite of a submission-path change, CONTRIBUTING); any other value, empty included, leaves the default. `_MIN_MIB` set and not empty is read with `strtoull` base 10: leading whitespace and an optional sign are accepted, the decimal integer prefix is consumed (`8junk` and ` +8` are 8, `1.5` is 1, `-1` wraps to the largest unsigned value before the shift to bytes), and text with no number is `0`; unset or empty keeps `8`. Read when the device's context is created; the choice is logged |
| `D9MT_PASS_BARRIER` | exactly `all`: every pass ends with the barrier before all stages, as before #18's narrowing; any other value leaves the narrow barrier (before Fragment\|Blit\|Dispatch, vertex fences for vertex textures). Without the attachment optimization the barrier is always before all stages. Read when the device's context is created; logged |
| `D9MT_CLEAR_DISCARD` | exactly `0`: a clear that ends a pass keeps the pass's stores; any other value, empty included, leaves the store-skip on. Only with the attachment optimization. Read when the device's context is created; logged |
| `D9MT_TEX_PFV` | a value starting with `1`: `PixelFormatView` on every texture, which turns lossless compression off (by default only block-compressed textures get it); anything else, `0` and empty included, leaves the default. Read once per process; logged |
| `D9MT_NO_SAMEVIEW_SKIP`, `D9MT_NO_PSOMRU`, `D9MT_NOPSOCACHE`, `D9MT_NO_PASSMERGE`, `D9MT_NO_RULE_{B,C,E,F}`, `D9MT_NOCOLORCLEAR`, `D9MT_NO_AB`, `D9MT_RTSKIP`, `D9MT_STALL_PACE=0` | A/B escape hatches that turn individual optimizations off |
| `PROFILE_INSTRUMENT=1` (build) + `D9MT_PROFILE_INSTRUMENT=1` | per-function CPU profiler build |
