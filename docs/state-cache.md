# Pipeline state cache (design, issue #10)

Status: design for review; nothing implemented yet.

The goal: record the render pipelines (PSOs) a game used and rebuild them in the
background in later sessions, before a draw needs them. That should hold after a
d9mt update, after PSO-archive eviction and after a macOS update too. This
document covers what the logs show, what defines a d9mt PSO, the file format,
the flow, and the expected gains.

## 1. Where render-thread compile waits come from today

### 1.1 What `renderWait` measures

`compile_stats::g_wait` (`driver/mtl/mtl_compile.h`) counts the time the render
(CS) thread blocks on a compile task. A wait starts in three places:

1. **Shader functions** in `MtlContextImpl::resolvePso`
   (`driver/backend/mtl_context_transfer.cpp`, ~line 119): the bound VS or PS
   function (SM3 → IR → AIR → `MTLLibrary`) is still compiling.
2. **PSOs** in `MtlContextImpl::resolvePsoPatches` (same file, ~line 276).
   - `RenderPsoCache::get` submits a missed PSO as an urgent task and returns
     at once.
   - The bind is patched in when the command chunk is encoded, and the render
     thread waits for the task there.
   - If no worker has taken the task yet, `MtlCompileTask::wait()` runs it
     inline on the render thread.
3. **Internal clear/blit PSOs** through `RenderPsoCache::getSync`.

Every PSO build calls `MTL4Compiler newRenderPipelineStateWithDescriptor:` with
the session's archives as `lookupArchives`. The build runs under `g_pso_lock`
shared (`bridge/unix/winemetal_unix.c` ~6797). Every 256 successful builds,
archive hits included, the build that hits the count flushes the serializer
while holding `g_pso_lock` exclusively (`pso_note_compiled`, ~6312).

### 1.2 What the session logs show

The logs analysed are `dev4-lap-opt-on-1344.log` (Desktop) and, under
`~/Library/Logs/SWTOR on Mac/`, `sessions/wine-20261017-1529-*`,
`sessions/wine-20261017-1658-*` and `sessions/dev2-rawmouse-on-20261017-2231/`.

All of them ran with the same AIR cache table (`cache_6138817854601476907`), so
the AIR cache was warm every time. Only the number of loaded PSO archives
differs.

| | dev4 | 1529 | 1658 | dev2 |
|---|---|---|---|---|
| PSO archives loaded | 2 | 4 | 8 | 13 |
| Shaders created (all within render frames 0–37, ≈ first 2 s) | 1925 | 1925 | 1925 | 1925 |
| Slow `AIRCOMPILE` (> 5 ms) | 2 | 0 | 0 | 1 |
| PSOs built / total ms | 369 / 2767 | 339 / 753 | 356 / 2122 | 365 / 1251 |
| PSO < 1 ms (archive hit) | 319 | 336 | 343 | 349 |
| PSO 20–100 ms / > 100 ms | 30 / 4 | 2 / 1 | 10 / 3 | 7 / 3 |
| renderWait total | 341 / 1880 ms | 277 / 705 ms | 305 / 1267 ms | 332 / 1154 ms |
| renderWait in the world | 1821 ms | 609 ms | 1133 ms | 957 ms |
| of it: the n=256 archive flush | 556 ms | 510 ms | 668 ms | 504 ms |
| PSOs before world entry / in the world | 75 / 294 | 131 / 208 | 71 / 285 | 113 / 252 |

The phase splits are inferred from `STATS` draw counts. World entry is at
s 21–27.

Findings:

- **Shaders never cause a render-thread wait** with a warm AIR cache:
  - Frames that only compiled shaders show 0 ms `renderWait`.
  - Linking a PS against its VS always produced IR identical to an existing
    function (`shared=` 201–215, no `ps_` name compiled twice).
- **All render-thread waits are PSO waits.** Builds are submitted at the draw
  and waited for at encode, so `renderWait` count ≈ PSO count.
