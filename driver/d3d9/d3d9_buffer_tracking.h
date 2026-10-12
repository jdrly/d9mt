#pragma once

// Use tracking and the adaptive demotion policy for directly mapped D3D9
// buffers. Header-only and dependency-free so it can be unit-tested natively
// (test/native/buffer_tracking_test.cpp).
//
// A DIRECT buffer is written by the CPU in place. A lock without DISCARD or
// NOOVERWRITE must not race GPU work that still reads the backing the CPU
// maps. Upstream DXVK answers that by synchronizing with the CS thread up to
// "everything" (no sequence number for DIRECT buffers) and then checking the
// GPU. Here every CS chunk that uses the current backing (draws reading the
// bound VB/IB, ProcessVertices writing it) stamps its sequence number, and a
// DISCARD rename resets the stamp, because the new backing has no uses yet.
// A lock then synchronizes only if that chunk has not executed yet, and only
// asks the GPU about a backing that has been used at all.

#include <cstdint>

namespace dxvk {

  class D3D9DirectBufferTracker {

  public:

    enum class LockAction : uint32_t {
      NoWait,   // current backing never used: write in place right away
      Wait,     // synchronize up to lastUse(), then wait if the GPU still uses it
      Demote,   // switch the buffer to BUFFER mode (staging copy, GPU upload)
    };

    // CS chunk sequence number of the latest use of the current backing.
    void onUse(uint64_t seq) {
      if (seq > m_lastUse)
        m_lastUse = seq;
    }

    // DISCARD rename: the new backing has no uses.
    void onRename() { m_lastUse = 0u; }

    uint64_t lastUse() const { return m_lastUse; }

    bool used() const { return m_lastUse != 0u; }

    uint32_t inPlaceLocks() const { return m_inPlaceLocks; }

    // Only CS chunks after lastExecuted can still have to record a use.
    static bool needsCsSync(uint64_t lastUse, uint64_t lastExecuted) {
      return lastUse > lastExecuted;
    }

    // A lock without DISCARD/NOOVERWRITE. Locks of a backing nothing has used
    // yet (initial fills, piecewise fills before the first draw, the first
    // lock after a DISCARD) are free and not counted. Every other one counts;
    // once more than demoteAfter of them happened, a demotable buffer
    // (DEFAULT + WRITEONLY, not DYNAMIC) goes to BUFFER mode for good, so a
    // buffer the game keeps rewriting in place never synchronizes again.
    // demoteAfter = 0: never demote. canDemoteNow = false (still locked,
    // needs a readback) postpones it to the next lock.
    LockAction onInPlaceLock(bool demotable, uint32_t demoteAfter, bool canDemoteNow) {
      if (!used())
        return LockAction::NoWait;

      if (m_inPlaceLocks != ~0u)
        m_inPlaceLocks++;

      if (demotable && demoteAfter && m_inPlaceLocks > demoteAfter && canDemoteNow)
        return LockAction::Demote;

      return LockAction::Wait;
    }

  private:

    uint64_t m_lastUse      = 0u;
    uint32_t m_inPlaceLocks = 0u;

  };

}
