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

Cold → warm across T0 and T1 (different LC_UUIDs): the same AIR cache table, metallibs consistent (`n013-issues/results/s1-cross-cache.txt`). The reviewer's verdict on part 1 (review-014/bridge-bundle-s1-part2.md): **MERGE** into the bundle. Still a bundle gate: the hash suite with the bundle bridge.

### 2. Startup integrity check without hashing every archive (implemented)

**Change:**
- **A structural check instead of a full SHA-256 at load (M4)**, in `bridge/unix/pso_archive_check.c` (pure C, unit-tested by `bridge/tests/pso_archive_check_test.c`).
- **The shape Metal writes:**
  - 623 archives on this machine (reviewer) and 347 in n013-issues, 2 or 3 slices each;
  - a GPU fat container: magic `CB FE BA BE`, big-endian `nfat_arch`, `{cputype, subtype, offset, size, align}` per slice;
  - one or two metallib slices (`MTLB` head, `ENDT` in its last 4 bytes) and a Mach-O slice (`CF FA ED FE` head, `ENDT` at 0, 4, 8 or 12 of its last 16 bytes: 0-12 bytes of padding after it);
  - the last slice ends at EOF.
- **The check returns:**
  - **1, load:** every slice inside the file with an `MTLB` or Mach-O head and `ENDT` in its last 16 bytes, and the last slice ending at EOF.
  - **0, delete, only proof of damage:** a slice offset/size outside the file, a short read, or the last slice not ending at EOF.
  - **−1, the full SHA-256 decides (P2-M2):** another magic or slice kind, `nfat_arch` outside 1..64, or no `ENDT` in the window. That may be a format change, and deleting there would delete a good archive at every start, a silently cold cache. It is logged once, so a format change shows up.
- **Residual:** a zeroed range *inside* a slice passes the shape (media rot); `D9MT_PSO_VERIFY=1` catches it.
- **The power-loss file** (the tail never written, size and mtime as in the sum) has no `ENDT`. The SHA fallback catches it and it's deleted (`test-pso-archive-check.sh` `tailzero`, with the log line). The fsync below keeps that file from arising in the first place.
- **`.sum` v2** = `2 size mtime_ns sha256`.
  - Parsed exactly (P2-M1): four tokens separated by single spaces, the first `2`, decimal size and mtime, 64 lowercase hex, nothing else. A v1 sum whose size starts with 2 can't pass for v2: with `sscanf("2 %llu %llu %64s")` that was ~11% of v1 archives, deleted instead of migrated.
  - Load = size + mtime + the shape check. `D9MT_PSO_VERIFY=1` = the full SHA as today.
- **fsync before rename (M4).** The flush `fsync`s (`F_BARRIERFSYNC`) the tmp archive and the tmp sum before the renames, on the thread that flushes: on this branch still the build path (P2-C), the flush thread once part 5 is in. A power loss after a flush then can't leave a renamed file whose data blocks never reached the disk; APFS checksums metadata, not data.
- **v1 migration (M5).** A v1 `.sum` (`size sha`) is verified once with the full SHA and rewritten as v2 (tmp + rename).
  - So the archives survive the update, which already makes the AIR cache cold: no double cold start. The archives stay valid across S1, because the AIR bytes don't change, only their key.
  - **Downgrade:** an old bridge compares whole strings, so a v2 sum mismatches and it deletes the archive. Safe, just cold.

**Gain:** ~1.2 s at the first draw with 430 MB of archives under Rosetta.

**Ship condition (P2-C):**
- On this branch the flush still runs inline on the build path, and part 2 adds `F_BARRIERFSYNC` × 2 (archive + sum) to that render-thread stall.
- **Part 2 must not reach main or a release ahead of part 5** (the flush thread). If it ever goes alone, measure the flush with and without the fsync first.

**Tests:**
- `bridge/tests/pso_archive_check_test.c`:
  - parser: the P2-M1 strings, extra tokens, newline, double space, case, length;
  - shape: ENDT at 0-12, tail zeroed, truncated, longer than the last slice, an offset outside, zero-filled, another magic, an unknown slice kind, 9 / 65 / 0 slices;
  - every real archive given on the command line has the shape.
- `scripts/test-pso-archive-check.sh` (through the bridge, no Wine): v2 written and loaded, v1 loaded and rewritten, trunc/zero/shape/tailzero deleted without a crash, and a wrong SHA with the right shape loads (so the shape check accepted it), which `D9MT_PSO_VERIFY=1` refuses.

