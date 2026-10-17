# Pipeline state cache (design, issue #10)

Status: implemented on `feat/state-cache` (phase 2). §12 has the
measurements. The design below is updated to match the code.

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
  uint64   headerCheck     64-bit FNV-1a of header bytes 0..55

Entry (fixed size, 384 bytes):
  uint64   check           64-bit FNV-1a of the rest of the entry
  uint8    vsOrigin        0 = none, 1 = D3D9 shader, 2 = fixed function
  uint8    psOrigin        same
  uint8    psLinked        1 = PS linked against the programmable VS
  uint8    reserved
  uint32   lastSession     header.session of the last process that used it
  uint32   sessionsUsed    saturating count
  uint32   firstUseMs      earliest first use, ms after device creation
  uint8    vsSha[20]
  uint8    psSha[20]
  uint8    key[304]        RenderPsoKey after its two 8-byte library cookies
  uint8    reserved[16]
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
  - The file is written to `<file>.tmp-<pid>-<n>`, then `FlushFileBuffers`, then
    `MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)`, which
    Wine turns into `rename(2)`.
  - Leftover `.tmp-*` files older than 10 minutes are deleted on load.
- **Merge on write**: the writer re-reads the current file and merges it
  (union by key, max `lastSession`, min `firstUseMs`). Two game processes
  therefore don't drop each other's entries; whoever writes last wins only
  the counters.
- **Bounded size**:
  - Cap 1024 entries by default ≈ 390 KB; SWTOR uses ~350 per session.
    `D9MT_STATE_CACHE_MAX` sets another cap within 64–16384. The default was
    lowered from 4096: every session's PSO archive captures every prewarmed
    pipeline (§12), and until the bridge keeps fewer archive sessions, the
    cap also bounds the archives.
  - Over the cap, entries with the lowest `lastSession` are dropped first,
    then the lowest `sessionsUsed`.
  - The cache keeps only a pointer to each announced shader, no reference:
    `RcObject` has no virtual destructor, so a type-erased `Rc` would be
    unsafe as the last owner. The D3D9 shader module set keeps every module
    for the device's lifetime, and the fixed-function adaptors are device
    members. `shutdown()`, called from the device destructor before either
    goes away, drops the pointers.
  - The checks are FNV-1a rather than SHA-1. They detect damage; they are
    not meant to resist tampering, since nothing in the file is executed or
    trusted: a forged entry can only cause a wasted background build.
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

1. `D9MT_STATE_CACHE=<unix path>`, a directory or a `.d9mtstate` file. This
   is the test override, and what the launcher should set (one line in
   `som.sh`:
   `export D9MT_STATE_CACHE="$(getconf DARWIN_USER_CACHE_DIR)d9mt/state"`).
   A path not starting with `/` is taken as a Windows path. `0` turns the
   cache off; an empty value counts as unset.
2. Otherwise from `TMPDIR`, if the Windows environment has it.
   - launchd sets it to `/var/folders/<a>/<b>/T/`, and the user cache dir is
     its sibling `/var/folders/<a>/<b>/C/`.
   - It is used only if it matches that pattern and `…/C/d9mt/` exists (the
     AIR cache creates it).
   - **Measured: CrossOver 26.3's Wine (SWTOR on Mac's engine) does not pass
     `TMPDIR` to Windows programs.** `TEMP`/`TMP` are Windows paths, and only
     `WINEHOMEDIR` arrives, from which the per-user `/var/folders/<random>`
     can't be derived. On that engine this fallback never triggers. It stays
     in the code because it is harmless and other Wine builds pass `TMPDIR`
     through.
3. Otherwise `~/Library/Caches/d9mt/state/`, the user's Library caches.
   - The home dir comes from Wine's `WINEHOMEDIR` (`\??\Z:\Users\<name>`),
     converted with `kernel32!wine_get_unix_file_name`, or directly for a
     `\??\unix\…` form.
   - It is used only if `<home>/Library/Caches` exists (a Mac). `d9mt/state`
     is created there.
4. Otherwise the cache is off, with a
   `STATECACHE off (no cache dir: no usable TMPDIR or ~/Library/Caches; set D9MT_STATE_CACHE)`
   line.

So on SWTOR on Mac, without the launcher line, the cache lives in
`~/Library/Caches/d9mt/state`. With the line it lives next to the AIR cache
and PSO archives, which keeps every d9mt cache in one place; that is still
the recommended setup.

