// Native unit test for the shared cheap profiler clock (d9mt #126) and PROF's scope/window
// (no Wine, no Metal):
//   clang++ -std=c++17 -I driver/backend test/native/prof_clock_test.cpp -o /tmp/pc && /tmp/pc
// run-native-tests.sh builds it twice: for the host (arm64: the steady_clock fallback) and with
// -arch x86_64 (Rosetta: rdtsc, the real calibration, as the x64 d3d9.dll sees it).
//
// Deterministic parts: calibration from fake clocks (conversion, every fallback reason, a clock
// that never advances), BasicScope with a fake clock (nested scopes stay inclusive), the window
// (rollover every 256 frames, reset, conversion). Real-clock parts: process() is consistent,
// deltas are monotonic, ticks agree with steady_clock over 50 ms, and threads agree.
// Scopes only record with D9MT_PROFILE set (read during static initialisation), so the test
// re-runs itself with it set.
#include <cstdlib>
#include <unistd.h>

#include "mtl_profiler_scope.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

using namespace dxvk;
using namespace dxvk::backend;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static bool near(double a, double b, double rel) {
  return std::fabs(a - b) <= rel * std::fabs(b);
}

// A deterministic clock: time moves only when the test says so.
struct FakeClock {
  static inline uint64_t t = 0u;
  static uint64_t now() { return t; }
};
using FakeScope = prof::BasicScope<FakeClock>;

