#pragma once

// D9MT_PROFILE: one warn-level PACE line per second (app thread, after each
// Present, next to STATS): how evenly the game calls Present, and how a frame
// splits into d9mt's D3D9 entries on the device and everything else. For
// telling dense-scene judder from uneven CPU frame cost, and d9mt's per-draw
// cost (under Rosetta) from the game's own work. d9mt's share: see d9mt below.
//
//   PACE t=63 n=60 present=15.2/16.7/18.4 sd=0.61 | d9mt=4.1/5.0/7.9 wait=1.2
//     game=9.0/11.7/13.0 | cpu=812 d9mtBusy=230 gameCpu=582
//
//   t        seconds since the first Present (as STATS)
//   n        Present calls in the window
//   present  interval between the game's Present calls (taken when Present is
//            entered, before d9mt's frame latency wait): min / avg / max ms
//   sd       standard deviation of those intervals, ms (at a steady 60 fps on a
//            60 Hz display, even pacing is 16.7 with an sd near 0)
//   d9mt     per frame, per-device outer-entry time: the wall time of the D3D9 entries on this
//            device (any thread, Present included) that began while no other entry was open
//            on it; one that begins inside another thread's isn't counted, even after that
//            one leaves. min / avg / max ms. From one calling thread: that thread's d9mt time
//   wait     per frame, average: blocked time on this device from any thread, counted in d9mt
//            or not (frame latency wait, the CS thread, the GPU, staging throttle, the hidden
//            window's sleep, the D9MT_FPS_CAP sleep), ms
//   game     per frame: the frame minus d9mt (the game's own work and waits): min / avg / max ms
//   cpu      GetThreadTimes(GetCurrentThread()) on the Present thread (user + kernel), ms per
//            second; on Wine for macOS that is the whole process's CPU time, every thread
//   d9mtBusy d9mt - wait (at least 0), ms per second: wall time, not CPU time
//   gameCpu  cpu - d9mtBusy, ms per second: cpu less the per-device figure, not a thread's CPU
//
// Off (D9MT_PROFILE unset): never constructed or called. Header-only and
// dependency-free so the window logic and the format can be unit-tested
// natively (test/native/pace_test.cpp).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace dxvk::pace {

  class Window {

  public:

    static constexpr uint64_t PeriodNs = 1000000000ull;

    // The game entered Present (before any d9mt work for it).
    void presentEntry(uint64_t nowNs) {
      if (m_lastEntryNs) {
        const double dt = double(nowNs - m_lastEntryNs) * 1e-6;
        m_present.add(dt);
      }
      m_lastEntryNs = nowNs;
    }

    // Present returned. `dtMs`: Present-to-Present frame time (0 for the first
    // frame), `inD9mtMs` the device's outer-entry time in that frame (d9mt),
    // `waitMs` the device's blocked time (wait). Returns true when a line is due.
    bool frameEnd(uint64_t nowNs, double dtMs, double inD9mtMs, double waitMs) {
      if (!m_startNs) {
        m_startNs = m_winStartNs = nowNs;
        return false;
      }
      if (dtMs > 0.0) {
        m_d9mt.add(inD9mtMs);
        m_game.add(std::max(0.0, dtMs - inD9mtMs));
        m_waitMs += waitMs;
        m_inMs   += inD9mtMs;
      }
      return nowNs - m_winStartNs >= PeriodNs;
    }

    // Seconds since the first Present (the line's t).
    double seconds(uint64_t nowNs) const {
      return m_startNs ? double(nowNs - m_startNs) * 1e-9 : 0.0;
    }

    // The line for the window that ends now; starts the next window.
    // `threadCpuNs`: GetThreadTimes' total so far (0: unknown; see cpu above).
    std::string take(uint64_t nowNs, uint64_t threadCpuNs) {
      const double winS = double(nowNs - m_winStartNs) * 1e-9;
      const double perS = winS > 0.0 ? 1.0 / winS : 0.0;
      const double cpuMs = (threadCpuNs && m_cpuNs && threadCpuNs >= m_cpuNs)
        ? double(threadCpuNs - m_cpuNs) * 1e-6 * perS : -1.0;
      const double busyMs = std::max(0.0, m_inMs - m_waitMs) * perS;
      const uint32_t frames = m_d9mt.n;

      char buf[512];
      int len = std::snprintf(buf, sizeof(buf),
        "PACE t=%.0f n=%u present=%.1f/%.1f/%.1f sd=%.2f | d9mt=%.1f/%.1f/%.1f wait=%.1f"
        " game=%.1f/%.1f/%.1f |",
        double(nowNs - m_startNs) * 1e-9, m_present.n,
        m_present.min(), m_present.avg(), m_present.max, m_present.sd(),
        m_d9mt.min(), m_d9mt.avg(), m_d9mt.max, frames ? m_waitMs / double(frames) : 0.0,
        m_game.min(), m_game.avg(), m_game.max);
      if (cpuMs >= 0.0)
        std::snprintf(buf + len, sizeof(buf) - size_t(len), " cpu=%.0f d9mtBusy=%.0f gameCpu=%.0f",
          cpuMs, busyMs, std::max(0.0, cpuMs - busyMs));
      else
        std::snprintf(buf + len, sizeof(buf) - size_t(len), " cpu=n/a d9mtBusy=%.0f", busyMs);

      if (threadCpuNs)
        m_cpuNs = threadCpuNs;
      m_winStartNs = nowNs;
      m_present = Series();
      m_d9mt    = Series();
      m_game    = Series();
      m_waitMs  = 0.0;
      m_inMs    = 0.0;
      return buf;
    }

  private:

    // min / max / mean / standard deviation of a window's values (Welford)
    struct Series {
      uint32_t n    = 0u;
      double   lo   = 0.0;
      double   max  = 0.0;
      double   mean = 0.0;
      double   m2   = 0.0;

      void add(double v) {
        if (!n || v < lo) lo = v;
        if (!n || v > max) max = v;
        n++;
        const double d = v - mean;
        mean += d / double(n);
        m2 += d * (v - mean);
      }

      double min() const { return n ? lo : 0.0; }
      double avg() const { return n ? mean : 0.0; }
      double sd()  const { return n > 1u ? std::sqrt(m2 / double(n)) : 0.0; }
    };

    uint64_t m_startNs     = 0u;
    uint64_t m_winStartNs  = 0u;
    uint64_t m_lastEntryNs = 0u;
    uint64_t m_cpuNs       = 0u;
    Series   m_present;
    Series   m_d9mt;
    Series   m_game;
    double   m_waitMs      = 0.0;
    double   m_inMs        = 0.0;

  };

}
