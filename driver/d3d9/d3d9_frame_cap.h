#pragma once

// D9MT_FPS_CAP=<fps>: a frame rate cap with even frame times, for heat and battery (SWTOR on
// Mac's "Frame Rate Limit"). Present returns on a fixed cadence; the sleep happens outside
// the device lock, after the frame was handed to the CS thread, so the game starts its next
// frame (and samples input) right when the cadence says.
//
//   vsync on, cap = refresh / N (30 at 60 Hz, 30/40/60 at 120 Hz): every frame stays on screen
//     for exactly N refreshes. The cadence is N refresh intervals, and each drawable is
//     presented with presentAfterMinimumDuration((N - 0.5) refreshes), so Core Animation
//     latches it at the N-th vsync after the previous one, whatever the timer's phase against
//     vsync (a plain timer at 33.3 ms beats against a 60 Hz vsync: a frame that becomes ready
//     a hair after a vsync waits for the next one, the one after it is early, and frames
//     alternate between one and three refreshes). D9MT_FPS_CAP_ALIGN=0: the timer only.
//   vsync on, cap not a divisor of the refresh (40 or 45 at 60 Hz): the timer only. Frames
//     leave d9mt evenly (25 or 22.2 ms), but a 60 Hz display can only show them for one or
//     two refreshes each (40: 2,1,2,1...; 45: 2,1,1...): even CPU and GPU load, uneven on
//     screen.
//   vsync on, cap >= refresh / present interval (60 at 60 Hz; 45 with D3DPRESENT_INTERVAL_TWO
//     at 60 Hz, whose ceiling is 30): nothing to do. Vsync, or DXVK's limiter that holds the
//     game's present interval, caps already; a timer on top would only beat against it.
//   vsync off: the timer.
//
// The sleep (Sleeper) is the system's (NtDelayExecution), in halves down to a last piece of at
// most 1 ms, then a spin. macOS coalesces timers: under Wine a sleep wakes late by a share of
// its length (seen on an M5 Max with the display asleep and the machine busy: about half, up
// to 4-10 ms: 0.5 ms of 1 ms, 2.5 of 5, 4.2 of 25), so the long first sleeps get their
// lateness made up by the later, shorter ones. The spin covers what the last piece recently
// overslept, at least D9MT_FPS_CAP_SPIN_US (default 200 us), at most 4 ms. DXVK's Sleep always
// spins 4 timer periods plus a sixth of the sleep, under Wine 4 ms + 1/6: at 30 fps a CPU core
// busy 15-20% of the time, the heat this cap is meant to save. The background throttle
// (D9MT_BG_FPS) sleeps the same way. A Sleeper is per thread (threadSleeper): Present may run
// on several threads of a multithreaded device at once, and it sleeps outside the device lock.
//
// Header-only and dependency-free (the sleep aside) so the plan and the cadence can be
// unit-tested natively (test/native/frame_cap_test.cpp).

#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <chrono>
#include <thread>
#endif

namespace dxvk::framecap {

  enum class Mode : uint32_t {
    Off,    // no cap (none asked for, or vsync caps already)
    Timer,  // cadence 1 / fps
    Vsync,  // cadence N refreshes, presents aligned to vsync
  };

  struct Plan {
    Mode     mode         = Mode::Off;
    uint32_t fps          = 0u;   // the cap asked for
    uint32_t syncInterval = 0u;   // the game's present interval (0 = immediate, no vsync)
    uint32_t refreshes    = 0u;   // Vsync: refreshes per frame (N)
    uint64_t intervalNs   = 0u;   // Present-to-Present cadence, 0 = none
    double   minDurationS = 0.0;  // presentAfterMinimumDuration, 0 = present as usual

    bool operator == (const Plan& o) const {
      return mode == o.mode && fps == o.fps && syncInterval == o.syncInterval
          && refreshes == o.refreshes && intervalNs == o.intervalNs && minDurationS == o.minDurationS;
    }
    bool operator != (const Plan& o) const { return !(*this == o); }
  };

  // A refresh rate the display mode reports (Wine: whole Hz) is usable between 20 and 1000.
  inline bool usableHz(double hz) { return hz >= 20.0 && hz <= 1000.0; }

