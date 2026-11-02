#pragma once

// D9MT_PROFILE: one warn-level SCREEN line per second, right after PACE (app thread): when the
// frames d9mt presented actually reached the screen, from each drawable's presentedTime
// (MTLDrawable addPresentedHandler, d9mt #153). PACE and STATS time Present calls; this times
// what the display showed, so it also sees what macOS's compositor does to the frames
// (a frame held for an extra refresh, frames dropped, a longer path to the screen).
//
//   SCREEN t=63 shown=60 dropped=0 pend=2 lost=0 onscreen=16.7/0.21/18.1 late=0 lat=24.1/26.3 hz=60
//
//   t         seconds since the first Present (as PACE and STATS)
//   shown     drawables whose presented handler ran in the window with a presentedTime
//   dropped   drawables whose presented handler ran with presentedTime 0 (never shown)
//   pend      tracked presents whose handler hasn't run yet (in flight at the window's end)
//   lost      records the bridge's ring overwrote before they were read (should stay 0)
//   onscreen  interval between consecutive shown frames' presentedTime: median / sd / max ms
//             (the first one of a window is measured from the previous window's last frame)
//   late      intervals of at least the nominal frame period + half a refresh: the previous
//             frame stayed on screen at least one refresh longer than the frames around it
//             (a repeated frame, a missed vsync). The nominal period is the refresh period
//             times the median interval rounded to whole refreshes (60 fps on a 120 Hz display:
//             2 refreshes, so 60 fps pacing has no late frames)
//   lat       presentedTime minus the moment present was called on the drawable (the render
//             thread's present, after the frame's command buffer was committed): median / p90 ms
//   hz        the display refresh rate these were judged against
//
// A SCREEN-INFO line comes before the first SCREEN line and again whenever one of its values
// changes:
//
//   SCREEN-INFO displaySync=1 drawables=3 cap=off hz=60.0 screenMaxFps=120
//     refreshInterval=8.3-41.7 nsFullscreen=1 d3dWindowed=1
//
//   displaySync, drawables   the CAMetalLayer's displaySyncEnabled and maximumDrawableCount
//   cap        off, or the presentAfterMinimumDuration in ms (D9MT_FPS_CAP)
//   hz         the refresh rate d9mt paces to (Wine's display mode)
//   screenMaxFps, refreshInterval  the window's NSScreen: maximumFramesPerSecond, and
//              minimumRefreshInterval-maximumRefreshInterval in ms (a range: adaptive sync)
//   nsFullscreen  the window is in macOS fullscreen (its own Space); "?" until AppKit answered
//   d3dWindowed   the game's D3DPRESENT_PARAMETERS.Windowed
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
    double   capMs         = 0.0; // 0 = off
    double   hz            = 0.0;
    bool     screenValid   = false;
    bool     nsFullscreen  = false;
    double   screenMaxFps  = 0.0;
    double   minIntervalMs = 0.0;
    double   maxIntervalMs = 0.0;
    bool     d3dWindowed   = true;
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
        "SCREEN-INFO displaySync=%d drawables=%u cap=%s hz=%.1f screenMaxFps=%.0f"
        " refreshInterval=%.1f-%.1f nsFullscreen=%d d3dWindowed=%d",
        int(i.displaySync), i.drawables, cap, i.hz, i.screenMaxFps,
        i.minIntervalMs, i.maxIntervalMs, int(i.nsFullscreen), int(i.d3dWindowed));
    else
      std::snprintf(buf, sizeof(buf),
        "SCREEN-INFO displaySync=%d drawables=%u cap=%s hz=%.1f screenMaxFps=?"
        " refreshInterval=? nsFullscreen=? d3dWindowed=%d",
        int(i.displaySync), i.drawables, cap, i.hz, int(i.d3dWindowed));
    return buf;
  }

  class Window {

  public:

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
    // so far, `lost`: ring records lost so far (both totals from the bridge), `hz`: the display
    // refresh rate (outside 20-1000: 60).
    std::string take(double tSec, double hz, uint64_t issued, uint64_t lost) {
      if (!(hz >= 20.0 && hz <= 1000.0))
        hz = 60.0;
      const double refreshMs = 1e3 / hz;

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
        const double nominal = refreshMs * std::max(1.0, std::round(med / refreshMs));
        uint32_t late = 0u;
        for (double v : iv)
          late += v >= nominal + 0.5 * refreshMs ? 1u : 0u;
        len += std::snprintf(buf + len, sizeof(buf) - size_t(len),
          " onscreen=%.1f/%.2f/%.1f late=%u", med, sd, mx, late);
      } else {
        len += std::snprintf(buf + len, sizeof(buf) - size_t(len), " onscreen=n/a late=0");
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
    double              m_lastShownS = 0.0;

  };

}
