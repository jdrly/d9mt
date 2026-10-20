# G1: commit early, complete late (heuristic flushes without pass splits)

Status: design, revision 2 (2026-10-20), after two reviews of revision 1 (f7e4787):
- the internal subagent review;
- 0a2ecf2d, `review-014/g1-design.md`: FIX FIRST.

Revision 2 changes the carry gate (§4.2), when the carried serial completes (§4.3), the
deferrable sites (§4.4) and the poll hooks (§4.5), and adds the invariants (§4.6) and the
regression tests (§5, §6). Base: main 2a1bc00. Branch: `perf/g1-early-commit`.

Origin:
- `research/d9mt-architecture/REPORT.md` §5.1 G1 and §4 A2;
- `docs/gpu-4k.md` §1 and candidate 4;
- the earlier attempt on `perf/pass-splits` (add2590, 57ddc7f).

## 1. The problem, priced from Jan's SWTOR logs

- DXVK's flush heuristics (`GpuFlushTracker`, `util/util_flush.cpp`) submit a command buffer
  every few 16 KiB CS chunks, at EndScene, and at render-target changes.
- On a TBDR GPU, a submit in the middle of a render pass ends the pass. Its attachments are
  stored, and the pass that resumes on the same targets loads them again: a *split*
  (`docs/gpu-4k.md` §1.1). That is 149 MB for 4K colour + D32FS8: 0.19–0.21 ms on the M5 Max
  (measured), ~1.5 ms at 100 GB/s.
- STATS of the 0.1.1-dev.1 sessions (world, 4K, median per second):

  | session | split/f | of them splitSubmit | splitMiB/f | cb/f | passes/f | fps |
  |---|---|---|---|---|---|---|
  | 1228 | 12.2 | 7.3 | 900 | 17.0 | 33.7 | 60 |
  | 1234 | 16.4 | 10.2 | 1159 | 18.2 | 38.5 | 90 |
  | 1239 | 16.5 | 10.1 | 1172 | 19.6 | 39.3 | 86 |
  | 1243 | 11.0 | 6.1 | 804 | 14.8 | 34.0 | 60 |
  | 1251 | 10.5 | 6.1 | 768 | 14.4 | 34.0 | 60 |

  `splitSubmit` is 6–10 per frame, ~60% of all splits: ~460–700 MiB per frame of avoidable
  attachment traffic before compression.

## 2. Why the earlier attempt failed, and what revision 1 got wrong

add2590 postponed a heuristic flush to the next pass boundary: no submit at all.
- **(a) Hangs.** The frontend counted the flush as done and then refused later flushes. Fixed
  in 57ddc7f by `FlushIfDeferred()` before waits and polls.
- **(b) Starvation.** With an idle GPU, the GPU got nothing until the pass ended: +35–64% in
  "wait every frame". 57ddc7f deferred only while the GPU was busy *at that moment*.

Revision 1 kept 57ddc7f's gate ("GPU busy now, or something to commit"). Both reviews showed
that most of its carries were 57ddc7f's postpone again:
- after the first carry in a big pass the prefix is empty, so every later chunk-full flush
  commits nothing;
- "busy now" doesn't mean "busy until the pass ends": with wait-every-frame the GPU catches up
  inside the frame and then idles while the pass's draws are held back. The base would have
  given them to it at the split.

Further findings:
- `FlushIfDeferred` flushed whenever the last flush was *deferrable*, not when the backend
  actually carried. That means extra submits and splits on every GetData S_FALSE (also without
  FLUSH), on DONOTWAIT, in the throttle.
- The carry cap was per pass, so one serial could span a frame: ring, renames, arg tables and
  staging fences then retire per frame, and memory grows while streaming.
- Carrying at SetRenderTarget/SetDepthStencilSurface saves no split (the pass ends at the next
  draw anyway), and only holds the finished pass back.

## 3. The idea

- With the attachment optimization (the default), the open pass has no encoder yet: its draws
  sit in `m_cmdChunk`, and the encoder is created when the pass ends.
- A heuristic flush in the middle of a pass can **commit the command buffer as it is**: every
  earlier pass, transfer and clear goes to the GPU now, with no `signalEvent` and no serial
  bump. A fresh command buffer is begun from the same allocator, and the pass is left open
  under the same serial N.
- The deferred flush then **completes at the next pass boundary**: the next `materializePass`
  (or any earlier real submit) runs the real submit. That commits the pass, now ended,
  signals N, and opens N+1.
- So the GPU gets the prefix at once; only the open pass's own draws wait for the pass's end.
  A serial spans at most one pass beyond the flush point.