  // fps: the cap (0 = none); hz: the display's refresh rate (0 = unknown); syncInterval: the
  // game's present interval (0 = immediate: no vsync; 2 = D3DPRESENT_INTERVAL_TWO: at most every
  // second refresh); align: presentAfterMinimumDuration in Vsync mode. A cap at or above the
  // interval's ceiling (refresh / interval) is Off: the existing pacing keeps the game's ceiling.
  inline Plan plan(uint32_t fps, double hz, uint32_t syncInterval, bool align) {
    Plan p;
    p.fps          = fps;
    p.syncInterval = syncInterval;
    if (!fps)
      return p;

    if (syncInterval && usableHz(hz)) {
      if (double(fps) >= hz / double(syncInterval) * 0.99)
        return p;

      const double   ratio = hz / double(fps);
      const uint32_t n     = uint32_t(ratio + 0.5);
      if (n >= 2u && std::fabs(ratio - double(n)) <= 0.02 * ratio) {
        p.mode      = Mode::Vsync;
        p.refreshes = n;
        // N refreshes, or 1 / fps if that is shorter: Wine reports whole Hz, so a 59.94 Hz
        // display may say 59, and a cadence slower than the display would make a frame miss
        // its vsync every few seconds (shown N + 1 refreshes). Slightly faster only queues a
        // frame now and then, which the drawable pool and frame latency hold back.
        const double intervalS = std::fmin(double(n) / hz, 1.0 / double(fps));
        p.intervalNs   = uint64_t(intervalS * 1e9 + 0.5);
        p.minDurationS = align ? (double(n) - 0.5) / hz : 0.0;
        return p;
      }
    }

    p.mode       = Mode::Timer;
    p.intervalNs = uint64_t(1e9 / double(fps) + 0.5);
    return p;
  }

  // The log line for a plan ("D9MT: " and the time stamp come from the caller).
  inline std::string describe(const Plan& p, double hz) {
    const bool vsync = p.syncInterval != 0u;
    char buf[256];
    switch (p.mode) {
      case Mode::Off:
        if (p.syncInterval > 1u)
          std::snprintf(buf, sizeof(buf), "frame cap %u fps: off (present interval %u at %.0f Hz caps at %.0f fps already)",
            p.fps, p.syncInterval, hz, hz / double(p.syncInterval));
        else
          std::snprintf(buf, sizeof(buf), "frame cap %u fps: off (vsync at %.0f Hz caps already)", p.fps, hz);
        break;
      case Mode::Vsync:
        std::snprintf(buf, sizeof(buf), "frame cap %u fps: every %u%s refresh at %.0f Hz, cadence %.2f ms%s",
          p.fps, p.refreshes, p.refreshes == 2u ? "nd" : p.refreshes == 3u ? "rd" : "th", hz,
          double(p.intervalNs) * 1e-6,
          p.minDurationS > 0.0 ? ", presents aligned to vsync" : ", timer only (D9MT_FPS_CAP_ALIGN=0)");
        if (p.minDurationS > 0.0) {
          std::string s = buf;
          std::snprintf(buf, sizeof(buf), " (minimum duration %.2f ms)", p.minDurationS * 1e3);
          return s + buf;
        }
        break;
      case Mode::Timer:
        if (vsync && usableHz(hz))
          std::snprintf(buf, sizeof(buf), "frame cap %u fps: timer, cadence %.2f ms (not a divisor of %.0f Hz:"
            " frames stay on screen for uneven numbers of refreshes)", p.fps, double(p.intervalNs) * 1e-6, hz);
        else
          std::snprintf(buf, sizeof(buf), "frame cap %u fps: timer, cadence %.2f ms (%s)", p.fps,
            double(p.intervalNs) * 1e-6, vsync ? "refresh rate unknown" : "vsync off");
        break;
    }
    return buf;
  }

  // Present-to-Present deadlines on a fixed grid. A frame that ends late by less than one
  // interval keeps the grid (the next frame makes up for it, as DXVK's FpsLimiter does); later
  // than that, the grid restarts from now, with no burst of catch-up frames.
  class Cadence {

  public:

    // `nowNs`: the frame is done (Present handed it over). Returns the time to sleep until, or
    // 0 for no sleep. A changed interval restarts the grid.
    uint64_t next(uint64_t nowNs, uint64_t intervalNs) {
      if (intervalNs != m_intervalNs) {
        m_intervalNs = intervalNs;
        m_nextNs     = 0u;
      }
      if (!intervalNs)
        return 0u;

      const uint64_t until = nowNs < m_nextNs ? m_nextNs : 0u;
      m_nextNs = (m_nextNs && nowNs < m_nextNs + intervalNs)
        ? m_nextNs + intervalNs
        : nowNs + intervalNs;
      return until;
    }

    // The cap paused (window hidden, presents skipped): start a new grid afterwards.
    void reset() { m_nextNs = 0u; }

  private:

    uint64_t m_intervalNs = 0u;
    uint64_t m_nextNs     = 0u;

  };

  // A whole decimal number: digits only (no sign, point or suffix), at most 9 of them.
  inline bool parseUint(const char* v, uint32_t& out) {
    if (!v || !*v)
      return false;
    uint32_t n = 0u, digits = 0u;
    for (; *v; v++) {
      if (!std::isdigit(static_cast<unsigned char>(*v)) || ++digits > 9u)
        return false;
      n = n * 10u + uint32_t(*v - '0');
    }
    out = n;
    return true;
  }