The Unix path is converted with `kernel32!wine_get_dos_file_name`, Wine's own
mapping, which gives a drive path such as `Z:\…`. If that function is missing
or fails, the path is opened as `\\?\unix\<path>`. Tested on the shipped engine,
through `exc_guard`, with a fake `HOME` under `/tmp` in every run:
- The explicit path: every Wine test in §12 wrote and read its file in
  `/tmp`, and it wins over the fallback.
- The `~/Library/Caches` fallback: `WINEHOMEDIR` arrives as
  `\??\Z:\tmp\…\home`. The file is created there, and the next run loads it
  and prewarms (39/40 hits).
- `=0`, and a home without `Library/Caches`: both log `STATECACHE off`.
- The `TMPDIR` derivation: tested with a fake `TMPDIR`; it stays unused, as
  described above.

Natively, `state_cache_test` covers every branch of the resolution, with
`HOME` standing in for `WINEHOMEDIR`.

Rejected: reusing the bridge's sqlite `CacheReader`/`CacheWriter` thunks (they
resolve relative paths under the cache dir). `CacheReader_get` returns a
`dispatch_data_t` whose bytes the PE side has no thunk to read, so that route
would need a winemetal.so change (§8).

## 5. Flow

All of it lives in `driver/mtl/mtl_state_cache.{h,cpp}` (threads, IO,
recording) and `driver/mtl/mtl_state_cache_core.{h,cpp}` (format, database,
index, ring; plain C++, unit-tested natively).

### Load (device creation)
- `D3D9DeviceEx::InitStateCache` resolves the file (§4), creates the cache
  and announces the fixed-function shaders.
- The `d9mt-statecache` thread (lowest priority) reads and validates the
  file, applies the cap and builds the prewarm index: shader → entries, plus
  a per-entry count of missing shaders. On the synthetic test this takes
  2–4 ms. Nothing waits for it.
- Shaders announced before the index exists are kept and matched once it
  does.
- Only entries used within the last 32 sessions are prewarmed. Older ones
  stay in the file until the cap pushes them out.

### Shader announcement (`D3D9ShaderModuleSet::GetShaderModule`, new modules only)
- `MtlStateCache::announce(stage, id, keepAlive, shader)`: one hash insert
  and lookup under the cache's mutex. Entries whose missing count reaches 0
  go into the ready queue.
- `D3D9ShaderProgram` implements `MtlStateCacheShader`. Its identity
  (`MtlShaderStateId`: origin, the module key's SHA-1) is stamped on every
  `MtlShaderFunction` it creates, with `linked` set for functions linked
  against a VS.
- The fixed-function module set announces its two ubershader functions.
- Cost: one lookup per new shader (SWTOR: 1925 at startup), nothing per
  frame.

### Prewarm (`d9mt-prewarm` threads, `ThreadPriority::Lowest`)
- Ready entries come out ordered by first use in earlier sessions
  (`firstUseMs`), then `sessionsUsed`, then `lastSession`. Character-select
  pipelines therefore come before the world's.
- Per entry:
  1. **Functions.**
     - `prewarmFunction(prev)` returns the same function object a draw
       would get: `GetFunction`'s slot for that (shader, linked VS) pair.
       It creates the slot if needed, but never queues it.
     - If the function's task is still pending, the prewarm thread runs it
       itself (`tryRun`): Phase A, AIR compile, `newLibrary`.
     - If a worker or a draw is already running it, the thread waits for it
       in 5 ms steps (`waitFor`, never inline), checking for shutdown.
  2. **Key.** `MtlDeviceImpl::createPrewarmPso` puts the recorded 304 bytes
     after this session's library handles, as `resolvePso` would, and uses
     the functions' entry names.
  3. **De-dup.**
     - The key goes into the device's `RenderPsoCache::Registry` (mutex +
       map), which the render thread's misses also use while the cache is
       on.
     - If the key is already there (a draw got there first), the entry is
       counted as `dup` and skipped.
  4. **Build.** `tryRun()` on the prewarm thread, through the same
     `RenderPsoCache::build` and compiler as a draw.
     - A pipeline that no longer builds is logged at debug level, not as an
       error, and dropped from the file.
     - Prewarm builds are not counted in the render path's
       `COMPILE psos=` and `PSOBUILD` lines; the `STATECACHE` lines report
       them.
