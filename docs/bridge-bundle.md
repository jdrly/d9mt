# Bridge bundle: AIR cache key off the LC_UUID (S1) + PSO archives without in-game cost (#11)

Status: design, 2026-10-20, revised after the design review (review-014/bridge-bundle-design.md, FIX FIRST): must-changes M1-M12 are worked in below. Part 1 is implemented on this branch; parts 3-5 wait for a re-review. Branch `bundle/bridge-s1-pso`. One bridge release: every user's AIR
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

### 1. S1: AIR cache key = codegen identity (implemented)

**Change:**
- **One object list (M2).** `scripts/build-bridge.sh` builds one array, `CODEGEN_OBJS`, of everything that decides the bytes an AIR compile returns, and links exactly that array:
  - the codegen: AirBuilder and the metallib writer, the dxbc-spirv ir/util objects, the d9mtair compile thunk (`airlib_unix_renamed.o`, which also produces the reflection blob), the LLVM archives;
  - **the cache in front of it (M3):** `air_cache_init`/`_key`/`_unpack`, the blob layout and the compile thunk moved from `winemetal_unix.c` into their own `bridge/unix/air_cache.c`, and the sqlite store `cache.c`.

  Nothing left in `winemetal_unix.c` can change what a cached compile returns, so the rule "Metal thunks don't matter" is true by construction, not by convention.
- **The identity in its own object (M1).** The script hashes that list (SHA-256 of the concatenated files) and generates `air_codegen_id.c` (`const char d9mt_air_codegen_id[] = "D9MT-AIR-CODEGEN-ID <hex>"`). It compiles and links it on every run, after hashing. No cached object includes it.
- **Fallback.** `air_cache.c` declares the symbol `weak`. A build without it (not made by the script) falls back to the LC_UUID as before.
- **Post-link check.** The script reads the identity back out of `winemetal.so` (`strings`) and fails the build if it doesn't equal the hash of the objects it just linked.

