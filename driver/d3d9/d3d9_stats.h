#pragma once

// D9MT_PROFILE: one warn-level STATS line per second (app thread, after each
// Present), for test sessions and scripts/session-report.sh in SWTOR on Mac:
//
//   STATS t=63 fps=59.9 ms=16.7/18.0/21.4 slow33=0 slow50=0 | rp=31.4 skip=2.1
//     cb=13.0 draws=1520 | ld=12.1 clr=9.0 ldDc=4.0 st=15.2 stDc=6.0 rsv=0.0
//     restore=0.0 savedMiB=231.4 | sh=0 pso=0 wait=0/0.0ms | allocMiB=5210 att=on
//
//   t           seconds since the first Present
//   fps         Presents per second over the window
//   ms          Present-to-Present frame time: average / 99th percentile / max
//   slow33/50   frames of the window that took longer than 33.4 / 50 ms
//   rp ... savedMiB   per-frame averages from the render thread:
//     rp        render encoders (draw passes, clears, blits, resolves, restores)
//     skip      draw/clear passes that needed no encoder at all
//     cb        command buffers committed
//     draws     draws encoded
//     ld/clr/ldDc   attachment loads: Load / Clear / DontCare
//     st/stDc   attachment stores: Store / DontCare
//     rsv       MSAA resolves folded into the rendering pass's store
//     restore   skipped stores that had to be redone because something read them
//     savedMiB  estimated attachment traffic avoided vs. Load/Store everywhere
//   sh/pso      shaders / pipelines compiled in the window
//   wait        render-thread waits for compiles: count / total ms
//   allocMiB    Metal's currentAllocatedSize
//   att         attachment optimization: on, off (startup) or live-off (no_attach_opt)
//
// GPU time isn't reported: MTL4 command buffers have no GPUStartTime /
// GPUEndTime (it would take MTL4CommitFeedback handlers in the bridge).
//
// Off (D9MT_PROFILE unset): never constructed or called. Header-only and
// dependency-free so the window logic and the format can be unit-tested
// natively (test/native/stats_test.cpp).

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "../mtl/mtl_render_stats.h"

namespace dxvk::stats {

  struct CompileTotals {
    uint64_t shaders = 0u, psos = 0u, waits = 0u, waitsNs = 0u;
  };

  struct Extra {
    uint64_t    allocBytes = 0u;
    const char* att        = "on";
  };

  class Window {

  public:

    static constexpr uint64_t PeriodNs = 1000000000ull;

    // One Present. Returns true when a line is due (a second has passed).
    bool addFrame(uint64_t nowNs) {
      if (!m_startNs) {
        m_startNs = m_winStartNs = m_lastNs = nowNs;
        return false;
      }
      m_dts.push_back(float(double(nowNs - m_lastNs) * 1e-6));
      m_lastNs = nowNs;
      return nowNs - m_winStartNs >= PeriodNs;
    }

    // The line for the window that ends now; starts the next window.
    std::string take(uint64_t nowNs, const rstats::Snapshot& rs, const CompileTotals& ct, const Extra& extra) {
      const double winS = double(nowNs - m_winStartNs) * 1e-9;
      const size_t n = m_dts.size();

      double sum = 0.0, mx = 0.0;
      uint32_t slow33 = 0u, slow50 = 0u;
      for (float dt : m_dts) {
        sum += dt;
        mx = std::max(mx, double(dt));
        slow33 += dt > 33.4f ? 1u : 0u;
        slow50 += dt > 50.0f ? 1u : 0u;
      }
      double p99 = 0.0;
      if (n) {
        std::vector<float> sorted = m_dts;
        const size_t k = std::min(n - 1u, size_t(double(n) * 0.99));
        std::nth_element(sorted.begin(), sorted.begin() + k, sorted.end());
        p99 = sorted[k];
      }

      // Render-thread counters: per frame of the render thread in this window.
      uint64_t d[rstats::Count];
      for (uint32_t i = 0u; i < rstats::Count; i++)
        d[i] = rs.v[i] - m_rs.v[i];
      const double rf = d[rstats::Frames] ? double(d[rstats::Frames]) : 1.0;
      auto pf = [&] (uint32_t c) { return double(d[c]) / rf; };
      const double savedMiB = double(int64_t(d[rstats::BytesSaved])) / rf / double(1u << 20);

      char buf[640];
      std::snprintf(buf, sizeof(buf),
        "STATS t=%.0f fps=%.1f ms=%.1f/%.1f/%.1f slow33=%u slow50=%u"
        " | rp=%.1f skip=%.1f cb=%.1f draws=%.0f"
        " | ld=%.1f clr=%.1f ldDc=%.1f st=%.1f stDc=%.1f rsv=%.1f restore=%.1f savedMiB=%.1f"
        " | sh=%llu pso=%llu wait=%llu/%.1fms | allocMiB=%llu att=%s",
        double(nowNs - m_startNs) * 1e-9, winS > 0.0 ? double(n) / winS : 0.0,
        n ? sum / double(n) : 0.0, p99, mx, slow33, slow50,
        pf(rstats::RenderPasses), pf(rstats::PassesSkipped), pf(rstats::CmdBufs), pf(rstats::Draws),
        pf(rstats::LoadLoad), pf(rstats::LoadClear), pf(rstats::LoadDontCare),
        pf(rstats::StoreStore), pf(rstats::StoreDontCare), pf(rstats::ResolveFolded),
        pf(rstats::Restores), savedMiB,
        (unsigned long long)(ct.shaders - m_ct.shaders), (unsigned long long)(ct.psos - m_ct.psos),
        (unsigned long long)(ct.waits - m_ct.waits), double(ct.waitsNs - m_ct.waitsNs) * 1e-6,
        (unsigned long long)(extra.allocBytes >> 20), extra.att);

      m_rs = rs;
      m_ct = ct;
      m_winStartNs = nowNs;
      m_dts.clear();
      return buf;
    }

  private:

    uint64_t           m_startNs    = 0u;
    uint64_t           m_winStartNs = 0u;
    uint64_t           m_lastNs     = 0u;
    std::vector<float> m_dts;
    rstats::Snapshot   m_rs;
    CompileTotals      m_ct;

  };

}
