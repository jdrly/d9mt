# d9mt's GPU cost at 4K (issue #7): analysis and measurement plan

Phase 2 (candidates 1–3 implemented, the split counter for 4) is in §8, with the in-game A/B steps.

Phase 1 of #7: no code changes, no in-game run. Base: `main` 4b7ab26. Machine:
M5 Max, macOS 27.0. Inputs:
- the d9mt source;
- SWTOR session logs from 2026-10-17 (dev2, 1529, 1658, dev4; `D9MT_PROFILE`);
- the three SWG Metal System Traces from #7 (`/tmp/gpu21/run{A,B,C}.trace`, 10 s each);
- a synthetic 4K Metal 4 probe (`test/perf/gpu4k_probe.m`).

## Summary

- **Every render pass ends with a barrier that serialises the GPU** (§1.4). After each encoder d9mt emits `barrierAfterStages:Fragment beforeQueueStages:All`. So the next pass's vertex work can't start until this pass's fragment work has finished, and Apple's GPU can't overlap one pass's vertex/tiling with the previous pass's fragment shading.
  - **In #7's traces** that overlap is 0: at only 1 of 13,269 pass boundaries does the next pass's vertex work start before the previous fragment work ends, and GPU busy = vertex + fragment (3.6 + 8.6 ms).
  - **In the probe**, the narrower barrier `before Fragment|Blit|Dispatch` overlaps 100% of boundaries and saves 16% of GPU time (12% when every pass samples the previous one). That equals the no-barrier result.
  - **This is the largest saving found and the cheapest to make.**
- **Lossless framebuffer compression is off for every texture.** `MtlDeviceImpl` adds `MTLTextureUsagePixelFormatView` to all textures (`mtl_device_impl.cpp:71`), and Apple documents that this disables lossless compression. d9mt's views only switch between sRGB and linear, which Apple says doesn't need the flag (§4). Without it the probe saves 20% (33% together with the barrier change). The saving goes straight to memory bandwidth, which is what base M-series Macs lack.
- **A pass split at 4K costs one store and one load** of colour + D32FS8, 149 MB. That's 0.19–0.21 ms measured on the M5 Max and about 1.5 ms worked out for a base M2's 100 GB/s.
  - SWTOR ends an open pass at a submit ~9 times a frame, and with a mid-pass Clear 3–8 times a frame (§1.3).
  - How many of those reopen the same targets isn't logged, so a counter comes first (§6.2).
  - A Clear that ends a pass makes that pass's store of the cleared aspect useless, and d9mt can tell that exactly, with no look-ahead (candidate 3). In practice that is the depth/stencil store: games clear colour where a target's use starts (folded into the load) and depth between draws.
- **Present:** a 4K compute copy (66 MB) in its own command buffer. Rendering straight into the drawable conflicts with D3D9 backbuffer semantics (§2), and a render-pass copy instead of the compute copy wouldn't save measurably. Present stays as it is for now.
- **Memoryless:** no SWTOR target qualifies (§3).
- **None of the GPU-side candidates changes `winemetal.so`.** Only the measurement helper "GPU time per frame from `MTL4CommitFeedback`" (§6.1) does, and it goes into the #11 bridge bundle.

## 1. Why a frame gets split into extra command buffers and passes

### 1.1 The two primitives, and what they cost on a TBDR GPU

- **Submit.** Defined at `ExecuteFlush` (`d3d9_device.cpp:5906`), which ends in `flushCommandList` → `submit()` (`mtl_context_record.cpp:204-286`).
  - It commits the command buffer, and first ends the open pass (`closePass`, counted as `passClose submit`).
  - It never waits on the GPU.
- **Pass end.** `closePass()` (`mtl_context_record.cpp:674-719`).
  - With the attachment optimization on (the default), the encoder and its load/store actions are created only now (`createPassEncoder`, :829). An attachment that may have been written is always stored (`attopt::decide`, `mtl_attachment_opt.h:133-143`).
  - The next pass on the same targets loads them again (contents Defined).
  - **Cost of a split:** one extra store plus one extra load of every attachment. A Metal encoder boundary has no cost of its own. The probe shows that: an extra command buffer every 2 passes, with all loads and stores unchanged, costs +2% (`split=2`).

Bytes per 4K attachment (3840×2160) and the time to move them uncompressed at Apple's published unified-memory bandwidth:

| | Bytes | M1 68 GB/s | M2/M3 100 | M4 120 | M5 153 | M2 Pro 200 |
|---|---|---|---|---|---|---|
| BGRA8 colour, one load or store | 33.2 MB | 0.49 ms | 0.33 | 0.28 | 0.22 | 0.17 |
| D32FS8 depth+stencil (D3D9 D24S8), one load or store | 41.5 MB | 0.61 | 0.41 | 0.35 | 0.27 | 0.21 |
| **One pass split** (store + load of colour + D32FS8) | 149 MB | 2.2 | 1.5 | 1.24 | 0.97 | 0.75 |
| Present copy (read + write BGRA8) | 66 MB | 0.97 | 0.66 | 0.55 | 0.43 | 0.33 |

- **This is an upper bound:** load and store traffic can overlap with shading, and the SLC catches part of it. The SLC is a few MB on base chips, far less than one 4K target.
- **M5 Max, measured with the probe** (`halves=1`, a split in each of 20 passes): +3.7 to +4.2 ms per frame, i.e. 0.19–0.21 ms per split. That is about 700 GB/s effective: Max bandwidth plus a large SLC.
- **MSAA multiplies every row by the sample count.** With 4× MSAA at 4K, one split is about 600 MB. The tester's "low FPS with shadows/AA" fits that. SWTOR runs with `AntiAliasingLevel=0` on Jan's machine.

### 1.2 Inventory

Locations are relative to `driver/`. "Splits" means the trigger ends the open pass and the same targets are likely to resume afterwards. Only then does the trigger cost a split.