**What changes the identity (S1 review note):**
- the codegen and its IR inputs, the cache glue, LLVM, `D9MT_AIR_ABI_VERSION` (it's compiled into those objects);
- **also a new clang/Xcode or LLVM build on the build machine.** That gives a cold cache, never a stale one.

Release builds are clean builds: `build-d9mtair.sh` rebuilds by `.cpp` mtime, so an edit to an `air/` header alone isn't compiled in. Then it isn't hashed either: no false hit, but the change isn't shipped.

**Tests** (`n013-issues/results/s1-identity.txt`, bridge builds of this branch):

| build | identity | LC_UUID |
|---|---|---|
| T0 base | cc6bf9c5… | 07C777C2 |
| T1 a Metal thunk's log string changed | **cc6bf9c5… (same)** | 9B42D914 (new) |
| T2 `air_builder.cpp` changed, `winemetal_unix.c` untouched (the M1 sequence) | 80279ab6… | new |
| T3 `air_cache.c` changed | a34449b9… | new |
| T4 back to base | cc6bf9c5… | **07C777C2 (reproducible)** |

Still to run: `scripts/test-compile-mt.sh` cold → warm across T0 and T1 (the same AIR cache table, 100% hits), and the hash suite with the bundle bridge.

### 2. Startup integrity check without hashing every archive

**Change:**
- **A structural check instead of a full SHA-256 at load (M4).** The archive is a GPU fat container (two real archives measured by the reviewer). Check:
  1. magic `CB FE BA BE`;
  2. big-endian `nfat_arch` in 1..8;
  3. every `offset + size ≤ st_size`, and the largest `== st_size`;
  4. `MTLB` at each slice offset;
  5. `ENDT` at each slice end.

  That's a few `pread`s, which catches truncation, zero-filled files and foreign files: the inputs that crash `newArchiveWithURL`.
  - **Unknown layout:** an unknown magic or layout (a future OS format) falls back to the full SHA-256, it isn't deleted. An OS update then costs the old 1.2 s, not the cache.
- **`.sum` v2** = `2 size mtime_ns sha256`. Load = size + mtime + structural check. `D9MT_PSO_VERIFY=1` = the full SHA as today.
- **fsync before rename (M4).** The flush `fsync`s (`F_BARRIERFSYNC`) the tmp archive and the tmp sum before the renames, on the flush thread. A power loss after a flush then can't leave a renamed file whose data blocks never reached the disk; APFS checksums metadata, not data.
- **v1 migration (M5).** A v1 `.sum` (`size sha`) is verified once with the full SHA and rewritten as v2 (tmp + rename).
  - So the archives survive the update, which already makes the AIR cache cold: no double cold start. The archives stay valid across S1, because the AIR bytes don't change, only their key.
  - **Downgrade:** an old bridge compares whole strings, so a v2 sum mismatches and it deletes the archive. Safe, just cold.

**Gain:** ~1.2 s at the first draw with 430 MB of archives under Rosetta.

**S3:** after parts 3+4 the archive set is about one working set: SWTOR ~350 PSOs × ~40 KB ≈ 14 MB, so ≈ 40 ms of SHA. Re-measure then. If part 2 stays, a background SHA after load (BACKGROUND QoS) restores rot detection for the next start at no startup cost.

### 3. Capture only misses (archive first): waits for the re-review

**Change:**
- For a render PSO the bridge first asks the loaded archives, newest first: `[MTL4Archive newRenderPipelineStateWithDescriptor:error:]` (MTL4Archive.h, nil if the archive doesn't hold the pipeline).
- That goes past the serializer-attached compiler, so it isn't captured, and it needs no `g_pso_lock`.
- **Archive hits skip `pso_note_compiled` (M6)**, so `pending` counts only captured misses.
- Only a miss is built through the compiler (serializer attached, `lookupArchives` unchanged).

**Verification test first** (native, against real archives). The header doesn't promise a cheap nil.
1. Hit cost vs `lookupArchives` (~0.45 ms today).
2. A miss returns nil without compiling or logging.
3. A PSO from an archive works with the same argument tables and reflection.
4. Miss cost × archive count at N = 1, 8, 64. Cap N by consolidating above 4 archives.
5. Not captured, measured: the serializer is empty after N hits (flush, check the size).
6. An IR-only archive entry doesn't compile synchronously inside the lookup.
7. Concurrent lookups from compile and prewarm threads under TSan (`compile_mt_test`).

**Ships only together with part 4 (M7).** Today's retention (the newest 8 sessions) is safe only because every session re-captures its whole working set. With part 3 a session holds only deltas, and dropping a session deletes PSOs nothing else holds: churn and new-PSO stalls.

### 4. Consolidation: waits for the re-review

**Dropped (M8):** consolidation from captured descriptors.
- `MTL4PipelineDataSetSerializerConfigurationCaptureDescriptors` only enables `serializeAsPipelinesScriptWithError:`, a script for the offline `metal-tt`.
- It doesn't put descriptors into the archive, and the program can't read data out of a serializer.

**Mechanism: the state cache's prewarm, routed past part 3:**
- The prewarm builds go through a bridge-internal path: a flag on the prewarm build or a separate unix call.
- Their own `MTL4Compiler`, with their own serializer and `lookupArchives` = the loaded parts, so they're archive hits, *captured*.
- One flush when the prewarm queue drains, renamed to a consolidated name the retention logic recognises. Then delete only the parts that were loaded at this start.

**Coverage:**
- PSOs the state cache doesn't replay are lost: state cache off, or keys it evicted. That's a recompile on next use, acceptable.
- No consolidation when the state cache is disabled. Then retention stays as today.

**Concurrency:**
- an `flock` on `<dir>/.consolidate` (two clients, or a crash and a fast restart);
- unlinking parts another process has mapped is fine on unix.

**QoS (S4):** `QOS_CLASS_BACKGROUND`. Gate on time-to-first-frame with consolidation running (archload-style). Nothing needs the result before the next start, so it may also wait for the first pause.

### 5. Flush at pauses, never in a build's way: waits for the re-review

**A compile never waits on a flush (M9):**
- The compile thunk does `pthread_rwlock_tryrdlock`.
- If a flush holds the lock, it builds the miss through a second, **serializer-less** `MTL4Compiler` (created at init, same `lookupArchives`) and doesn't wait.
- That PSO isn't captured this session; it is the next time it misses.
- Why: Darwin's `pthread_rwlock` probably doesn't donate QoS to the holder, which is to be verified. Lowering the flusher's QoS would otherwise lengthen exactly the wait #11 is about.
- This keeps the hard rule for every flush trigger, the 60 s backstop included.

**When to flush:**
- **(M10) Visibility:** the bridge's own `g_vis_flags` (AppKit notifications, `winemetal_unix.c` ~4016). No new thunk, no per-Present call.
- **Hint:** `d9mt_pso_pause_hint` (PE → unix) only for device Reset, edge-triggered.
- **The "frame > 250 ms with no draws" loading trigger is dropped (S5):** SWTOR's loading screens draw.
- Flush when due and (not visible, or Reset, or idle 10 s); the 60 s backstop stays.
- The flush thread runs at UTILITY (safe once M9 holds).

**No flush at device destruction (M11).** SWTOR destroys the device on quit, and an async flush then races ExitProcess: the exit crash 9a40b43 removed.

**No flush at exit.** 9a40b43's removal of the atexit flush stays.

## Order and gates

1. **Part 1** (S1, implemented) and **part 2** (M4 + M5).
2. **Re-review** of the design for parts 3-5.
3. **The part-3 verification test**, then **parts 3+4 together (M7)**, then part 5.
4. **Gates for the whole bundle** (uncapped `D9MT_BG_FPS=0 D9MT_STALL_PACE=0`, ABBA, in a quiet window):
   - psoab steady phase with an idle flush landing inside it and the **visibility trigger forced off (M12, `D9MT_PSO_PAUSE_VIS=0`)**: the vd window is occluded for AppKit and would otherwise read as "paused". Must be 0/6 worse in worst and average frame. The pause path in a separate run.
   - psoab new-PSO phase with a forced flush during PSO creation (M9's fallback compiler): no frame > 50 ms caused by the flush.
   - Part-3 miss cost at 1, 8 and 64 archives; hit cost vs `lookupArchives`.
   - archload: time-to-first-frame with and without consolidation running.
   - Part-2 corruption tests:
     - a truncated archive, a zero-filled archive with the size/mtime of its `.sum`, and a foreign file → deleted, no crash;
     - an unknown magic → SHA fallback;
     - v1 `.sum` archives survive (loaded, rewritten as v2).
   - The S1 build tests (above) and `compile_mt_test` (plain, TSan) across two builds.
   - Hash suite with the bundle bridge.
   - One SWTOR session by Jan.
   - **Release notes:** measure the one AIR-cold start on the SWTOR working set and publish it ("the first start after this update recompiles shaders once: ~X s longer").
5. **Migration, as users see it:**
   - one AIR-cold start;
   - PSO archives stay warm (M5);
   - a downgrade gives one cold start of both caches, no crash;
   - after an Xcode update on the build machine, the next release is AIR-cold once.

## Not in the bundle

- The PE-side typed handle wrapper (architecture A6, second half): no `.so` change.
- Anything that needs measurements on Jan's machine in-game before design (#22 is ARM64EC-only).
