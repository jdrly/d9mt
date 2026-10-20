# State cache: an append-only journal for the records since the last write (design)

Status: implemented (n013-perf, 2026-10-21, `feat/state-cache-journal` on 2a1bc00), with the
tests of §7 in `test/native/state_cache_test.cpp` (`testJournalFormat`,
`testJournalSessions`, `testJournalTwoProcesses`); `scripts/test-state-cache.sh` passes plain
and under ThreadSanitizer. No file-format change to the main file and no key-signature bump.
Where the implementation differs from the design below:
- Compaction **removes** the journal after a successful write (§5 step 3 said "truncate to
  its header"). The next append creates it again with a fresh header. A session that never
  appends leaves no journal at all, so a load with no journal costs one failed open.
- A failed append is retried once, after re-preparing the file (another process's write may
  have removed it). If that fails too, the journal is off for the rest of the session, with
  one log line. The main file is still written as before.
- The `load` line reads `journal=<status>/<records>/<replayed>`, plus ` corrupt=`,
  ` torn=` and ` invalid=` when they aren't 0. `Stats` gets `journaled` and
  `journalReplayed`; forged journal records count in `invalid`.
- The session number after a killed session: `max(file's session, journal's newest
  lastSession) + 1`. Otherwise the killed session's records would count as used in this one.
- A journal from a newer d9mt (same magic, newer version) is left alone, and this session
  doesn't journal.

## 1. What is already crash-safe, and what isn't

`docs/state-cache.md` §3 and `driver/mtl/mtl_state_cache.cpp`:
- The file is rewritten whole, atomically: a temp file, `FlushFileBuffers`, then
  `MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH)` (`writeAtomic`, `mtl_state_cache.cpp:235-260`).
  A crash during a write leaves the previous file.
- Every 384-byte entry has its own FNV-1a `check`, and the header has `headerCheck`. A
  truncated file keeps the complete entries before the cut. A damaged entry is skipped.
  `psoStateValid` rejects forged entries.
- The crash-loop guard (`<file>.prewarm`, `<file>.trips`) drops only the prewarm jobs that
  were in flight.

**The gap: records made since the last write are lost** when the process dies without
`shutdown()`. The writer thread (`writerMain`, `:799-833`) drains the record ring every 250 ms
(`intake`, `:1001`). It writes the file 20 s after the last change (`WriteDebounce`), or at
most every 5 minutes while changes keep coming (`WriteMaxInterval`, `:31-32`).
- SWTOR always exits this way: `ExitProcess`, so `~D3D9DeviceEx` and the final write never
  run (`docs/state-cache.md` §3, "Games that never release the device").
- A player who enters a new zone and quits within 20 s loses every PSO that zone added.
  Continuous loading can also hold the write back for up to 5 minutes.
- The next session then compiles those PSOs on the render thread again: the 20–220 ms
  in-world stutters of `docs/state-cache.md` §1.2.

A journal closes this gap without writing the main file more often. Every main-file write
re-reads and merges the file and rewrites up to ~390 KB, so writing it more often is the
wrong fix.

## 2. Format: `<file>.journal`

```
Header, 32 bytes, little-endian:
  char     magic[8]        "D9MTSTJ\0"
  uint32   formatVersion   1
  uint32   entrySize       sizeof(StateCacheEntry) (384)
  uint64   keySignature    the main file's (§2 of state-cache.md)
  uint64   headerCheck     FNV-1a of bytes 0..23
Records: StateCacheEntry, exactly the main file's entry encoding, with its own `check`.
```
- Fixed-size records, each self-checking (`check` covers the entry), so the record
  checksum already exists. The journal adds only a header that ties it to the key
  signature.
- Each record is what `StateDb::use()` made of the entry in this session: `lastSession` is
  this session, plus `sessionsUsed` and `firstUseMs`.

## 3. Writing (writer thread only; nothing on the render thread)

- In `intake()`, after the ring is drained: append one record for each entry that
  `StateDb::use()` *added* or moved to this session (`lastSession` changed). At most one record
  per entry per session, so ~350 records (~134 KB) for a SWTOR session.
  - Entries seen again in the same session are not appended again.
- **One `WriteFile` per intake** with all its records, opened with `FILE_APPEND_DATA`
  (POSIX path: `O_APPEND`) and closed again. Under Wine that is `write(2)` on an
  `O_APPEND` fd, so two processes' appends don't interleave inside a record (whole
  `write`s, local filesystem).
- **No `FlushFileBuffers`**: the journal only has to outlive the *process*. After a kill or
  `ExitProcess`, the data is in the OS page cache. Power loss is out of scope, as it is for the
  marker file.
- **Alignment after a torn tail**: on open, if `(size - 32) % 384 != 0` (a kill in the middle
  of an append), truncate to the last whole record (`SetEndOfFile`) before appending.
  Otherwise every later record would be misaligned. An empty or headerless file gets a
  fresh header.
- A failed append is logged once (like `write failed`, `:1078`) and changes nothing else;
  the main file still gets written as today.

## 4. Load (`load()`, `:835`)

After the main file is parsed and merged (`:886-910`):
1. Read `<file>.journal`. A bad magic, `headerCheck`, `entrySize` or `keySignature`, or a
   newer `formatVersion`: ignore it and delete it (newer: leave it alone, same rule as the
   main file).
2. Every whole record goes through the same filters as a main-file entry, in the same
   order:
   - `check` and `entryValid`;
   - the crash-loop guard's `suspects` (a journal record is exactly what an in-flight
     prewarm or draw added, so the marker must also apply here);
   - `psoStateValid` unless `noValidate`;
   - `m_dropped`.
   Then `m_db.merge(e)`: union by key, max `lastSession`, min `firstUseMs`.
