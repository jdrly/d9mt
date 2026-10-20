# Bridge bundle: AIR cache key off the LC_UUID (S1) + PSO archives without in-game cost (#11)

Status: design, 2026-10-20. Branch `bundle/bridge-s1-pso`. One bridge release: every user's AIR
cache goes cold once (the last time a bridge change does that), and the PSO archives stop costing
render time.

## Why one bundle

- `winemetal.so`'s LC_UUID is the AIR cache key (`air_build_identity`, `winemetal_unix.c:5853`).
  Any change in that file, including a Metal thunk or a log line, recompiles every shader
  for every user.
- #11's fix changes `winemetal.so`, so on its own it would cost one cold session for every user.
- S1 makes that the last time: after it, only changes to the AIR codegen invalidate the cache.

## Measurements this design rests on (n013-issues, 2026-10-20)

| finding | number | source |
|---|---|---|
| inline flush stall | ~0.5-0.67 s per flush in SWTOR (#11); psobench: base 4 frames > 50 ms per run, ~2.1 s | psoab |
| flush moved to a thread (9a40b43) | stalls −1.2 s/run (6/6), but steady frames slower in 5/6 (worst +5.9 ms median, max 32 ms) | psoab |
| archive size | ~51 MB per session for 1200 trivial PSOs (~40 KB/PSO); 8 kept sessions = 411 MB, mostly the same pipelines | psoab's archive dirs |
| startup | SHA-256 check of the archives: 1.23-1.34 s for 430 MB in the x86_64 bridge (Rosetta) vs 0.14 s arm64; `newArchiveWithURL` 15-21 ms | archload |
| serializer vs submission | a native Metal commit loop is unchanged while another thread serializes + hashes an archive | flushcontend |

Two conclusions:
1. The cost is the **amount** written and verified, not a lock.
2. The amount is large because every session re-captures every PSO it touches, archive hits
   included, and up to 8 such sessions are kept and fully re-hashed at every start.

## Parts, in commit order

### 1. S1: AIR cache key = codegen identity

**Change:**
- `scripts/build-bridge.sh` computes a SHA-256 over what determines AIR output:
  - the airlib objects linked into winemetal.so (`air_air_builder.cpp.o`, `air_air_metallib.cpp.o`, the renamed `airlib_unix`);
  - the dxbc-spirv ir/util objects;
  - the LLVM archives;
  - `D9MT_AIR_ABI_VERSION`.
- The objects are path-free (`-ffile-prefix-map`) and the build is reproducible: ac70a8c rebuilt gives the shipping 2BBB585C.
- The hash goes into a generated header (`build/bridge/air_codegen_id.h`). `air_build_identity` returns it instead of the LC_UUID.
- The LC_UUID stays only as the fallback when the header is missing (dev builds without the script).

**What still invalidates the cache:** AIR codegen, its IR inputs, LLVM, the ABI version. Not thunks, the PSO archive code or logging.

**Risk:** a codegen-relevant change outside those objects would reuse stale metallibs.
- The cache-key thunk (`air_cache_key`) is in `winemetal_unix.c`. It hashes flags, spec constants, the entry name and the IR bytes, which are its inputs, so a change there that alters output must bump `D9MT_AIR_ABI_VERSION`.
- Written next to the function, and checked by a test (below).

**Test:**
- Two bridge builds that differ only in a Metal thunk give the same identity, so the second build's run hits the AIR cache 100%.
- A build with a changed `air_builder.cpp` gives a different one.
- `scripts/test-compile-mt.sh` cold/warm across the two builds.

### 2. Startup integrity check without hashing every archive

**Change:**
- `.sum` v2 = `size mtime_ns sha256`, written at flush as today plus the mtime.
- At startup an archive is loaded when size and mtime match its `.sum`. A mismatch means it's deleted, as today.
- What the check is for: `newArchiveWithURL` segfaults on non-archive files and accepts truncated ones (the reason for the `.sum`). Truncated or replaced files change size or mtime.
- The full SHA-256 is only needed against silent media corruption, and stays available as `D9MT_PSO_VERIFY=1`.

**Gain:** ~1.2 s at the first draw with 400 MB of archives (Rosetta); proportionally less with fewer. Zero in-game.

### 3. Capture only misses (archive first)

**Change:**
- For a render PSO the bridge first asks the loaded archives, newest first: `[MTL4Archive newRenderPipelineStateWithDescriptor:error:]` (MTL4Archive.h).
  - It returns nil when the archive doesn't hold the pipeline.
  - It doesn't go through the serializer-attached compiler, so it isn't captured.
- Only a miss is built through the compiler (serializer attached, `lookupArchives` unchanged for partial hits of functions). That's the only thing captured.
- A session's archive then holds only pipelines that no kept archive had.

**To verify first** (native test against real archives, as in archload):
1. Hit cost through `MTL4Archive` vs `lookupArchives` (today ~0.45 ms per hit).
2. A miss returns nil cheaply (no compile, no log spam).
3. A PSO from an archive works with the same argument tables and reflection as one from the compiler.

Fallback if (3) fails: capture-only-misses by asking the archive whether it can satisfy the descriptor, and keep the compiler path for the PSO itself. That is also cheap, because it's an archive lookup.

**Consequence:** old sessions are no longer superseded by newer ones (they aren't re-captured). Retention changes to consolidation (4).

### 4. Consolidation at low QoS

**Change:**
- At startup, with more than a few delta archives (say > 4) or more than N MB, a background thread at `QOS_CLASS_UTILITY` builds one consolidated archive:
  1. its own `MTL4Compiler` with its own serializer;
  2. every pipeline of the kept archives re-created through it, via archive lookups;
  3. one flush;
  4. rename into place, then delete the parts.
- It holds none of the game's locks. Its own compiler doesn't contend with the game's `g_pso_lock`.
- It needs the pipeline descriptors: the serializer can capture them (`MTL4PipelineDataSetSerializerConfigurationCaptureDescriptors`), and switching the capture configuration to descriptors + binaries makes each archive carry what's needed to rebuild it.
- **Alternative without descriptors:** the state cache (#10) already records every pipeline per exe and prewarms them in the background at startup. If the prewarm builds through a serializer, it re-captures the working set once: that *is* the consolidation, and part 3 keeps gameplay from re-capturing it.

**Gain:** the startup set stays one archive of the working set instead of 8 overlapping ones.

### 5. Flush at pauses, off the game's frame time

**Change:** the flush thread (9a40b43) keeps the quiet-moment rule but gets a stronger signal and a lower priority:
- d3d9.dll calls a new unix entry `d9mt_pso_pause_hint` (PE thunk + unix call, part of the same ABI bump):
  - on Present while the window isn't visible (the presenter's `isVisible`);
  - on device Reset;
  - on device destruction;
  - and when a frame took > 250 ms with no draws (a loading screen).
- Flush when due *and* (paused *or* idle 10 s). Otherwise wait.
- The 60 s "force between builds" rule stays as the backstop.
- The flush thread runs at `QOS_CLASS_UTILITY`.
- With part 3, a due flush holds tens of new pipelines, not the whole working set, so even the backstop is short.

The no-atexit part of 9a40b43 stays: the exit crash.

## Order and gates

1. S1 (part 1) with its tests. Nothing else depends on the bridge being otherwise unchanged.
2. Parts 2 and 5 (small, independent).
3. Part 3 after its verification test, then part 4.
4. Then the whole bundle:
   - hash suite with the bundle bridge;
   - `compile_mt_test` (plain, TSan);
   - psoab with the spike trace in a quiet window: no stall in the new-PSO phase, no change in the steady phase;
   - an archload-style startup check;
   - one SWTOR session by Jan (the archives of a real working set: ~350 PSOs, sizes unknown).
5. The other bridge items of the architecture report (S4 commit feedback, fused calls) can follow without another cold cache once part 1 is in.

## Not in the bundle

- The PE-side typed handle wrapper (architecture A6, second half): no `.so` change.
- Anything that needs measurements on Jan's machine in-game before design (#22 is ARM64EC-only).
