#pragma once

// The one cheap tick source of the D9MT_PROFILE timers (d9mt #126): prof::Scope in the backend
// (mtl_profiler_scope.h) and hitch::Scope in the front end (d3d9_hitch.h) both read now() and
// convert with process().
//
// On x86 a tick is the TSC (rdtsc: no unix call under Wine, where steady_clock goes through
// __wine_unix_call -> clock_gettime). The tick length is measured once, while the module's
// statics initialize and only with D9MT_PROFILE set (the profilers' switch), against
// steady_clock over kCalibrationNs. It never changes afterwards: every thread reads the same
// immutable Calibration without a race or an initialization guard. When there is no TSC (any
// other architecture), profiling is off, or the measurement is implausible, ticks are
// steady_clock nanoseconds and nsPerTick is 1.
//
// With D9MT_PROFILE unset nothing here runs but one getenv at load: callers reach now() and
// process() only behind their enabled flag, and now() inlines to a load, a branch and rdtsc.
//
// Dependency-free so it builds natively (test/native/prof_clock_test.cpp).

#include <chrono>
#include <cstdint>
#include <cstdlib>

#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#define D9MT_CHEAP_CLOCK_TSC 1
#else
#define D9MT_CHEAP_CLOCK_TSC 0
#endif

namespace dxvk::cheap_clock {

  // Target span per calibration window: 2 ms, only at load with profiling on.
  constexpr uint64_t kCalibrationNs = 2000000u;

  // A plausible tick length: a 10 MHz to 100 GHz counter. Anything else falls back.
  constexpr double kMinNsPerTick = 0.01;
  constexpr double kMaxNsPerTick = 100.0;

  // Upper bound on calibration loop turns, so a clock that never advances can't hang the caller.
  constexpr uint32_t kMaxCalibrationSpins = 1u << 22;

  inline uint64_t steadyNs() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
  }

  // The raw counter: the TSC on x86, steady_clock ns elsewhere.
  inline uint64_t tsc() {
#if D9MT_CHEAP_CLOCK_TSC
    return __rdtsc();
#else
    return steadyNs();
#endif
  }

  struct Calibration {
    bool   tsc       = false;  // ticks are the TSC; false: ticks are steady_clock ns
    double nsPerTick = 1.0;

    double toNs(uint64_t ticks) const { return double(ticks) * nsPerTick; }
    double toMs(uint64_t ticks) const { return double(ticks) * nsPerTick * 1e-6; }
  };

  // Two (ticks, ns) samples -> a TSC calibration, or the steady_clock fallback when the samples
  // don't span minSpanNs, either clock didn't advance, or the ratio is implausible.
  inline Calibration fromSamples(uint64_t t0, uint64_t ns0, uint64_t t1, uint64_t ns1,
                                 uint64_t minSpanNs = kCalibrationNs) {
    if (ns1 <= ns0 || t1 <= t0 || ns1 - ns0 < minSpanNs)
      return Calibration();
    const double r = double(ns1 - ns0) / double(t1 - t0);
    if (!(r >= kMinNsPerTick && r <= kMaxNsPerTick))
      return Calibration();
    Calibration c;
    c.tsc       = true;
    c.nsPerTick = r;
    return c;
  }

  // Bracket each tick read with ns reads. Reject preempted endpoints and retry at
  // most four windows; a stalled clock or persistent scheduling noise falls back.
  // The ns clock must resolve <= 1 us. Endpoint uncertainty is <= 0.1% of 2 ms.
  constexpr uint64_t kMaxEndpointNs = 2000u;
  constexpr uint32_t kCalibrationAttempts = 4u;

  template<typename TickFn, typename NsFn>
  Calibration measure(TickFn ticks, NsFn ns, uint64_t spanNs = kCalibrationNs) {
    auto sample = [&] (uint64_t& t, uint64_t& midpoint) {
      const uint64_t a = ns();
      t = ticks();
      const uint64_t b = ns();
      if (b < a || b - a > kMaxEndpointNs)
        return false;
      midpoint = a + (b - a) / 2u;
      return true;
    };
    uint32_t spins = 0u;
    for (uint32_t attempt = 0u; attempt < kCalibrationAttempts; attempt++) {
      uint64_t t0, ns0;
      if (!sample(t0, ns0)) continue;
      while (spins++ < kMaxCalibrationSpins) {
        uint64_t t1, ns1;
        if (!sample(t1, ns1) || ns1 < ns0) break;
        if (ns1 - ns0 >= spanNs)
          return fromSamples(t0, ns0, t1, ns1, spanNs);
      }
      if (spins >= kMaxCalibrationSpins) break;
    }
    return Calibration();
  }

  inline Calibration initialCalibration() {
#if D9MT_CHEAP_CLOCK_TSC
    if (std::getenv("D9MT_PROFILE") != nullptr)
      return measure(tsc, steadyNs);
#endif
    return Calibration();
  }

  // The process's calibration: set during static initialization, then immutable.
  inline const Calibration g_calibration = initialCalibration();

  inline const Calibration& process() {
    return g_calibration;
  }

  // The fallback read, kept out of line so now() stays small where it inlines (the device lock).
  [[gnu::noinline]] inline uint64_t steadyNsOutOfLine() {
    return steadyNs();
  }

  // Now, in process()'s ticks.
  inline uint64_t now() {
#if D9MT_CHEAP_CLOCK_TSC
    return __builtin_expect(g_calibration.tsc, 1) ? tsc() : steadyNsOutOfLine();
#else
    return steadyNs();
#endif
  }

  // t1 - t0, or 0 if the counter went backwards (never a wrapped huge value).
  inline uint64_t elapsed(uint64_t t0, uint64_t t1) {
    return t1 > t0 ? t1 - t0 : 0u;
  }

}
