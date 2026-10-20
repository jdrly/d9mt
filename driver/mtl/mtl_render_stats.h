#pragma once

// D9MT_PROFILE: backend totals for the once-a-second STATS line
// (d3d9/d3d9_stats.h). The render (CS) thread counts into a plain array and
// publishes it at each endFrame; the app thread takes snapshots and logs
// per-frame averages over the last second. Never reset.
//
// Header-only and dependency-free (test/native/stats_test.cpp).

#include <atomic>
#include <cstdint>

namespace dxvk::rstats {

  enum Counter : uint32_t {
    Frames = 0,      // endFrame calls on the render thread
    RenderPasses,    // render encoders created (draw passes, clears, blits, resolves, restores)
    PassesSkipped,   // draw/clear passes that needed no encoder (no draws, nothing to store)
    CmdBufs,         // command buffers committed
    Draws,           // draws encoded
    LoadLoad,        // attachment aspects of draw/clear passes: load action Load
    LoadClear,       //   ... Clear
    LoadDontCare,    //   ... DontCare
    StoreStore,      //   store action Store (with or without a folded MSAA resolve)
    StoreDontCare,   //   ... DontCare (with or without a folded MSAA resolve)
    ResolveFolded,   // MSAA color resolves done by the rendering pass's store action
    Restores,        // re-clears of skipped stores because something read the attachment
    BytesSaved,      // estimated attachment bytes not loaded/stored vs. Load/Store (signed, two's complement)
    AttNs,           // render-thread CPU time in the attachment optimization (pass decisions, state, restores), ns
    ClearsFolded,    // clears folded into the load action of the pass that renders to the target next
    ClearPasses,     // clears that needed a pass of their own (no pass of that target pending)
    Splits,          // passes that reopen exactly the attachments the previous pass closed and load them
    SplitsSubmit,    //   ... with a command buffer submit in between
    SplitsXfer,      //   ... else with a copy/blit/resolve encoder in between
    SplitsClear,     //   ... else with a clear in between (the rest: a view change or a vertex fence)
    SplitBytes,      // attachment bytes those passes load
    FlushesCarried,  // heuristic flushes that kept the open pass (G1, D9MT_EARLY_COMMIT)
    CarryCapped,     // ... that ended it after kMaxCarries carries
    Count
  };

  struct Snapshot {
    uint64_t v[Count] = { };
  };

  class Totals {

  public:

    void publish(const uint64_t (&local)[Count]) {
      for (uint32_t i = 0u; i < Count; i++)
        m_v[i].store(local[i], std::memory_order_relaxed);
    }

    Snapshot snapshot() const {
      Snapshot s;
      for (uint32_t i = 0u; i < Count; i++)
        s.v[i] = m_v[i].load(std::memory_order_relaxed);
      return s;
    }

  private:

    std::atomic<uint64_t> m_v[Count] = { };

  };

}