- **Yield rules**, checked before the functions and again before the build:
  - Wait in 2 ms steps while the compile pool has urgent tasks queued
    (`MtlCompileWorkers::urgentQueued()`, a relaxed atomic).
  - Wait while the render thread is blocked on a compile
    (`compile_stats::RenderWaitScope`: two relaxed atomics, only when the
    render thread actually waits). The design had a 50 ms grace period after
    each wait. Measured (§12), that starved the prewarm exactly when the
    draws caught up with it, while on-demand builds got no faster from it,
    so it was dropped.
  - At most one build in flight per prewarm thread.
  - Default 2 threads. The second only takes work while more than 32
    entries are ready. `D9MT_STATE_CACHE_THREADS` (0–4) overrides the count.

### Render thread (`RenderPsoCache::get`)
- Unchanged on every hit path (`m_last`, the PSO MRU in `resolvePso`,
  `m_cache`). With the cache off, the only addition to the miss path is one
  atomic pointer load.
- The miss path (SWTOR: ~350 per session), which already allocates an
  `Entry`:
  1. `Registry::adoptOrInsert` under its mutex. A prewarmed entry replaces
     the fresh one.
     - Done: no wait, a **hit**.
     - Not done: submitted urgent as today. If a prewarm thread is already
       building it, the draw waits for that build: a **late** hit.
  2. `MtlStateCache::record` copies the key bytes and both shader identities
     into a preallocated single-producer ring of 1024 records.
     - No allocation, no lock, no IO.
     - If the ring is full, the record is dropped and counted (`ringDrops`).
       It is recorded again next session.
  3. Internal clear/blit pipelines (`getSync`) carry no shader identity and
     are counted as `unrecorded`.

### Write (`d9mt-statecache` thread)
- Every 250 ms: drains the ring. A record is kept only once its pipeline
  has built successfully (`isDone() && handle()`), so a failing descriptor
  is never replayed. Prewarm failures are removed.
- Writes the file 20 s after the last change, at most every 5 min while
  changes keep coming, and once more in `shutdown()`.
- `D3D9DeviceEx::~D3D9DeviceEx` calls `shutdown()` after the CS thread is
  synchronized. It joins the prewarm threads (at most one in-flight build
  each), then the writer (one final write, a few ms), and drops all shader
  and pipeline references.
- A killed process loses at most the last 20 s of records.

### Threading summary

| Thread | Work | Locks |
|---|---|---|
| App (D3D9) | `announce`: hash insert/lookup per new shader | state cache mutex, short |
| CS (render) | per PSO miss: registry adopt/insert, ring push | registry mutex, only on a miss |
| Compile workers | unchanged (plus an atomic urgent count) | |
| `d9mt-prewarm` × 2 | functions + PSO builds, lowest priority, yielding | state cache mutex to pop; registry |
| `d9mt-statecache` | load, intake, merge, atomic writes, stats | file IO only here |

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
    instead of ~350), and the newest 8 are kept. §12 measures the bytes per
    PSO. The 1024-entry default cap and the 32-session prewarm window bound
    it.
  - Consequence 3: the 256-PSO flush happens during the prewarm at startup.
    That moves the 0.5–0.67 s flush out of the world, but more prewarmed
    PSOs mean more flushes at startup (one per 256). Builds the render thread
    needs at that moment still wait for the flush. The real fix is §9.

## 7. Opt-out and logging

- `D9MT_STATE_CACHE=0`: off. No load, no record, no prewarm, no registry;
  the miss path is today's plus one atomic load.
- `D9MT_STATE_CACHE=<unix dir|file>`: location (the launcher sets it; tests
  point it at `/tmp`).
- `D9MT_STATE_CACHE_THREADS=<0..4>`: prewarm threads. `0` loads and records
  but never prewarms; this is the A/B switch.
- `D9MT_STATE_CACHE_MAX=<64..16384>`: the entry cap (default 1024).
- `dxvk.conf`: `d9mt.stateCache = False` turns it off like the env var.

Log lines (info level, so they show with Debug Logging; a few per session):