- Every stamp made while recording refers to N, and N completes after the GPU's last use, so
  all serial-keyed bookkeeping stays conservative:
  - `trackingSerial`, ring chunks, arg tables;
  - retirements, query/event serials;
  - the fences signalled through `ctx->signal`.

## 4. Design

### 4.1 A2: commit ≠ complete (backend, `mtl_context_record.cpp`)
- `commitRecorded()`:
  - ends and commits the main and init (hoisted-upload) command buffers;
  - `residency.beforeSubmit()` and the GPU-capture hooks.

  No serial, no signal.
- `submit()` = close the pass + `commitRecorded()` + `onSubmit` + the serial bookkeeping, as
  before.
- `commitPrefix()` = `commitRecorded()`, then:
  - the committed command buffers are retired after the open serial (`retireAfter(N)`);
  - a new command buffer is begun from the *same* allocator slot, still serial N;
  - `m_allocatorSerial[slot]` is set at the real submit, so the allocator, which backs both
    command buffers, is reset only once N has completed.

### 4.2 When a flush is carried: the backlog gate
A deferrable flush (§4.4) is carried only if all of these hold:
1. the attachment optimization is on, `D9MT_DEFERRED` is off, a pass is open, and its encoder
   isn't created yet;
2. **the GPU is still working on an earlier frame**:
   `completedSerial() < m_frameLastSerial`, where `m_frameLastSerial` is the last serial
   submitted before this frame (recorded at the Present submit, in `synchronizeWsi`). It is
   re-checked at every carry.
   - This answers "will the GPU be busy until this pass ends" with the one signal that holds
     for a whole frame: a backlog of finished frames. While it lasts, the open pass's draws
     would queue behind that work anyway, so holding them back costs no GPU time.
   - **With wait-every-frame the GPU finishes the previous frame before the next one starts,
     so G1 never carries: base behaviour by construction.**
   - Consequence: at a vsync cap with GPU headroom (SWTOR at 60 fps on the M5 Max) the GPU
     finishes each frame early, there is rarely a backlog, and G1 rarely carries. The gain is
     where the GPU is the limit (base chips at 4K, uncapped). That is the price of the hard
     rule: holding draws back from an idle GPU is what made perf/pass-splits slower.
   - `D9MT_EARLY_COMMIT=always` drops this gate (hang tests only);
3. **cost gate**: the open pass's attachments sum to ≥ `D9MT_EARLY_COMMIT_MIN_MIB` (default
   8 MiB). A 1080p colour+depth pass qualifies (~16 MiB); a 1080p colour-only pass (7.9 MiB)
   doesn't, since its split is cheap;
4. **cap per serial**: at most `kMaxCarriesPerSerial` (2) carries before the serial
   completes. In practice §4.3 completes the serial at the next pass boundary first.
   `m_carries` resets only in `submit()`.

Otherwise a normal `submit()` as today.

What a carry does:
- `commitPrefix()` if the command buffer holds encoded work;
- set `m_completePending`;
- publish the backend's `carryOpen` (atomic, §4.5).

### 4.3 Completion
- `materializePass()` (the next pass opens), with `m_completePending`: `submit(false);
  beginRecording()` first, i.e. the deferred flush. The serial N then covers exactly the work
  up to the end of the carried pass.
- Transfers and clears between the carried pass's end and the next pass
  (`prepareDirectEncode`) also complete it first. They must not extend N either.
