// Native unit test for DIRECT buffer use tracking and WRITEONLY demotion (no Wine, no Metal):
//   clang++ -std=c++17 -I driver/d3d9 -I driver/mtl test/native/buffer_tracking_test.cpp -o /tmp/btt && /tmp/btt
//
// Part 1 checks the policy itself. Part 2 is a model of the frontend (CS chunk
// numbering, draws stamping the bound buffer, DISCARD renames, in-place locks)
// that replays SWTOR-like lock patterns and counts how many locks would have
// to synchronize with the CS thread, compared with upstream DXVK's
// SynchronizeAll for DIRECT buffers.
#include "d3d9_buffer_tracking.h"
#include "mtl_memory_profile.h"

#include <cstdio>
#include <cstdlib>

using namespace dxvk;
using Action = D3D9DirectBufferTracker::LockAction;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

namespace {

  // Frontend model: chunks are numbered like D3D9DeviceEx (dispatch returns
  // ++seq; the chunk being recorded is seqDispatched + 1 once non-empty). The
  // CS thread executes chunks in order, lagging by `lag` chunks.
  struct Model {
    uint64_t dispatched = 0u;  // m_csSeqNum
    uint64_t executed   = 0u;  // m_csThread.lastSequenceNumber()
    bool     chunkEmpty = true;
    uint32_t lag        = 2u;

    enum class Mode { Direct, Buffer } mode = Mode::Direct;
    bool     tracked   = true;   // false = upstream DXVK (SynchronizeAll)
    bool     demotable = true;
    uint32_t demoteAfter = 2u;
    D3D9DirectBufferTracker tracker;

    // Results.
    uint32_t locks = 0u, csSyncs = 0u, demotions = 0u;

    uint64_t currentSeq() const { return chunkEmpty ? dispatched : dispatched + 1u; }

    void flush() {
      if (chunkEmpty)
        return;
      dispatched++;
      chunkEmpty = true;
      executed = dispatched > lag ? dispatched - lag : 0u;
    }

    void draw() {
      chunkEmpty = false;              // the draw is emitted first...
      if (mode == Mode::Direct && tracked)
        tracker.onUse(currentSeq());   // ...then stamped (TrackDirectBufferUses)
    }

    void discardLock() {
      chunkEmpty = false;              // invalidateBuffer command
      if (mode == Mode::Direct && tracked)
        tracker.onRename();
      locks++;
    }

    // Lock without DISCARD/NOOVERWRITE. Returns true if it synchronized.
    bool inPlaceLock() {
      locks++;
      if (mode == Mode::Buffer)
        return false;                  // BUFFER mode never waits (no readback)

      uint64_t seq = ~0ull;            // upstream: SynchronizeAll
      if (tracked) {
        switch (tracker.onInPlaceLock(demotable, demoteAfter, true)) {
          case Action::NoWait: return false;
          case Action::Demote: mode = Mode::Buffer; demotions++; return false;
          case Action::Wait:   seq = tracker.lastUse(); break;
        }
      }

      // WaitForResource -> SynchronizeCsThread(seq): flushes the current chunk
      // if seq is in it, waits if the CS thread hasn't executed seq yet.
      const bool synced = seq == ~0ull
        ? (dispatched > executed || !chunkEmpty)
        : D3D9DirectBufferTracker::needsCsSync(seq, executed);
      if (seq != ~0ull && seq > dispatched)
        flush();
      if (synced) {
        flush();
        executed = dispatched;
        csSyncs++;
      }
      return synced;
    }

    void endFrame() { flush(); }
  };

}