| Trigger | Path | Effect | Splits? | SWTOR |
|---|---|---|---|---|
| **CS chunk full** (16 KiB of recorded API calls) | `EmitCs` → `ConsiderFlush(ImplicitWeakHint)` (`d3d9_device.h:1137-1158`) | submit if ≥6 chunks since the last flush and <2 submissions pending (else ≥ min(20, 3×pending)) (`util/util_flush.cpp:44-58`) | **yes**: fires at arbitrary 16 KiB points, usually mid-pass | ~2,300–2,650 draws per world frame, ~20+ chunks; most likely source of the mid-pass submits |
| `SetRenderTarget` (changed) | `ConsiderFlush(Strong idx 0 / Weak)` (`d3d9_device.cpp:1599-1601`) | submit at a target change | no: the pass ends anyway | part of the 6.5–14 `rt` pass ends |
| `SetDepthStencilSurface` (changed) | WeakHint (`:1680`) | as above | no | |
| `EndScene` | StrongHint (`:1740`) | submit if ≥3 chunks | sometimes (if the next scene uses the same targets) | |
| Texture/buffer upload end | WeakHint (`:1025`, `:5159`, `:5374`) | submit | the upload itself already ended the pass (below) | `xfer` = 1.0/frame: only the present copy |
| Staging throttle | `ThrottleAllocation` (`:4624-4651`): ImplicitSynchronization, ExplicitFlush past `stagingInFlightMB` | submit; app-thread GPU wait past the cap | yes (mid-pass) | not visible in logs |
| Initializer throttle | `flushCommandList` after 512 transfer commands (`d3d9_initializer.cpp:162-165`), bypasses the tracker | submit | yes | at load/creation time |
| Event query `End` | stalling: ExecuteFlush; else StrongHint (`:7487-7492`) | submit | yes | HITCH shows `query=1`/frame |
| `GetData(FLUSH)` S_FALSE | ImplicitSynchronization (`d3d9_query.cpp:194-197`) | submit | yes | |
| Occlusion query Begin/End | visibility mode change inside the pass (`mtl_context_transfer.cpp:1769-1803`) | none | no | |
| `LockRect` of an RT/DS or of a texture written by the GPU | `copyTextureToBuffer` + `WaitForResource` (`d3d9_device.cpp:4851-4887`) | pass end + submit + **GPU wait** | yes | no readbacks seen in world |
| VB/IB `Lock` without NOOVERWRITE/DISCARD on a busy buffer | `WaitForResource` (`:5289-5316`) | submit + GPU wait | yes | |
| DEFAULT VB/IB unlock, managed/sysmem buffers at draw | `FlushBuffer` → `copyBuffer` → `prepareDirectEncode` (`mtl_context_record.cpp:1634`) | pass end (compute encoder); `D9MT_XFER_HOIST=1` moves staging→VB/IB copies ahead of the command buffer | yes | `xfer` 1.0/frame says not in steady state |
| Managed texture upload at draw, mip generation | `UploadManagedTextures` / `GenerateTextureMips` at `PrepareDraw` (`d3d9_device.cpp:7046-7052`) | pass end | yes | not in steady state |
| `StretchRect`, `UpdateSurface`, `UpdateTexture`, resolves | copy / blit / resolve encoder (`mtl_context_transfer.cpp`) | pass end | depends | |
| **`Clear()` after draws** | `clearRenderTarget` closes the active pass, then folds the clear into the next load (`mtl_context_record.cpp:451-473`) | pass end | yes, if the next pass keeps the other attachments | **3.0–8.0/frame** (`passClose clear`) |
| Partial clear (rects, scissor) | `clearImageView` / `clearDepthStencilRect` → own encoder (`mtl_context_transfer.cpp:1224-1348`) | pass end | yes | RESTORE shows 4K D32FS8 partial clears (rare) |
| Different view of the same target | read-only vs writable DSV, sRGB vs linear RTV are distinct views (`d3d9_device.cpp:6454-6495`) | pass end | yes | not counted |
| Present | `Flush()` (`d3d9_swapchain.cpp:785`), then the copy and `synchronizeWsi` (submit #2) | 2 command buffers/frame | no (frame end) | |
| Resource creation mid-frame | high-priority CS chunk → `initImage` / `initBuffer` (own clear pass / fill) | pass end | yes | spike frames (#7: buffer creation in 31% of them) |
| attachment-opt restore | `attRestore` (`mtl_context_record.cpp:1120`) | pass end + restore pass | yes | ≪0.01/frame |
| **End of every render pass** | producer barrier `after Fragment → before All` (§1.4) | **no split, but no vertex/fragment overlap across passes** | n/a | every pass, 20–35/frame |

`d3d9.reproducibleCommandStream=True` disables every implicit hint (`GetMaxFlushType`, `d3d9_device.cpp:8707-8712`). Explicit flushes, throttles, waits and Present remain.

### 1.3 What SWTOR's logs show (world, M5 Max, 3840×2160)

Per frame, median / p90 of the one-second STATS windows:

| Log | cb | Encoders | Draws | ld | st | stDc | savedMiB |
|---|---|---|---|---|---|---|---|
| dev2 (392 s) | 11.0 / 14.1 | 20 / 34 | 2283 / 3657 | 31 | 45 | 14.6 | 367 |
| 1529 (306 s) | 10.9 / 14.6 | 20 / 35 | 2376 / 4033 | 31 | 48 | 13.4 | 388 |
| 1658 (404 s) | 12.1 / 15.4 | 29 / 36 | 2436 / 4191 | 45 | 61 | 18 | 784 |
| dev4 (320 s) | 11.2 / 16.3 | 22 / 39 | 2652 / 4481 | 35 | 51 | 13 | 385 |

- **Two kinds of world frame:**
  - light (60–70%): ~19 encoders, 10 cb, ~340 MiB saved;
  - heavy (`clrFold` ≥20): ~33 encoders, 13–14 cb, ~40 folded clears, 870 MiB saved.
- **Why passes end (PROF, per frame):** submit 8.8–10.2, target change 6.5–13.9, clear 3.0–8.0, transfer 1.0 (the present copy), barrier/blit 0.
- **What isn't logged:**
  - why a flush happened;
  - whether the pass after a submit or clear reopens the same targets (a split) or would have changed targets anyway (free);
  - the colour/depth split and the sizes of the loads and stores;
  - GPU time.
- **Inference (unlogged):** savedMiB / stDc ≈ 25 MiB per skipped store, so the skipped stores are mostly 4K-sized.
- **Illustration only:** if a third of a light frame's ~76 loads and stores are 4K, attachment traffic is ~0.8 GB per frame, i.e. ~8 ms per frame on a base M2. `att_trace` (§6) gives the real per-pass sizes.

### 1.4 The barrier after every pass

**Where.** At the end of every render encoder:
- `closePass` (`mtl_context_record.cpp:687,710`), restore passes (:1170) and deferred replay (:1932) emit `emitProducerBarrier(enc, WMTStageFragment)`;
- the blit, clear and resolve render encoders emit the same (`mtl_context_transfer.cpp:1059,1132,1296,1413,1561,1659`).
- It becomes

  ```objc
  [enc barrierAfterStages:MTLStageFragment beforeQueueStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice]
  ```
  (`emitProducerBarrier`, `mtl_context_transfer.cpp:661-666`; bridge `winemetal_unix.c:6871-6877`).
- Every stage of every later encoder on the queue waits for this pass's fragment stage, including the next pass's vertex stage. d9mt relies on that ordering across its mid-frame submits too.

**Evidence from #7's traces** (SWG, M5 Max, 4K). Columns are runs A / B / C, via `test/perf/mst_overlap.py`:

| | A | B (`reproducibleCommandStream`) | C (`D9MT_DEFERRED`) |
|---|---|---|---|
| Vertex / fragment busy per frame, median | 3.58 / 8.63 ms | 3.77 / 9.08 | 3.53 / 8.52 |
| Vertex ∩ fragment overlap, median | 0.00 ms | 0.00 | 0.00 |
| Pass boundaries where the next vertex starts before the previous fragment ends | 0 of 4,457 | 0 of 3,791 | 1 of 5,021 |
| Gap from fragment end to next vertex start, median | 3.5 µs | 0.25 µs | 3.0 µs |

The vertex work of each pass runs alone between two fragment phases, and GPU busy is their sum. With 20–35 passes a frame that is 20–35 drains of the pipeline. Shadow cascades (vertex-heavy, depth-only) next to the main and post passes (fragment-heavy) are the classic case where overlap pays.

**Who needs the next pass to wait, and for which stage.**
- *Fragment of the next pass:* sampling a target this pass wrote, and write-after-write / load actions on shared attachments. Load and store actions run in the fragment stage.
- *Blit and Dispatch:* d9mt's copies are compute encoders (copy to the drawable, readbacks, uploads into textures).
- *Vertex:* only D3D9 vertex texture fetch (`D3DVERTEXTEXTURESAMPLER0-3`, exposed with SM3: `d3d9_adapter.cpp:592`) of a texture a previous pass rendered.
  - Vertex and index buffers come from the CPU or from compute copies, whose own barrier (`Blit|Dispatch → All`) stays.
  - The visibility buffer is read by the CPU only.
  - Argument tables are CPU-written.
  - D3D9 has no UAVs or stream-out.

**The fix is in d3d9.dll; `winemetal.so` doesn't change.** The stages are arguments of `MTL4CommandEncoder_barrier`.
- The end-of-pass barrier becomes `before Fragment|Blit|Dispatch`.
- A draw whose vertex stage has a render-written image bound first gets a fence. The bridge only has producer barriers, so the fence is an empty compute encoder with `after Fragment → before Vertex` (or, if the pass's encoder is already open, a pass split). A consumer barrier (`barrierAfterQueueStages:beforeStages:`) would do it inside the encoder; it needs a bridge entry (#11 bundle).
- §7 and §8 have the details.

## 2. Present path

What happens per Present (`d3d9_swapchain.cpp:781-895`, `mtl_context_record.cpp:340-362`, `mtl_context_transfer.cpp:765-795`):

1. **End the frame.** `EndFrame`, then `Flush()` commits the frame's command buffer (submit #1).
2. **Get a drawable.** On the CS thread, a drawable comes from the `d9mt-drawable` acquirer thread (requested only now; timeout `max(3/refresh, 50 ms)`; skipped while hidden).
3. **Copy.** Same size (SWTOR: 3840×2160 BGRA8 backbuffer and a BGRA8 drawable): `copyTexture`, a **MTL4 compute-encoder copy**. It reads 33.2 MB and writes 33.2 MB, with no format conversion. It sits in **its own command buffer**, behind `waitForDrawable`, and is followed by the queue's `signalDrawable` and the CPU-side `[drawable present]`.
   - A different size goes through `blitTextureView`: a fullscreen-triangle render pass with linear filtering, loading the drawable with `Load` (`mtl_context_transfer.cpp:1518`), which isn't needed.
   - `D9MT_METALFX` takes a MetalFX spatial upscale path instead.
4. **The layer.** BGRA8Unorm, `framebufferOnly = false` because of the compute copy (`mtl_presenter_impl.cpp:471-473`), opaque, `displaySyncEnabled = presentInterval != 0`, `maximumDrawableCount` left at 3, colorspace unset.
   - The only thing in the code that bears on Direct presentation is the opaque/black window setup (`winemetal_unix.c:1504-1534`).
   - The Metal HUD shows whether presentation is Direct or Composited. That's the check for every present-path change.

**Rendering straight into the drawable** doesn't fit D3D9 semantics without a fallback copy:
- the backbuffer is one long-lived, lockable surface the game holds (`GetBackBuffer`), and every swap effect behaves like COPY (no rotation);
- `GetRenderTargetData`, `StretchRect` from the backbuffer, `GetDC` and the dump tool read it;
- the drawable is only known at Present, after mid-frame submits have already rendered into the backbuffer;
- skipped presents (hidden window, no drawable within the timeout) still need somewhere to render;
- partial present rects, multiple swapchains, MSAA resolve;
- the X8 alpha swizzle and the 16-bit formats.

Doing it would mean acquiring the drawable before the first draw to the backbuffer, a mid-frame block on WindowServer, and a fallback copy whenever any of the above applies. **Rejected** for d9mt's general path.

**Cheaper variants:**
- **(a) Copy as a render pass** (fullscreen read, load DontCare). That allows `framebufferOnly = true`, so the drawable can be compressed and the compositor or display reads less. The probe can't tell `present=draw` from `present=copy` (both +0.3–0.5 ms on the M5 Max, within noise). Whether `framebufferOnly` keeps or improves Direct has to be checked on screen. Low priority.
- **(b) Compression.** With lossless compression on the backbuffer (§4) the copy's read side shrinks. The write side depends on the drawable, which with `framebufferOnly = false` is probably not compressed.
- **(c) Folding the copy into the frame's command buffer.** That would hold the whole frame behind `waitForDrawable`. Rejected.

**Expected saving:**
- The copy is about 0.66 ms per frame on a 100 GB/s chip.
- (a) and (b) might recover a part of it.
- Removing the copy outright isn't possible without the semantics changes above.

## 3. Render targets in SWTOR, and memoryless

From DIAG MEM, RESTORE and the swapchain lines (`texRT` / `texDS` are MiB / live count):

| Target | Size / format | Memory | Notes |
|---|---|---|---|
| Backbuffer | 3840×2160 BGRA8 (A8R8G8B8) | 31.6 MiB | lockable, read by the present copy every frame |
| Auto depth-stencil | 3840×2160 D24S8 → Depth32Float_Stencil8 | 63.3 MiB by d9mt's estimate (8 B/px), ~40 MiB of actual depth + stencil planes | spans many passes and is partially cleared (RESTORE `partial-clear@Clear`) |
| Shadow map | 2048×2048 D32FS8 | 32 MiB | sampled (`sample@Draw`); probably a 2×2 atlas of 1024² cascades (`ShadowMapResolution=1024`, 4 cascades) |
| Other RTs | `texRT` 205 MiB / 71 in world (146.6 / 10 at splash) | | named in the logs: a 4K BGRA8 that is sampled, 100×100 and 1029×790 BGRA8; most of the 71 are small |

- **Memoryless needs a target whose whole lifetime fits in one encoder**: loaded with Clear or DontCare, and never stored.
  - D3D9 targets are long-lived objects used across passes and frames.
  - SWTOR's 4K depth is reused across 20+ passes, and passes split by submits.
  - The shadow map is sampled.
  - So **no SWTOR target qualifies**, and memorylessness is fixed when the texture is created, so it can't be decided pass by pass.
  - What memoryless would give comes from DontCare stores (the attachment optimization) and from avoiding splits (candidates 3 and 4).
  - The one d9mt-internal candidate would be an MSAA colour surface whose resolve is folded into the store (`ResolveFolded`) and that is never loaded. MSAA is off in SWTOR, and D3D9 MSAA surfaces persist too.

## 4. Lossless compression: `MTLTextureUsagePixelFormatView`

- **Where.** `textureUsage()` (`mtl_device_impl.cpp:61-73`) ORs `WMTTextureUsagePixelFormatView` into every texture, including all render targets and depth buffers. They are private and untracked (`:54-57`), so the flag is the only thing standing in the way of compression.
- **Apple documents the effect** ([MTLTextureUsage.pixelFormatView](https://developer.apple.com/documentation/metal/mtltextureusage/pixelformatview), [Optimizing texture data](https://developer.apple.com/documentation/metal/optimizing-texture-data)):
  - lossless compression on Apple GPUs requires usage without `shaderWrite`, `pixelFormatView` or unknown, a format that isn't block-compressed, and no buffer backing;
  - "Don't set this option if your texture view only converts between linear space and sRGB";
  - "Don't set this option if your texture view needs to read the component values in a different order. Instead, create a texture view with a swizzle".

**Which views d9mt actually creates:**
- D3D9 textures: the image's format or its sRGB twin (`PickSRGB`, `d3d9_common_texture.cpp:481-483`), plus a swizzle. Neither needs the flag.
- StretchRect views: the image's own format, as `e2DArray` and `e2D` view types (`d3d9_device.cpp:1331-1352`).
- Cube-face and mip views: the same format.
- Depth sampling: the D32FS8 image's own format, depth aspect.
- Only the ColorFill of block-compressed textures views BC data as `RG32Uint` / `RGBA32Float` (`d3d9_device.cpp:1438-1456`). BC formats never compress losslessly, so they keep the flag at no cost.
- Still to verify: that view-type changes don't need the flag either. The Metal API validation layer settles it (§5.3).

**Effect in the probe** (pixel results are bit-identical by definition; lossless):
- `bar=all` 5.46 → 4.37 ms (−20%);
- `bar=frag` 4.59 → 3.68 ms (−20%; −33% against today).

How well SWTOR's content compresses is unknown. Smooth synthetic content and flat depth compress better than game frames, so read the probe as an upper bound. Compression reduces every attachment load and store, every texture sample of an RT, and the present copy's read. That's the bandwidth that's scarce on base chips.

## 5. Measured without the game

### 5.1 Synthetic probe (`test/perf/gpu4k_probe.m`, native Metal 4, M5 Max)

- **The frame:** 20 passes of 3840×2160 BGRA8 + D32FS8. Each pass draws one fullscreen triangle with fragment ALU, plus 125 draws of 120 small vertex-heavy triangles: 2,500 draws/frame, close to SWTOR's count.
- **The number:** median GPU span per frame (`MTL4CommitFeedback`), frames back to back, so the GPU ran at full clocks.
- **Repeats:** each row is the median of 3 runs of 150 frames, interleaved, run under `exc_guard`. Raw output: `/tmp/gpu4k/synth/matrix{1,2,3}.txt`.

| Variant | GPU ms/frame | vs today |
|---|---|---|
| `bar=all` (d9mt today) | 5.46 | |
| `bar=frag` (before Fragment\|Blit\|Dispatch) | 4.59 | −16% |
| `bar=none` (no barrier: the floor) | 4.57 | −16% |
| `pfv=0` | 4.37 | −20% |
| `bar=frag,pfv=0` | 3.68 | −33% |
| `sample=1` (each pass samples the previous one), `bar=all` → `bar=frag` | 6.81 → 5.97 | −12% |
| `split=5` / `split=2` (extra command buffers, same loads/stores) | 5.48 / 5.56 | +0.4% / +2% |
| `halves=1` (each pass split once, reloads everything), `bar=all` | 10.28 vs 6.09 in the same batch | +0.21 ms per split |
| `ds=dontcare` (19 passes clear depth instead of loading it, and don't store it: an upper bound) | 3.02 | −45% |
| `present=copy` / `present=draw` (400 frames × 4) | +0.37 / +0.47 ms | noise ±0.3 |

**Traces of the probe** (`mst_overlap.py`):
- `bar=all` reproduces #7's signature exactly: 0 of 11,035 boundaries overlap, gap 6.8 µs.
- `bar=frag` overlaps at 100% of boundaries, with vertex ∩ fragment = 1.60 of 1.68 ms per frame.

So the trace analysis tells the two apart, and the in-game A/B can be read the same way.

### 5.2 Trace tooling, checked on the probe

- `xctrace record --attach <pid> --time-limit` works as given in §6.
- Metal System Trace ran at about 25–30 MB/s on the probe. #7's 10 s game traces were ~150 MB, i.e. ~15 MB/s.
- From the CLI, neither counter instrument records bandwidth. With `--instrument 'Metal GPU Counters'` the counter tables stayed empty. `'Metal Performance Overview'` recorded the default set, which on the M5 Max is a single counter ("RT Unit Active").
- Bandwidth and limiter counters need a template saved from Instruments.app with a counter set selected. That's optional; §6 works without counters.

### 5.3 Still to do without the game (phase 2, per candidate)

**Rules for every run:**
- Wine only in /tmp prefixes on APFS clones of the engine under /tmp;
- `D9MT_AIR_CACHE` and `D9MT_PSO_ARCHIVE_DIR` set to /tmp;
- every run under `exc_guard`;
- nothing runs while SWTOR runs (`swtor-running.sh`), except builds under `taskpolicy -b` with ≤6 jobs.

**Correctness:**
- `run-hash-tests.sh` and the `test/harness` from `arm64ec` (port them, or point them at this branch's `build/driver-x64`).
- Every test's last-frame hash must equal `main`'s. Run once more with `MTL_DEBUG_LAYER=1` (Metal API validation) to catch a missing usage flag or a bad barrier.

**New D3D9 test programs** (each with a readback hash):
- `code_vtf_rt`: vertex texture fetch of a texture rendered by the previous pass. This is the hazard that candidate 1 has to keep correct.
- `code_clear_split`: draw, then Clear depth and/or colour, then draw on the same targets (candidate 3).
- `code_srgb_view`: sRGB and linear views of the same RT and texture, StretchRect into a 2D array view, cube faces (candidate 2).
- `code_4kpasses`: a 4K multi-pass D3D9 workload (shadow-like depth-only passes, a main pass, post passes, ~2,500 draws). It runs under Wine with `D9MT_PROFILE`, and with an `xctrace --launch` of the /tmp engine as a d9mt-level A/B of candidates 1–4. It gives GPU ms once §6.1 exists, and overlap from the trace already now.

**Base-Mac reasoning:** we have no base chip, so the M5 Max numbers are scaled by bandwidth (§1.1). Candidates 2–4 save bandwidth, and their share of the saving grows on base chips. Candidate 1 hides vertex time, and that share stays about the same.

## 6. Measurement plan for Jan's in-game session

### 6.1 What d9mt can't report yet (phase 2, before the A/B builds)

- **Pass-split counter** (d3d9.dll). In STATS: `rpResume`, a pass that opens on exactly the attachments of the pass that just closed (same views, no clear) with nothing in between that changes their contents. Plus `rpResumeMiB`, the bytes reloaded, and the reason the previous pass closed (submit / clear / xfer / view change). That prices §1.2 directly in SWTOR.
- **Flush reasons** (d3d9.dll): submits per frame by `GpuFlushType` and caller (chunk full, RT/DS change, EndScene, upload, throttle, query, WaitForResource, Present).
- **GPU time per frame** (bridge, `winemetal.so`): `MTL4CommitOptions addFeedbackHandler:` → `GPUStartTime` / `GPUEndTime` into STATS (`d3d9_stats.h:38-39` notes the gap). The probe shows the API works. This changes `winemetal.so` and goes into the #11 bundle; until then, GPU time comes from the trace.

### 6.2 Session with today's build (no new code needed)

**Setup:**
- 3840×2160, the usual settings.
- One fixed spot on the Fleet (crowded), camera still, 30 s settle per run. If possible, the same in an operation or warzone.
- Environment for every run: `D9MT_PROFILE=1 D9MT_MEMSTATS=256 MTL_HUD_ENABLED=1`. The HUD shows GPU ms and Direct/Composited; note both per run.

**Per-pass detail:** while standing at the spot in run A, add `att_trace` to `C:\d9mt\flags.txt` for a few seconds, then remove it. It logs the next 400 passes (~15 frames) as `D9MT-ATT` lines: per pass the attachment sizes and formats, draws, and load/store decisions. That answers which of the 20–35 passes are full-size 4K, and which pass pairs are splits.

**Runs:**

| Run | Setting | Answers |
|---|---|---|
| A | as today | baseline: overlap signature, cb/rp/ld/st |
| B | `DXVK_CONFIG="d3d9.reproducibleCommandStream=True"` | what SWTOR's implicit flushes cost: cb, rp and ld/st drop, GPU ms |
| C | `DXVK_CONFIG="d3d9.presentInterval=0"` | GPU ms at higher clock states (#7 showed minimum clocks under vsync) |
| D | B + `presentInterval=0` | B's effect at higher clocks |
| E | A with AA on and shadows high | the tester's case: MSAA multiplies every load/store |

**One trace per run**, at the spot, after the settle. At most 60 s; use 20 s (~300 MB at #7's ~15 MB/s):

```sh
PID=$(ps -axo pid=,%cpu=,args= | grep -i '[s]wtor\.exe' | sort -k2 -nr | awk 'NR==1{print $1}')
ps -o pid=,comm=,args= -p "$PID"     # check it is the game, not the launcher
xcrun xctrace record --template 'Metal System Trace' --attach "$PID" \
  --time-limit 20s --no-prompt --output ~/Desktop/swtor-4k-A.trace
```

**Optional, run A only:** a 10 s trace with a counter template saved from Instruments.app (Metal GPU Counters, counter set with memory bandwidth / limiters):

```sh
xcrun xctrace record --template ~/Desktop/d9mt-counters.tracetemplate --attach "$PID" \
  --time-limit 10s --no-prompt --output ~/Desktop/swtor-4k-A-counters.trace
```

**Analysis:**
- `python3 -I test/perf/mst_overlap.py <trace>` for vertex/fragment overlap and per-frame busy time;
- `/tmp/gpu21/analyze.py` for performance state, cb/encoders and present interval;
- `session-report.sh --compare` on the wine.logs.

### 6.3 A/B of the phase-2 build

**Toggles:** each candidate has an env toggle, so one build A/Bs all of them in one session (implemented, §8):
- `D9MT_PASS_BARRIER=all` restores today's barrier;
- `D9MT_TEX_PFV=1` restores the flag;
- `D9MT_CLEAR_DISCARD=0`;
- candidate 4 has no toggle yet: only its counter exists (`split=` in STATS).

**Procedure:**
- Same spot, same runs A/C/E, each with all candidates on, then with one switched off at a time.
- Accept a candidate only if:
  - GPU ms drops or stays the same in every run;
  - the HUD still shows Direct;
  - the logs show no new hitches;
  - the hash tests (§5.3) are identical.
- **Tester (M2 Pro 16 GB):** if they're willing, a 20 s trace of runs A and E with the build and with all toggles off, plus a HUD screenshot. That's the only real base-bandwidth number we can get.

## 7. Ranked candidates

| # | Candidate | Expected GPU saving at 4K | Correctness risk | `winemetal.so` |
|---|---|---|---|---|
| 1 | Narrow the end-of-pass barrier, plus a fence for vertex-stage reads of rendered images | probe −16% (−12% with per-pass sampling); in SWG's trace, up to the vertex share of 28% (3.6 of 12.6 ms) | low–medium: VTF of render targets needs the fence | no (a consumer barrier later would: #11) |
| 2 | No `PixelFormatView` on non-block-compressed textures (lossless compression) | probe −20% (bandwidth-bound); SWTOR's content ratio unknown; largest on base chips | low: lossless; a missing usage flag shows up in API validation | no |
| 3 | A Clear that ends a pass doesn't store the cleared aspects | one 4K depth/stencil store per hit (0.41 ms at 100 GB/s); SWTOR 3–8 clear pass ends/frame, upper bound ~1–3 ms on a base M2 | low: exact, no look-ahead | no |
| 4 | TBDR-aware implicit flush: implicit hints flush at the next pass boundary, not mid-pass | one split (149 MB, ~1.5 ms at 100 GB/s) per avoided mid-pass submit; count unknown (≤9/frame), measure first | medium: fence/serial bookkeeping, query latency, staging throttle | no |
| 5 | Present copy as a render pass + `framebufferOnly = true` | 0–0.3 ms (not measurable in the probe) | medium for Direct presentation (check the HUD); pixels exact | no |
| 6 | Hoist VB/IB uploads by default (`D9MT_XFER_HOIST`) | ~0 for SWTOR (`xfer` = present only); SWG maybe | low–medium | no |
| — | Depth stores at frame end with look-ahead, render into the drawable, memoryless targets, Metal 4 suspend/resume across command buffers | rejected (below) | | |

### 1. Narrow the end-of-pass barrier

**Change (as implemented, §8):**
- `closePass` uses `barrierAfterStages:Fragment beforeQueueStages:Fragment|Blit|Dispatch`. The blit, clear, resolve and restore render encoders keep `before All`.
- Per image, an epoch: "rendered since the last barrier after Fragment before Vertex at the end of an encoder". At a draw whose vertex-stage texture bindings hold such an image, d9mt fences first.
  - The bridge has only producer barriers (`barrierAfterStages:beforeQueueStages:`). So the fence is an empty compute encoder with `after Fragment → before Vertex`, placed ahead of the pass's encoder (which attachment-opt creates when the pass ends).
  - If that encoder already exists (4 MiB of commands, or a feedback barrier), the pass is split for the fence.
- `emitGraphicsBarrier` (feedback loops inside a pass) emits its barrier directly and does **not** start a new epoch. Metal orders only later encoders after it, not the same pass's later draws (verifier finding D1, `code_vtf_fb`).
- The compute encoders' `Blit|Dispatch → All` barriers stay as they are.

**What the fence relies on (D2).** Apple documents a producer barrier as waiting for its after-stages "in the pass and all previous passes". The fence encoder runs no work of its own, so it depends on that "all previous passes" part to wait for earlier fragment work. Apple's header is less explicit than the article.
- On the M5 Max the negative controls show it working: `code_vtf_rt` and `code_vtf_alias` fail without the fence and pass with it.
- **Plan:** add the consumer barrier `barrierAfterQueueStages:Fragment beforeStages:Vertex` to the bridge with the #11 bundle (it changes `winemetal.so`). The fence then becomes a barrier inside the pass's own encoder, which removes both the reliance and the pass split.

**Why it saves:** each pass's vertex/tiling runs during the previous pass's fragment phase instead of after it. The upper bound is the frame's vertex time minus whatever can't overlap (the first pass, and passes whose vertex stage really depends on the previous pass). On base chips at 4K the fragment phases grow relative to vertex, so nearly all of the vertex time can hide.

**Risk:**
- Any vertex-stage read d9mt doesn't know of. The list in §1.4 is complete for D3D9: vertex textures are the only GPU-produced input to the vertex stage.
- Barriers across command buffers behave as today, since queue barriers already order them.
- The empty fence encoder (D2 above).
- Tests: `code_vtf_rt`, `code_vtf_alias`, `code_vtf_fb`, `code_split_load`, the hash suite, `MTL_DEBUG_LAYER`.

**CPU cost:** one epoch compare per vertex texture bind.

### 2. No `PixelFormatView` on non-block-compressed textures

**Change:** `textureUsage()` sets the flag only for block-compressed formats (BC1–BC5), whose ColorFill uses RG32Uint / RGBA32Float views. Nothing else changes.

**Why it saves:** Apple GPUs keep private render targets and textures losslessly compressed in memory. Every load, store, sample and copy of them then moves fewer bytes.

**Risk:**
- Output is bit-identical (lossless).
- A view creation that needed the flag would fail. Check that with the API validation layer across the hash suite and `code_srgb_view`.
- Slightly more memory for compression metadata.
- Before relying on compression, verify that a CPU readback (`copyTextureToBuffer`) of a compressed RT still gives the same bytes. It should, because the copy decompresses.

### 3. A Clear that ends a pass skips the cleared aspects' stores

**Change:**
- `clearRenderTarget` (`mtl_context_record.cpp:451-454`) knows, before it calls `closePass`, that the next access of the cleared aspects is a full clear.
- If the cleared view's image is an attachment of the closing pass, pass that to `createPassEncoder` as a per-slot `discardAfter`. Today that exists only for the auto depth-stencil (`discardDs`). The store of those aspects becomes DontCare.
- The aspect then takes the clear's state as today, Cleared or Lazy.
- Only for full clears of eligible single-subresource views, and per aspect: clearing depth but not stencil keeps the stencil store.
- The decision table change is unit-testable in `test/native/attachment_opt_test.cpp`.

**Why it saves:** the store writes data the very next command overwrites.

**In practice** it skips depth/stencil stores. D3D9 games clear colour where a target's use starts (folded into that pass's load, so there's no store to skip) and clear depth between draws. A colour store is skipped only when a colour-only Clear ends a pass after draws to that target. `code_clear_split` does that; games rarely do.

**Risk:** low. Nothing can run between the closing pass and the clear: the clear is the next command in stream order, and every reader in between would itself have closed the pass first.

### 4. TBDR-aware implicit flush

**Change:**
- `flushCommandList` gets the flush type: today `reason` is ignored, and the CS command doesn't know whether the flush was implicit.
- On the CS thread, an implicit hint (Weak or Strong) that arrives while a pass with draws is open on targets that stay bound is deferred. It's taken at the next `closePass` caused by a target change, a clear, a transfer or EndFrame, or at a cap of N draws or bytes.
- Synchronisation flushes are never deferred: `ImplicitSynchronization`, `ExplicitFlush`, `WaitForResource`, stalling queries, Present and the staging throttle.
- The frontend's submission fence and the `MtlSubmitStatus` serial must then refer to the command buffer that actually carries the work. The fence signal already rides in the same CS command, so it moves with it.

**Why it saves:** a mid-pass submit forces a store and a load of every attachment (§1.1).

**Risk:**
- A deferred hint delays the GPU's start on that work by up to one pass. That's harmless when GPU-bound; when CPU-bound, the cap limits it.
- Query results may arrive one pass later. `GetData(FLUSH)` still forces a flush.
- First measure `rpResume` and the flush reasons (§6.1). If SWTOR's ~9 submit pass ends mostly sit at target changes already, this candidate is worth little.
- #7's run B (cb 5 → 3, no GPU change) was at minimum clocks on an M5 Max, where bandwidth isn't the limit. So it doesn't predict base chips.

### 5. Present copy as a render pass

**Change:**
- Use the existing blit pipeline in the same-size case (exact `read()`, load DontCare, store Store, no `Load` of the drawable), and set `framebufferOnly = true`.
- Fix the blit path's needless `Load` (`mtl_context_transfer.cpp:1518`) independently.

**Risk:**
- Direct presentation: compare the HUD before and after.
- The MetalFX path and the compute copy need `framebufferOnly = false`. Keep the compute copy as the fallback and switch the layer property only when the render path is in use.

### 6. Hoist VB/IB uploads by default

`D9MT_XFER_HOIST=1` exists (`mtl_context_transfer.cpp:708-758`). SWTOR shows no upload pass ends in steady state, so it's low priority. Measure it for SWG first.

### Rejected

- **Look-ahead depth stores at frame end** (the next frame clears the depth first). The store must be decided before the frame's last command buffer is committed, and the next use comes after Present. That needs whole-frame deferral; `D9MT_DEFERRED` costs 1 ms CPU per frame (#7), and a speculative DontCare would lose data on a misprediction. Only the exact cases stay: candidate 3, and `D3DPRESENTFLAG_DISCARD_DEPTHSTENCIL` (already handled, but SWTOR doesn't set it).
- **Rendering into the drawable:** §2.
- **Memoryless targets:** §3.
- **Metal 4 render-pass suspend/resume** (`MTL4RenderEncoderOptionSuspending` / `Resuming`). It joins one pass across command buffers only when they are committed together, which removes the reason for the implicit flush. Candidate 4 gets the same result without the extra command buffer.

### Bundling

- Candidates 1–6 and the `rpResume` / flush-reason counters change only `d3d9.dll`. The users' AIR cache (keyed by `winemetal.so`'s LC_UUID 2BBB585C-5076-3157-9A6A-BAE494B00ACC) stays valid.
- The GPU-time counter (§6.1) is the only `winemetal.so` change. It goes into the #11 bundle with the PSO-flush fix, the exit-crash fix and the QoS thunk.
- **Suggested order:**
  1. counters (§6.1);
  2. candidates 1 and 2, each behind its toggle;
  3. Jan's A/B;
  4. candidate 3, then 4 if `rpResume` shows enough splits;
  5. candidate 5 only with a Direct check.

## 8. Phase 2: candidates 1–3 and the split counter (2026-10-18)

Branch `perf/gpu-4k`, one commit each. All of them change only `d3d9.dll`. `bridge/` is unchanged, and `winemetal.so` built from the branch is byte-identical to the shipped one (LC_UUID 2BBB585C-5076-3157-9A6A-BAE494B00ACC), so the users' AIR caches stay valid.

| Candidate | Change | Switch (restores `main`) |
|---|---|---|
| (tests) | `code_vtf_rt`, `code_srgb_view`, `code_depthtex`, `code_clear_split`, `bench_4k`; from the verification: `code_vtf_fb`, `code_vtf_alias`, `code_split_load` | — |
| 2 | `PixelFormatView` only on block-compressed textures | `D9MT_TEX_PFV=1` |
| 1 | End-of-pass barrier before Fragment\|Blit\|Dispatch, vertex fences for vertex textures | `D9MT_PASS_BARRIER=all` |
| 3 | A clear that ends a pass skips the pass's store of what it clears | `D9MT_CLEAR_DISCARD=0` |
| 4 (counter) | `split= splitSubmit= splitXfer= splitClear= splitMiB=` in STATS (`D9MT_PROFILE`) | — |

At startup each switch logs one line at info level: "PixelFormatView only on block-compressed textures", "end-of-pass barrier before Fragment|Blit|Dispatch", "a clear that ends a pass skips the pass's store". All three are read once, so restart the game between runs.

### 8.1 How each was checked

**Every commit:**
- run on CX26 Wine in a /tmp prefix on a /tmp copy of the engine, under `exc_guard`, with clean builds;
- all 21 test programs plus the 4 new ones give the same hash in every frame as `main`'s `d3d9.dll`;
- the same holds with the Metal API validation layer on (`MTL_DEBUG_LAYER=1`), which reports only `main`'s fixed-function binding warnings.
- `code_mtlock` (`scripts/run-hang-test.sh`, a multithreading hang test) isn't deterministic even on `main`, so it stays out of the identity comparisons.

**Per candidate:**
- **Candidate 2 (`PixelFormatView`).** Every view creation was audited (commit message). Views keep the image's format, its sRGB twin, or a swizzle. Only ColorFill's block views of BC data change layout, and BC textures keep the flag.
- **Candidate 1 (barrier).**
  - **Negative control:** without the fence, `code_vtf_rt` shows the previous frame's value in every frame (4 of 4 runs).
  - With the fence its hashes match `main`, also with `d3d9.reproducibleCommandStream=True` (which takes the pass-split path), `D9MT_PASS_BARRIER=all`, `D9MT_NO_ATTACHMENT_OPT=1` and `D9MT_DEFERRED=1`.
  - **Trace of bench_4k:** the next pass's vertex work starts before the previous fragment work ends at 100% of boundaries (0% before).
- **Candidate 1, verifier tests and finding D1.**
  - `code_vtf_alias`: vertex texture fetch through another view of what a pass rendered (mip 1; the sRGB view of a target rendered through its linear view). Without the fence, every frame shows the previous frame's values.
  - `code_split_load`: a submit split right after a heavy pass, a depth-only Clear split, and a heavy pass followed by a same-size StretchRect copy.
    - An end-of-pass barrier without Fragment changes the hashes of the first two.
    - One without Blit|Dispatch changes all three.
  - `code_vtf_fb`: a VTF draw in a pass that has already emitted feedback barriers.
    - **D1:** `emitGraphicsBarrier` advanced the vertex epoch from inside the open encoder, which let that draw skip its fence.
    - **Fixed:** with the fix it takes the pass split and fence (logged once). Before, it took neither.
    - The race itself doesn't show on the M5 Max; the hashes match `main`'s either way.
- **Candidate 3 (Clear store-skip).** `att_trace` on `code_clear_split`:
  - the pass a depth-only Clear ends stores colour and stencil, not depth;
  - a colour-only Clear keeps the depth and stencil stores.
- **Counter.** On bench_4k with 8 depth clears it reports `split=9 splitClear=8 splitSubmit=1`. With implicit flushes off, `splitSubmit` drops to 0.

### 8.2 GPU time

Measured on the M5 Max, with the machine otherwise quiet (no other Wine runs).
- bench_4k ran without vsync, with the display asleep: presents skipped, `D9MT_BG_FPS=0 D9MT_STALL_PACE=0`. It is GPU-bound: 91–99% GPU busy at the maximum performance state.
- Numbers are medians of interleaved runs.

| | `main` | Final build | Switches off |
|---|---|---|---|
| bench_4k (4 post passes, 1 depth clear) | 9.54 ms | 7.45 ms (−22%) | 9.54 ms |
| bench_4k (24 post passes, 8 depth clears) | 15.19 ms | 12.18 ms (−20%) | 15.18 ms |

After the merge of main 1440584 (#8, #10), with another agent's Wine tests running (4 interleaved runs, medians): 9.74 → 8.17 ms (−16%) and 15.62 → 13.07 ms (−16%). The verifier measured −17%/−18% paired, and −11% to −22% under load.

**Per candidate**, each against the commit before it:

| Candidate | bench_4k | gpu4k_probe |
|---|---|---|
| 2 (`PixelFormatView`) | −2.4%; with 24 post passes −8.3% (−21% per bandwidth-bound pass) | −19% |
| 1 (barrier) | −15% to −20%; with 24 post passes −7% to −9% | −13%, or −11% with per-pass sampling |
| 3 (Clear store-skip) | 8 clears: 253 MiB/frame fewer stores (STATS); GPU time −1% with compression on (within noise), −4.4% (0.49 ms) with `D9MT_TEX_PFV=1` | — |

**Reading the numbers:**
- **Candidates 2 and 3 save bandwidth.** On the M5 Max that buys little. At a base chip's 100 GB/s the same bytes are 5–6× the time.
- **Candidate 3's bytes:** one 4K depth store is 31.6 MiB, i.e. 0.33 ms at 100 GB/s, and SWTOR ends 3–8 passes a frame with a clear.
- **Candidate 1 hides vertex time** behind fragment work, so its share stays about the same on any chip.

### 8.3 In-game A/B for Jan

This needs a SWTOR on Mac build with this `d3d9.dll`. `winemetal.so` is unchanged, so the shader cache stays warm.

**Setup:**
- One fixed spot on the Fleet, camera still, 30 s settle per run, 3840×2160 and the usual settings. If possible, repeat in an operation or warzone.
- Environment for every run: `D9MT_PROFILE=1 MTL_HUD_ENABLED=1`. Add the run's switches below.
- Restart the game between runs.
- Note the HUD's GPU ms, fps, and "Direct" vs "Composited" for each run. Presentation must stay Direct: nothing in these changes touches the present path.

| Run | Switches | Answers |
|---|---|---|
| 1 | none (all on) | the build |
| 2 | `D9MT_TEX_PFV=1` | what lossless compression gives |
| 3 | `D9MT_PASS_BARRIER=all` | what the barrier gives |
| 4 | `D9MT_CLEAR_DISCARD=0` | what the Clear store-skip gives |
| 5 | `D9MT_TEX_PFV=1 D9MT_PASS_BARRIER=all D9MT_CLEAR_DISCARD=0` | `main`'s behaviour |
| 6 | run 1 + `DXVK_CONFIG="d3d9.presentInterval=0"` | GPU-bound fps: vsync keeps the M5 Max at minimum clocks (#7) |
| 7 | run 5 + `DXVK_CONFIG="d3d9.presentInterval=0"` | the same for `main`'s behaviour |
| 8 | run 1 + `DXVK_CONFIG="d3d9.reproducibleCommandStream=True"` | splits without implicit flushes (STATS `split=` / `splitSubmit=`): is candidate 4 worth doing |

**One 20 s trace per run** (§6.2 command, `--time-limit 20s`, at most 60 s). Read it with `python3 -I test/perf/mst_overlap.py <trace>`:
- runs 1, 2, 4 and 6 should show vertex/fragment overlap at most pass boundaries;
- runs 3, 5 and 7 should show 0%.

**What the logs should show:**
- PROF `vtxFence` near 0, and no "pass split for a vertex fence" line, unless SWTOR uses vertex textures. If it does, those lines show where.
- STATS `split=` / `splitSubmit=` / `splitMiB=` in run 1 against run 8 price SWTOR's mid-pass submits. That decides candidate 4.

**Compare** with `session-report.sh --compare` on the wine.logs (STATS frame times, hitches).

**Tester (M2 Pro 16 GB):**
- runs 1 and 5, and 6 and 7 if possible, each with a HUD screenshot, plus a trace if they're willing;
- also `code_vtf_rt` and `code_vtf_alias` with this `d3d9.dll`. Their hashes must match `main`'s: the vertex fence on a GPU with a different schedule than the M5 Max (D2).

### 8.4 Found on the way

On `main`, StretchRect with a filter between two render-target textures of different sizes (`blitTextureView`) leaves the destination black (`code_srgb_view`, bottom-left pair). This branch doesn't change it. It is #14, fixed on `fix/blit-views`: texture views were always null.
