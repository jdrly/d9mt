// Native unit test for the D9MT_PROFILE SCREEN line (no Wine, no Metal):
//   clang++ -std=c++17 -I driver/d3d9 test/native/screen_test.cpp -o build/st && build/st
// Synthetic presentedTime records: steady pacing, a repeated frame, drops, a sustained 33 ms on
// 60 Hz, 60 fps on 120 Hz (swap interval 1, and capped at 60), the refresh histogram, the
// requested cadence, latency quantiles, the window carry-over, pending and lost counts, the
// stream binding (a recreated device, two devices), the D3D9 interval mapping, SCREEN-INFO.
#include "d3d9_screen.h"

#include <cstdio>
#include <string>

using namespace dxvk;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static bool has(const std::string& line, const char* part) {
  return line.find(part) != std::string::npos;
}

static const double R60 = 1000.0 / 60.0, R120 = 1000.0 / 120.0;

int main() {
  const double P60 = 1.0 / 60.0;

  // 1. steady 60 fps on 60 Hz, swap interval 1, latency 25 ms, 60 frames: one interval less than
  //    frames in the first window (no previous frame)
  {
    screen::Window w;
    double t0 = 1000.0;
    for (int i = 0; i < 60; i++)
      w.add({ uint64_t(i + 1), t0 + i * P60 - 0.025, t0 + i * P60 });
    std::string line = w.take(1.0, 60.0, screen::requestedMs(1u, 0.0, R60), 62u, 0u);
    std::printf("%s\n", line.c_str());
    CHECK(has(line, "SCREEN t=1 shown=60 dropped=0 pend=2 lost=0"));
    CHECK(has(line, "onscreen=16.7/0.00/16.7 refreshes=59/0/0 late=0 req=16.7"));
    CHECK(has(line, "lat=25.0/25.0"));
    CHECK(has(line, "hz=60"));

    // 2. next window: one frame held for an extra refresh (33.3 ms), one dropped; the first
    //    interval is measured from the previous window's last frame
    double p = t0 + 59 * P60;
    for (int i = 0; i < 59; i++) {
      p += (i == 20) ? 2 * P60 : P60;
      w.add({ uint64_t(61 + i), p - 0.020, p });
    }
    w.add({ 120u, p, 0.0 });
    line = w.take(2.0, 60.0, screen::requestedMs(1u, 0.0, R60), 120u, 0u);
    std::printf("%s\n", line.c_str());
    CHECK(has(line, "shown=59 dropped=1 pend=0"));
    CHECK(has(line, "/33.3 refreshes=58/1/0 late=1 req=16.7"));
    CHECK(has(line, "onscreen=16.7/"));
    CHECK(has(line, "lat=20.0/20.0"));
  }

  // 3. a sustained 33.3 ms on a 60 Hz display (a compositor slowdown, not one repeated frame):
  //    with swap interval 1 every interval is late, and the histogram shows them all at 2x;
  //    the same cadence asked for with swap interval 2 is not late
  {
    for (uint32_t interval : { 1u, 2u }) {
      screen::Window w;
      double p = 20.0;
      for (int i = 0; i < 31; i++) {
        p += 2 * P60;
        w.add({ uint64_t(i + 1), p - 0.030, p });
      }
      std::string line = w.take(3.0, 60.0, screen::requestedMs(interval, 0.0, R60), 31u, 0u);
      std::printf("%s\n", line.c_str());
      CHECK(has(line, "onscreen=33.3/0.00/33.3 refreshes=0/30/0"));
      CHECK(has(line, interval == 1u ? "late=30 req=16.7" : "late=0 req=33.3"));
    }
  }

  // 4. 60 fps on a 120 Hz display: with swap interval 1 the game asked for 120, so every frame is
  //    late (2 refreshes each); capped at 60 (presentAfterMinimumDuration 16.7 ms) none is; a
  //    25 ms interval (3 refreshes) is late either way
  {
    for (int capped = 0; capped < 2; capped++) {
      screen::Window w;
      const double R = 1.0 / 120.0;
      double p = 50.0;
      for (int i = 0; i < 61; i++) {
        p += (i == 30) ? 3 * R : 2 * R;
        w.add({ uint64_t(i + 1), p - 0.010, p });
      }
      std::string line = w.take(5.0, 120.0, screen::requestedMs(1u, capped ? 1000.0 / 60.0 : 0.0, R120), 61u, 0u);
      std::printf("%s\n", line.c_str());
      CHECK(has(line, "onscreen=16.7/"));
      CHECK(has(line, "refreshes=0/59/1"));
      CHECK(has(line, capped ? "late=1 req=16.7" : "late=60 req=8.3"));
      CHECK(has(line, "hz=120"));
    }
  }

  // 5. jitter without repeats: alternating 15/18.3 ms on 60 Hz: sd > 0, no late frames;
  //    latency quantiles from 1..10 ms
  {
    screen::Window w;
    double p = 10.0;
    for (int i = 0; i < 40; i++) {
      p += (i % 2) ? 0.0183 : 0.0150;
      w.add({ uint64_t(i + 1), p - 0.001 * double(i % 10 + 1), p });
    }
    std::string line = w.take(7.0, 60.0, screen::requestedMs(1u, 0.0, R60), 40u, 0u);
    std::printf("%s\n", line.c_str());
    CHECK(has(line, "refreshes=39/0/0 late=0"));
    CHECK(!has(line, "/0.00/"));
    CHECK(has(line, "lat=5.0/9.0"));
  }

  // 6. no records: n/a fields; lost is per window (a delta of the stream's total); hz outside
  //    20-1000 → 60; a requested cadence <= 0 → 1 refresh
  {
    screen::Window w;
    std::string line = w.take(3.0, 0.0, 0.0, 5u, 2u);
    std::printf("%s\n", line.c_str());
    CHECK(has(line, "shown=0 dropped=0 pend=3 lost=2 onscreen=n/a refreshes=0/0/0 late=0 req=16.7 lat=n/a hz=60"));
    line = w.take(4.0, 0.0, 0.0, 5u, 2u);
    CHECK(has(line, "pend=3 lost=0"));
  }

  // 7. one frame only: no interval yet; records out of order are sorted
  {
    screen::Window w;
    w.add({ 1u, 0.0, 20.0 });
    std::string line = w.take(1.0, 60.0, R60, 1u, 0u);
    CHECK(has(line, "onscreen=n/a"));
    CHECK(has(line, "lat=n/a"));
    w.add({ 3u, 20.020, 20.0 + 2 * P60 });
    w.add({ 2u, 20.010, 20.0 + P60 });
    line = w.take(2.0, 60.0, R60, 3u, 0u);
    std::printf("%s\n", line.c_str());
    CHECK(has(line, "onscreen=16.7/0.00/16.7 refreshes=2/0/0 late=0"));
  }

  // 8. the stream binding: device A presents and drains 3600, then is destroyed; device B's
  //    presenter has a new stream whose totals start at 0: the window starts over (pend from B
  //    only, no carry-over interval, A's lost total not re-reported). Two devices: two windows,
  //    each with its own stream's totals.
  {
    screen::Window w;
    CHECK(w.bindStream(1u));
    double p = 100.0;
    for (int i = 0; i < 3600; i++) { p += P60; w.add({ uint64_t(i + 1), p - 0.02, p }); }
    std::string line = w.take(60.0, 60.0, R60, 3600u, 7u);
    CHECK(has(line, "pend=0 lost=7"));
    CHECK(!w.bindStream(1u));                   // same stream: nothing restarts
    CHECK(w.bindStream(2u));                    // the recreated device's presenter
    double q = 500.0;
    for (int i = 0; i < 60; i++) { q += P60; w.add({ uint64_t(i + 1), q - 0.02, q }); }
    line = w.take(1.0, 60.0, R60, 61u, 0u);
    std::printf("%s\n", line.c_str());
    CHECK(has(line, "shown=60 dropped=0 pend=1 lost=0"));
    CHECK(has(line, "/16.7 refreshes=59/0/0"));  // no interval from A's last frame

    screen::Window a, b;                         // two devices at once
    a.bindStream(3u); b.bindStream(4u);
    for (int i = 0; i < 30; i++) a.add({ uint64_t(i + 1), 1.0 + i * P60, 1.02 + i * P60 });
    for (int i = 0; i < 10; i++) b.add({ uint64_t(i + 1), 2.0 + i * P60, 2.02 + i * P60 });
    CHECK(has(a.take(1.0, 60.0, R60, 30u, 0u), "shown=30 dropped=0 pend=0"));
    CHECK(has(b.take(1.0, 60.0, R60, 12u, 0u), "shown=10 dropped=0 pend=2"));
  }

  // 9. D3DPRESENT_INTERVAL_* in refreshes, and the requested cadence
  {
    CHECK(screen::d3dIntervalRefreshes(0u) == 0u);
    CHECK(screen::d3dIntervalRefreshes(1u) == 1u);
    CHECK(screen::d3dIntervalRefreshes(2u) == 2u);
    CHECK(screen::d3dIntervalRefreshes(4u) == 3u);  // D3DPRESENT_INTERVAL_THREE
    CHECK(screen::d3dIntervalRefreshes(8u) == 4u);  // D3DPRESENT_INTERVAL_FOUR
    CHECK(screen::requestedMs(0u, 0.0, R60) == R60);          // immediate: 1 refresh
    CHECK(screen::requestedMs(3u, 0.0, R60) == 3 * R60);
    CHECK(screen::requestedMs(1u, 33.3, R60) == 33.3);        // the cap is longer
  }

  // 10. SCREEN-INFO
  {
    screen::Info i;
    i.displaySync = true; i.drawables = 3u; i.hz = 60.0; i.d3dWindowed = true;
    std::string a = screen::formatInfo(i);
    std::printf("%s\n", a.c_str());
    CHECK(a == "SCREEN-INFO displaySync=1 drawables=3 interval=1 cap=off hz=60.0 screenMaxFps=?"
               " refreshInterval=? nsFullscreen=? d3dWindowed=1");
    i.screenValid = true; i.nsFullscreen = true; i.screenMaxFps = 120.0; i.interval = 2u;
    i.minIntervalMs = 1000.0 / 120.0; i.maxIntervalMs = 1000.0 / 24.0; i.capMs = 33.3;
    std::string b = screen::formatInfo(i);
    std::printf("%s\n", b.c_str());
    CHECK(b == "SCREEN-INFO displaySync=1 drawables=3 interval=2 cap=33.3 hz=60.0 screenMaxFps=120"
               " refreshInterval=8.3-41.7 nsFullscreen=1 d3dWindowed=1");
  }

  std::printf("%s (%d failures)\n", g_fail ? "FAILED" : "ok", g_fail);
  return g_fail ? 1 : 0;
}