```
STATECACHE load path=<unix path> status=ok entries=400 session=2 prewarmable=400 readyAtLoad=0 corrupt=0 truncated=0 capped=0 threads=2 ms=3.8
STATECACHE prewarm t=0.83s built=399 dup=1 failed=0 skipped=0 buildMs=991 pausedMs=201
STATECACHE write entries=400 bytes=153664 ms=1.7
STATECACHE use t=5s hits=399 late=0 misses=1 recorded=0 unrecorded=0 ringDrops=0 hitBuildMs=991 | prewarm built=399 dup=1 failed=0 skipped=0 buildMs=991 pausedMs=201 ready=0 | entries=400 (final)
```

- `status`: `ok`, `new` (no file), `bad-header`, `older-version`,
  `newer-version` (left alone, never written), `key-changed` (the
  `RenderPsoKey` layout or mapping changed; entries dropped).
- `prewarmable`: entries used within the last 32 sessions.
- `hits`: a draw needed a PSO that was already prewarmed and done.
- `late`: prewarmed, but still building when the draw needed it.
- `misses`: not in the cache.
- `recorded`: new entries this session.
- `unrecorded`: internal clear/blit pipelines.
- `hitBuildMs`: the sum of the prewarm build times of the hits, an upper
  bound on the render-thread wait they saved.
- `dup`: a draw built it first.
- `skipped`: its shader didn't compile in this d9mt.
- `pausedMs`: time spent yielding to the render thread.
- The `prewarm` line is printed each time the queue drains. The `use` line
  is printed every 60 s while something changed, and at shutdown.

## 8. Which files change; winemetal.so stays byte-identical

d3d9.dll only:

| File | Change |
|---|---|
| `driver/mtl/mtl_state_cache_core.{h,cpp}` (new) | file format, parse/serialize, database (merge, use, cap), prewarm index, SPSC ring |
| `driver/mtl/mtl_state_cache.{h,cpp}` (new) | `MtlStateCache`: path resolution, threads, record, prewarm, Win32/POSIX file IO, stats |
| `driver/mtl/mtl_types.h` | `MtlShaderStateId` |
| `driver/mtl/mtl_context.h` | `MtlShaderFunction::stateId` (set once at creation, not copied when functions share a library) |
| `driver/mtl/mtl_compile.h` | `urgentQueued()`; `RenderWaitScope` / `renderWaitedWithin()` |
| `driver/mtl/mtl_device.h`, `mtl_device_stub.cpp` | `stateCacheBackend()`, `psoKeySignature()`, `stateCache()` / `setStateCache()` |
| `driver/backend/mtl_pso_cache.{h,cpp}` | `RenderPsoKey::signature()`, `kPsoKeyMappingVersion`, `Registry`, record hook in the miss path, prewarm entries (quiet failures, not counted as render-path builds) |
| `driver/backend/mtl_device_impl.{h,cpp}` | implements `MtlStateCacheBackend`; owns the registry |
| `driver/backend/mtl_context_record.cpp`, `mtl_context_transfer.cpp` | pass the registry, and the bound functions to `get()` |
| `driver/d3d9/d3d9_shader.{h,cpp}` | program identity, `prewarmFunction()`, announce new modules |
| `driver/d3d9/d3d9_fixed_function.{h,cpp}` | FF identity and announcement |
| `driver/d3d9/d3d9_device.{h,cpp}`, `d3d9_options.{h,cpp}` | create/shut down the cache; `d9mt.stateCache` |
| `scripts/build-driver.sh` | the two new sources |
| `test/native/state_cache_test.cpp`, `scripts/test-state-cache.sh` (new) | unit tests, TSan, record micro-benchmark |
| `test/code_statecache.c`, `scripts/run-statecache-bench.sh` (new) | synthetic many-pipeline workload under Wine |

For #8's later rebase: the only shader-path files touched are
`d3d9_shader.{h,cpp}`, and only in `D3D9ShaderProgram` (constructor, identity,
`prewarmFunction`, `CreateFunction`) and `GetShaderModule`. The SM3
converter (`d3d9_shader_compiler.cpp`) is untouched.

**winemetal.so is not changed.** Nothing under `bridge/` or `airlib/` was
touched, so its `LC_UUID` stays 2BBB585C-5076-3157-9A6A-BAE494B00ACC and
users' AIR caches stay valid. winemetal.dll doesn't change either: only
existing thunks are used, and the file IO is plain Win32 in d3d9.dll.

