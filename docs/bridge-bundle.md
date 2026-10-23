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
| **C-4** unlinked parts | verified: after the file and its sum are unlinked, every lookup in the loaded `MTL4Archive` still hits and renders the same pixels (plain and TSan). If MTL4Archive reopened the URL, it would fail on the missing path whatever the page cache holds. **Not covered then:** a *compiler* with the archive in lookupArchives whose first compile comes after the unlink throws (found in part 6; fixed by converting at load: see "A crash found in the shared stress test"). | `archfirst_test check` item 6; `unlink_lookup_test` |
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

Notes from the milestone 6 review (no code):
- **Who takes the flat directory:** the first executable that loads d9mt after the update. If that isn't `swtor.exe`, SWTOR starts PSO-cold once; its archives then live in the other executable's directory until that one's retention drops them.
- **Case:** the directory name is lowercased, the state cache file name isn't. On a case-sensitive volume, `SWTOR.EXE` and `swtor.exe` share one PSO directory but keep two state files, so their coverage signals and consolidations churn each other's archives. The state file name stays as it is: lowercasing it would make every existing state file cold.
- **Skipped** also covers "function gone" (the shader was released before its prewarm job ran). A game that releases and recreates its shaders would keep skipping, signal complete, and consolidate away what those entries' PSOs were: churn. SWTOR keeps its shaders (0 skipped in Jan's logs).
- **The argv check under Wine is a batch gate:** if the init line doesn't show `<base>/<name>.exe/os-…`, the fallback has silently turned parts 3+4 off; the dynamic batch fails on that.

**Known, open: failed builds shadow archived vertex functions for the direct lookup.** `MTL4Archive newRenderPipelineStateWithDescriptor:` composes a pipeline from per-function binaries, keyed by function and a key that depends on the pipeline. A failed build through the serializer-attached compiler (here: VS/PS interface mismatches, which the harness makes by pairing every VS with every PS) leaves something in the serializer that hides the good pipelines' variant of the same vertex function from the direct lookup ("Failed to find vertex function main with key"). The compiler's `lookupArchives` still serves them, possibly with a real VS compile.
- **Measured** (compile_mt_test, vs_mvp with pixel shaders, warm after one clean flush):
  - the 11 good pixel shaders alone: 44/44 direct hits;
  - plus one failing pixel shader: 0-1 good pairs miss (tex3d, texcoords, texldd: 1; the rest: 0);
  - plus all 12 failing ones: all 44 good pairs miss.
  - The misses repeat every session: each session writes a new ~420 KB delta of the same pipelines, and the archive captured from exactly those builds doesn't serve them either.
  - Full harness set: 884 of 928 good pipelines hit directly.
- **Detecting "served by lookupArchives, no compile" isn't possible in Metal 4:**
  - `MTL4CompilerTaskOptions` has only `lookupArchives`, and `MTL4PipelineOptions` has only validation/reflection; `MTLPipelineOptionFailOnBinaryArchiveMiss` is Metal 3 only;
  - a second lookup is the direct lookup that misses;
  - a compile-time threshold would be a guess.
  - So consolidation can't count them as archived, and they're captured again.
- **SWTOR:** d9mt links pixel shaders to the vertex shader's outputs, and Jan's sessions show prewarm `failed=0` and no "pipeline build failed" lines. So this doesn't apply today.
- **If a game shows repeated archive-first misses across sessions** (`D9MT_PSO_STATS=1`: misses in session 2+ beyond its new PSOs), a candidate fix is to build descriptors whose last build failed (the state cache knows them) through the serializer-less compiler.

Known limit: a partly corrupt state cache file blocks `complete` for that session only. The next session's file has lost those entries, and its consolidation drops their PSOs: a recompile on next use, as for an evicted key.

**Future, not in the bundle: per-part coverage, for games with per-zone shaders.** Today `complete` is all or nothing: one entry whose shaders weren't announced (content this session didn't load) means the prewarm never drains its whole index, no signal is sent, and nothing consolidates. SWTOR doesn't hit this: Jan's logs show prewarm built = prewarmable (318/330/344) with 0 skipped. A game that loads shaders per zone would never consolidate and keep accumulating deltas up to `D9MT_PSO_MAX_ARCHIVES`/`D9MT_PSO_MAX_MB`. The sketch: the state cache reports coverage per delta (the deltas whose recorded pipelines were all replayed) and the consolidation deletes only those, keeping the rest as deltas. Open question: does SWGL Legends load shaders per zone? Check its prewarm lines (built vs prewarmable, skipped) before building this.

### 5. Flush at pauses, never in a build's way: implemented