**S3:** after parts 3+4 the archive set is about one working set: SWTOR ~350 PSOs × ~40 KB ≈ 14 MB, so ≈ 40 ms of SHA. Re-measure then. If part 2 stays, a background SHA after load (BACKGROUND QoS) restores rot detection for the next start at no startup cost.

### 3. Capture only misses (archive first): implemented (422d434)

**Change:**
- For a render PSO the bridge first asks the loaded archives: **the consolidated archive first** (it holds most PSOs), then the deltas newest first (R4). The call is `[MTL4Archive newRenderPipelineStateWithDescriptor:error:]` (MTL4Archive.h, nil if the archive doesn't hold the pipeline).
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

**Verification result** (`bridge/tests/archfirst_test.m check`, native x86_64 as the bridge runs, plain and TSan, 2026-10-20):

| item | result |
|---|---|
| 2 hits / misses | 64/64 archived descriptors come back from `MTL4Archive`. A descriptor never built returns **nil without compiling**, with the error "Failed to find fragment function … with key …". Blend state is part of the function key on Apple GPUs. |
| 3 usable | a PSO from the archive renders the same pixel as the compiler's (ff4080bf = ff4080bf) |
| 5 not captured | after 64 archive hits a serializer-attached compiler's serializer has "Nothing to serialize"; after one compiler build its archive holds that PSO and 0 of the 64 hits |
| 7 concurrency | 4 threads × 4 rounds of lookups: all hits, no TSan report |
| 1, 4 cost | `archfirst_test time`: needs a quiet window. The miss on one archive was 0.14-0.97 ms in the check run (not a measurement) |
| 6 IR-only entries | not tested: the bridge's serializer captures binaries (CaptureBinaries), so its archives hold none |

**Ships only together with part 4 (M7).** Today's retention (the newest 8 sessions) is safe only because every session re-captures its whole working set. With part 3 a session holds only deltas, and dropping a session deletes PSOs nothing else holds: churn and new-PSO stalls.

### 4. Consolidation: implemented (422d434)

**Dropped (M8):** consolidation from captured descriptors.
- `MTL4PipelineDataSetSerializerConfigurationCaptureDescriptors` only enables `serializeAsPipelinesScriptWithError:`, a script for the offline `metal-tt`.
- It doesn't put descriptors into the archive, and the program can't read data out of a serializer.

**As implemented: the bridge records, the bridge rebuilds.**
- Instead of routing the prewarm builds through a flag (a PE → unix ABI change), the PSO thunk records every render-pipeline descriptor requested this session, hits included.
- The state cache's prewarm requests every recorded pipeline, so with it the record *is* the working set. That's the same coverage as the prewarm routing below, and nothing changes on the PE side.
- Once the session has been idle for `D9MT_PSO_CONSOLIDATE_IDLE_MS` (10 s), a `QOS_CLASS_BACKGROUND` thread (`pso_consolidate`) rebuilds them:
  - through its own compiler and serializer;
  - `lookupArchives` = the loaded archives + this session's deltas, so mostly lookups;
  - into one consolidated archive `c-<time>-<pid>.mtl4archive` (v2 sum, fsync before rename).
- Then it deletes the archives it replaces: the ones loaded at this start and the deltas flushed before it began.
- **Limits:**
  - **Once per session**, under an `flock` on `.consolidate`.
  - **Skipped** when the session only hit a single archive and had no miss: rebuilding then would only drop what this session didn't use.
  - **Exit during it:** a `.tmp` (cleaned at a later start), and the parts stay.
- **Startup:** keeps the newest consolidated archive plus every delta newer than it, looked up in that order (R4, R5).

**The design-review alternative it replaces: the state cache's prewarm, routed past part 3:**
- The prewarm builds go through a bridge-internal path: a flag on the prewarm build or a separate unix call.
- Their own `MTL4Compiler`, with their own serializer and `lookupArchives` = the loaded parts, so they're archive hits, *captured*.
- One flush when the prewarm queue drains, renamed to a consolidated name the retention logic recognises. Then delete only the parts that were loaded at this start.

**Coverage:**
- PSOs the state cache doesn't replay are lost: state cache off, or keys it evicted. That's a recompile on next use, acceptable.
- **State cache off (R1):** consolidation can't cover the working set, and then archive-first is off too. Every build is captured as today, and today's retention (the newest 8 sessions) stays safe.
  - In code: `D9MT_STATE_CACHE=0` (or `D9MT_PSO_ARCHIVE_FIRST=0`) switches both off in `pso_archive_init`.
  - The state cache is controlled by that variable only (`MtlStateCache::resolveConfig`), and the bridge reads it from the same process environment.

**Tests** (`scripts/test-pso-archive-first.sh`, compile_mt_test, no Wine, plain and TSan):
- **cold:** every PSO misses and is captured; after the idle period one consolidated archive replaces the deltas.
- **warm:** "16 hits, 0 misses", and no delta of the run's own. Hits aren't captured: verification item 5, now in the real path.
- **miss:** more shaders on the warm archives; the new PSOs miss, are compiled, and the results (metallib hashes, PSO ok/fail) equal a cold run of the same set.
- **R1:** archive first off, the run captures its PSOs, no consolidation.
- **TSan:** no reports.
- `test-pso-archive-check.sh` still passes.

**Retention with consolidation (R5):**
- Keep the consolidated archive plus all deltas newer than it. Consolidate above 4 deltas.
- A failed or interrupted consolidation keeps the parts. The `.tmp` cleanup at startup removes its leftovers.
- The consolidated archive gets a v2 sum and an fsync before any part is deleted.

**Concurrency:**
- an `flock` on `<dir>/.consolidate` (two clients, or a crash and a fast restart);
- unlinking parts another process has mapped is fine on unix.

**QoS (S4):** `QOS_CLASS_BACKGROUND`. Gate on time-to-first-frame with consolidation running (archload-style). Nothing needs the result before the next start, so it may also wait for the first pause.

### Parts 3+4 review fixes (review-014/bridge-bundle-part34.md, FIX FIRST)

| point | fix | test |
|---|---|---|
| **C-1** coverage | Consolidation only after a positive signal from the PE side: `D9MTPsoArchiveCoverage(complete)` (unix call 213, the bundle's ABI bump). The state cache sends it once its prewarm has handled every prewarmable entry loaded at start. `complete` requires (as revised in milestone 6, below): the file loaded `Ok` (not new, corrupt or truncated), recorded under its key for at least `CoverageMinSessions` (8) sessions, nothing invalid or suspect, prewarm on. Without the signal, or with `complete = 0`, no consolidation runs and nothing is deleted. Deltas stay up to `D9MT_PSO_MAX_ARCHIVES` files and `D9MT_PSO_MAX_MB` (512) MB, not by session count. | `test-pso-archive-first.sh`: no signal → no consolidation; signal 0 → none; signal 1 → consolidated |
| **C-2** rename order | an archive without its `.sum`, and a `.sum` without its archive, are left alone while younger than 600 s (04e3389) | `test-pso-archive-shared.sh` young/orphan |
| **C-3** naming and the startup lock | The consolidated archive is named by consolidation time (`c-<time>-<pid>`). A delta is dropped for being older than it by its file time, not its session start. The startup scan takes the `.consolidate` flock non-blocking; while another process holds it, that start deletes only proven damage. Rule-based deletes skip files of live pids (04e3389). | shared: stress, owner |
| **C-4** unlinked parts | verified: after the file and its sum are unlinked, every lookup in the loaded `MTL4Archive` still hits and renders the same pixels (plain and TSan). If MTL4Archive reopened the URL, it would fail on the missing path whatever the page cache holds. | `archfirst_test check` item 6 |
| **cost** | Consolidation never compiles: it rebuilds only descriptors a loaded archive (or this session's deltas) already holds, checked with an `MTL4Archive` lookup. A PSO no archive holds yet comes later as a delta, which the retention keeps. It runs at a pause (the window known not visible: `g_vis_observing && !VISIBLE`, R2; off with `D9MT_PSO_PAUSE_VIS=0`, M12) or after `D9MT_PSO_CONSOLIDATE_IDLE_MS` (now 120 s) without a PSO request, never at the old 10 s. | first: the cold run reports "N not archived yet"; R2 run |
| should | Recording stops at the consolidation, and its record (which retains every MTLLibrary) is released. Only this session's deltas are loaded for the rebuild, not the archives already in `g_pso_lookup`. | code |
| quit during consolidation | idle 0 + exit right after the builds, looped: no crash, no hang, the next start finds only sound archives (this branch still has the atexit flush) | shared: quit |

**Still a gate (dynamic, game gate / quiet window):**
- `archfirst_test time`;
- `test-compile-mt.sh` 1700 PSOs cold/warm with archive-first on, plain + TSan, `D9MT_PSO_CONSOLIDATE_IDLE_MS=500` plus a coverage signal so that consolidation overlaps live compiles;
- the hash suite twice;
- psoab with a consolidation inside the steady phase (`D9MT_PSO_CONSOLIDATE_IDLE_MS=2000 D9MT_PSO_PAUSE_VIS=0`, state cache on with a loaded file so the signal comes): 0/6 worse, plus the RSS peak.

### Milestone 6: review of dff452a (FIX FIRST)

| point | fix | test |
|---|---|---|
| **F-1** consolidation name time | The `c-<time>` stamp is taken with the snapshot of the record and the parts, before the rebuild. A delta flushed while the rebuild runs is newer than the name, so the next start keeps it. Before, the name was taken after the rebuild and such a delta (its PSOs in no other archive) was deleted. | first: F-1. `D9MT_PSO_CONSOLIDATE_DELAY_MS` (test hook, read once at init) holds the consolidation 4 s after its snapshot; `MT_LATE_PSOS` builds new PSOs in that window. 3 deltas land in it; the next start keeps them and has 0 misses. With the old naming: 13 misses. |
| **lookup cap** | Archive first asks the consolidated archive and the newest `D9MT_PSO_ARCHIVE_FIRST_MAX` (8) deltas; a miss costs one lookup per archive asked. Older deltas are still the compiler's `lookupArchives`. A PSO only in one of them is an archive-first miss, built by the compiler from that archive, and **captured into a new delta by design**: the old delta can then age out under the retention without losing it. | first: cap. 3 deltas of a recording run, then 9 newer ones: its 16 PSOs miss archive first, the results equal the recording run's, and the run writes a delta. The same directory with a cap of 64: 0 misses. Uncapped (old code): 0 misses, nothing captured. |
| **restore** | A v2 sum whose size matches but whose mtime doesn't (a backup restore or copy that didn't keep times): the SHA-256 decides, and on a match the sum is rewritten with the new time (one SHA per file, once). Before, every such archive was deleted. | first: restore. `cp -R` of a warm directory: all hits, nothing dropped, every sum carries the copy's time. A byte flipped in the copy (same size): deleted. Old code: 16 misses. |
| should: skipped | Entries whose shader doesn't compile here (or whose function is gone) are skipped by the prewarm and no longer block `complete`: they can't match an archived PSO either. | state cache: testCoverage |
| should: capped / stale | Entries over the state cache's cap, and entries unused for more than 32 sessions (outside the prewarm window), no longer block `complete`. They are the state cache's own retention decision, as today's archive retention is. | testCoverage: half the entries stale → complete, 50 built; cap 64 of 100 → complete |
| should: young file | A file whose entries were dropped (another key signature, a bad header, an older format) starts its session count over at 1; `complete` needs `CoverageMinSessions` (8) sessions under the key, so a reset file doesn't consolidate the archives down to its first session's draws. (A file dropped by the crash guard is deleted and so starts at 1 already.) | testCoverage: session 7 → not, session 8 → complete; key change → session 1, no signal. The existing key-change test now expects session 1. |

| **exe directory** | Archives live in `<base>/<exe>/os-<leaf>/`: consolidation deletes what one executable's state cache doesn't replay, so two games (or a game and its launcher) must not share a directory. `<exe>` is the basename of `_NSGetArgv()[0]` after the last `/` or `\` (Wine sets argv[0] to the Windows path: `ps` shows `C:\windows\system32\services.exe`), lowercased, `[a-z0-9._-]` (others `_`), at most 64 characters, a leading `os-` made `os_` (the cleanup's prefix). **Not an .exe** (empty, no `.exe` suffix): exactly today's behaviour, the flat `<base>/os-<leaf>/` with archive first off, so no consolidation, no consolidation deletes, and the 8-session retention. **Migration:** the first executable to start takes the flat directory of earlier releases with one atomic `rename` into `<base>/<exe>/` under an flock on `<base>/.migrate`; the others start cold. The cleanup of other macOS builds' sets runs in the executable's directory and in the flat one, never on the flat `os-<leaf>` (an earlier release may still use it); an executable's directory never starts with `os-`, so neither today's flat cleanup nor an earlier release's touches it. | shared: exe (SWTOR.EXE under `C:\Program Files\...` → `swtor.exe`, `My Game!.exe` → `my_game_.exe`, `OS-x.exe` → `os_x.exe`, 74 characters → 64; `launcher`, `C:\games\`, `game.exe.bak` → flat, archive first off). migrate: 10 rounds of two executables started at once on a flat directory: exactly one takes it, the other loads 0 archives, the flat directory is gone, no crash; a flat start afterwards leaves both executables' archives alone and removes another build's flat set; an executable's start removes another build's set in its directory and leaves the (recreated) flat directory alone. The harness sets argv[0] (`MT_EXE`, default `C:\games\mt.exe`). Under Wine: a game-gate item (the init line names the directory). |

Mutation check: with the three bridge fixes reverted, the F-1, cap and restore tests fail (5 failures); with them, `test-pso-archive-first.sh` (plain + TSan, 0 reports), `-shared.sh`, `-check.sh`, `check-abi-mix.sh` and `test-state-cache.sh` (plain + TSan) pass.

**Duplicates (MT_REPEAT, n013-issues/scripts/dedup.sh).** The same 16 descriptors requested 1× and 3× in one session give a consolidated archive of the same size (97504 B): the serializer keeps a pipeline once, and so does consolidation, whatever the record's repeats ("N builds" in its log line counts requests). Without a flush in between, the repeats also give one delta of the same size. Across a flush they don't: a flush empties the serializer and this session's deltas aren't asked first, so a repeat after it is compiled and captured again (flush every 5: 3 deltas / 253 KB for 1×, 7 / 593 KB for 3×). That costs disk until the next consolidation plus one compile per repeat. d9mt asks the bridge once per key: no eviction in RenderPsoCache or the registry; only a failed prewarm build is retried once. The same descriptor comes back under another key only through key fields that don't reach the descriptor. The systematic one is `topologyClass` (in the key, overwritten with Unspecified at build: one shader pair drawn as triangles, lines and points is up to three keys and builds). The rare ones are depth/stencil formats, identical metallibs from different bytecode, and a recreated device. Dropping `topologyClass` from the key (and zeroing RenderPsoKey's 2 padding bytes) is the CPU lane's own branch, with the one state cache key change there. So this is not regular, and adding each new delta to `g_pso_lookup` stays out of the bundle.

Known limit: a partly corrupt state cache file blocks `complete` for that session only. The next session's file has lost those entries, and its consolidation drops their PSOs: a recompile on next use, as for an evicted key.

**Future, not in the bundle: per-part coverage, for games with per-zone shaders.** Today `complete` is all or nothing: one entry whose shaders weren't announced (content this session didn't load) means the prewarm never drains its whole index, no signal is sent, and nothing consolidates. SWTOR doesn't hit this: Jan's logs show prewarm built = prewarmable (318/330/344) with 0 skipped. A game that loads shaders per zone would never consolidate and keep accumulating deltas up to `D9MT_PSO_MAX_ARCHIVES`/`D9MT_PSO_MAX_MB`. The sketch: the state cache reports coverage per delta (the deltas whose recorded pipelines were all replayed) and the consolidation deletes only those, keeping the rest as deltas. Open question: does SWGL Legends load shaders per zone? Check its prewarm lines (built vs prewarmable, skipped) before building this.

### 5. Flush at pauses, never in a build's way: waits for the re-review

**A compile never waits on a flush (M9):**
- The compile thunk does `pthread_rwlock_tryrdlock`.
- If a flush holds the lock, it builds the miss through a second, **serializer-less** `MTL4Compiler` (same `lookupArchives`) and doesn't wait.
  - **Where it comes from (R3):** created in `pso_archive_init` next to the serializer, for that device. One device per process is assumed: d9mt creates one MTLDevice. If that ever changes, key the fallback by device.
  - **MetalFX scaler creation** (`_MTLDevice_newMTL4SpatialScaler`) also takes `g_pso_lock` shared and can't use the fallback, so it still waits on a flush. That's rare: creation at init and on resize.
- That PSO isn't captured this session; it is the next time it misses.
- Why: Darwin's `pthread_rwlock` probably doesn't donate QoS to the holder, which is to be verified. Lowering the flusher's QoS would otherwise lengthen exactly the wait #11 is about.
- This keeps the hard rule for every flush trigger, the 60 s backstop included.

**When to flush:**
- **(M10) Visibility:** the bridge's own `g_vis_flags` (AppKit notifications, `winemetal_unix.c` ~4016). No new thunk, no per-Present call.
  - **(R2)** `g_vis_flags` is zero until the first visibility query, which would read as "not visible": during startup and loading before the first Present, in headless harnesses (`compile_mt_test`), and on present paths that never query it.
  - So "paused" means `g_vis_observing && !(g_vis_flags & VISIBLE)`.
  - Gate: harness runs don't take the pause path unless asked.
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
   - R2: a harness run (no visibility query) doesn't take the pause path.
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