## 9. Bridge follow-ups worth bundling into the next winemetal.so change

These are not part of this work; each one changes `LC_UUID`.

1. **Flush off the build path.** This is the largest warm-path win: 0.5–0.67 s
   per session, 30–72 % of all render-thread compile wait.
   - Flush from a background thread or at a natural pause.
   - Or swap in a fresh serializer: flush the old one while new builds go
     to the new one, without holding builds exclusively for the whole
     write.
2. A QoS thunk (`pthread_set_qos_class_self_np(QOS_CLASS_UTILITY)`) for the
   prewarm threads. Until then, `THREAD_PRIORITY_LOWEST` is all there is
   (§12 measures what it does).
3. A thunk returning the user cache dir, which would replace the `TMPDIR`
   derivation (which CrossOver's Wine doesn't support anyway, §4).
4. Fewer PSO archive sessions while the state cache is on: 2–3 instead of 8.
   Every session's archive then holds all recorded pipelines (§12, archive
   growth).

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

## 11. Tests

- `scripts/test-state-cache.sh` (`test/native/state_cache_test.cpp`, no Wine
  or GPU), plain `-O2` and under ThreadSanitizer, both passing:
  - file format round trip;
  - single-byte damage anywhere (2000 random cases: never a damaged entry
    out), truncation mid-entry and at a boundary, a damaged header, random
    garbage;
  - older/newer format versions, a key signature change;
  - the cap on the database, and on a file 300 entries above the default
    1024 (loaded capped, rewritten capped, the most recently used kept); a
    configured larger cap keeps them all;
  - the location and limits from the environment: explicit dir or file,
    `=0`, an empty value, the cache dir next to the AIR cache, the
    `~/Library/Caches` fallback, neither (off), and the
    `D9MT_STATE_CACHE_MAX`/`_THREADS` clamps;
  - `use()`/`merge()` counters; the prewarm index (readiness, order, stale
    entries, FF and linked identities);
  - the SPSC ring (200 000 items, in order, references released);
  - the whole cache with a fake backend:
    - record → write → load → announce → prewarm → all 300 draws hit;
    - prewarm off;
    - partial shader sets;
    - failing pipelines dropped from the file;
    - a corrupt, garbage, newer or key-changed file;
    - two processes on one file;
    - announce/record/prewarm on separate threads with the urgent-queue
      pause toggling, including shutdown during the prewarm.
- The existing native tests (`buffer_tracking`, `fenced_pool`, `hitch`,
  `memory_profile`, `upload_ring`, `attachment_opt`, `stats`, `pace`) and
  the compile-pool TSan test (`mtl_compile_test`) still pass.
- `scripts/run-hang-test.sh --runs 3`: PASS with the new `d3d9.dll`. The
  script now also keeps the state cache in its scratch dir.

## 12. Measurements (phase 2)

### Setup
- Wine: a copy of SWTOR on Mac's engine (CrossOver 26.3, `winemetal.so`
  2BBB585C-…, unchanged) in `/tmp`, with a throwaway prefix.
- Driver: `d3d9.dll` from this branch (x64, clean build), against `main`'s
  (4b7ab26).
- Workload: `test/code_statecache.c`, SWTOR-shaped:
  - 16 VS and 200 PS created up front (generated SM3, 40–120 ALU rounds);
  - 3 s of "loading" with one pipeline;
  - then a "world" bringing in 4 new pipelines per frame (400 in total:
    VS/PS pairs × 3 blend modes × 3 render target formats), 160 world frames
    at vsync.
- Every AIR cache, PSO archive and state cache is in `/tmp`
  (`scripts/run-statecache-bench.sh`); 3–4 runs per arm.
- "Fastmath" runs use `D9MT_AIR_FASTMATH=1`: the AIR bytes differ from the
  recording run, so Metal gets nothing for free. That is exactly a bridge
  update that changes the AIR. A rerun with the same AIR flags and fresh
  d9mt caches cost as much as the first run (8.0–8.4 s vs 8.0–8.8 s), so
  Metal's own shader cache doesn't help MTL4 pipeline builds either.

### Results