3. A torn tail (a partial record) is ignored. A record with a bad `check` is skipped and
   counted (`journalBad=`).
4. If the journal added anything, mark the cache dirty: the next regular write folds it into
   the main file.
5. The `load` line gets `journal=<records>/<merged>/<bad>`.

## 5. Compaction (`write()`, `:1043`)

`write()` already re-reads and merges the main file. It now also:
1. re-reads the journal *before* serializing and merges it the same way. That picks up other
   processes' appends since our load;
2. writes the main file atomically (unchanged);
3. **only if the write succeeded**: truncates the journal to its header.
   - A crash between 2 and 3 leaves records that are already in the main file; replaying them
     is idempotent, since `merge` is a union.

Race: another process appending between our step 1 and step 3 loses those records. They are
re-recorded the next time they are used. This is the same class, and the same window of a
few ms, as the existing main-file merge race (`docs/state-cache.md` §3, "Merge on write").

## 6. What it costs

- Render thread: nothing. Records reach the writer through the existing ring.
- Writer thread: one append per 250 ms intake that produced new entries. In practice that is
  bursts at zone loads, a few KB each. At load, one extra read of ≤ ~0.4 MB.
- Disk: the journal stays below one session's worth of new entries between main-file writes.
  The cap (`D9MT_STATE_CACHE_MAX`) still applies to the main file. The journal is bounded by
  the number of distinct entries per session, plus at most one write interval of
  re-appends after a crash.

## 7. Tests (add with the implementation)

Native (`test/native/state_cache_test.cpp`, the existing fake backend):
1. **Kill before the write**: record N entries, run `intake` (journal appended), drop the cache
   without `write()`/`shutdown()` (simulates `ExitProcess`). A new cache on the same file
   loads all N.
2. **Torn tail**: cut the journal at a random byte inside the last record. The others load,
   the next append realigns (file size ≡ 32 mod 384), and those records load too.
3. **Damaged record**: flip a byte in record k. Only k is skipped (`journalBad=1`).
4. **Key signature change**: a journal from another signature is ignored and deleted, and
   the main file's rules apply.
5. **Crash-loop guard**: a suspect named by the marker that is only in the journal is dropped
   and not merged back.
6. **Compaction**: after `write()`, the main file has the entries and the journal is
   header-only. A write that fails (read-only dir) leaves the journal intact.
7. **Two processes**: two caches on one file, interleaved intakes and writes. The union ends up
   in the main file, except within the documented race window.
8. **Forged record** with a valid check but an invalid key: rejected by `psoStateValid`, never
   prewarmed.

In-game (Jan): enter a zone not in the cache, quit within 20 s (SWTOR's `ExitProcess`), start
again. The `load` line shows `journal=` with the new entries, and the zone's first-visit
`STATECACHE`/HITCH `psos` waits go away on the second visit.

## 8. Alternatives considered

- **Write the main file more often** (shorter debounce). Every write re-reads, merges and
  rewrites the whole file, and flushes. The cost grows with the cache size, and the window
  only shrinks, it doesn't close.
- **An exit hook** (DllMain `PROCESS_DETACH`, atexit). Rejected for the same reason as in
  state-cache.md §3: after `ExitProcess` the other threads are dead, possibly holding Wine's
  heap or fd locks.
- **sqlite** (as the AIR cache). Transactional, but it means a second sqlite database on
  the PE side, or a bridge call. The bridge would change `winemetal.so` and its LC_UUID,
  which is the AIR cache key.