- Any real submit (a non-deferrable flush, Present, a wait's flush, `submit(true)`) completes
  it as well and clears `m_completePending` and `carryOpen`.
- The context destructor completes an open carry (`submit(true)`).

### 4.4 Deferrable sites (frontend)
- Only the CS chunk-full flush (`EmitCs`/`EmitCsCmd`, `d3d9_device.h`) and EndScene pass
  `AllowDefer`. These are the two that fire inside a pass that continues on the same targets.
- SetRenderTarget and SetDepthStencilSurface are not deferrable: their pass ends at the next
  draw anyway, so carrying would only hold the finished pass back (internal review).
- Never deferrable: ImplicitSynchronization, ExplicitFlush, Present, WaitForResource,
  uploads, query End, the initializer, 9on12.

### 4.5 Polls and waits: no extra submits
- The backend publishes `carryOpen` (an atomic on the device: set at a carry, cleared at the
  completing submit). The frontend reads it through `MtlDevice::carryOpen()`.
- **GetData S_FALSE** (with or without FLUSH):
  - the query/event status gains `PendingUnsubmitted` (the backend's serial ≥ `nextSerial()`);
  - the frontend flushes only if `carryOpen()` and the status is `PendingUnsubmitted`. A query
    from an earlier, submitted serial never forces a flush;
  - otherwise exactly base's behaviour: no flush without FLUSH, `ConsiderFlush(Sync)` with
    FLUSH.
- **D3DLOCK_DONOTWAIT**: flushes only if `carryOpen()` and the resource's `trackingSerial()`
  ≥ `getNextSerial()`. Otherwise base's hint.
- **Staging throttle** (a blocking wait): before waiting on the staging fence, it flushes only
  if the last flush was deferrable *and* (the CS thread hasn't executed it yet
  (`lastSequenceNumber() < m_flushSeqNum`) or `carryOpen()`).
- Polls repeat, so a poll that comes before the CS thread carried simply sees S_FALSE, and
  the next one sees `carryOpen`.
- Threading: GetData without FLUSH can now call `ExecuteFlush`. It does so only under
  `carryOpen` with an unsubmitted query, and on a device without D3DCREATE_MULTITHREADED
  polled from another thread that is the same class of race as base's GetData(FLUSH).

### 4.6 Invariants
- **Everything recorded is keyed to the open serial N.** A committed prefix runs under N, which
  is signalled only after the continuation. Every wait that matters waits for N or later and is
  preceded by a real submit (§4.5, `WaitForResource`'s `ExecuteFlush`, Present).
- `waitForIdle()`/`drainAndStopPump()` wait for `lastSubmittedSerial`, which does **not**
  cover a committed prefix of the open serial. That is safe only because nothing outlives
  the device without completing N: the context destructor completes an open carry before the
  tracker drains.
- `waitFor(serial ≥ nextSerial())` is a bounded 10 s stall plus a one-time log line
  ("not submitted yet"). It must never happen. The tests treat that line as a FAILURE (§5).
- One queue, one submitter: the serial is taken and signalled in submission order (the same
  invariant as perf/serial-cache-retire).

### 4.7 Counters
- STATS `carried=` (carries per frame), `carryRefused=` (deferrable flushes refused by the
  backlog gate) and `carryCap=` (the per-serial cap hit).
- `cb=` and the PROF `Submit` count include prefix commits: a carried frame shows *more* `cb`.
- `splitSubmit` drops by construction: a carried pass keeps its serial and isn't closed.

### 4.8 Rebase onto perf/serial-cache-retire
- The prefix command buffers stay on `retireAfter(N)` (RetireBatch has one slot per command
  buffer kind).
- The open RetireBatch is handed over only at the completing submit, with serial N.

## 5. Tests (correctness)
- **Hash suite** (33 programs) with G1 on, with `D9MT_EARLY_COMMIT=always`, and with it off:
  all frame-identical to base, also with `MTL_DEBUG_LAYER=1` (two sequential command buffers
  per allocator).
- **`test/code_g1_hang.c`** (in the tree; 57ddc7f's adversarial tests were in /tmp and are
  gone). With `D9MT_EARLY_COMMIT=always`, a 4K pass of 1500 UP draws, so chunk-full flushes
  happen inside it, and a 5 s bound on every poll:
  1. event GetData without FLUSH (base's result is the reference);
  2. event GetData(FLUSH);
  3. 4 × 26 MiB managed uploads in one pass (the staging throttle);
  4. VB Lock(DONOTWAIT);
  5. dynamic texture LockRect(DONOTWAIT);
  6. occlusion query over the carried pass, samples > 0.
- **Every** G1 run (suites, hang tests, benches) fails if the log contains "not submitted yet".
- Counters (D9MT_PROFILE):
  - `carried` > 0 and `splitSubmit` down on bench_4k (GPU-bound);
  - **`carried` ≈ 0 on bench_4kw "wait"** (the backlog gate);
  - submits per frame (`cb`) on the GetData-spin bench, base vs g1;
  - `ringMiB`/`recycledMiB` (D9MT_MEMSTATS) on the upload bench, base vs g1.

## 6. Performance (the hard rule: no scenario slower)

Variants:
- base;
- basecopy (identical bytes: the harness control);
- g1;
- g1-off (the G1 DLL with `D9MT_EARLY_COMMIT=0`: the cost of the code alone).

Uncapped, 10 rotated rounds, paired diffs, in a quiet window.

| bench | what it guards |
|---|---|
| bench_4k (4K, ~2200 draws, GPU-bound) | the gain: fewer splits |
| **bench_4kw wait** (the same, waiting for the GPU every frame) | the case perf/pass-splits lost: the backlog gate means G1 must not carry, so = base |
| bench_draws flush / present | small targets: no carry (cost gate), = base |
| **bench_getdata** (new: 4K, chunk flushes, GetData spin on the previous frame's occlusion query) | no extra submits from the poll hooks |
| **bench_upload** (new: 4K pass, per-draw DISCARD renames + managed texture updates per frame) | bounded serial length: ring/rename/staging recycling |
| bindbench, churnbench | CPU regression check |