- **(a) The answer to "archive hits or real compiles":** mostly hits.
  - 87–96 % of the 340–370 PSOs per session are archive hits. They take
    about 0.45 ms each, about 150 ms per session, all of it render-thread
    wait.
  - The rest are real compiles of 20–220 ms. They come from content that is
    not in the loaded archives (dev4 had only 2), and they are the in-world
    stutter.
- **The single biggest wait is the archive flush**: 500–670 ms in every
  session, 30–72 % of all render-thread compile wait. It lands in the world at
  s 60–224.
  - The flush runs inside the 256th build of the session, which the render
    thread is waiting for.
  - This is a bridge issue, separate from the state cache (§9).
- **Frame 1 always pays one 34–162 ms build** (compiler or archive start-up,
  inferred).
- **Timing (question 3):** SWTOR creates all 1925 shaders (292 VS, 1633 PS) in
  the first ≈ 2 s. Its first PSOs are built at character select (s ≈ 10), the
  in-world ones from s 21–27 onward, and new ones keep coming until
  s 375.
  - So **"rebuild when the matching shaders appear" is early enough**: the
    shaders exist 8 s before the first character-select PSO and ≥ 20 s
    before the world.
  - No need to store shader copies, and no need for a debug counter.

### 1.3 What a state cache saves, per scenario

