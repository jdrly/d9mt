# feat/state-cache-journal: merge notes

From the review (review-014/state-cache-journal.md, 0a2ecf2d: MERGE). Trial merges of ddf8588 in
n013-perf/wt/trial-merge, 2026-10-21.

## Against bundle/bridge-s1-pso (25b6bf4): two conflicts

1. `driver/mtl/mtl_state_cache.cpp`, `load()`, the session number. The bundle (08a8795, the
   coverage predicate) restarts the count when the file's entries were dropped; this branch
   counts past a killed session's journal records. Take **both**: the journal block from this
   branch, then the bundle's `restarted`, with one combined line:

   ```cpp
   const bool restarted = r.status == ParseStatus::KeyMismatch || r.status == ParseStatus::BadHeader
                       || r.status == ParseStatus::OlderVersion;
   m_session = restarted ? 1u : std::max(r.session, jr.maxSession) + 1u;
   ```

   After a restart the journal had the old key signature, so `parseJournal` returned
   KeyMismatch and the journal was removed (`jr.maxSession` is 0). Otherwise sessions that ended
   through `ExitProcess` and only journaled count towards `psoArchiveCoverage`'s ≥ 8 sessions,
   which is what SWTOR needs.
   - The journal block has to come **before** the `restarted` line: `jr` is declared in it.
2. `test/native/state_cache_test.cpp`, `main()`: take both test lists:
   `testJournalFormat(); testJournalSessions(); testJournalTwoProcesses();` and `testCoverage();`.

## Against fix/pso-key-normalize (64f3269)

- No textual conflict in the trial merges, either on ddf8588 alone or after the bundle merge
  above. If one shows up in the test fixtures (another merge order), take both sides: pso-key's
  `topologyClass = 0` in the key fixtures, and the journal tests unchanged. The journal tests
  build entries through `makeEntry`/`Workload` and validate them with `FakeBackend`, not
  `backend::psoStateValid`.
- The mapping version: pso-key's bump to 4 is the only one. The journal header carries the main
  file's `keySignature`, so an old journal reads as KeyMismatch and is removed with the old file.
  No journal-format bump.

## After merging

`scripts/test-state-cache.sh` (plain + TSan) on the merge result.
