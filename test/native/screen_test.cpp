// Native unit test for the D9MT_PROFILE SCREEN line (no Wine, no Metal):
//   clang++ -std=c++17 -I driver/d3d9 test/native/screen_test.cpp -o build/st && build/st
// Synthetic presentedTime records, in present order as the stream retires them: steady pacing, a
// repeated frame, drops, a sustained 33 ms on 60 Hz, 60 fps on 120 Hz (swap interval 1, and
// capped at 60), the refresh histogram, the requested cadence, latency quantiles, the carry-over
// between windows, chain breaks (a seq gap, a break marker, a stream switch A -> B -> A), the
// D3D9 interval mapping, SCREEN-INFO (unknown values as "?"), the unprofiled SCREEN-INFO
// cadence. The ring itself, and delayed handlers across drains: present_ring_test.cpp.
#include "d3d9_screen.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

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
    std::string line = w.take(1.0, 60.0, screen::requestedMs(1u, 0.0, R60), 2u);
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
    line = w.take(2.0, 60.0, screen::requestedMs(1u, 0.0, R60), 0u);
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
      std::string line = w.take(3.0, 60.0, screen::requestedMs(interval, 0.0, R60), 0u);
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
      std::string line = w.take(5.0, 120.0, screen::requestedMs(1u, capped ? 1000.0 / 60.0 : 0.0, R120), 0u);
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
    std::string line = w.take(7.0, 60.0, screen::requestedMs(1u, 0.0, R60), 0u);
    std::printf("%s\n", line.c_str());
    CHECK(has(line, "refreshes=39/0/0 late=0"));
    CHECK(!has(line, "/0.00/"));
    CHECK(has(line, "lat=5.0/9.0"));
  }

  // 6. no records: n/a fields; pend is the stream's (passed in), lost is what the drains of
  //    this window reported; hz outside 20-1000 → judged against 60, printed as "?"; a
  //    requested cadence <= 0 → 1 refresh
  {
    screen::Window w;
    w.addLost(2u);
    std::string line = w.take(3.0, 0.0, 0.0, 3u);
    std::printf("%s\n", line.c_str());
    CHECK(has(line, "shown=0 dropped=0 pend=3 lost=2 onscreen=n/a refreshes=0/0/0 late=0 req=16.7 lat=n/a hz=?"));
    line = w.take(4.0, 0.0, 0.0, 3u);
    CHECK(has(line, "pend=3 lost=0"));
  }

  // 7. one frame only: no interval yet. A seq gap (a lost present between) breaks the chain: no
  //    interval across it; the chain goes on after it. A break marker (times -1) breaks it too,
  //    and is no frame. A dropped frame (presentedTime 0) doesn't break it.
  {
    screen::Window w;
    w.add({ 1u, 0.0, 20.0 });
    std::string line = w.take(1.0, 60.0, R60, 0u);
    CHECK(has(line, "onscreen=n/a"));
    CHECK(has(line, "lat=n/a"));
    w.add({ 3u, 20.0 + 2 * P60 - 0.01, 20.0 + 2 * P60 });   // seq 2 lost: no 2-refresh interval
    w.add({ 4u, 20.0 + 3 * P60 - 0.01, 20.0 + 3 * P60 });
    w.addLost(1u);
    line = w.take(2.0, 60.0, R60, 0u);
    std::printf("%s\n", line.c_str());
    CHECK(has(line, "shown=2 dropped=0 pend=0 lost=1 onscreen=16.7/0.00/16.7 refreshes=1/0/0 late=0"));
    w.add({ 5u, -1.0, -1.0 });                                // a break marker
    w.add({ 6u, 30.0 - 0.01, 30.0 });
    w.add({ 7u, 30.0 + P60, 0.0 });                           // dropped: the frame at 30.0 stays up
    w.add({ 8u, 30.0 + 2 * P60 - 0.01, 30.0 + 2 * P60 });
    line = w.take(3.0, 60.0, R60, 0u);
    std::printf("%s\n", line.c_str());
    CHECK(has(line, "shown=2 dropped=1 pend=0 lost=0 onscreen=33.3/0.00/33.3 refreshes=0/1/0 late=1"));
  }

  // 8. review 2 #1: A -> B -> A (the swapchain's presenters are cached per window). The counts are
  //    the streams' (pend passed in, lost from the drains), so returning to A reports nothing
  //    stale; the rebinds only break the chain: no interval spans a switch.
  {
    screen::Window w;
    CHECK(w.bindStream(1u));
    double p = 100.0;
    for (int i = 0; i < 100; i++) { p += P60; w.add({ uint64_t(i + 1), p - 0.02, p }); }
    std::string line = w.take(1.0, 60.0, R60, 0u);
    CHECK(has(line, "shown=100 dropped=0 pend=0 lost=0"));
    CHECK(!w.bindStream(1u));                   // same stream: nothing changes
    CHECK(w.bindStream(2u));                    // window B's presenter
    double q = 500.0;
    for (int i = 0; i < 60; i++) { q += P60; w.add({ uint64_t(i + 1), q - 0.02, q }); }
    line = w.take(2.0, 60.0, R60, 0u);
    std::printf("%s\n", line.c_str());
    CHECK(has(line, "shown=60 dropped=0 pend=0 lost=0 onscreen=16.7/0.00/16.7 refreshes=59/0/0"));
    CHECK(w.bindStream(1u));                    // back on A: its seq goes on from 100
    p += 30.0;
    for (int i = 0; i < 60; i++) { p += P60; w.add({ uint64_t(101 + i), p - 0.02, p }); }
    line = w.take(3.0, 60.0, R60, 0u);
    std::printf("%s\n", line.c_str());
    CHECK(has(line, "shown=60 dropped=0 pend=0 lost=0 onscreen=16.7/0.00/16.7 refreshes=59/0/0 late=0"));
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
               " refreshInterval=? nsFullscreen=? d3dWindowed=1 forceComposited=0");
    i.screenValid = true; i.nsFullscreen = true; i.screenMaxFps = 120.0; i.interval = 2u;
    i.minIntervalMs = 1000.0 / 120.0; i.maxIntervalMs = 1000.0 / 24.0; i.capMs = 33.3;
    i.forceComposited = true;
    std::string b = screen::formatInfo(i);
    std::printf("%s\n", b.c_str());
    CHECK(b == "SCREEN-INFO displaySync=1 drawables=3 interval=2 cap=33.3 hz=60.0 screenMaxFps=120"
               " refreshInterval=8.3-41.7 nsFullscreen=1 d3dWindowed=1 forceComposited=1");

    // Wine's display mode not known (0, or NaN from a 0/0 mode), and a window without a screen
    // (AppKit answers valid with zeros): "?", never 0 or a guess
    i.hz = 0.0;
    CHECK(has(screen::formatInfo(i), " hz=? screenMaxFps=120 "));
    i.hz = std::nan("");
    CHECK(has(screen::formatInfo(i), " hz=? "));
    i.hz = 120.0; i.screenMaxFps = 0.0; i.minIntervalMs = 0.0; i.maxIntervalMs = 0.0;
    i.nsFullscreen = false;
    std::string c = screen::formatInfo(i);
    std::printf("%s\n", c.c_str());
    CHECK(has(c, " hz=120.0 screenMaxFps=? refreshInterval=? nsFullscreen=0 "));

    // the SCREEN line with the mode not known: judged against 60 Hz, printed as "?"
    screen::Window w;
    for (int k = 0; k < 4; k++)
      w.add({ uint64_t(k + 1), 0.0, 10.0 + k * P60 });
    std::string d = w.take(1.0, 0.0, screen::requestedMs(1u, 0.0, R60), 0u);
    std::printf("%s\n", d.c_str());
    CHECK(has(d, "refreshes=3/0/0 late=0 req=16.7"));
    CHECK(has(d, " hz=?") && !has(d, "hz=60"));
  }

  // 11. the unprofiled SCREEN-INFO cadence: which Presents read, and which reads may write
  {
    auto run = [] (screen::InfoCadence& c, int from, int to, int knownFrom,
                   std::vector<int>& reads, std::vector<int>& writes, bool restartEach = false) {
      for (int p = from; p <= to; p++) {
        if (restartEach)
          c.restart();
        if (c.due()) {
          reads.push_back(p);
          if (c.mayWrite(knownFrom > 0 && p >= knownFrom))
            writes.push_back(p);
        }
      }
    };

    // the values known from the start: the first read only asks (they may be another window's
    // or stale), the second writes, then every 512th Present
    {
      screen::InfoCadence c;
      std::vector<int> reads, writes;
      run(c, 1, 1200, 1, reads, writes);
      CHECK((reads  == std::vector<int>{ 1, 65, 577, 1089 }));
      CHECK((writes == std::vector<int>{ 65, 577, 1089 }));
    }

    // known only from the 200th Present (AppKit answered late): fast reads until then
    {
      screen::InfoCadence c;
      std::vector<int> reads, writes;
      run(c, 1, 800, 200, reads, writes);
      CHECK((reads  == std::vector<int>{ 1, 65, 129, 193, 257, 769 }));
      CHECK((writes == std::vector<int>{ 257, 769 }));
    }

    // never known: no line at the fast cadence, one at the 9th read (the 513th Present), then
    // the slow cadence
    {
      screen::InfoCadence c;
      std::vector<int> reads, writes;
      run(c, 1, 1100, 0, reads, writes);
      CHECK(reads.size() == 10u && reads[8] == 513 && reads[9] == 1025);
      CHECK((writes == std::vector<int>{ 513, 1025 }));
    }

    // a Reset at the 700th Present (slow, next read due at 1089): the fast cadence again, its
    // first read only asking
    {
      screen::InfoCadence c;
      std::vector<int> reads, writes;
      run(c, 1, 699, 1, reads, writes);
      c.restart();
      reads.clear(); writes.clear();
      run(c, 700, 1400, 1, reads, writes);
      CHECK((reads  == std::vector<int>{ 763, 827, 1339 }));
      CHECK((writes == std::vector<int>{ 827, 1339 }));
    }

    // restarts at every Present (a game presenting to two windows in turn): still a read every
    // 64th Present at most, and none of them writes
    {
      screen::InfoCadence c;
      std::vector<int> reads, writes;
      run(c, 1, 640, 1, reads, writes, true);
      CHECK(reads.size() == 10u && reads[0] == 1 && reads[1] == 65);
      CHECK(writes.empty());
    }
  }

  std::printf("%s (%d failures)\n", g_fail ? "FAILED" : "ok", g_fail);
  return g_fail ? 1 : 0;
}
