#pragma once

// D9MT_PROFILE: one warn-level SCREEN line per second, right after PACE (app thread): when the
// frames d9mt presented actually reached the screen, from each drawable's presentedTime
// (MTLDrawable addPresentedHandler, d9mt #153). PACE and STATS time Present calls; this times
// what the display showed, so it also sees what macOS's compositor does to the frames
// (a frame held for an extra refresh, frames dropped, a longer path to the screen).
//
//   SCREEN t=63 shown=60 dropped=0 pend=2 lost=0 onscreen=16.7/0.21/18.1 refreshes=59/0/0 late=0
//     req=16.7 lat=24.1/26.3 hz=60
//
//   t         seconds since the first Present (as PACE and STATS)
//   shown     drawables whose presented handler ran in the window with a presentedTime
//   dropped   drawables whose presented handler ran with presentedTime 0: macOS reported the
//             drawable as not shown (dropped, or a presentation path that reports no time).
//             Data, not missing data: `pend` and `lost` are the presents without data
//   pend      tracked presents whose handler hasn't run yet (in flight at the window's end)
//   lost      records of this presenter's stream lost in the window: overwritten before they were
//             read, or dropped by a presented handler that found its slot busy (should stay 0)
//   onscreen  interval between consecutive shown frames' presentedTime: median / sd / max ms
//             (the first one of a window is measured from the previous window's last frame)
//   refreshes on-screen intervals by length in refreshes: about 1 / about 2 / 3 or more (an
//             interval counts as k refreshes when it rounds to k; under 1.5: 1). A sustained
//             33 ms on a 60 Hz display shows as 0/59/0, 60 fps on 120 Hz as 0/59/0 too
//   late      intervals of at least the requested cadence + half a refresh: a frame stayed on
//             screen at least one refresh longer than the game asked for (a repeated frame, a
//             missed vsync). The requested cadence is the game's swap interval in refreshes, or
//             the D9MT_FPS_CAP minimum duration if longer, and 1 refresh with an immediate swap
//             interval. So 60 fps with swap interval 1 on a 120 Hz display is late every frame
//             (it asked for 120), and with the cap at 60 it isn't
//   req       that requested cadence, ms
//   lat       presentedTime minus the moment present was called on the drawable (the render
//             thread's present, after the frame's command buffer was committed): median / p90 ms
//   hz        the display refresh rate these were judged against
//
// A SCREEN-INFO line comes before the first SCREEN line and again whenever one of its values
// changes:
//
//   SCREEN-INFO displaySync=1 drawables=3 interval=1 cap=off hz=60.0 screenMaxFps=120
//     refreshInterval=8.3-41.7 nsFullscreen=1 d3dWindowed=1 forceComposited=0
//
//   displaySync, drawables   the CAMetalLayer's displaySyncEnabled and maximumDrawableCount
//   interval   the game's swap interval (vsyncs per frame; 0 = immediate)
//   cap        off, or the presentAfterMinimumDuration in ms (D9MT_FPS_CAP)
//   hz         the refresh rate d9mt paces to (Wine's display mode)
//   screenMaxFps, refreshInterval  the window's NSScreen: maximumFramesPerSecond, and
//              minimumRefreshInterval-maximumRefreshInterval in ms (a range: adaptive sync)
//   nsFullscreen  the window is in macOS fullscreen (its own Space); "?" until AppKit answered
//   d3dWindowed   the game's D3DPRESENT_PARAMETERS.Windowed
//   forceComposited  D9MT_FORCE_COMPOSITED=1: the bridge keeps the layer off the direct-to-display
//              path (a 1x1-point sublayer; for a direct-vs-composited A/B)
//
// Each presenter has its own stream (the bridge's ring, present_ring.h); the window restarts
// (bindStream) when the swapchain's presenter changes, so a recreated device or a second device
// never inherits another stream's counts.
//
// Off (D9MT_PROFILE unset): never constructed or called, and drawables are presented without a
// handler. Header-only and dependency-free so the statistics and the format can be unit-tested
// natively (test/native/screen_test.cpp).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dxvk::screen {

  struct Record {
    uint64_t seq        = 0u;
    double   submitS    = 0.0; // host time, s
    double   presentedS = 0.0; // host time, s; 0 = dropped
  };

  struct Info {
    bool     displaySync   = true;
    uint32_t drawables     = 0u;
    uint32_t interval      = 1u;  // swap interval (0 = immediate)
    double   capMs         = 0.0; // 0 = off
    double   hz            = 0.0;
    bool     screenValid   = false;
    bool     nsFullscreen  = false;
    double   screenMaxFps  = 0.0;
    double   minIntervalMs = 0.0;
    double   maxIntervalMs = 0.0;
    bool     d3dWindowed   = true;
    bool     forceComposited = false;
  };

  inline std::string formatInfo(const Info& i) {
    char cap[32];
    if (i.capMs > 0.0)
      std::snprintf(cap, sizeof(cap), "%.1f", i.capMs);
    else
      std::snprintf(cap, sizeof(cap), "off");
    char buf[320];
    if (i.screenValid)
      std::snprintf(buf, sizeof(buf),
        "SCREEN-INFO displaySync=%d drawables=%u interval=%u cap=%s hz=%.1f screenMaxFps=%.0f"
        " refreshInterval=%.1f-%.1f nsFullscreen=%d d3dWindowed=%d forceComposited=%d",
        int(i.displaySync), i.drawables, i.interval, cap, i.hz, i.screenMaxFps,
        i.minIntervalMs, i.maxIntervalMs, int(i.nsFullscreen), int(i.d3dWindowed), int(i.forceComposited));
    else
      std::snprintf(buf, sizeof(buf),
        "SCREEN-INFO displaySync=%d drawables=%u interval=%u cap=%s hz=%.1f screenMaxFps=?"
        " refreshInterval=? nsFullscreen=? d3dWindowed=%d forceComposited=%d",
        int(i.displaySync), i.drawables, i.interval, cap, i.hz, int(i.d3dWindowed), int(i.forceComposited));
    return buf;
  }

  // D3DPRESENT_INTERVAL_* (ONE 1, TWO 2, THREE 4, FOUR 8; 0 here = immediate) in refreshes.
  inline uint32_t d3dIntervalRefreshes(uint32_t interval) {
    switch (interval) {
      case 4u: return 3u;
      case 8u: return 4u;
      default: return interval;
    }
  }

  // The game's requested on-screen cadence, ms: the swap interval in refreshes, or the cap's
  // minimum duration if longer; 1 refresh for an immediate swap interval.
  inline double requestedMs(uint32_t interval, double capMs, double refreshMs) {
    return std::max(refreshMs * double(std::max(interval, 1u)), capMs);
  }

  class Window {

  public:

    // Binds the window to a presenter's stream (its id, 0 = none); a different stream than
    // before starts the window over: its counts are that stream's alone. Returns true then.
    bool bindStream(uint64_t streamId) {
      if (streamId == m_streamId)
        return false;
      *this = Window();
      m_streamId = streamId;
      return true;
    }

    void add(const Record& r) {
      m_seen++;
      if (r.presentedS > 0.0) {
        m_shown.push_back(r.presentedS);
        if (r.submitS > 0.0 && r.presentedS >= r.submitS)
          m_latMs.push_back((r.presentedS - r.submitS) * 1e3);
      } else {
        m_dropped++;
      }
    }

    // The line for the window that ends now; starts the next one. `issued`: tracked presents
    // so far, `lost`: records lost so far (both this stream's totals from the bridge), `hz`: the
    // display refresh rate (outside 20-1000: 60), `reqMs`: the requested cadence (requestedMs;
    // <= 0: 1 refresh).
    std::string take(double tSec, double hz, double reqMs, uint64_t issued, uint64_t lost) {
      if (!(hz >= 20.0 && hz <= 1000.0))
        hz = 60.0;
      const double refreshMs = 1e3 / hz;
      if (!(reqMs > 0.0))
        reqMs = refreshMs;

      std::sort(m_shown.begin(), m_shown.end());
      std::vector<double> iv;
      iv.reserve(m_shown.size());
      double prev = m_lastShownS;
      for (double p : m_shown) {
        if (prev > 0.0 && p > prev)
          iv.push_back((p - prev) * 1e3);
        if (p > prev)
          prev = p;
      }
      m_lastShownS = prev;

      const uint64_t accounted = m_seen + lost;
      const uint64_t pend = issued > accounted ? issued - accounted : 0u;
      const uint64_t lostNow = lost >= m_lostBase ? lost - m_lostBase : 0u;
      m_lostBase = lost;

      char buf[384];
      int len = std::snprintf(buf, sizeof(buf), "SCREEN t=%.0f shown=%zu dropped=%u pend=%llu lost=%llu",
        tSec, m_shown.size(), m_dropped, (unsigned long long)pend, (unsigned long long)lostNow);

      if (!iv.empty()) {
        double mean = 0.0, mx = 0.0;
        for (double v : iv) { mean += v; mx = std::max(mx, v); }
        mean /= double(iv.size());
        double var = 0.0;
        for (double v : iv) var += (v - mean) * (v - mean);
        const double sd = iv.size() > 1u ? std::sqrt(var / double(iv.size())) : 0.0;
        const double med = quantile(iv, 0.5);
        uint32_t late = 0u, hist[3] = { 0u, 0u, 0u };
        for (double v : iv) {
          late += v >= reqMs + 0.5 * refreshMs ? 1u : 0u;
          const double k = std::round(v / refreshMs);
          hist[k >= 3.0 ? 2 : k >= 2.0 ? 1 : 0]++;
        }
        len += std::snprintf(buf + len, sizeof(buf) - size_t(len),
          " onscreen=%.1f/%.2f/%.1f refreshes=%u/%u/%u late=%u req=%.1f", med, sd, mx,
          hist[0], hist[1], hist[2], late, reqMs);
      } else {
        len += std::snprintf(buf + len, sizeof(buf) - size_t(len),
          " onscreen=n/a refreshes=0/0/0 late=0 req=%.1f", reqMs);
      }

      if (!m_latMs.empty())
        len += std::snprintf(buf + len, sizeof(buf) - size_t(len), " lat=%.1f/%.1f",
          quantile(m_latMs, 0.5), quantile(m_latMs, 0.9));
      else
        len += std::snprintf(buf + len, sizeof(buf) - size_t(len), " lat=n/a");

      std::snprintf(buf + len, sizeof(buf) - size_t(len), " hz=%.0f", hz);

      m_shown.clear();
      m_latMs.clear();
      m_dropped = 0u;
      return buf;
    }

  private:

    // nearest-rank on a sorted copy (q in [0, 1])
    static double quantile(std::vector<double> v, double q) {
      std::sort(v.begin(), v.end());
      size_t i = size_t(std::ceil(q * double(v.size())));
      i = i ? i - 1u : 0u;
      return v[std::min(i, v.size() - 1u)];
    }

    std::vector<double> m_shown;
    std::vector<double> m_latMs;
    uint32_t            m_dropped    = 0u;
    uint64_t            m_seen       = 0u; // records seen in total (shown + dropped)
    uint64_t            m_lostBase   = 0u;
    uint64_t            m_streamId   = 0u;
    double              m_lastShownS = 0.0;

  };

}
