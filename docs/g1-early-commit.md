# G1: commit early, complete late (heuristic flushes without pass splits)

Status: design + implementation on `perf/g1-early-commit` (base: main 2a1bc00). Origin:
`research/d9mt-architecture/REPORT.md` §5.1 G1 and §4 A2, `docs/gpu-4k.md` §1 and candidate 4,
and the earlier attempt on `perf/pass-splits` (add2590 and 57ddc7f).

## 1. The problem, priced from Jan's SWTOR logs

- DXVK's flush heuristics (`GpuFlushTracker`, `util/util_flush.cpp`) submit a command buffer
  every few 16 KiB CS chunks, at SetRenderTarget, at SetDepthStencilSurface and at EndScene.
  They were built for desktop GPUs.
- On a TBDR GPU, a submit in the middle of a render pass ends the pass. Its attachments are
  stored, and the pass that resumes on the same targets loads them again (a *split*,
  `docs/gpu-4k.md` §1.1). That is 149 MB for a 4K colour + D32FS8 target: 0.19–0.21 ms on the
  M5 Max (measured), ~1.5 ms at 100 GB/s.
- STATS of the 0.1.1-dev.1 sessions (world, 4K, median per second):

  | session | split/f | of them splitSubmit | splitMiB/f | cb/f | passes/f |
  |---|---|---|---|---|---|
  | 1228 | 12.2 | 7.3 | 900 | 17.0 | 33.7 |
  | 1234 | 16.4 | 10.2 | 1159 | 18.2 | 38.5 |
  | 1239 | 16.5 | 10.1 | 1172 | 19.6 | 39.3 |
  | 1243 | 11.0 | 6.1 | 804 | 14.8 | 34.0 |
  | 1251 | 10.5 | 6.1 | 768 | 14.4 | 34.0 |

  `splitSubmit` (a split whose pass reopened after a submit) is 6–10 per frame, about 60% of
  all splits. That is ~460–700 MiB per frame of avoidable attachment traffic, before
  compression.

## 2. Why the earlier attempt failed (perf/pass-splits)

add2590 postponed a heuristic flush to the next pass boundary: no submit at all until then.
- **(a) Hangs.** The frontend's tracker counted the flush as done (`notifyFlush`) and then
  refused later flushes without new CS chunks, including ImplicitSynchronization. Event
  GetData, D3DLOCK_DONOTWAIT and the staging throttle hung in 5 of 6 adversarial tests.
  57ddc7f fixed it with `FlushIfDeferred()` before every wait or poll on GPU progress.
- **(b) GPU starvation.** With an idle GPU, postponing means the GPU gets *nothing* until the
  pass ends: +35–64% in bench_draws "wait every frame". 57ddc7f deferred only while the GPU
  was busy.

Both are consequences of one design choice: the *commit* (feeding the GPU) and the
*completion* (the serial, the fences, every serial-keyed bookkeeping) were postponed
together.

## 3. The idea

- With the attachment optimization (the default), the open pass has **no encoder yet**: its
  draws sit in `m_cmdChunk`, and the encoder is created when the pass ends (`closePass` →
  `createPassEncoder`).
- So a heuristic flush in the middle of a pass can **commit the command buffer as it is**:
  every earlier pass, transfer and clear goes to the GPU *now*, with no `signalEvent` and no
  serial bump. A fresh command buffer is begun from the same allocator, and the pass is left
  open.
- The pass is encoded at its natural end into the new command buffer, under the **same
  serial**. The next real submit commits it and signals that serial. Queue order guarantees
  the signal fires only after both command buffers completed.

What this changes against add2590/57ddc7f:
- **No starvation of earlier work.** The GPU gets everything before the open pass at once.
  Only the open pass's own draws wait, and they are not encoded before the pass ends anyway.
- **No serial-bookkeeping change.** Every stamp made while recording refers to serial N:
  `trackingSerial`, ring chunks, arg tables, retirements, query/event serials, the fences
  signalled through `ctx->signal`. N completes *after* the GPU's last use, which is
  conservative.
- The hang class is the same: whoever waits on N needs N to be submitted. So the frontend
  hooks of 57ddc7f are ported.
- The starvation case that remains is narrow: an idle GPU with an empty prefix (the frame's
  work is mostly the open pass). It gets the old behaviour (gate in §4.2).

## 4. Design

### 4.1 A2: commit ≠ complete (backend, `mtl_context_record.cpp`)

`submit()` hand-orchestrated everything in one block. It is split into:

- `commitRecorded()`:
  - ends the main command buffer and the init (hoisted-upload) command buffer, with its
    producer barrier;
  - `residency.beforeSubmit()`, the GPU-capture hooks, `MTL4CommandQueue_commit`.

  No serial, no signal.
- `submit()` = close the pass + `commitRecorded()` + `onSubmit` (fetch-add + `signalEvent`) +
  the serial-keyed bookkeeping, as before.