int main() {
  // --- Part 1: policy -------------------------------------------------------

  { // Fresh backing: free, not counted.
    D3D9DirectBufferTracker t;
    CHECK(!t.used());
    CHECK(t.onInPlaceLock(true, 2u, true) == Action::NoWait);
    CHECK(t.onInPlaceLock(true, 2u, true) == Action::NoWait);  // piecewise fill
    CHECK(t.inPlaceLocks() == 0u);
  }

  { // Used backing: wait up to the last use; demote on the (N+1)th lock.
    D3D9DirectBufferTracker t;
    t.onUse(5u);
    t.onUse(3u);                       // out of order never lowers it
    CHECK(t.lastUse() == 5u);
    CHECK(t.onInPlaceLock(true, 2u, true) == Action::Wait);
    CHECK(t.onInPlaceLock(true, 2u, true) == Action::Wait);
    CHECK(t.onInPlaceLock(true, 2u, true) == Action::Demote);
    CHECK(t.inPlaceLocks() == 3u);
  }

  { // demoteAfter = 0 (unlimited/legacy, D9MT_WRITEONLY_DEMOTE=never): never.
    D3D9DirectBufferTracker t;
    t.onUse(1u);
    for (int i = 0; i < 100; i++)
      CHECK(t.onInPlaceLock(true, 0u, true) == Action::Wait);
  }

  { // Not demotable (DYNAMIC, or SWVP): waits, never demotes.
    D3D9DirectBufferTracker t;
    t.onUse(1u);
    for (int i = 0; i < 10; i++)
      CHECK(t.onInPlaceLock(false, 2u, true) == Action::Wait);
  }

  { // Still locked / needs readback: demotion postponed to the next lock.
    D3D9DirectBufferTracker t;
    t.onUse(1u);
    CHECK(t.onInPlaceLock(true, 1u, true) == Action::Wait);
    CHECK(t.onInPlaceLock(true, 1u, false) == Action::Wait);
    CHECK(t.onInPlaceLock(true, 1u, true) == Action::Demote);
  }

  { // DISCARD rename: the new backing is unused again; the count stays.
    D3D9DirectBufferTracker t;
    t.onUse(7u);
    CHECK(t.onInPlaceLock(true, 2u, true) == Action::Wait);
    t.onRename();
    CHECK(!t.used() && t.lastUse() == 0u);
    CHECK(t.onInPlaceLock(true, 2u, true) == Action::NoWait);
    CHECK(t.inPlaceLocks() == 1u);
    t.onUse(9u);
    CHECK(t.onInPlaceLock(true, 2u, true) == Action::Wait);
    CHECK(t.onInPlaceLock(true, 2u, true) == Action::Demote);
  }

  { // CS sync only when the last use hasn't executed.
    CHECK(!D3D9DirectBufferTracker::needsCsSync(0u, 0u));
    CHECK(!D3D9DirectBufferTracker::needsCsSync(5u, 5u));
    CHECK(!D3D9DirectBufferTracker::needsCsSync(5u, 9u));
    CHECK(D3D9DirectBufferTracker::needsCsSync(6u, 5u));
  }

  { // D9MT_WRITEONLY_DEMOTE parsing and the profile defaults.
    CHECK(parseDemoteLocks("3", 2u) == 3u);
    CHECK(parseDemoteLocks("0", 2u) == 0u);
    CHECK(parseDemoteLocks("never", 2u) == 0u && parseDemoteLocks("OFF", 2u) == 0u);
    CHECK(parseDemoteLocks("", 2u) == 2u && parseDemoteLocks("lots", 2u) == 2u);
    CHECK(parseDemoteLocks("1234567890", 2u) == 2u);
    CHECK(memoryProfileForTier(MtlMemoryTier::Unlimited).writeOnlyDemoteLocks == 0u);
    for (MtlMemoryTier x : { MtlMemoryTier::Low, MtlMemoryTier::Medium, MtlMemoryTier::High, MtlMemoryTier::Ultra })
      CHECK(memoryProfileForTier(x).writeOnlyDemoteLocks == 2u);
  }

  // --- Part 2: frontend model -------------------------------------------------

  { // Static buffer: fill once (even piecewise), draw every frame for 600
    // frames. No sync, no demotion, tracked or not (no in-place lock).
    Model m;
    for (int i = 0; i < 4; i++) m.inPlaceLock();
    for (int f = 0; f < 600; f++) { for (int d = 0; d < 3; d++) m.draw(); m.endFrame(); }
    CHECK(m.csSyncs == 0u && m.demotions == 0u && m.mode == Model::Mode::Direct);
  }

  { // Buffer rewritten in place once per frame right after being drawn (the
    // SWTOR pattern behind 18,698 bufLockSync in 6 min). Upstream-style
    // SynchronizeAll syncs every frame; tracked: at most demoteAfter syncs,
    // then BUFFER mode forever.
    Model up; up.tracked = false;
    Model tr;
    for (Model* m : { &up, &tr }) {
      m->inPlaceLock();                      // initial fill
      for (int f = 0; f < 600; f++) { m->draw(); m->inPlaceLock(); m->draw(); m->endFrame(); }
    }
    std::printf("in-place per frame, 600 frames: upstream csSyncs=%u, tracked csSyncs=%u demotions=%u\n",
      up.csSyncs, tr.csSyncs, tr.demotions);
    CHECK(up.csSyncs == 600u);
    CHECK(tr.csSyncs <= tr.demoteAfter && tr.demotions == 1u && tr.mode == Model::Mode::Buffer);
  }

  { // Rewritten in place long after its last draw (CS caught up): no sync
    // needed, but it still counts towards demotion.
    Model m; m.lag = 0u;
    m.inPlaceLock();
    m.draw(); m.endFrame();
    for (int i = 0; i < 2; i++) { m.endFrame(); CHECK(!m.inPlaceLock()); }
    CHECK(m.csSyncs == 0u && m.mode == Model::Mode::Direct);
    m.inPlaceLock();
    CHECK(m.mode == Model::Mode::Buffer);
  }

  { // DISCARD every frame (dynamic-style use of a WRITEONLY buffer), plus an
    // in-place append before the draw: never syncs, never demotes.
    Model m;
    for (int f = 0; f < 600; f++) { m.discardLock(); m.inPlaceLock(); m.draw(); m.endFrame(); }
    CHECK(m.csSyncs == 0u && m.demotions == 0u);
  }

  { // Not demotable (DYNAMIC): syncs only when the last draw is still queued.
    Model m; m.demotable = false; m.lag = 0u;
    m.inPlaceLock();
    m.draw(); m.endFrame();            // lag 0: executed right away
    CHECK(!m.inPlaceLock());
    m.draw();                          // still in the current chunk
    CHECK(m.inPlaceLock());
    CHECK(m.csSyncs == 1u && m.mode == Model::Mode::Direct);
  }

  { // Randomized: the tracked model never skips a sync it needs, i.e. a lock
    // that doesn't sync never happens while a chunk with a use of the current
    // backing is unexecuted.
    std::srand(1234);
    for (int run = 0; run < 200; run++) {
      Model m; m.lag = uint32_t(std::rand() % 4); m.demoteAfter = uint32_t(std::rand() % 4);
      uint64_t lastUseShadow = 0u;     // independent bookkeeping
      for (int op = 0; op < 400; op++) {
        switch (std::rand() % 5) {
          case 0: m.draw(); if (m.mode == Model::Mode::Direct) lastUseShadow = m.currentSeq(); break;
          case 1: m.endFrame(); break;
          case 2: m.discardLock(); lastUseShadow = 0u; break;
          default: {
            const bool direct = m.mode == Model::Mode::Direct;
            const bool pending = lastUseShadow > m.executed;
            const bool synced = m.inPlaceLock();
            if (direct && m.mode == Model::Mode::Direct && pending)
              CHECK(synced);
            if (synced)
              CHECK(m.executed >= lastUseShadow);
          }
        }
      }
    }
  }

  if (g_fail) {
    std::printf("%d check(s) failed\n", g_fail);
    return 1;
  }
  std::printf("buffer_tracking_test: all checks passed\n");
  return 0;
}
