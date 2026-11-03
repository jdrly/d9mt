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
//   shown     drawables retired in the window with a presentedTime
//   dropped   drawables retired with presentedTime 0: macOS reported the
//             drawable as not shown (dropped, or a presentation path that reports no time).
//             Data, not missing data: `pend` and `lost` are the presents without data
//   pend      this stream's presents not yet retired at the window's end (handlers in flight,
//             or an earlier one the reader is holding for)
//   lost      presents retired in the window without a record: overwritten before they were
//             read, given up by a handler that found its slot busy, or unresolved past the
//             bridge's hold bound (should stay 0)
//   onscreen  interval between consecutive shown frames' presentedTime: median / sd / max ms.
//             Records arrive strictly in present order (the bridge retires them by present seq,
//             present_ring.h), so an interval spans exactly the frames between two presents. The
//             chain carries over from the previous window, through dropped frames (the previous
//             frame stayed up), but breaks at a lost present (a seq gap), at a break marker (a
//             record with times -1 that the presenter puts in before its first present after a
//             window switch back to it, or after presents it skipped) and when the swapchain
//             switches to another window's stream: no interval is computed across those
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
//   forceComposited  D9MT_FORCE_COMPOSITED=1: the bridge requests composition (a 1x1-point
//              sublayer; for a direct-vs-composited A/B). The request, not the measured present
//              mode: verify with the Metal HUD
//
// Each presenter has its own stream (the bridge's ring, present_ring.h), which keeps its own
// accounting (pending, lost): switching between windows (A -> B -> A, presenters cached) or a
// recreated or second device never mixes counts. bindStream only breaks the interval chain.
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

    // The swapchain presents into this stream (its id, 0 = none). Another stream than before
    // breaks the interval chain (no interval across a switch of windows); the counts are the
    // streams' own and need no reset. Returns true on a switch.
    bool bindStream(uint64_t streamId) {
      if (streamId == m_streamId)
        return false;
      m_streamId = streamId;
      breakChain();
      return true;
    }

    // A record retired by the stream, in present order.
    void add(const Record& r) {
      if (m_lastSeq && r.seq != m_lastSeq + 1u)
        breakChain(); // a lost present between: no interval across it
      m_lastSeq = r.seq;
      if (r.presentedS < 0.0) {
        // a break marker (window switch back to this stream, or skipped presents): not a frame
        m_lastShownS = 0.0;
        return;
      }
      if (r.presentedS > 0.0) {
        m_shown++;
        if (m_lastShownS > 0.0 && r.presentedS > m_lastShownS)
          m_iv.push_back((r.presentedS - m_lastShownS) * 1e3);
        m_lastShownS = r.presentedS;
        if (r.submitS > 0.0 && r.presentedS >= r.submitS)
          m_latMs.push_back((r.presentedS - r.submitS) * 1e3);
      } else {
        m_dropped++; // the chain goes on: the previous frame stayed on screen
      }
    }

    // Presents the stream retired as lost (a drain's count).
    void addLost(uint64_t n) {
      m_lost += n;
    }

    // The line for the window that ends now; starts the next one. `pending`: the stream's
    // presents not yet retired, `hz`: the display refresh rate (outside 20-1000: 60), `reqMs`:
    // the requested cadence (requestedMs; <= 0: 1 refresh).
    std::string take(double tSec, double hz, double reqMs, uint64_t pending) {
      if (!(hz >= 20.0 && hz <= 1000.0))
        hz = 60.0;
      const double refreshMs = 1e3 / hz;
      if (!(reqMs > 0.0))
        reqMs = refreshMs;

      char buf[384];
      int len = std::snprintf(buf, sizeof(buf), "SCREEN t=%.0f shown=%u dropped=%u pend=%llu lost=%llu",
        tSec, m_shown, m_dropped, (unsigned long long)pending, (unsigned long long)m_lost);

      const std::vector<double>& iv = m_iv;
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

      m_iv.clear();
      m_latMs.clear();
      m_shown   = 0u;
      m_dropped = 0u;
      m_lost    = 0u;
      return buf;
    }

  private:

    void breakChain() {
      m_lastShownS = 0.0;
      m_lastSeq    = 0u;
    }

    // nearest-rank on a sorted copy (q in [0, 1])
    static double quantile(std::vector<double> v, double q) {
      std::sort(v.begin(), v.end());
      size_t i = size_t(std::ceil(q * double(v.size())));
      i = i ? i - 1u : 0u;
      return v[std::min(i, v.size() - 1u)];
    }

    std::vector<double> m_iv;
    std::vector<double> m_latMs;
    uint32_t            m_shown      = 0u;
    uint32_t            m_dropped    = 0u;
    uint64_t            m_lost       = 0u;
    uint64_t            m_streamId   = 0u;
    uint64_t            m_lastSeq    = 0u; // the last record's seq (0: chain broken)
    double              m_lastShownS = 0.0;

  };

}