int main(int argc, char** argv) {
  (void) argc;
  if (!prof::g_profEnabled) {
    CHECK(!cheap_clock::process().tsc && cheap_clock::process().nsPerTick == 1.0);
    { FakeScope scope(prof::Submit); FakeClock::t += 100u; }
    CHECK(prof::g_ticks[prof::Submit] == 0u);
    if (g_fail) return 1;
    setenv("D9MT_PROFILE", "1", 1);
    execv(argv[0], argv);
    std::printf("FAIL: could not re-run with D9MT_PROFILE=1\n");
    return 1;
  }

  { // Conversion: two samples -> ns per tick.
    cheap_clock::Calibration c = cheap_clock::fromSamples(1000u, 5000000u, 1000u + 7500000u, 5000000u + 3000000u);
    CHECK(c.tsc);
    CHECK(near(c.nsPerTick, 0.4, 1e-12));                  // 2.5 GHz
    CHECK(near(c.toNs(2500u), 1000.0, 1e-12));
    CHECK(near(c.toMs(2500000000u), 1000.0, 1e-12));

    cheap_clock::Calibration d;                             // default: the fallback
    CHECK(!d.tsc && d.nsPerTick == 1.0 && d.toNs(123u) == 123.0);
  }

  { // Every fallback reason gives steady_clock ns (tsc false, 1 ns per tick).
    auto isFallback = [] (const cheap_clock::Calibration& c) { return !c.tsc && c.nsPerTick == 1.0; };
    CHECK(isFallback(cheap_clock::fromSamples(0u, 0u, 1000u, 1000000u)));          // span < 2 ms
    CHECK(isFallback(cheap_clock::fromSamples(5u, 0u, 5u, 3000000u)));             // ticks stuck
    CHECK(isFallback(cheap_clock::fromSamples(9u, 0u, 5u, 3000000u)));             // ticks backwards
    CHECK(isFallback(cheap_clock::fromSamples(0u, 3000000u, 100u, 0u)));           // ns backwards
    CHECK(isFallback(cheap_clock::fromSamples(0u, 0u, 1000u, 3000000u)));          // 3000 ns/tick: too slow
    CHECK(isFallback(cheap_clock::fromSamples(0u, 0u, 3000000000000ull, 3000000u))); // 0.001 ns/tick: too fast
    CHECK(!isFallback(cheap_clock::fromSamples(0u, 0u, 72000u, 3000000u)));        // 24 MHz: plausible
  }

  { // Coherent simulated clocks: a tick is 0.4 ns, independent of read count.
    uint64_t wall = 0u, calls = 0u;
    auto noDeadline = [] { return uint64_t(0u); };
    auto c = cheap_clock::measure([&] { return wall * 5u / 2u; },
      [&] { calls++; return wall += 1000u; }, cheap_clock::kCalibrationNs, noDeadline);
    CHECK(c.tsc && near(c.nsPerTick, 0.4, 1e-12));
    CHECK(wall >= cheap_clock::kCalibrationNs && calls < 4000u);

    // Every third endpoint candidate is preempted by 500 us. Clean candidates
    // still calibrate; the old whole-window rejection exhausts its retries.
    wall = 0u;
    uint64_t candidates = 0u;
    bool closing = false;
    c = cheap_clock::measure([&] { closing = true; return wall * 2u; }, [&] {
      uint64_t step = 1000u;
      if (closing) { closing = false; if ((candidates++ % 3u) == 0u) step = 500000u; }
      return wall += step;
    }, cheap_clock::kCalibrationNs, noDeadline);
    CHECK(c.tsc && near(c.nsPerTick, 0.5, 1e-12) && candidates >= 6u);

    // The narrowest candidate changes position between start and end. Picking
    // the first candidate would bias the ratio by the asymmetric bracket widths.
    wall = 0u; candidates = 0u; closing = false;
    c = cheap_clock::measure([&] { closing = true; return wall * 2u; }, [&] {
      uint64_t step = 1000u;
      if (closing) {
        closing = false;
        const uint64_t k = candidates++ % 6u;
        step = (k == 1u || k == 3u) ? 0u : 2000u;
      }
      return wall += step;
    }, cheap_clock::kCalibrationNs, noDeadline);
    CHECK(c.tsc && near(c.nsPerTick, 0.5, 1e-12));

    // Accepted 2 us brackets need >3 ms once 1 us quantization is included.
    wall = 0u;
    c = cheap_clock::measure([&] { return wall * 2u; },
      [&] { return wall += 2000u; }, cheap_clock::kCalibrationNs, noDeadline);
    CHECK(c.tsc && near(c.nsPerTick, 0.5, 1e-12) && wall >= 3004001u);

    wall = 0u;
    CHECK(!cheap_clock::measure([&] { return wall; }, [&] { return wall += 500000u; },
      cheap_clock::kCalibrationNs, noDeadline).tsc); // no clean candidate
    uint64_t backwards = 10000000u;
    CHECK(!cheap_clock::measure([&] { return backwards; }, [&] { return --backwards; },
      cheap_clock::kCalibrationNs, noDeadline).tsc);
    calls = 0u;
    CHECK(!cheap_clock::measure([] { return uint64_t(1u); }, [&] {
      const uint64_t n = calls++;
      return (n % 4u < 2u ? uint64_t(10000u) : uint64_t(1000u)) + n % 2u;
    }, cheap_clock::kCalibrationNs, noDeadline).tsc); // backwards between brackets

    // Both clocks frozen: exactly the finite reference-read allowance is used.
    calls = 0u;
    CHECK(!cheap_clock::measure([] { return uint64_t(42u); }, [&] {
      calls++; return uint64_t(42u);
    }, cheap_clock::kCalibrationNs, noDeadline).tsc);
    CHECK(calls == cheap_clock::kMaxCalibrationReads);

    // Independent deadline expires even if the reference clock is frozen.
    uint64_t budgetCalls = 0u;
    calls = 0u;
    CHECK(!cheap_clock::measure([] { return uint64_t(42u); }, [&] {
      calls++; return uint64_t(42u);
    }, cheap_clock::kCalibrationNs, [&] { return budgetCalls++ * 5000000u; }).tsc);
    CHECK(calls < cheap_clock::kMaxCalibrationReads);
    // Forward and backwards budget-clock steps terminate without a long spin.
    for (bool back : {false, true}) {
      budgetCalls = 0u; wall = 0u;
      CHECK(!cheap_clock::measure([&] { return wall * 2u; }, [&] { return wall += 1000u; },
        cheap_clock::kCalibrationNs, [&] {
          if (back) return budgetCalls++ ? uint64_t(999u) : uint64_t(1000u);
          return ++budgetCalls > 3u ? cheap_clock::kCalibrationBudgetNs : uint64_t(0u);
        }).tsc);
    }
    // A backwards budget step must expire even while still above the start.
    budgetCalls = 0u; wall = 0u;
    CHECK(!cheap_clock::measure([&] { return wall * 2u; }, [&] { return wall += 1000u; },
      cheap_clock::kCalibrationNs, [&] {
        const uint64_t n = budgetCalls++;
        return n == 0u ? uint64_t(0u) : n == 1u ? uint64_t(1000000u) : uint64_t(500000u);
      }).tsc);
    CHECK(cheap_clock::elapsed(10u, 25u) == 15u);
    CHECK(cheap_clock::elapsed(25u, 10u) == 0u);
  }

  { // Nested scopes with a fake clock: buckets are inclusive, each counts its own span.
    for (uint64_t& t : prof::g_ticks) t = 0u;
    FakeClock::t = 1000u;
    { FakeScope outer(prof::PrepareDraw);
      FakeClock::t += 100u;
      { FakeScope inner(prof::ResolvePso);
        FakeClock::t += 40u;
        { FakeScope innermost(prof::FlushCmdChunk);
          FakeClock::t += 7u;
        }
      }
      { FakeScope again(prof::ResolvePso);                 // same bucket twice: adds up
        FakeClock::t += 3u;
      }
      FakeClock::t += 50u;
    }
    CHECK(prof::g_ticks[prof::PrepareDraw] == 200u);       // 100 + 47 + 3 + 50
    CHECK(prof::g_ticks[prof::ResolvePso] == 50u);         // 47 + 3
    CHECK(prof::g_ticks[prof::FlushCmdChunk] == 7u);
    CHECK(prof::g_ticks[prof::Submit] == 0u);

    // A clock that steps backwards inside a scope adds nothing (not 2^64 - n).
    { FakeScope back(prof::Submit);
      FakeClock::t -= 5u;
    }
    CHECK(prof::g_ticks[prof::Submit] == 0u);
  }

  { // Window rollover: totals appear on the 256th frame only, converted, then reset.
    cheap_clock::Calibration cal = cheap_clock::fromSamples(0u, 0u, 10000000u, 4000000u); // 0.4 ns/tick
    CHECK(cal.tsc);
    for (uint64_t& t : prof::g_ticks) t = 0u;
    prof::g_frames = 0u;
    double ms[prof::BucketCount] = {};
    uint32_t windows = 0u;

    prof::g_ticks[prof::Submit] = 5000000u;                // 2 ms at 0.4 ns/tick
    for (uint32_t f = 1u; f < prof::kWindowFrames; f++)
      windows += prof::takeWindow(cal, ms) ? 1u : 0u;
    CHECK(windows == 0u && prof::g_ticks[prof::Submit] == 5000000u);
    CHECK(prof::takeWindow(cal, ms));                      // frame 256
    CHECK(prof::g_frames == 256u);
    CHECK(near(ms[prof::Submit], 2.0, 1e-12));
    CHECK(ms[prof::PrepareDraw] == 0.0);
    for (uint64_t t : prof::g_ticks) CHECK(t == 0u);

    // The next window starts empty and is independent of the previous one.
    prof::g_ticks[prof::EncodeDraw] = 2500000u;            // 1 ms
    for (uint32_t f = 1u; f < prof::kWindowFrames; f++)
      CHECK(!prof::takeWindow(cal, ms));
    CHECK(prof::takeWindow(cal, ms) && prof::g_frames == 512u);
    CHECK(near(ms[prof::EncodeDraw], 1.0, 1e-12) && ms[prof::Submit] == 0.0);

    // The fallback calibration converts ticks as ns.
    prof::g_ticks[prof::EndFrame] = 3000000u;
    for (uint32_t f = 1u; f < prof::kWindowFrames; f++) prof::takeWindow(cheap_clock::Calibration(), ms);
    CHECK(prof::takeWindow(cheap_clock::Calibration(), ms) && near(ms[prof::EndFrame], 3.0, 1e-12));
  }

  { // The real clock: one calibration for the process, matching the build's architecture.
    const cheap_clock::Calibration& c = cheap_clock::process();
    CHECK(&c == &cheap_clock::process());
    std::printf("process calibration: tsc=%d nsPerTick=%.6f (%.1f MHz)\n",
      int(c.tsc), c.nsPerTick, 1e3 / c.nsPerTick);
#if D9MT_CHEAP_CLOCK_TSC
    CHECK(c.tsc);                                          // Rosetta/Intel with D9MT_PROFILE: the TSC calibrates
#else
    CHECK(!c.tsc && c.nsPerTick == 1.0);                   // no TSC: steady_clock ns
#endif
    CHECK(c.nsPerTick >= cheap_clock::kMinNsPerTick && c.nsPerTick <= cheap_clock::kMaxNsPerTick);

    // Monotonic deltas.
    uint64_t prev = cheap_clock::now(), backwards = 0u;
    for (int i = 0; i < 200000; i++) {
      const uint64_t t = cheap_clock::now();
      backwards += t < prev ? 1u : 0u;
      prev = t;
    }
    CHECK(backwards == 0u);

    // Converted ticks agree with steady_clock over 50 ms, through a real Scope.
    for (uint64_t& t : prof::g_ticks) t = 0u;
    const uint64_t ns0 = cheap_clock::steadyNs();
    { prof::Scope s(prof::SynchronizeWsi);
      while (cheap_clock::steadyNs() - ns0 < 50000000u) { }
    }
    const double steadyMs = double(cheap_clock::steadyNs() - ns0) * 1e-6;
    const double scopeMs  = c.toMs(prof::g_ticks[prof::SynchronizeWsi]);
    std::printf("50 ms spin: scope %.4f ms, steady %.4f ms\n", scopeMs, steadyMs);
    CHECK(near(scopeMs, steadyMs, 0.002));

    // Threads (and whatever cores they land on) agree: a tick read after another thread's read,
    // ordered by an atomic handoff, is never earlier.
    // rdtsc (and Rosetta's counter read) isn't ordered against loads, so allow 1 us of slack.
    const uint64_t slack = uint64_t(1000.0 / c.nsPerTick) + 1u;
    std::atomic<uint64_t> baton = { 0u };
    std::atomic<uint32_t> bad = { 0u };
    std::atomic<bool> stop = { false };
    std::vector<std::thread> threads;
    for (int k = 0; k < 4; k++) {
      threads.emplace_back([&, slack] {
        uint64_t local = 0u;
        while (!stop.load(std::memory_order_relaxed)) {
          const uint64_t seen = baton.load(std::memory_order_acquire);
          const uint64_t t = cheap_clock::now();
          if (t + slack < seen || t < local) bad.fetch_add(1u, std::memory_order_relaxed);
          local = t;
          uint64_t cur = seen;
          while (cur < t && !baton.compare_exchange_weak(cur, t, std::memory_order_acq_rel)) { }
          std::this_thread::yield();
        }
      });
    }
    const uint64_t end = cheap_clock::steadyNs() + 200000000u;
    while (cheap_clock::steadyNs() < end) std::this_thread::yield();
    stop = true;
    for (std::thread& t : threads) t.join();
    CHECK(bad.load() == 0u);
  }

  if (g_fail) {
    std::printf("%d check(s) failed\n", g_fail);
    return 1;
  }
  std::printf("prof_clock_test: all checks passed\n");
  return 0;
}