| Scenario | Today | With the state cache |
|---|---|---|
| **(a) Warm** (AIR warm, archives hold the session's PSOs) | ~0.45 ms × ~350 hit builds ≈ 150 ms of render-thread wait (≈ 110 ms in the world). Plus in-world cold builds for content that dropped out of the archives (1529: 3, ≈ 0.6 s; dev2: 10, ≈ 1 s). Plus the 0.5–0.67 s flush stall in the world. | The hit builds run in the background during login; the draw finds a finished PSO, so ≈ 0 ms. Previously seen content is rebuilt in the background. The 256th build now happens during the prewarm at startup, so the flush stall leaves the world (it can still delay a character-select draw that builds at that moment). The gain is small but real: ≈ 0.1–0.2 s of small waits plus moving one 0.5 s stall. |
| **(b) Bridge update, AIR codegen output unchanged** (only `LC_UUID` changed) | AIR cache cold: 1925 shaders recompile on the workers during the first seconds (cold AIR cost to be measured, phase 2). PSO archives should still hit: the archive directory isn't keyed by `LC_UUID`, and identical metallib bytes give identical function hashes. This is inferred, and phase 2 verifies it. | Same shader cost (the existing prefetch already compiles every shader at creation). PSO side as in (a). |
| **(b') Bridge update that changes the AIR bytes** (codegen fix, new flags), or **macOS update** (the `os-<build>` archive directory is wiped) | Every PSO is cold: ~350 builds × 20–220 ms (dev4 mean ≈ 51 ms for ≥ 20 ms builds), ≈ 250 of them in the world. That is several seconds of render-thread wait spread over the first minutes: dev4 had 1.8 s with only 34 cold builds. Estimate: 5–15 s. | The prewarm starts at ≈ 2 s (shaders created and their functions compiled), ordered by first use in earlier sessions, ≈ 50–60 ms per PSO. With 2 prewarm threads ≈ 350 PSOs take ≈ 10 s, so the character-select set is ready around s 10 and the world set before s 21. Expected in-world compile waits: near zero (first-time-ever content excepted). |
| **(c) Archive eviction** (content last seen more than 8 sessions ago) | Those PSOs are cold builds (20–220 ms each) in the world. | Every recorded PSO is rebuilt each session, cheaply as an archive hit while one exists, and captured into this session's archive again. Recorded PSOs therefore never age out of the archives, and the 8-session limit stops mattering for them. |

The main value is (b'), (c) and the macOS-update case, as #10 expects. In (a)
the state cache gains little: about 0.1–0.2 s, plus taking the flush stall out
of the world. The bigger warm-path win is fixing the flush itself (§9).

## 2. What determines a d9mt PSO

`RenderPsoKey` (`driver/backend/mtl_pso_cache.h:41`) is the complete build
input. `RenderPsoCache::build` (`mtl_pso_cache.cpp:87`) turns it into a
`WMTMtl4RenderPipelineInfo` together with the two libraries and entry names.
The struct is 320 bytes on x86_64:

| Bytes | Field | Source | Stable across d9mt versions? |
|---|---|---|---|
| 0–15 | `vsCookie`, `fsCookie` | `MTLLibrary` handles (`resolvePso`, `mtl_context_transfer.cpp:173`) | **No.** Runtime pointers; replaced by the shader identity below |
| 16–175 | `attrs[20]` (Metal vertex format, offset, buffer index) | D3D9 vertex declaration through `attributeFormat()` | Derived from D3D9 state; stable while the mapping is unchanged |
| 176–255 | `layouts[20]` (stride, step function, buffer index in `pad`) | Declaration + live stream stride + instancing (`streamFreq`) | Derived |
| 256–257 | `attrCount`, `layoutCount` | | Derived |
| 260–307 | `colors[4]` (Metal pixel format, blend enable, write mask, factors, ops) | RT view format (D3DFORMAT → Metal, sRGB write) + `D3DRS_*BLEND*`, `COLORWRITEENABLE*` | Derived |
| 308–311 | `colorCount`, `sampleCount`, `topologyClass`, `alphaToCoverage` | Pass + MSAA state | Derived |
| 312–319 | `depthFormat`, `stencilFormat` | Internal depth clear only | Derived |

Shader identity, which is D3D9-side and stable:

- **Programmable VS/PS**: `D3D9ShaderModuleKey` = stage + SHA-1(bytecode ‖
  `D3D9ShaderOptions`) (`d3d9_device.cpp:7639`).
  - It survives d9mt updates unless `D3D9ShaderOptions` itself changes, which
    would turn every recorded shader into a harmless miss.
- **PS linkage**: a programmable PS is linked against the bound programmable VS
  (`BindProgrammablePixelShaderLinkage`, `d3d9_device.cpp:7262`), or used
  standalone under the fixed-function VS. Its library is
  `D3D9ShaderProgram::GetFunction(prevVs)` (`d3d9_shader.cpp:178`). So the PS
  identity is (PS SHA-1, linked yes/no), with the VS SHA-1 as the link partner.
- **Fixed function**: one VS and one FS ubershader from the embedded metallib
  (`D3D9FFShaderModuleSet`, entry names `ffVsMain`/`ffFsMain`). The identity
  is just "FF".
- **Not recorded**:
  - Software-VP emulation shaders (`d3d9_swvp_emu.cpp`): SWTOR doesn't use
    them, and they are keyed by vertex elements.
  - Internal clear/blit/depth-clear PSOs (`getSync`): a handful per render
    target format, built from libraries the context creates lazily. They are
    counted in `STATECACHE` so we can see whether they matter.

Specialization: D3D9 spec state (alpha test, fog, sampler types, bool
constants) is **not** part of the PSO. It is read at run time from the SpecData
CBV (`emitSpecConstant`, `d3d9_shader_compiler.cpp:619`; `spec_count = 0` at
the AIR compile). No spec constants need recording.

Side observation, out of scope here: `build()` always overrides the topology
class to `Unspecified`, but `topologyClass` is still part of the key. A
triangle and a line draw with otherwise equal state therefore build two
identical PSOs; the second is an archive hit, ≈ 0.45 ms. Normalizing it is a
separate, tiny change.

### Recorded entry = stable shader identity + the derived key bytes

The derived bytes are stored as they are, the full `RenderPsoKey` with
both cookies zeroed. The alternative, storing raw D3D9 state and re-deriving
it, would duplicate the CS thread's derivation code outside the CS thread, for
little gain: the mapping rarely changes.

A mapping change is never a correctness risk. The draw always looks the PSO
up by its live key, so a stale entry is only a wasted background build that
no draw ever hits. To avoid even that waste, the file header carries a
**key signature**:

- a hash of `sizeof(RenderPsoKey)`, each field's offset, and a manual
  `kPsoKeyMappingVersion`;
- that version is bumped in the same commit as any change to
  `attributeFormat()`, the D3DFORMAT → Metal format table, or the blend
  mapping;
- on a mismatch the file's entries are dropped.

## 3. File format

One binary file per game executable, rewritten whole and atomically. Format:

```
Header, 64 bytes, little-endian:
  char     magic[8]        "D9MTSTC\0"
  uint32   formatVersion   1
  uint32   headerSize      64
  uint32   entrySize       sizeof(StateCacheEntry)
  uint32   entryCount
  uint64   keySignature    see §2
  uint32   session         incremented by every process that writes the file
  uint32   reserved[5]
  uint64   headerCheck     first 8 bytes of SHA-1(header bytes 0..55)

Entry (fixed size, currently 384 bytes):
  uint64   check           first 8 bytes of SHA-1(rest of the entry)
  uint8    vsOrigin        0 = none, 1 = D3D9 shader, 2 = fixed function
  uint8    psOrigin        same
  uint8    psLinked        1 = PS linked against the programmable VS
  uint8    reserved
  uint32   lastSession     header.session of the last process that used it
  uint32   sessionsUsed    saturating count
  uint32   firstUseMs      earliest first use, ms after device creation
  uint8    vsSha[20]
  uint8    psSha[20]
  uint8    key[320]        RenderPsoKey, cookies zeroed
  padding to entrySize
```

- **Versioned**:
  - A file with an older `formatVersion` is converted, if a converter
    exists, or dropped.
  - A newer `formatVersion` is left alone, neither read nor written, so a
    downgrade never destroys a newer cache.
  - A different `keySignature` or `entrySize` drops all entries.
- **Corruption**:
  - A bad magic or `headerCheck` drops the file, which is then rewritten.
  - A truncated file keeps the complete entries before the cut.
  - An entry with a bad `check`, an origin out of range or counts above 20/4
    is skipped and counted. A corrupt file is never fatal.
- **Atomic writes**:
  - The file is written to `<file>.tmp-<pid>`, then `FlushFileBuffers`, then
    `MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)`, which
    Wine turns into `rename(2)`.
  - Leftover `.tmp-*` files older than 10 minutes are deleted on load.
- **Merge on write**: the writer re-reads the current file and merges it
  (union by key, max `lastSession`, min `firstUseMs`). Two game processes
  therefore don't drop each other's entries; whoever writes last wins only
  the counters.
- **Bounded size**:
  - Cap 4096 entries ≈ 1.6 MB; SWTOR uses ~350 per session.
  - Over the cap, entries with the lowest `lastSession` are dropped first,
    then the lowest `sessionsUsed`.
  - Unlike DXVK, the state cache holds no reference to unmatched shaders. It
    keeps a program only while some entry still waits for it, and drops it
    once that entry has been prewarmed.
  - Entries whose build failed in the current d9mt are dropped.

## 4. Location

`<macOS user cache dir>/d9mt/state/<exe name>.d9mtstate`, next to
`d9mt/air-cache.sqlite3` and `d9mt/pso/`. For SWTOR that is
`$(getconf DARWIN_USER_CACHE_DIR)d9mt/state/swtor.exe.d9mtstate`. It never
lives in the game folder.

The file is GPU- and OS-independent (it describes D3D9-side state), so unlike
the PSO archives it survives macOS updates and GPU changes.

d3d9.dll is a PE module and cannot call `confstr`, so the path is resolved
like this:

1. `D9MT_STATE_CACHE=<unix path>`, a directory or a file. This is the test
   override, and what the launcher should set (one line in `som.sh`:
   `export D9MT_STATE_CACHE="$(getconf DARWIN_USER_CACHE_DIR)d9mt/state"`).
2. Otherwise from `TMPDIR`, which Wine passes through to the Windows
   environment unchanged.
   - launchd sets it to `/var/folders/<a>/<b>/T/`, and the user cache dir is
     its sibling `/var/folders/<a>/<b>/C/`.
   - It is used only if it matches that pattern and `…/C/d9mt/` exists (the
     AIR cache creates it).
3. Otherwise the cache is off, with a `STATECACHE off (no cache dir)` line.

The Unix path is opened as `\\?\unix\<path>`, the NT path form Wine itself
uses for `WINEHOMEDIR`. `kernel32!wine_get_dos_file_name` is the fallback.
Phase 2 tests both on the shipped engine before relying on either.

Rejected: reusing the bridge's sqlite `CacheReader`/`CacheWriter` thunks (they
resolve relative paths under the cache dir). `CacheReader_get` returns a
`dispatch_data_t` whose bytes the PE side has no thunk to read, so that route
would need a winemetal.so change (§8).

## 5. Flow

### Load (device creation)
- A `d9mt-statecache` thread (lowest priority) reads and validates the file,
  then builds an index `shader SHA-1 → entries` and a per-entry count of
  missing shaders.
- Expected ≈ 2 ms for 1.6 MB. Nothing waits for it.
- Shaders announced before the index is ready are queued and matched
  afterwards.

### Shader announcement (app thread, `D3D9ShaderModuleSet::GetShaderModule`, new modules only)
- `onShaderCreated(stage, sha, program)`:
  - one hash lookup under the state cache's mutex;
  - if the SHA is referenced, it stores `Rc<D3D9ShaderProgram>` (modules
    live as long as the device) and decrements each entry's missing count;
  - an entry at 0 goes into the ready queue.
- The fixed-function shaders are announced at device init.
- Cost: 1925 lookups at startup, nothing per frame.

### Prewarm (1–2 `d9mt-prewarm` threads, `THREAD_PRIORITY_LOWEST`)
- Takes ready entries ordered by `firstUseMs`, so character-select PSOs come
  before the world's; `sessionsUsed` breaks ties.
- Per entry:
  1. **Functions.**
     - The VS standalone function, and the PS function linked to it, come
       from `GetFunction(prev, urgent = false)`. The new `urgent` parameter
       keeps the prewarm from filling the urgent queue.
     - If the task is still pending, the prewarm thread runs it itself
       (`tryRun`). If another thread is running it, `waitFor` in 5 ms steps
       while checking the pause rules.
  2. **Key.** The recorded bytes plus the live library handles as
     cookies, and the entry names from the functions.
  3. **De-dup and publish.**
     - The key goes into the device's `PsoRegistry`, a mutex-protected map
       `RenderPsoKey → Rc<RenderPsoCache::Entry>` that both the prewarm and
       the render thread's miss path use.
     - If the key is already there, the render thread got there first, and
       the entry is skipped.
  4. **Build.** `entry->tryRun()` on the prewarm thread, with the same
     `RenderPsoCache::build`.
     - A failure is logged once at debug level, not as the current
       `Logger::err`, and the entry is dropped from the file.
- Pause rules, checked before each entry and while waiting:
  - **Pause** while the compile pool has urgent tasks queued.
  - **Pause** for 50 ms after any render-thread compile wait
    (`waitOnRenderThread` and the wait budget store an atomic timestamp when,
    and only when, they actually wait).
  - **Never more than one build in flight per prewarm thread.**
  - Threads: default 2 while the ready queue is longer than 32, otherwise 1.
    `D9MT_STATE_CACHE_THREADS` overrides it (0–4). Phase 2 measures 1 vs 2.

### Render thread (`RenderPsoCache::get`)
- Unchanged on every hit path: `m_last`, PSO MRU, `m_cache`.
- The miss path, about 350 times per session, which already allocates an
  `Entry`:
  1. Lock the registry. If a prewarmed entry exists, adopt it into `m_cache`.
     - Done: no wait (a **hit**).
     - Pending: submit it urgent as today.
     - Running on a prewarm thread: wait for it as today (a **late hit**,
       still shorter than a fresh build).
  2. Otherwise create the entry as today and insert it into the registry.
  3. Record: copy one `StateCacheEntry` (384 B) into a preallocated
     single-producer ring of 1024 slots.
     - No allocation, no lock, no IO.
     - If the ring is full the record is dropped and counted. It is
       recorded again next session.
- `resolvePso` passes the shader identities, which come from new fields on
  `MtlShaderFunction` set at creation (origin, SHA-1, linked).

### Write (state cache thread)
- Drains the ring.
- Keeps an entry only once its build has succeeded (`isDone() && handle()`).
  A failed descriptor is never replayed.
- Writes the file 20 s after the last new record, and at most every 5 min
  while new records keep coming.
- On device destruction it writes the file. The device destructor waits at most 500 ms
  and never blocks on IO otherwise.
- A killed process loses at most the last 20 s of records.

### Threading summary

| Thread | Work | Locks |
|---|---|---|
| App (D3D9) | `onShaderCreated` hash lookup | state cache mutex, short |
| CS (render) | miss path: registry lookup/insert + ring push | registry mutex, short, only on a miss |
| Compile workers | unchanged | |
| `d9mt-prewarm` × 1–2 | functions + PSO builds, low priority, pausing | registry, state cache mutex |
| `d9mt-statecache` | load, merge, write, stats | file IO |

## 6. Interaction with the AIR cache and PSO archives

- **AIR cache**: unchanged.
  - Prewarm function compiles go through the same `D9MTAirCompile` path
    and hit or fill the AIR cache like any other compile.
  - After a bridge update the existing shader prefetch already recompiles
    all shaders at creation; the prewarm adds only the linked PS functions,
    which are cheap (they share IR, §1.2).
- **PSO archives**: unchanged in the bridge.
  - Prewarm builds use the same compiler, so they are archive hits when
    possible and are captured by the serializer.
  - Consequence 1: the current session's archive contains every prewarmed
    PSO. Eviction after 8 sessions no longer loses recorded PSOs.
  - Consequence 2: each session's archive gets bigger (all recorded PSOs
    instead of ~350), and the newest 8 are kept. Phase 2 measures bytes per
    PSO in an archive. If the total is a concern, the cap on prewarmed
    entries (default all 4096; `lastSession` within 32 sessions) bounds it.
  - Consequence 3: the 256-PSO flush happens during the prewarm at startup.
    That moves the 0.5–0.67 s flush out of the world, but more prewarmed
    PSOs mean more flushes at startup (one per 256). Builds the render thread
    needs at that moment still wait for the flush. The real fix is §9.

## 7. Opt-out and logging

- `D9MT_STATE_CACHE=0` (or empty): off. No load, no record, no prewarm, no
  registry; the miss path is exactly today's.
- `D9MT_STATE_CACHE=<unix dir|file>`: location override (tests point it at
  `/tmp`).
- `D9MT_STATE_CACHE_THREADS=<0..4>`: prewarm threads. 0 records and loads,
  but never prewarms; this is the A/B switch for measurements.
- `dxvk.conf`: `d9mt.stateCache = True|False`, same as the env var.

Log lines (info level, so they show with Debug Logging; a few per session):

```
STATECACHE load path=<unix path> entries=812 dropped: corrupt=0 version=0 failed=0 cap=0 ms=2.1
STATECACHE prewarm t=12.4s ready=790 built=790 hit=0 failed=2 paused=840ms threads=2
STATECACHE use t=60s hits=301 late=4 misses=38 recorded=38 ringDrops=0 internal=6 hitBuildMs=15230
STATECACHE write entries=850 bytes=326464 ms=3.0
```

- `hits`: a draw needed a PSO that was already prewarmed and done.
- `late`: prewarmed, but still building when the draw needed it.
- `misses`: not in the cache.
- `hitBuildMs`: the sum of the prewarm build times of the hit entries, an
  upper bound on the render-thread wait avoided.
- `internal`: clear/blit PSOs built (not recorded).
- The `use` line is printed every 60 s while something changes, and at the
  end.

## 8. Which files change; winemetal.so stays byte-identical

d3d9.dll only:

| File | Change |
|---|---|
| `driver/d3d9/d3d9_state_cache.{h,cpp}` (new) | file format, load/merge/write, index, prewarm threads, stats |
| `driver/backend/mtl_pso_registry.h` (new) | `PsoRegistry` (mutex + map), owned by `MtlDeviceImpl` |
| `driver/backend/mtl_pso_cache.{h,cpp}` | miss path: registry adopt/insert + record hook; quiet failure for prewarm builds |
| `driver/backend/mtl_context_transfer.cpp` | `resolvePso` passes the shader identities |
| `driver/mtl/mtl_context.h` | `MtlShaderFunction`: origin, SHA-1, linked (set once at creation) |
| `driver/mtl/mtl_compile.h` | last-render-wait timestamp; urgent-queue size; `waitFor` used by the prewarm |
| `driver/d3d9/d3d9_shader.{h,cpp}` | program knows its module SHA-1; `GetFunction(prev, urgent)`; `onShaderCreated` hook |
| `driver/d3d9/d3d9_device.cpp`, `d3d9_fixed_function.cpp` | create/destroy the state cache; announce FF shaders |
| `test/native/state_cache_test.cpp` (new) | format, corruption, version, cap, merge, concurrency (TSan) |
| `README.md` | env vars, `STATECACHE` lines |

**winemetal.so is not changed.** Its `LC_UUID` stays
2BBB585C-5076-3157-9A6A-BAE494B00ACC, so users' AIR caches stay valid.
winemetal.dll doesn't change either: only existing thunks are used, and the
file IO is plain Win32 in d3d9.dll.

## 9. Bridge follow-ups worth bundling into the next winemetal.so change

These are not part of this work; each one changes `LC_UUID`.

1. **Flush off the build path.** This is the largest warm-path win: 0.5–0.67 s
   per session, 30–72 % of all render-thread compile wait.
   - Flush from a background thread or at a natural pause.
   - Or swap in a fresh serializer: flush the old one while new builds go
     to the new one, without holding builds exclusively for the whole
     write.
2. A QoS thunk (`pthread_set_qos_class_self_np(QOS_CLASS_UTILITY)`) for the
   prewarm threads. Until then, `THREAD_PRIORITY_LOWEST` is all there is.
   - How Wine maps it on macOS, and whether it reaches `MTLCompilerService`
     (XPC carries QoS, not Mach precedence), is measured in phase 2.
3. A thunk returning the user cache dir, which would replace the `TMPDIR`
   derivation.

## 10. DXVK's state cache, and why DXVK's reason for dropping it doesn't apply

DXVK's state cache lived from 0.80 to v2.6.2. It was removed in DXVK 2.7
(2025-07, commit `57d5a1e4`). The references below are at the parent commit,
`ca1aa206`.

### How it worked
- **File**: `<exe>.dxvk-cache`, version 18.
- **Entry**: a SHA-1 per shader stage (D3D9: the SHA-1 of the DXSO
  bytecode), the full graphics state as raw struct bytes, and a SHA-1 per
  entry used as a checksum.
- **Recording**: a pipeline is recorded when it is created, through a writer
  thread.
- **Prewarm**: `registerShader()` runs on every shader creation and queues
  each cached pipeline once all of its stages exist. A worker compiles it at
  low priority. A draw on a pipeline that is still being built waits for that
  build instead of starting a second one.

The design here keeps that core: recorded state keyed by shader hash, and a
rebuild when the shaders appear. It avoids DXVK's known weaknesses:
- DXVK appended in place and rewrote non-atomically; here every write goes
  to a temp file and is renamed over the old one.
- DXVK had no size cap; here the file is capped.
- DXVK's variable-size entries could put the reader out of sync after one bad
  `entrySize`; here entries have a fixed size.
- DXVK went through 18 format versions as raw-struct layouts changed; here a
  `keySignature` derived from the layout drops stale entries automatically.

### Why DXVK dropped it
Quoting the 2.7 commit: "nowadays just isn't really needed anymore since
everyone … supports GPL". With `VK_EXT_graphics_pipeline_library`:
- each shader compiles on its own when the game creates it;
- the draw only does a cheap link;
- the driver's disk cache keeps the binaries.

Without GPL, DXVK 2.7+ simply compiles at draw time, which stutters.

### Why that doesn't apply to d9mt on Metal 4
- Metal 4's "flexible" pipelines (unspecialized pixel format, write mask and
  blend, plus `newRenderPipelineStateBySpecialization…`) are the equivalent
  of GPL's **fragment-output** library only.