**As implemented:**
- `pso_note_compiled` no longer flushes. It stamps the last build and wakes the `d9mt-pso-flush` thread for the first pending PSO and when `D9MT_PSO_FLUSH_EVERY` are pending (the backstop's clock starts then).
- **The thread (UTILITY QoS) flushes whatever is pending:**
  - on the Reset hint;
  - while the window is known not visible (`pso_paused`, R2) and no build came for 500 ms, so a hidden window that compiles in bursts writes one delta per burst, not per PSO;
  - after `D9MT_PSO_FLUSH_IDLE_MS` (10 s) without a build;
  - or once `FLUSH_EVERY` have waited `D9MT_PSO_FLUSH_BACKSTOP_MS` (60 s).
  - While anything is pending it looks again every 250 ms; otherwise it sleeps until woken (at most 1 s).
  - "Not visible" comes from the presenter's `isVisible()`, which the CS thread calls on every present (`d3d9_swapchain.cpp:829`) whatever `D9MT_BG_FPS` is, so the pause trigger doesn't depend on the background throttle. In batch-dyn2 the vd window was occluded only for its first 20 ms (0x2000, then 0x2002), so that run had no pause, and no pause flush was correct.
- **The lock:** the thread takes `g_pso_lock` with `trywrlock`, retried every 50 ms, so a waiting flush never queues ahead of builds.
- **M9:** a build that finds the lock held (`tryrdlock` fails) goes to `g_pso_fallback`, a serializer-less `MTL4Compiler` of the same device with the same task options (`lookupArchives`). It is not captured and not counted as pending; it misses again next session and is captured then.
- **Reset hint:** `D9MTPsoPauseHint` (unix call 214, appended to both tables after 213). `D3D9DeviceEx::Reset`/`ResetEx` call it through `MtlDevice::psoPauseHint`, which looks it up with `GetProcAddress` like the coverage signal, so an older bridge means no hint and an older d3d9.dll means no call. `scripts/check-abi-mix.sh` covers both entries and that the tables end with 213, 214.
- **Nothing at exit, nothing at device destruction:** the atexit flush is gone. `D9MT_PSO_STATS=1` (tests) prints the hit/miss/uncaptured counts at exit, which is the only atexit work, and only then.
- **Cost:**
  - PSOs built in the last 10 s before a quit, with no pause after them, aren't saved; they miss again next session.
  - A long session with sporadic misses writes a small delta after each quiet 10 s. That means more files, up to `D9MT_PSO_MAX_ARCHIVES`, and the next consolidation merges them.

**Tests** (`test-pso-archive-first.sh` part 5, plain and TSan):
- **idle:** a delta before exit; the next run has 0 misses.
- **no flush at exit:** with the idle out of reach, the run leaves no delta.
- **Reset hint:** `MT_HINT=1`, unix call 214 → a delta without the idle.
- **backstop** 300 ms → a delta without the idle.
- **M9:** a flush held 3 s inside the write lock (`D9MT_PSO_FLUSH_HOLD_MS`, test hook) with 16 builds during it. All succeed, all 16 through the serializer-less compiler ("not captured: 16"), and the next run misses exactly those 16.
- The harness now waits `MT_SETTLE_MS` (700) before exit, with `D9MT_PSO_FLUSH_IDLE_MS=200`.
- The quit test (shared) exits while the flush thread or a consolidation may run.
- **S-1** (whole-bundle review): a captured build is counted before `g_pso_lock` is released. Counted after it, a flush could serialize the build in between and reset the count; the late count then made the next flush write a drained serializer ("flush failed", and fragmented deltas taking archive-first slots). Test: `D9MT_PSO_NOTE_DELAY_MS` (test hook) holds every other count 600 ms. The old order gives 1 failed flush and 3 deltas; counting before the unlock gives 0 failures and 1 delta.

**Under Wine** (results/dyn2-summary.txt, dyn3-pause.txt):
- **Quit tests:** quitting during a consolidation or during a held flush, and exiting without an idle flush, all give rc=0, no hang and a clean next start.
- **Pause flush while the window isn't visible:** proven. repro/pauseflush.c on the vd, in a locked session where every window reads as occluded (`occlusionState` 0x2000). With the idle flush out of reach, exactly one delta was written ≈500 ms after the build burst, and the next start had 135 hits. Minimise is seen (0x2 → 0x6 → 0x2).
- **The transition visible → hidden:** still open. It needs an unlocked session (the same batch, ≈2 min).

**The design, as reviewed:**

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

### Sampler LOD bias, the bridge side (review-014/sampler-lod-bias-design.md M-1)

- **`WMTSamplerInfo`:** `uint8_t lod_bias_present` at offset 23 (the former padding byte), `gpu_resource_id` stays at 24, `float lod_bias` at 32, then `uint32_t padding`; 40 bytes (STATIC_ASSERT). The header hunk is byte-identical to `fix/sampler-lod-bias` 131f28a, so the two branches merge without a conflict.
- **`_MTLDevice_newSamplerState`:** `if (info->lod_bias_present) sampler_desc.lodBias = info->lod_bias;`.
  - No copy-in: the unix side dereferences the caller's struct in place (`params->info.ptr`, as before), so the only bytes it reads past offset 31 are behind the flag.
  - An older d3d9.dll zeroes its 32-byte struct (`WMTSamplerInfo samplerInfo = {}`, the only driver caller; `bridge_smoke` too), so it sends 0 at 23 and the bridge never reads past it.
  - A new d3d9.dll next to an older bridge: the old bridge ignores the flag and the bias; `gpu_resource_id` is at 24 for both.
  - The apitrace sampler record carries `lod_bias` (0 without the flag).
- **The PE side** (createSampler sets the flag and the quantised bias; the key's quantisation; the fallback to bias 0) is `fix/sampler-lod-bias`'s.
- **`scripts/check-abi-mix.sh`:**
  - the layout (23/24/32/40) compiled for the unix side, mingw x86_64 and i686;
  - every read of `info->lod_bias` behind the flag (an awk check; removing the `if` makes it fail);
  - no `WMTSamplerInfo` caller in driver/ or bridge/tests without `= {}`.
- **Game-gate list:** `code_lodbias` (T-2/T-4/T-5) under Wine:
  - with `fix/sampler-lod-bias`'s d3d9.dll and this bridge;
  - the skew cases: that d3d9.dll with the shipping bridge, and base d3d9.dll with this bridge (bias ignored, no crash);
  - the hash suite with base d3d9.dll and this bridge (no stray bias).

### 6. Proposal: consolidation in the startup window, complete chunked sets (after the quiet window, 2026-10-21; revised after review)

**What the quiet window and batch-rss showed** (n013-issues/results/qw-psoab.txt, rss-summary.txt, psobench 350 4 5000 on a 9-session seed):
- **New-PSO phase:** the bundle is far ahead of base: worst frame 1501 → 418 ms, −2.45 s, 0/6 worse.
- **Steady phase:** avg +0.001 ms in 4/6 pairs, with ~5% fewer frames in 3. In every bundle run a **consolidation** ran inside it, starting 12.0-12.4 s (coverage at 9.0-10.7 s, then the 2 s idle): it rebuilds all 945 descriptors and serializes ~70 MB. Besides it, the flush of the session's 143 misses (11 MB, serialize 165-493 ms + sync/sum ~60 ms).
- **No consolidation completed:** psobench exits ~1.5 s after the consolidation starts, and it needs ≈2.7 s (af0's flush of 945 PSOs: serialize 2326 ms + sync/sum 341 ms). The cut-off consolidation leaves Metal's intermediate `.metallib` (removed at the next start since 64b28e1).
- **Memory: a transient peak, not a per-PSO cost.**
  - The footprint at 11 s, before the consolidation, is at base level: 327-433 MB vs 394 MB.
  - max RSS 561 MB comes during the consolidation, whose serializer holds the whole working set again plus serialization buffers, ≈ 2-3× the archive size.
  - With archive first off (af0), the one 945-PSO flush peaks at 489 MB for the same reason.
  - The floor without archives is 234 MB.
  - GPU memory for the pipelines (IOAccelerator) is 151-153 MB in every bundle variant (archive first on or off) vs 92 MB on base: a separate, smaller question (archfirst_test mem).

**Proposal (revised after review-014/bridge-bundle-part6.md):**

Jan's logs: SWTOR was "not visible" in 0 of 4 sessions, and ResetSwapChain only runs at device setup, so a pause-only trigger would hardly ever fire.

1. **Primary trigger: the startup window (S-1).**
   - Consolidate at the coverage signal if it arrives within the first ~30 s of the process.
   - In Jan's sessions coverage arrives at 1.6-8 s and world entry at 21-27 s; ~350 PSOs ≈ 1 s of rebuild. So the consolidation and its peak fall into login / character select, not gameplay.
   - Order: startup window → real pause (hidden or minimised, device Reset) → chunked fallback (3).
   - No 120 s idle trigger in gameplay any more.
2. **A chunked set only counts once complete (M-1).**
   - Chunks are written as `c-<time>-<pid>-<n>.mtl4archive` (each with its sum), then a marker **written last**: `c-<time>-<pid>.set` with the chunk count and each chunk's v2 sum, fsynced before its rename.
   - Only a set with its marker exists for the startup rules: it anchors the delta rule (deltas older than it are dropped) and all its chunks are kept and looked up first.
   - A set without a marker (an exit between chunks) is ignored: it anchors nothing, its chunks are removed by the young-file rule, and the parts it would have replaced stay. Nothing is lost.
   - The parts are deleted only after the marker is in place.
   - A single-chunk consolidation is the same with n = 1, so there is one format.
   - Test (native, compile_mt_test): exit between chunks (a hold hook after chunk k) → the next start uses the old archives, 0 misses, nothing dropped; a complete set → its chunks loaded, the parts gone.
3. **Fallback for sessions with neither:** no completed consolidation for 8 sessions and the deltas over K files or X MB → chunked (≤256 descriptors per serializer) after 120 s idle at BACKGROUND QoS. The session count since the last completed set goes in a small file beside the archives.
4. **Skip when it isn't worth it** (kept): nothing new and at most one archive; and at most N small deltas.
5. **Chunked capture flushes (point 4 of the review), with conditions:**
   - At ≥ `D9MT_PSO_FLUSH_EVERY` (256) pending, flush only after a **quiet gap** of ≥500 ms without a build: the end of a burst, not inside it.
   - The cold-session numbers report `g_pso_uncaptured` (builds that went to the fallback compiler during these flushes, captured only next time).
   - Gate: psoab with a 256-flush inside the steady phase, 0/6 worse.
6. **Before shipping: GPU memory.**
   - IOAccelerator (graphics) is 151-153 MB in every bundle variant, archive first on or off, vs 92 MB on base.
   - archfirst_test `attr` attributes it step by step: a serializer-less compiler, the serializer-attached one, the fallback compiler, 0/1/8 archives loaded, 800 PSOs, a consolidation compiler and serializer; footprint and IOAccelerator after each step.
   - If it stays unexplained, it's a regression for 8 GB Macs and blocks the bundle.
7. **Numbers before code** (native, archfirst_test):
   - `mem direct|compiler|probe`: live PSO memory by path;
   - `peak 0|256|128`: a consolidation's peak footprint and time by chunk size;
   - `archload`: the startup cost per extra consolidated archive;
   - `attr`: point 6.
   - Then the psoab re-run in a quiet window with the changes.

#### 6 as implemented (0bc955b) and its numbers

**Implemented as proposed (points 1-5), with two changes the tests forced:**
- The consolidation thread is woken by the coverage signal: a condition variable instead of a 1 s sleep. Before, the startup-window consolidation started up to 1 s late.
- Before its snapshot, the consolidation flushes the session's own captures through the flush thread (the Reset-hint path, waited for up to 5 s). At the coverage signal this session's misses are still in the game's serializer. Without that flush the set left them out, and a cold session wrote no set at all.
- Fixed on the way (part 5): a wake of the flush thread that came between its look at the pending count and its wait was lost, so the first pending PSO could wait up to 1 s. The wait is now decided under the mutex the wake signals with.

**Native numbers** (archfirst_test, n013-issues/results/native-mem.txt, native-mem2.txt; 800 PSOs with a large fragment function, each mode in its own process, two runs):

| question | result |
|---|---|
| live PSO memory by path (`mem`) | MTL4Archive direct 105.7 KB/PSO, compiler + lookupArchives 107.5, probe + compiler 106.9. With a large vertex function too: 106.0 vs 108.1. Archive first costs nothing extra. |
| where GPU memory comes from (`attr`) | Compilers (game, fallback, consolidation) ±0, archives (1, 8) ±0 GPU (≈0.8 MB footprint each); live PSOs ≈ 80-95 KB each, by either path. |
| a consolidation's peak (`peak`), above its start | whole set +185 / +256 MB; chunks of 256 +139 to +151; chunks of 128 +131 to +139. A fresh compiler per chunk doesn't lower it (+130 to +154) and costs 0.2-0.4 ms in all. The floor (≈ 2× the archive) is the binaries the lookups load, not the compiler. **Chunks of 256, one compiler.** |
| startup cost per extra consolidated archive (`archload`) | open 2.8 ms + one lookup 1.9 ms |

**The GPU-memory gate (point 6): resolved.**
- `footprint --sample 1` over whole psobench runs (n013-issues/results/ioacc2-summary.txt).
- Maximum dirty IOAccelerator (graphics): base d3d9 + shipping bridge 135-144 MB; every combination with a bundle component 151 MB, including bundle d3d9 + the shipping bridge.
- The earlier 92 vs 151 MB came from single 11 s snapshots taken in different phases of the runs. No bridge-specific regression.

**The total peak:**
- Maximum footprint: base + shipping 454-465 MB; bundle d3d9 + shipping 469-470; the bundle bridge with archive first off 603-604.
- The +140 MB with archive first off: that mode captures every build, like base. Base flushes inline every 256 (a bounded serializer, at the cost of the old stalls). The bundle flushes only at a quiet gap, an idle or a pause, so a continuous build burst (psobench's new phase) accumulates every capture.
- With archive first on only misses are captured, except in a cold first session, where everything misses. The cold-session numbers with the quiet-gap rule are the next measurement.

**Cold first session vs base** (n013-issues/results/cold-summary-2.txt; psobench 350 4 5000 from an empty cache, then the warm next start; `footprint --sample 1` on the Wine process, maximum over the run; two runs each):

| | base (shipping bridge, base d3d9) | bundle 0bc955b | bundle − base |
|---|---|---|---|
| cold first session, max footprint | 476 / 471 MB | 648 / 643 MB | **+172 MB** |
| warm next start, max footprint | 452 / 450 MB | 424 / 424 MB | −27 MB |
| max IOAccelerator (cold and warm) | 151 MB | 147-151 MB | ≈ 0 |
| warm next start, misses | – | 1 / 1 (of 946) | |
| cold: flushes / uncaptured | inline every 256 | one of 945 PSOs (72 MB; serialize 2.2-2.3 s, sync + sum 0.35 s) / 0 | |
| worst frame (information, not a timing window) | cold 424-433 ms, warm 471-479 ms (the inline flushes) | cold 4.2-4.3 ms, warm 227-228 ms (the first frame, see below) | |

- Base holds at most 256 captures before its inline flush. The bundle holds a cold session's whole capture until the quiet gap after the burst.
- That is the +172 MB, once, in the first session after the update; the warm sessions after it are 27 MB below base.
- Release notes: "the first start after the update uses up to ~170 MB more memory for a moment while it saves the shader pipelines".
- Double-buffered capture caps it (the first item for 0.1.5).

**The warm start's first frame** (review M-a: a bundle warm run's worst frame 230 ms against 8.8 ms in another):
- It is the first frame of psobench's new phase: 226-228 ms in 4 of 8 warm bundle runs, none in the other 4.
- With the state cache's prewarm off it still occurs (1 of 2), so it isn't the prewarm.
- No consolidation runs then (the coverage signal is incomplete before 8 sessions), so there is no consolidation and no consolidation flush in it.
- Base warm starts have a first frame of 215 ms (and 410-660 ms on later starts, with its inline flushes).
- So it's a warm-start cost both have, not one the bundle adds. Most likely the first lookup in a large archive, depending on the page cache; not measured.
- For psoab, the bundle's startup-window consolidation would fall into psobench's measured phases, because psobench has no menu. `qw-bundle.sh psoab` therefore runs psobench with an 8 s menu phase first (`menu`, the default; frames with one already-built PSO, not measured, where the consolidation runs as in login or character select), or without a menu and with the startup window off (`nostartup`). Both are reported.

**A crash found in the shared stress test (2 of 20 processes aborted), fixed before any measurement went further.**
- **What Metal does** (bridge/tests/unlink_lookup_test.m): `[MTL4Compiler newRenderPipelineState…compilerTaskOptions:]` converts the options' lookupArchives at that compiler's first compile with them. If an archive's file was deleted before that, every compile with it throws (NSInvalidArgumentException in convertBinaryArchives: "object cannot be nil"). An uncaught exception ends the process. After one compile with the archive, a deletion is harmless: the conversion is cached on the MTL4Archive, for every later compiler and options object.
- **C-4** checked only direct `MTL4Archive` lookups after an unlink, which are fine.
- **How it would hit a player:**
  - In a warm session every PSO is an archive-first hit, so the game's compiler never compiles with its lookupArchives.
  - The consolidation's own rebuild compiles with the archives it looks up (the consolidated set, the newest 8 deltas, the session's), which converts those; they are the same MTL4Archive objects.
  - With more than 8 deltas, the older ones are only the game compiler's lookupArchives. The consolidation deletes them with the rest, and the first miss after it (a world entry) ends the game.
  - Another process deleting old deltas does the same to every archive (the stress test).
  - Part 4 had it latently; part 6 consolidates early and made it likely.
- **Fix:**
  - **pso_convert_lookups:** right after loading, on a UTILITY queue, one tiny pipeline (a few lines of MSL) is compiled through a serializer-less compiler with every loaded archive as lookupArchives, which makes Metal convert them while the files exist. A consolidation deletes nothing before that has finished (it waits up to 5 s).
  - **pso_compile:** every compile with lookupArchives (the thunk, the M9 fallback, the consolidation) catches that exception. It logs one line, uses no lookupArchives for the rest of the session and builds again. A consolidation that meets it stops and keeps the parts.
  - **The catch is a last resort.** Metal's Objective-C code isn't guaranteed to be exception-safe: an exception thrown through it can leave locks held or objects half-built. What has to hold is the conversion; the catch only keeps a missed case from ending the game.
  - **Not converted within 5 s** (a loaded system, a slow first compile): the consolidation skips this session ("no consolidation this session (the loaded archives not converted within 5 s)"). It deletes nothing, and the next session tries again. The 5 s are a clock deadline: the test found that 100 sleeps of 50 ms on the BACKGROUND consolidation thread took over 6 s (macOS stretches timers at that QoS). The wait for the session's own flush uses the same deadline.
  - **`.verify`:** if the session's first lookup failure finds every loaded archive's file still there, the cause isn't a deletion. Whichever compile meets that failure first produces the marker: the game's miss, the fallback, the conversion or the consolidation (`pso_lookup_failed`, off the compiling thread). Review fix: the first version only checked the conversion's own false→true transition, so a game compile failing first left no marker. A damaged archive whose size and time match its `.sum` would pass the startup shape check. The bridge writes `<dir>/.verify`, and the next start checks every archive's SHA-256 once (like `D9MT_PSO_VERIFY=1`), removes the bad ones and then the marker.
- **Tests:**
  - unlink_lookup_test `unused` throws (Metal's behaviour), `warmup` doesn't (the fix);
  - first.sh part 6: 11 deltas, a warm start with only hits (the compiler unused), whose startup-window consolidation replaces all 11, then 16 new PSOs. No crash, all built, no fallback, converted at load.
  - **Mutations:** without both layers, that run aborts with exactly the convertBinaryArchives exception. With the catch alone, it survives: one "file is gone" line, all built without lookupArchives.
  - The shared stress three times: 0 crashes. Before the fix: 2 of 20 processes aborted.
  - first.sh part 6, the review's shoulds:
    - the conversion held 6 s (`D9MT_PSO_CONVERT_DELAY_MS`, tests only): no consolidation that session, no set, the deltas kept;
    - `.verify` beside a delta damaged with its size and time kept: the SHA-256 check runs once, removes that delta and keeps the others, and the marker is gone.
    - the producer: the game's miss fails first (`D9MT_PSO_TEST_LOOKUP_THROW`, the conversion held 2 s), every file there → the marker, consumed at the next start;
    - a loaded file deleted (`D9MT_PSO_TEST_UNLINK_LOADED`) → the same failure, no marker.

**Tried and dropped: flushes inside a burst at a minimum interval.**
- A burst flush has to wait for the compiles in flight before it can take the write lock (it starves otherwise; def1's flush waited 7 s).
- While it waited, it sent new builds past the lock to the fallback compiler.
- Measured on a cold run of all 1700 harness PSOs with six compile threads (FLUSH_EVERY 100, minimum interval 200 ms): one burst flush, 826 of ~930 good builds uncaptured, and the warm run missed 1604 (772 that never link + 832).
- Waiting for in-flight compiles inside a parallel burst costs most of the capture. Not kept.

**Proposal (not built; the reviewer decides): double-buffered capture.**
- Two serializer-attached compilers A and B, each with its own lock. PSO builds use the active one; the PE's compiler handle isn't needed for that (the bridge made it).
- A flush flips the active compiler (an atomic) and then waits only for the builds still in flight on the old one. New builds already go to the other compiler and **are captured**.
- It flushes the old one, and its serializer is empty for the next flip.
- **What it buys:**
  - no build ever waits or goes uncaptured for a flush;
  - the serializer is bounded per buffer;
  - FLUSH_EVERY flushes are possible inside bursts, without the quiet-gap condition.
  - The fallback compiler (M9) stays only as a safety net.
- **Cost:** one more serializer-attached compiler (≈0 MB without content: `attr`). The code is about part 5's size.
- **Gate:** the cold-session peak, uncaptured = 0, and psoab's new phase.

### 7. Planned: MTL4 commit feedback errors as a log line (after #11's psoab gate)

**Why.** Jan's Cartel Market freeze on 2026-10-21 was a real GPU hang, followed by the WindowServer watchdog and a logout (field-reports/20261021-jan-freeze/ANALYSIS.md). Nothing in wine.log could tell a timeout from a page fault. The unified log only had `IOGPUMetalError: <private>`, so the hang was inferred from timing. Metal 4 gives the cause to whoever asks. The proposal is field-reports/20261021-jan-freeze/BRIDGE-COMMIT-FEEDBACK.md.

**The API, checked against the macOS 26 SDK** (MTL4CommandQueue.h, MTL4CommitFeedback.h, MTLCommandBuffer.h):
- `[MTL4CommandQueue commit:count:options:]` takes an `MTL4CommitOptions`, and `addFeedbackHandler:` registers a `^(id<MTL4CommitFeedback>)` block.
- The feedback has `error` (`MTLCommandBufferErrorDomain`: Internal 1, **Timeout 2, PageFault 3**, AccessRevoked 4, NotPermitted 7, OutOfMemory 8, **InvalidResource 9**) and `GPUStartTime` / `GPUEndTime`.
- `MTL4CommitOptions` is "not thread-safe if modified from several threads". The bridge builds one, adds the handler once under `dispatch_once`, and only passes it from then on.

**Design (unix side only):**
- `_MTL4CommandQueue_commit` passes the long-lived options to `commit:count:options:`.
- The block returns at once when `error == nil`. On an error it writes, in an `@autoreleasepool`: `D9MT-GPU-ERROR: <name> (<code>) <localizedDescription> gpu=<start>-<end> commits=<n>`.
  - The first 20 errors get a line each, then one count line per 10 s. The counters are atomics; no lock, no allocation outside the error path.
  - After a hang the dead channel rejects every later commit (one every 10 s in Jan's log), so the rate limit matters.
- **Not the commit's number:** one shared options object means one block for every commit, and the feedback carries no commit identity. A per-commit options object or block would allocate on every commit. The line therefore carries the GPU times and the count of commits submitted so far (an atomic counter that is always on), which places the error in time next to the SubmissionTracker / HITCH lines.
- In the same change, cache `getenv("D9MT_WSILOG")` in a static. Today it runs on every commit.
- **Dropped from the proposal:** `MTLCommandBufferErrorOptionEncoderExecutionStatus` is an `MTLCommandBufferDescriptor` option (Metal 3). `MTL4CommandBufferOptions` has only `logState`, so it doesn't exist for d9mt's MTL4 command buffers.
- **Later, d9mt side:** a unixcall for the last error code, so that `D9MT-GPU-HUNG` / `D9MT-GPU-LOST` name the cause. That is an ABI addition (a new call number, check-abi-mix, the skew case of a new d3d9 on an old bridge), so it is a separate step.

**Cost per commit.** Our block costs nothing when there's no error. The real cost is Metal's: with a feedback handler, Metal has to deliver a completion callback for every commit, on its own thread. Without one, an MTL4 commit probably needs no host-side completion at all, since d9mt tracks completion with its shared events. This is unverified, and the A/B decides. At ~60–600 commits/s that is that many callbacks a second, off the render thread but not free under Rosetta.
- **Gate (hard rule):** an env A/B (`D9MT_COMMIT_FEEDBACK=0/1`) in a quiet window on bindbench and bench_4k, ABBA, nothing measurably slower (the noise floor is ±1.5 %). Also the CPU time of Metal's completion thread from a sample.
- If it measures worse, it ships off by default and on under `D9MT_WSILOG` / Collect Logs.

**ABI and LC_UUID.**
- PE side: no change. No new unixcall, and winemetal.dll (32/64) stays byte-identical. check-abi-mix is unaffected.
- winemetal.so gets a new LC_UUID; that is the next engine/bridge drop. The AIR cache is keyed on the codegen identity (part 1, S1), which doesn't include winemetal_unix.c, so every user's AIR cache stays warm.
- PSO archives (`os-<leaf>`, OS build + GPU) and the state cache don't depend on the bridge build, so no cold start.

**Tests:**
- unix-side unit test: a forced GPU timeout (an endless-loop compute kernel) or, if it can be made reliable natively, a page fault (a read through the GPU address of a released buffer) on an MTL4 queue. The test checks one `D9MT-GPU-ERROR: Timeout (2)` / `PageFault (3)` line and the rate limit (50 failing commits → 20 lines + a count).
- No-error path: the hash suite stays identical and no line appears.
- TSan on the handler counters.

### 8. The archive index: a miss costs a hash probe, not a walk (after A3, 2026-10-22)

**Why (measured).** A3 failed with the new phase +5.45 ms (6/6, display asleep). The cause batch (n013-issues/results/a3-cause-static.md) crossed d3d9 × bridge:
- bundle bridge with base d3d9: +6.60 (6/6), with no consolidation at all (no coverage signal);
- bundle d3d9 with the shipping bridge: +0.35 (4/6);
- menunowin: +4.85 (6/6).

So it is the bridge, and not the consolidation.

Native costs (tools/hitpos.m, one process per measurement):
- each archive asked costs ~32 µs;
- a hit at position 0 costs ~70 µs, the same as the compiler path with one archive;
- compiler + lookupArchives costs 69/114/137/224 µs for 1/3/4/8 archives.

The new phase's bridge traffic is its 143 misses, each paying the 2-delta walk (~65 µs) before the compiler searches the same archives again. That is ≈ 9 ms of CPU against the observed +5–7 ms.

**Design (unix side only, no ABI change):**
- **Key:** every value `_MTL4Compiler_newRenderPipelineState` sets on the descriptor, as set: the 8 color attachments after `to_metal_pixel_format`, the vertex descriptor with the same layout-skip rule, the sample count (0 → 1), alpha-to-coverage, rasterization and topology. Each library counts as 16 bytes of the SHA-256 of its metallib bytes, taken once when `_MTLDevice_newLibrary` creates it (an associated object), and each entry point by its name.
  - The hash is FNV-1a 64 over field-wise values, never raw struct bytes (the info struct has padding).
  - A library without an identity gives no key: that descriptor is walked as before, and an archive holding one gets no index.
- **`.idx` per archive:** "D9IX", version 1, count, the archive's size and SHA-256 (as in its `.sum`), the sorted keys, and an FNV-1a checksum.
  - Deltas: the keys of the epoch's captured builds (noted before the unlock, like `pso_note_compiled`), written after the archive and its sum are in place.
  - Chunks: the keys of the descriptors built into each chunk.
  - Removed with its archive (`pso_remove_archive`). A stray `.idx` without an archive goes like a stray `.sum`.
- **At load:** the loaded archives' indexes merge into one sorted key → archive table (binary search; the first-loaded archive wins: the newest set, then the newest deltas). The walk list becomes `g_pso_lookup` minus the indexed archives. Older deltas past the walk's cap are indexed too, so they hit directly.
- **A wrong key never gives a wrong PSO:** Metal still looks the full descriptor up in the archive the index names.
  - **False hit:** the named archive returns nil → walk/compile as before.
  - **False miss:** the index has no key but an archive holds the PSO → the compiler builds it, captured once more. Every 64th index miss (every one with `D9MT_PSO_IDX_CHECK=1`) also asks the indexed archives and counts it.
  - Both appear in the exit line `PSO archive index: N hits, F false hits, M false misses in C checked` (with `D9MT_PSO_STATS=1`).
- **Compatibility:**
  - Archives without an index (written before part 8) are walked as before.
  - A damaged or mismatched index (magic, version, size, checksum, or the archive's size/SHA-256 not matching its `.sum`) is removed, its archive walked, and `.verify` checks every archive's SHA-256 at the next start.
  - A missing index can't be told apart from an archive older than part 8, so it is walked without `.verify`.
  - `D9MT_PSO_INDEX=0` turns the whole part off: no library hash, no key, no key bookkeeping at capture, no index read or written (review M: the legacy-cost control).
  - An older bridge ignores `.idx` files (and its stray rules don't match them); after a downgrade and upgrade they still match their archives by size and SHA-256.

**Tests** (first.sh "part 8", plain and TSan):
- an index written with the delta, its key count = the flushed builds;
- a warm start's hits all through the index (keys match across processes);
- new PSOs with every index miss checked: 0 false misses;
- the hooks `D9MT_PSO_TEST_IDX=2` (keys dropped: every hit a detected false miss) and `=1` (keys shifted to the other archive: false hits, built instead, correct);
- archives without an index walked;
- a damaged and a mismatched index both removed, walked, `.verify` written;
- a stray index removed;
- a consolidated set indexed per chunk, the next start all through it, the replaced parts' indexes gone;
- `D9MT_PSO_INDEX=0` writes none.

The cap test now runs the walk's cap with `D9MT_PSO_INDEX=0`; with the index the old delta's PSOs hit.

**The steady phase's +0.10–0.15 ms worst frame** (A3 5/6, menunowin 5/6) is part 5's idle flush of the new phase's misses. psoab sets `D9MT_PSO_FLUSH_IDLE_MS=1000` to land it in the steady phase on purpose; in the game the idle is 10 s. It runs at UTILITY QoS: serialize (~0.3 s for 143 PSOs), fsync, SHA-256. Part 8 adds no flush work beyond the small `.idx`.
- `D9MT_PSO_FLUSH_QOS=background` (opt-in, default unchanged) runs the flush thread at BACKGROUND, for an A/B.
- psoab's variants `bq` (bundle + that) and `bn` (bundle without the idle flush in the steady phase: its cost's control) go into the A3 re-run.

**Gate:** A3 again (display asleep, psobench-menu2, x4 incl. xb, plus bq/bn): the new phase not measurably slower, steady 0/6 worse.

### 9. A validation session (2026-10-24)

**9a, archives off under Metal shader validation (implemented).**
- a48a1119's shader-validation check of the game crashed in the PSO thunk (unix call 196, an access violation) and ran with `D9MT_PSO_ARCHIVE=0`.
- Natively, reproduced by this part's mutation: the bridge without the gate **loads** the existing archives under `MTL_SHADER_VALIDATION=1` ("1 loaded") and then **crashes (SIGSEGV)** when it asks them, the same access violation as in Wine. Apple documents shader validation as incompatible with binary archives (research/metal4-validation.md).
- An archive **written** under validation didn't load in the same session (archfirst_test `check`: its freshly serialized archive, newArchiveWithURL nil). So a validation session's captures would be deltas of unknown use for the next normal start, and instrumented binaries mustn't reach the shared archives.
- So with `MTL_SHADER_VALIDATION` set (and not "0"), `pso_archive_init` returns as with `D9MT_PSO_ARCHIVE=0`: nothing loaded, asked, captured or consolidated. One line: `d9mt: PSO archives off: Metal shader validation is on (…); archives neither asked nor written`. Without `MTL_SHADER_VALIDATION_REPORT_TO_STDERR`, a second line says to set it: Metal 4 reports validation errors only on stderr with that switch (the probe's round 2).
- Part 8's library hash and key don't run then (no archive first, no serializer).
- The debug layer (`MTL_DEBUG_LAYER=1`) loads and uses archives fine (archfirst check ok), so no gate for it.
- Test (first.sh part 9a): a seeded directory, then a run under `MTL_SHADER_VALIDATION=1` → the line, nothing loaded, no delta, no crash. The next normal start: 0 misses.
- Mutation (the gate off): the validation run crashes (signal 11), its line is missing and the archive is loaded: 3 failures.

**9b, labels for the validation report (implemented).** The probe (n013-issues/scripts/qw-valprobe.sh, three rounds, research/metal4-validation.md):
- **Metal 4 reports a fault only on stderr**, and only with `MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1`. There's no API: the commit feedback, an `MTLLogState` handler and the default unified log all stay silent.
- It checks **per buffer** (64 bytes past a 1000-byte buffer, inside its page, is caught) for argument-table bindings too.
- The default zerofill keeps the process running. `RETURN_BADFOOD_ON_FAULT` and `ABORT_ON_FAULT` end it.
- A report names the buffer (label, length, GPU address), the pipeline's label and UID, the encoder's label, the command, and where the buffer was allocated. d9mt names every AIR shader "main", so without labels a report can't say which pipeline or pass.
- Under `MTL_SHADER_VALIDATION` only, the bridge now labels:
  - **pipelines** `d9mt vs:<libid>/<entry> fs:<libid>/<entry>`: libid is the first 8 hex digits of the library's identity from part 8 (the SHA-256 of its metallib bytes), now taken under validation too;
  - **encoders** `d9mt enc#<n> render <rt0 pixel format> <w>x<h>` or `d9mt enc#<n> compute`.
- No buffer labels: their meaning (which object's VB/IB/constants) is on the PE side, which would be an ABI change.
- Off: one read-once bool per pipeline and encoder creation (measured: the 9b off A/B).
- Test (scripts/test-validation-labels.sh, native, one render pass through the bridge's thunks):
  - off: no report;
  - in bounds under validation: no report;
  - out of bounds under validation: one report naming `d9mt vs:…/vmain fs:…/fmain` and `d9mt enc#0 render 80 4x4`, zero-filled, the process goes on.
  - The out-of-bounds read only ever runs under validation; the GPU-alarm check runs after each case.
  - Mutation (labels never applied): 2 failures.
- A validation session for the game is an environment, not a build: `MTL_SHADER_VALIDATION=1 MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1`, without BADFOOD/ABORT. 9a turns the archives off; without the switch, 9a says to set it.

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
