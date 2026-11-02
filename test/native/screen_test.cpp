// Native unit test for the D9MT_PROFILE SCREEN line (no Wine, no Metal):
//   clang++ -std=c++17 -I driver/d3d9 test/native/screen_test.cpp -o build/st && build/st
// Synthetic presentedTime records: steady pacing, a repeated frame, drops, 60 fps on a
// 120 Hz display, latency quantiles, the window carry-over, pending and lost counts.
#include "d3d9_screen.h"

#include <cstdio>
#include <string>

using namespace dxvk;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static bool has(const std::string& line, const char* part) {
  return line.find(part) != std::string::npos;
}

int main() {
  const double P60 = 1.0 / 60.0;

  // 1. steady 60 fps on 60 Hz, latency 25 ms, 60 frames: one interval less than frames in the
  //    first window (no previous frame)
  {
    screen::Window w;
    double t0 = 1000.0;
    for (int i = 0; i < 60; i++)
      w.add({ uint64_t(i + 1), t0 + i * P60 - 0.025, t0 + i * P60 });
    std::string line = w.take(1.0, 60.0, 62u, 0u);
    std::printf("%s\n", line.c_str());
    CHECK(has(line, "SCREEN t=1 shown=60 dropped=0 pend=2 lost=0"));
    CHECK(has(line, "onscreen=16.7/0.00/16.7 late=0"));
    CHECK(has(line, "lat=25.0/25.0"));
    CHECK(has(line, "hz=60"));

    // 2. next window: one frame held for an extra refresh (33.3 ms), one dropped; the first
    //    interval is measured from the previous window's last frame
    double t1 = t0 + 59 * P60;
    double p = t1;
    for (int i = 0; i < 59; i++) {
      p += (i == 20) ? 2 * P60 : P60;
      w.add({ uint64_t(61 + i), p - 0.020, p });
    }
    w.add({ 120u, p, 0.0 });
    line = w.take(2.0, 60.0, 120u, 0u);
    std::printf("%s\n", line.c_str());
    CHECK(has(line, "shown=59 dropped=1 pend=0"));
    CHECK(has(line, "late=1"));
    CHECK(has(line, "/33.3 late=1"));
    CHECK(has(line, "onscreen=16.7/"));
    CHECK(has(line, "lat=20.0/20.0"));
  }

  // 3. 60 fps on a 120 Hz display: 2 refreshes per frame are nominal, not late; a 25 ms
  //    interval (3 refreshes) is late
  {
    screen::Window w;
    const double R = 1.0 / 120.0;
    double p = 50.0;
    for (int i = 0; i < 61; i++) {
      p += (i == 30) ? 3 * R : 2 * R;
      w.add({ uint64_t(i + 1), p - 0.010, p });
    }
    std::string line = w.take(5.0, 120.0, 61u, 0u);
    std::printf("%s\n", line.c_str());
    CHECK(has(line, "onscreen=16.7/"));
    CHECK(has(line, "late=1"));
    CHECK(has(line, "hz=120"));
  }

  // 4. jitter without repeats: alternating 15/18.3 ms on 60 Hz: sd > 0, no late frames;
  //    latency quantiles from 1..10 ms
  {
    screen::Window w;
    double p = 10.0;
    for (int i = 0; i < 40; i++) {
      p += (i % 2) ? 0.0183 : 0.0150;
      w.add({ uint64_t(i + 1), p - 0.001 * double(i % 10 + 1), p });
    }
    std::string line = w.take(7.0, 60.0, 40u, 0u);
    std::printf("%s\n", line.c_str());
    CHECK(has(line, "late=0"));
    CHECK(!has(line, "/0.00/"));
    CHECK(has(line, "lat=5.0/9.0"));
  }

  // 5. no records: n/a fields; lost is per window (a delta of the bridge's total);
  //    hz outside 20-1000 → 60
  {
    screen::Window w;
    std::string line = w.take(3.0, 0.0, 5u, 2u);
    std::printf("%s\n", line.c_str());
    CHECK(has(line, "shown=0 dropped=0 pend=3 lost=2 onscreen=n/a late=0 lat=n/a hz=60"));
    line = w.take(4.0, 0.0, 5u, 2u);
    CHECK(has(line, "pend=3 lost=0"));
  }

  // 6. one frame only: no interval yet; records out of order are sorted
  {
    screen::Window w;
    w.add({ 1u, 0.0, 20.0 });
    std::string line = w.take(1.0, 60.0, 1u, 0u);
    CHECK(has(line, "onscreen=n/a"));
    CHECK(has(line, "lat=n/a"));
    w.add({ 3u, 20.020, 20.0 + 2 * P60 });
    w.add({ 2u, 20.010, 20.0 + P60 });
    line = w.take(2.0, 60.0, 3u, 0u);
    std::printf("%s\n", line.c_str());
    CHECK(has(line, "onscreen=16.7/0.00/16.7 late=0"));
  }

  // 7. SCREEN-INFO
  {
    screen::Info i;
    i.displaySync = true; i.drawables = 3u; i.hz = 60.0; i.d3dWindowed = true;
    std::string a = screen::formatInfo(i);
    std::printf("%s\n", a.c_str());
    CHECK(a == "SCREEN-INFO displaySync=1 drawables=3 cap=off hz=60.0 screenMaxFps=?"
               " refreshInterval=? nsFullscreen=? d3dWindowed=1");
    i.screenValid = true; i.nsFullscreen = true; i.screenMaxFps = 120.0;
    i.minIntervalMs = 1000.0 / 120.0; i.maxIntervalMs = 1000.0 / 24.0; i.capMs = 33.3;
    std::string b = screen::formatInfo(i);
    std::printf("%s\n", b.c_str());
    CHECK(b == "SCREEN-INFO displaySync=1 drawables=3 cap=33.3 hz=60.0 screenMaxFps=120"
               " refreshInterval=8.3-41.7 nsFullscreen=1 d3dWindowed=1");
  }

  std::printf("%s (%d failures)\n", g_fail ? "FAILED" : "ok", g_fail);
  return g_fail ? 1 : 0;
}