- The VS+PS pair, the vertex descriptor, the sample count, alpha-to-coverage
  and the topology class are still fixed when the pipeline is created.
- In D3D9 those are only known at the draw.
- So the expensive compile of each first-seen combination stays at the draw,
  and that is exactly what a state cache moves into the background.
- Like DXVK's own non-GPL fallback before 2.7.

### What does carry over
- Metal has no depth format in the pipeline. Rasterizer and depth/stencil
  state are encoder state. D9MT's D3D9 spec state is a runtime CBV. The key
  space is therefore small: SWTOR uses ~350 PSOs per session.
- The system Metal shader cache, plus d9mt's PSO archives, already make
  repeats cheap (§1.2: 0.45 ms per archive hit). That is why the warm gain is
  small and the value is in (b'), (c) and macOS updates.
- A later, separate option: build unspecialized pipelines and specialize them
  per format and blend. A missing color/blend combination would then cost
  only the fast fragment-output step, but there is a small GPU cost (Apple:
  "compile with full state in the background" for expensive shaders). That
  complements the state cache rather than replacing it.

Other Metal layers:
- DXMT caches shader variants only (sqlite, like our AIR cache). It does no
  PSO prewarm.
- MoltenVK caches MSL only.
- D3DMetal's `d3dm/<game>/shaders.cache` is closed source, and its content
  is unknown.

## 11. Phase 2 plan

- **Unit tests** (`test/native/state_cache_test.cpp`, no Wine or GPU):
  - record → write → load → index → ready queue on shader announcement;
  - a corrupt header, entry and truncation;
  - an older/newer `formatVersion` and a changed `keySignature`;
  - the cap eviction order;
  - merge with a concurrently written file;
  - ring producer/consumer and registry adopt/insert races under TSan.
- **Overhead on the render thread**:
  - a micro-benchmark of the miss path with and without recording (ring
    push), and of the hit paths (they must be identical code);
  - a hit-path A/B with `D9MT_STATE_CACHE=0` on a test program.
- **Wine runs**:
  - a synthetic many-pipeline test, N VS×PS×blend×format combinations
    created up front and drawn later;
  - run 1 records; run 2 with the state cache vs `D9MT_STATE_CACHE_THREADS=0`;
  - cold simulation: `D9MT_AIR_CACHE` and `D9MT_PSO_ARCHIVE_DIR` pointed at
    empty `/tmp` dirs, state cache from run 1;
  - measured: `renderWait`, `STATECACHE` hits/late, the latency of urgent
    builds while the prewarm runs, the flush timing and archive bytes per PSO;
  - also checks `\\?\unix\` and `TMPDIR` on the shipped engine.
- **Game route (Jan)**: exact steps delivered with the implementation, for
  three cases:
  - fresh `/tmp` AIR+PSO dirs with a state cache from a previous session;
  - the same with `D9MT_STATE_CACHE_THREADS=0`;
  - a warm A/B on the standard route with `session-report.sh --compare`.
