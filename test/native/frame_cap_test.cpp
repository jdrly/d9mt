// Native unit test for D9MT_FPS_CAP's plan and cadence (no Wine, no Metal):
//   clang++ -std=c++17 -I driver/d3d9 test/native/frame_cap_test.cpp -o /tmp/fct && /tmp/fct
#include "d3d9_frame_cap.h"

#include <chrono>
#include <cstdio>
#include <string>

using namespace dxvk;
using framecap::Mode;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static bool has(const std::string& line, const char* part) {
  return line.find(part) != std::string::npos;
}

static bool near(double a, double b, double eps) { return std::fabs(a - b) <= eps; }

int main() {
  constexpr uint64_t ms = 1000000u;

  // --- plan
  { // off
    auto p = framecap::plan(0u, 60.0, true, true);
    CHECK(p.mode == Mode::Off && p.intervalNs == 0u);
  }
  { // 30 at 60 Hz, vsync: every 2nd refresh, aligned
    auto p = framecap::plan(30u, 60.0, true, true);
    CHECK(p.mode == Mode::Vsync);
    CHECK(p.refreshes == 2u);
    CHECK(p.intervalNs == 33333333u);
    CHECK(near(p.minDurationS, 0.025, 1e-9));
    std::string d = framecap::describe(p, 60.0, true);
    std::printf("%s\n", d.c_str());
    CHECK(has(d, "frame cap 30 fps: every 2nd refresh at 60 Hz, cadence 33.33 ms, presents aligned to vsync (minimum duration 25.00 ms)"));
  }
  { // the same without alignment: N refreshes, no minimum duration
    auto p = framecap::plan(30u, 60.0, true, false);
    CHECK(p.mode == Mode::Vsync && p.refreshes == 2u && p.minDurationS == 0.0);
    CHECK(has(framecap::describe(p, 60.0, true), "timer only (D9MT_FPS_CAP_ALIGN=0)"));
  }
  { // Wine reports a 59.94 Hz display as 59: the cadence stays 1 / 30 (never slower than the display)
    auto p = framecap::plan(30u, 59.0, true, true);
    CHECK(p.mode == Mode::Vsync && p.refreshes == 2u);
    CHECK(p.intervalNs == 33333333u);
    CHECK(near(p.minDurationS, 1.5 / 59.0, 1e-9));
  }
  { // ... and as 60 (0.1 % slower than 60 Hz): 2 / 60
    auto p = framecap::plan(30u, 60.0, true, true);
    CHECK(p.intervalNs == 33333333u);
  }
  { // 40 and 45 at 60 Hz: not divisors, the timer
    auto p40 = framecap::plan(40u, 60.0, true, true);
    CHECK(p40.mode == Mode::Timer && p40.intervalNs == 25000000u && p40.minDurationS == 0.0);
    std::string d = framecap::describe(p40, 60.0, true);
    std::printf("%s\n", d.c_str());
    CHECK(has(d, "frame cap 40 fps: timer, cadence 25.00 ms (not a divisor of 60 Hz"));
    auto p45 = framecap::plan(45u, 60.0, true, true);
    CHECK(p45.mode == Mode::Timer && p45.intervalNs == 22222222u);
  }
  { // 60 at 60 Hz (and above): vsync caps already
    CHECK(framecap::plan(60u, 60.0, true, true).mode == Mode::Off);
    CHECK(framecap::plan(60u, 59.0, true, true).mode == Mode::Off);
    CHECK(framecap::plan(144u, 60.0, true, true).mode == Mode::Off);
    std::string d = framecap::describe(framecap::plan(60u, 60.0, true, true), 60.0, true);
    CHECK(has(d, "frame cap 60 fps: off (vsync at 60 Hz caps already)"));
  }
  { // 120 Hz (ProMotion): 30 = every 4th, 40 = every 3rd, 60 = every 2nd; 45 the timer
    auto p30 = framecap::plan(30u, 120.0, true, true);
    CHECK(p30.mode == Mode::Vsync && p30.refreshes == 4u && near(p30.minDurationS, 3.5 / 120.0, 1e-9));
    auto p40 = framecap::plan(40u, 120.0, true, true);
    CHECK(p40.mode == Mode::Vsync && p40.refreshes == 3u && p40.intervalNs == 25000000u);
    CHECK(has(framecap::describe(p40, 120.0, true), "every 3rd refresh at 120 Hz"));
    auto p60 = framecap::plan(60u, 120.0, true, true);
    CHECK(p60.mode == Mode::Vsync && p60.refreshes == 2u && p60.intervalNs == 16666667u);
    CHECK(framecap::plan(45u, 120.0, true, true).mode == Mode::Timer);
  }
  { // 90 Hz: 45 = every 2nd, 30 = every 3rd
    CHECK(framecap::plan(45u, 90.0, true, true).refreshes == 2u);
    CHECK(framecap::plan(30u, 90.0, true, true).refreshes == 3u);
  }
  { // vsync off, or the refresh rate unknown: the timer at any cap
    auto p = framecap::plan(30u, 60.0, false, true);
    CHECK(p.mode == Mode::Timer && p.intervalNs == 33333333u && p.minDurationS == 0.0);
    CHECK(has(framecap::describe(p, 60.0, false), "(vsync off)"));
    CHECK(framecap::plan(60u, 60.0, false, true).mode == Mode::Timer);
    auto u = framecap::plan(30u, 0.0, true, true);
    CHECK(u.mode == Mode::Timer);
    CHECK(has(framecap::describe(u, 0.0, true), "(refresh rate unknown)"));
  }
  { // plans compare by value (the swapchain logs a changed one)
    CHECK(framecap::plan(30u, 60.0, true, true) == framecap::plan(30u, 60.0, true, true));
    CHECK(framecap::plan(30u, 60.0, true, true) != framecap::plan(30u, 120.0, true, true));
  }

  // --- env
  CHECK(framecap::capFromEnv(nullptr) == 0u);
  CHECK(framecap::capFromEnv("") == 0u);
  CHECK(framecap::capFromEnv("30") == 30u);
  CHECK(framecap::capFromEnv("45") == 45u);
  CHECK(framecap::capFromEnv("0") == 0u);
  CHECK(framecap::capFromEnv("5") == 0u);
  CHECK(framecap::capFromEnv("19") == 0u);
  CHECK(framecap::capFromEnv("20") == 20u);
  CHECK(framecap::capFromEnv("abc") == 0u);
  CHECK(framecap::capFromEnv("2000") == 0u);
  CHECK(framecap::spinNsFromEnv(nullptr) == 200000u);
  CHECK(framecap::spinNsFromEnv("0") == 0u);
  CHECK(framecap::spinNsFromEnv("500") == 500000u);
  CHECK(framecap::spinNsFromEnv("99999") == 5000000u);

  // --- cadence
  {
    framecap::Cadence c;
    const uint64_t iv = 25u * ms;
    uint64_t t = 1000u * ms;
    CHECK(c.next(t, iv) == 0u);                    // the first frame: no sleep, the grid starts
    CHECK(c.next(t + 10u * ms, iv) == t + 25u * ms);  // early: sleep to the grid
    CHECK(c.next(t + 25u * ms, iv) == t + 50u * ms);  // right after waking: the next grid point
    // 5 ms late for t + 75: no sleep, the grid stays (the next frame is shorter)
    CHECK(c.next(t + 80u * ms, iv) == 0u);
    CHECK(c.next(t + 90u * ms, iv) == t + 100u * ms);
    // more than one interval late: the grid restarts from now, no catch-up burst
    CHECK(c.next(t + 160u * ms, iv) == 0u);
    CHECK(c.next(t + 161u * ms, iv) == t + 185u * ms);
    // a new interval restarts the grid
    CHECK(c.next(t + 190u * ms, 40u * ms) == 0u);
    CHECK(c.next(t + 200u * ms, 40u * ms) == t + 230u * ms);
    // reset (the window was hidden): the next frame starts a new grid
    c.reset();
    CHECK(c.next(t + 500u * ms, 40u * ms) == 0u);
    CHECK(c.next(t + 501u * ms, 40u * ms) == t + 540u * ms);
    // no interval: never sleeps
    CHECK(c.next(t + 600u * ms, 0u) == 0u);
    CHECK(c.next(t + 601u * ms, 0u) == 0u);
  }
  { // a steady game: 10 ms of work per frame at a 25 ms cadence gives even 25 ms frames
    framecap::Cadence c;
    uint64_t t = 0u, prev = 0u, lo = ~0ull, hi = 0u;
    for (int i = 0; i < 1000; i++) {
      t += 10u * ms;                                 // the frame's work
      const uint64_t until = c.next(t, 25u * ms);
      if (until) t = until;                          // the cap's sleep
      if (i > 0) { lo = std::min(lo, t - prev); hi = std::max(hi, t - prev); }
      prev = t;
    }
    CHECK(lo == 25u * ms && hi == 25u * ms);
  }

  // --- the sleeper, on a fake clock: the system's sleep oversleeps by `over` plus `share` of
  // its length (coalesced timers), a spin step advances the clock by 1 us (and counts as spin)
  {
    uint64_t now = 0u, over = 0u, share = 0u, spun = 0u, sleeps = 0u;
    auto clock = [&] { return now; };
    auto spinClock = [&] { now += 1000u; spun += 1000u; return now; };
    auto sys = [&] (uint64_t ns) { now += ns + over + (share ? ns / share : 0u); sleeps++; };
    auto sleep = [&] (framecap::Sleeper& s, uint64_t ns) {
      spun = 0u; sleeps = 0u;
      const uint64_t until = now + ns;
      return int64_t(s.sleepUntil(now, until, spinClock, [&] (uint64_t n) { sys(n); })) - int64_t(until);
    };
    framecap::Sleeper s(200000u);
    CHECK(s.spinNs() == 200000u);                      // nothing overslept yet: the floor

    // a precise system sleep: 20 ms in halves (10, 5, 2.5, 1.25 ... the last <= 2 ms), on time,
    // spinning the floor
    int64_t miss = sleep(s, 20u * ms);
    CHECK(miss >= 0 && miss <= 1000);
    CHECK(spun >= 190000u && spun <= 210000u);
    CHECK(sleeps >= 4u && sleeps <= 6u);
    CHECK(s.lateNs() < 2000u);

    // late by a fifth of every sleep (a background process, display asleep): the halves make it
    // up; the last piece is at most 2 ms, so at most 0.4 ms late: the first deadline is missed
    // by at most 0.2 ms (the floor's spin), the later ones met
    share = 5u;
    miss = sleep(s, 29u * ms);
    CHECK(miss >= 0 && miss <= 250000);
    std::printf("sleeper, 20%% late: sleeps %llu, spun %.2f ms, late %.2f ms\n",
      (unsigned long long)sleeps, double(spun) * 1e-6, double(s.lateNs()) * 1e-6);
    for (int i = 0; i < 100; i++) {
      miss = sleep(s, 29u * ms);
      CHECK(miss >= 0 && miss <= 1000);
    }
    CHECK(spun <= 1000000u);
    share = 0u;

    // a fixed 3 ms of oversleep: the first deadline is missed, then the spin covers it
    over = 3u * ms;
    miss = sleep(s, 20u * ms);
    CHECK(miss > 0);
    CHECK(s.lateNs() >= 3u * ms);
    miss = sleep(s, 20u * ms);
    CHECK(miss >= 0 && miss <= 1000);
    std::printf("sleeper, 3 ms late: spin %.2f ms, spun %.2f ms\n", double(s.spinNs()) * 1e-6, double(spun) * 1e-6);

    // the oversleep stops: the spin decays back to the floor
    over = 0u;
    for (int i = 0; i < 1000; i++)
      CHECK(sleep(s, 20u * ms) >= 0);
    CHECK(s.spinNs() == 200000u);

    // 10 ms of fixed oversleep: the spin stops at 4 ms
    over = 10u * ms;
    for (int i = 0; i < 3; i++)
      sleep(s, 20u * ms);
    CHECK(s.spinNs() == framecap::Sleeper::MaxSpinNs);
    over = 0u;

    // a deadline already past: returns at once
    const uint64_t t = now;
    CHECK(s.sleepUntil(t, t - 1u, clock, [&] (uint64_t ns) { sys(ns); }) == t);
    // the floor is capped at 4 ms
    CHECK(framecap::Sleeper(9u * ms).spinNs() == framecap::Sleeper::MaxSpinNs);
  }
  { // the real one (std::this_thread::sleep_for natively)
    auto clock = [] {
      return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    };
    framecap::Sleeper s;
    const uint64_t t0 = clock();
    const uint64_t woke = s.sleepUntil(t0, t0 + 5u * ms, clock);
    CHECK(woke >= t0 + 5u * ms);
    CHECK(clock() >= t0 + 5u * ms);
  }

  if (g_fail)
    std::printf("frame_cap_test: %d failures\n", g_fail);
  else
    std::printf("frame_cap_test: all checks passed\n");
  return g_fail ? 1 : 0;
}