  // D9MT_FPS_CAP: unset, empty or 0 = off; 20..1000 = the cap (below 20, a drawable held for N
  // refreshes would outlast the presenter's drawable timeout). Anything else (5, 30abc, 30.5) is
  // off and sets *invalid.
  inline uint32_t capFromEnv(const char* v, bool* invalid = nullptr) {
    if (invalid)
      *invalid = false;
    if (!v || !*v)
      return 0u;
    uint32_t fps = 0u;
    const bool ok = parseUint(v, fps) && (fps == 0u || (fps >= 20u && fps <= 1000u));
    if (!ok) {
      if (invalid)
        *invalid = true;
      return 0u;
    }
    return fps;
  }

  // D9MT_FPS_CAP_SPIN_US: 0..5000 (more is 5000), default 200; not a whole number: the default.
  inline uint64_t spinNsFromEnv(const char* v) {
    uint32_t us = 0u;
    if (!parseUint(v, us))
      return 200000u;
    return uint64_t(us > 5000u ? 5000u : us) * 1000u;
  }

  // The system's sleep for `ns` nanoseconds.
  inline void systemSleep(uint64_t ns) {
#ifdef _WIN32
    using NtDelayExecutionProc = LONG (WINAPI *) (BOOLEAN, LARGE_INTEGER*);
    static const NtDelayExecutionProc s_delay = [] {
      HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
      return ntdll ? reinterpret_cast<NtDelayExecutionProc>(::GetProcAddress(ntdll, "NtDelayExecution")) : nullptr;
    } ();
    if (s_delay) {
      LARGE_INTEGER t;
      t.QuadPart = -int64_t((ns + 99u) / 100u);  // relative, 100 ns units
      s_delay(FALSE, &t);
    } else {
      ::Sleep(DWORD(ns / 1000000u));
    }
#else
    std::this_thread::sleep_for(std::chrono::nanoseconds(ns));
#endif
  }

  // Sleeps to a deadline: the system's sleep in halves, then a spin as long as the last piece
  // recently overslept. One per thread that sleeps (not thread-safe).
  class Sleeper {

  public:

    static constexpr uint64_t MaxSpinNs  = 4000000u;
    static constexpr uint64_t LastNs     = 1000000u;  // the last piece is at most this long

    explicit Sleeper(uint64_t floorNs = 200000u)
    : m_floorNs(floorNs < MaxSpinNs ? floorNs : MaxSpinNs) { }

    // The spin before a deadline: the recent oversleep plus a quarter and 100 us, between the
    // floor and MaxSpinNs.
    uint64_t spinNs() const {
      uint64_t ns = m_lateNs + m_lateNs / 4u + 100000u;
      ns = ns < m_floorNs ? m_floorNs : ns;
      return ns > MaxSpinNs ? MaxSpinNs : ns;
    }

    // The last piece of the system's sleep recently overslept by this much (fast up, decays by
    // 1/64 per sleep: half in about 44 sleeps, 1.5 s at 30 fps).
    uint64_t lateNs() const { return m_lateNs; }

    // Sleeps from `nowNs` until `untilNs` (both on `clockNs`'s time line), with `sleepNs` as the
    // system's sleep. Returns the time it woke up (>= untilNs).
    template<typename ClockNs, typename SleepNs>
    uint64_t sleepUntil(uint64_t nowNs, uint64_t untilNs, ClockNs clockNs, SleepNs sleepNs) {
      const uint64_t spin = spinNs();
      while (nowNs + spin < untilNs) {
        const uint64_t left = untilNs - spin - nowNs;
        const bool     last = left <= LastNs;
        const uint64_t ns   = last ? left : left / 2u;
        const uint64_t target = nowNs + ns;
        sleepNs(ns);
        nowNs = clockNs();
        // what the spin has to cover: the last piece's lateness, or any piece's that overslept
        // past the spin's start
        if (last || nowNs + spin > untilNs) {
          const uint64_t late = nowNs > target ? nowNs - target : 0u;
          m_lateNs = late > m_lateNs ? late : m_lateNs - (m_lateNs - late) / 64u;
        }
      }
      while (nowNs < untilNs)
        nowNs = clockNs();
      return nowNs;
    }

    template<typename ClockNs>
    uint64_t sleepUntil(uint64_t nowNs, uint64_t untilNs, ClockNs clockNs) {
      return sleepUntil(nowNs, untilNs, clockNs, [] (uint64_t ns) { systemSleep(ns); });
    }

  private:

    uint64_t m_floorNs;
    uint64_t m_lateNs = 0u;

  };

  // The calling thread's Sleeper (its floor from the first call on that thread). Present sleeps
  // outside the device lock, so two threads presenting on one multithreaded device must not share
  // one.
  inline Sleeper& threadSleeper(uint64_t floorNs) {
    thread_local Sleeper s_sleeper(floorNs);
    return s_sleeper;
  }

}