- `commitPrefix()` (new) = `commitRecorded()`. Then the committed command buffers are retired
  after the open serial (`retireAfter(m_currentSerial)`: Metal 4 command buffers must outlive
  their execution). Then a new command buffer is begun from the *same* allocator slot, and
  `m_currentSerial` stays as it is.
  - `m_allocatorSerial[slot]` is set at the real submit, so the allocator, which backs both
    command buffers, is reset only after the serial completed.
  - The open pass, `m_cmdChunk`, the bound arg tables and the PSO patches are untouched. The
    chunk replays every state command since the pass opened, so it can be encoded into any
    command buffer.
  - `m_dirty` stays: nothing that was bound for the open pass needs re-emitting.
  - Push-constant ring slices allocated so far stay valid: they are stamped with serial N at
    the real submit.

### 4.2 When a flush is carried (`flushCommandList(reason, status, deferrable)`)

A flush commits the prefix and carries the open pass only if all of these hold:
1. **deferrable** (frontend, §4.3): a pure heuristic hint nothing waits on;
2. the attachment optimization is on, `D9MT_DEFERRED` is off, a pass is open and its
   encoder isn't created yet;
3. **cost gate**: the open pass's attachments sum to ≥ `D9MT_EARLY_COMMIT_MIN_MIB` (default
   8 MiB, about a 1080p BGRA8 colour target). Smaller targets split cheaply;
4. **busy gate**: the GPU still has submitted work (`completedSerial < lastSubmittedSerial`),
   or the prefix holds encoded work (it then goes to the GPU now). Only "idle GPU and empty
   prefix" keeps the old split: carrying there would starve an idle GPU (57ddc7f's finding
   (b));
5. **cap**: at most `kMaxCarries` (8) carries in a row for one pass; then the flush goes
   ahead as a normal submit.

Otherwise a normal `submit()` as today.

`D9MT_EARLY_COMMIT=0` turns G1 off. `=always` skips the busy gate (tests only).

### 4.3 Frontend (ported from add2590 + 57ddc7f)

- `ConsiderFlush(type, AllowDefer)`. AllowDefer is set only at the pure heuristic sites: CS
  chunk full (`EmitCs`/`EmitCsCmd`), SetRenderTarget, SetDepthStencilSurface, EndScene.
  `ExecuteFlush(sync9on12, deferrable)` passes it to `flushCommandList`.
- `m_flushDeferPending` (atomic: GetData can come from any thread) is set by a deferrable
  ExecuteFlush and cleared by every other one.
- `FlushIfDeferred()` submits before anything waits or polls on GPU progress:
  - GetData returning S_FALSE, with or without FLUSH;
  - the D3DLOCK_DONOTWAIT branch of WaitForResource;
  - the staging throttle before its fence wait.
- Never deferrable, so they always complete: ImplicitSynchronization, ExplicitFlush,
  Present, WaitForResource's flush, uploads, query End, the initializer, 9on12.

### 4.4 Backend waits

- `waitFor(serial)` on the open serial would stall (it is never signalled before the real
  submit). The frontend's flush-before-wait paths always do a real submit. Inside the
  backend, `beginRecording` waits only on completed slots' serials, and `waitForIdle` on
  `lastSubmittedSerial`.
- Debug check: `waitFor(serial >= nextSerial())` logs once.

### 4.5 Counters

- STATS / rstats `FlushesCarried` (carries) and `CarryCapped` (the cap hit).
- `splitSubmit` drops by construction: a carried pass keeps its serial and is not closed.
- PROF: `submit` now also covers the prefix commits.

## 5. Tests

- **Hash suite** (33 programs) with G1 on, with `D9MT_EARLY_COMMIT=always`, and with it off:
  all frame-identical to base, also with `MTL_DEBUG_LAYER=1`. The validation layer checks that
  two sequential command buffers per allocator are fine.
- **Hang tests** (`test/code_g1_hang.c`, new: 57ddc7f's adversarial tests were in /tmp and are
  gone), each with `D9MT_EARLY_COMMIT=always`, 1000+ draws per pass so chunk-full flushes
  happen inside it, and a bound on every wait:
  1. event query End, GetData without FLUSH polled to completion;
  2. the same with FLUSH;
  3. staging throttle: 4 × 25 MiB dynamic uploads in one pass;
  4. VB Lock(DONOTWAIT) spin on a buffer the pass reads;
  5. texture LockRect(DONOTWAIT) spin;
  6. occlusion query over the carried pass, its result checked.
- **Counters**: STATS `splitSubmit`/`carried` on bench_4k (4K, ~20 chunk-full flushes per frame).

## 6. Performance (the hard rule: no scenario slower)

Variants: base, basecopy (the same bytes: harness control), g1 (on), g1-off
(`D9MT_EARLY_COMMIT=0`, the cost of the code alone). Uncapped, rotated rounds, paired diffs.
- **bench_4k** (GPU-bound 4K): expect fewer splits, lower frame time.
- **bench_draws flush** ("wait every frame", the scenario that failed in add2590), **present**
  and **spinflush**.
- bindbench, churnbench (CPU, regression check).