| Scenario | Arm | World max frame | World frames > 50 ms (of 160) | Render-thread compile wait | Prewarm hits / late / misses |
|---|---|---|---|---|---|
| (a) warm | `main` | 436–475 ms | 1 (the archive flush) | 622–1004 ms | – |
| (a) warm | branch, state cache off | 451–479 ms | 1 | 748–1010 ms | – |
| (a) warm | branch, state cache on | **18.3–18.6 ms** | **0** | 193–388 ms, 3 waits, all during loading | 399 / 0 / 1 |
| (b') AIR-changing update | off | 331–351 ms | 99 | 6.8–6.9 s | – |
| (b') AIR-changing update | on, 2 threads | 58–68 ms | **4.7** | **0.29–0.55 s** | 378 / 2 / 20 |
| (b') AIR-changing update | on, 1 thread | 329–344 ms | 55 | 3.9–4.1 s | 171 / 4 / 225 |
| (b) fresh AIR + PSO dirs, same AIR | off | 331–369 ms | 100 | 8.0–8.1 s | – |
| (b) fresh AIR + PSO dirs, same AIR | on | 34–108 ms | 19 | 0.1–2.7 s | 324 / 3 / 73 |
| (c) eviction (warm AIR, no archives) | off | 352–363 ms | 100 | 8.1–8.4 s | – |
| (c) eviction (warm AIR, no archives) | on | 90–101 ms | 27 | 1.9–2.4 s | 292 / 5 / 103 |

- **(a) warm, no regression:**
  - `main` vs this branch with the cache off, paired runs 2–4: render
    waits differ by +3, +6 and +10 ms on 830–1010 ms; per-PSO build
    2.2 vs 2.3 ms. That is noise; the code difference is one atomic load per
    PSO miss.
  - With the cache on, the world is flat (16.7 ms frames, max 18.6 ms). The
    0.44 s archive flush moves into the loading phase, where the prewarm's
    256th build triggers it.
  - The 399-pipeline prewarm (archive hits) finishes 0.8 s after startup
    and is paused 0.2–0.7 s of that.
- **Render-thread cost of recording:** 47–62 ns per PSO miss (native arm64
  micro-benchmark, `state_cache_test bench`). That is ≈ 20 µs per SWTOR
  session (350 misses), against ≥ 0.45 ms that each miss already costs.
  The hit paths are unchanged code.
- **Cold:**
  - The remaining misses are the pipelines the draws reached before the
    prewarm did. Here the lead is 3 s for 400 builds at ≈ 21–26 ms each
    (5.3 s of work on 2 threads), and in (b)/(c) the AIR compiles of the
    fresh shaders take part of those 3 s too.
  - SWTOR's lead is ≈ 20 s to the world (§1.2) for ≈ 350 pipelines, so the
    world set should be ready well before it is needed.
  - One thread is too slow for the cold case; 2 is the default.
- **Prewarm vs the game's own compiles and frames ("overlap": the world
  starts at frame 0 while the prewarm has 400 cold builds queued):**
  - The prewarm yields almost completely: paused 13.7 s, 7–13 builds.
  - On-demand builds are not slower: 23.0 vs 22.6 ms per PSO with no
    prewarm. World frames: 50.9 vs 50.3 ms average.
  - In (b') the misses built on demand while the prewarm ran took
    20.5 ms vs 21.8 ms without a prewarm.
  - So with the yield rule the prewarm costs the game nothing measurable.
    The measured case where it can't help is a game that needs everything
    at once.
- **The grace period that was dropped.** The design paused the prewarm for
  50 ms after every render-thread wait. With it, (b) and (c) paused
  4.3–4.7 s and missed 104–114 of 400 pipelines (first measurement batch):
  once the draws catch up, each miss is a wait, and each wait stopped the
  prewarm. Without it: 73–103 misses, (b') 20 instead of 21. The pause now
  lasts only while an on-demand compile is pending.
- **`THREAD_PRIORITY_LOWEST` under Wine.**
  - `ps -M` during a run shows it maps to a lower Mach priority: 17 instead
    of the default 31, for the prewarm and writer threads and for compile
    workers doing prefetch.
  - That is scheduling precedence, not a QoS class. It does not move the
    threads to E-cores, nor the XPC requests to `MTLCompilerService`, which
    carry the caller's QoS.
  - The yield rule, not the priority, is what keeps the prewarm out of the
    game's way (above). A real QoS class is the bridge follow-up (§9).

### PSO archive growth
- The synthetic pipelines take ≈ 51 KB each in an archive (400 PSOs = 12.6 +
  8.0 MB per session). SWTOR's real archives are 9–18 MB per session for
  340–370 PSOs, ≈ 30–50 KB each (file sizes only).
- Every session's archive captures every pipeline the compiler builds, so
  with the state cache it holds every prewarmed pipeline, not just the
  ~350 used.
- With N recorded pipelines, a session archive is ≈ N × 45 KB, and 8 are
  kept:

  | N | Per session | 8 sessions |
  |---|---|---|
  | today's ~350 | ≈ 16 MB | ≈ 130 MB |
  | 1000 | ≈ 45 MB | ≈ 360 MB |
  | the default cap, 1024 | ≈ 46 MB | ≈ 370 MB |
  | the old cap, 4096 | ≈ 185 MB | ≈ 1.5 GB |

- The default cap is therefore 1024 until the bridge follow-up below
  lands. Eviction is least recently used, so the pipelines of recent
  sessions stay. In Wine, `D9MT_STATE_CACHE_MAX=64` on a 400-entry file
  dropped 336 and wrote 64 (24 640 bytes).

- Proposed bridge follow-up, with #11 and the QoS thunk: with a state
  cache, the newest one or two sessions already hold everything recorded,
  so the bridge could keep 2–3 sessions instead of 8. That bounds the total
  near 2–3 × N × 45 KB.

## 13. In-game A/B (Jan)

The `som.sh` line for shipping:

```sh
export D9MT_STATE_CACHE="$(getconf DARWIN_USER_CACHE_DIR)d9mt/state"
```

`getconf` prints the directory with a trailing `/`, hence no slash before
`d9mt`. Without it the cache goes to `~/Library/Caches/d9mt/state` (§4).
With it, it sits next to the AIR cache and PSO archives.

For the A/B, quit SWTOR, then:

1. **Install.** Put this branch's `build/driver-x64/d3d9.dll` into the
   prefix's `system32` the way you install dev builds, and keep the
   original. `winemetal` stays as it is: the engine's bridge is
   byte-identical.
2. **Env.** Add these lines to `~/Library/Application Support/SWTOR on
   Mac/env`, below the d9mt block. The Settings window rewrites that file,
   so add them again after changing settings.

   ```sh
   D9MT_STATE_CACHE=/tmp/swtor-sc/state
   D9MT_AIR_CACHE=/tmp/swtor-sc/<run>/air-cache.sqlite3
   D9MT_PSO_ARCHIVE_DIR=/tmp/swtor-sc/<run>/pso
   ```

   Use a new `<run>` name for each cold session. Debug Logging stays on, for
   `STATECACHE`, `COMPILE`, `PSOBUILD` and `HITCH`.
3. **Session R (record).** `<run>=R`, the standard route. This is also a
   fully cold session without a state cache.
   - At the end, `wine.log` has `STATECACHE write entries=…`.
   - `/tmp/swtor-sc/state/swtor.exe.d9mtstate` exists, ≈ 384 bytes per
     pipeline.
4. **Session C1 (cold, no state cache).** `<run>=C1` plus
   `D9MT_STATE_CACHE=0`, the same route.
5. **Session S1 (cold, with the state cache).** `<run>=S1`, with
   `D9MT_STATE_CACHE=/tmp/swtor-sc/state` as in step 2, the same route.
   - `STATECACHE prewarm t=…` should show the prewarm finishing during
     login/character select.
   - The `use` lines show hits/late/misses.
6. **Compare.** `session-report.sh --compare <C1 wine.log> <S1 wine.log>`.
   Expect most of C1's in-world `renderWait(compiles)` to disappear in S1.
7. **Warm A/B.** Remove the `D9MT_AIR_CACHE` / `D9MT_PSO_ARCHIVE_DIR` lines
   to use your real caches, and run the route:
   - once with `D9MT_STATE_CACHE=0`;
   - once with `D9MT_STATE_CACHE=/tmp/swtor-sc/state`.

   Compare with `--compare`. Expected: no regression. In-world PSO waits
   drop to ≈ 0, and the 0.5 s flush moves to login.
8. **Optional.** Repeat 4–5 once more with fresh `<run>` names (C2, S2) for
   a second pair.

Afterwards, delete `/tmp/swtor-sc` and the env lines, and restore the
original `d3d9.dll`.
