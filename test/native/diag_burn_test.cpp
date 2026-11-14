// Native unit test for D9MT_DIAG_BURN_US / D9MT_DIAG_BURN_CS_US (d9mt #199; no Wine, no Metal):
//   clang++ -std=c++17 -I driver/d3d9 test/native/diag_burn_test.cpp -o /tmp/dbt && /tmp/dbt
// run-native-tests.sh builds it for the host (arm64: ticks are steady_clock ns) and with
// -arch x86_64 (Rosetta: the TSC). It re-runs itself with D9MT_PROFILE=1, which calibrates the
// TSC on x86, so the tick conversion is tested with a real calibration too.
//
// The burn's length must be within 10 % of n twice: the median wall time of 101 runs, and the
// process CPU time over those runs divided by 101. Two stubs run through the same check and must
// fail it: one that returns at once, and one that sleeps for n (right wall time, no CPU time).
//
// Process CPU, not the thread's: under Rosetta on GitHub's virtual M1, CLOCK_THREAD_CPUTIME_ID
// reads 2 % of a pure ALU loop, while process CPU (and getrusage) reads all of it (#199's probe).
// The test has one thread, and process CPU is what the speed guard reads.
#include "d3d9_diag_burn.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace dxvk;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static double wallUs() {
  return double(std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::steady_clock::now().time_since_epoch()).count()) * 1e-3;
}

static double cpuUs() {
  timespec ts;
  clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
  return double(ts.tv_sec) * 1e6 + double(ts.tv_nsec) * 1e-3;
}

static bool within10(double v, double n) {
  return v >= n * 0.9 && v <= n * 1.1;
}

// The median wall time of `burn(ticksFromUs(us))` and its mean process CPU time, and whether both
// are within 10 % of us. Only the wall clock is read between runs: a CPU clock read can cost
// microseconds (Rosetta), which a per-run CPU time would count.
template<typename Burn>
static bool burnsUs(const char* name, Burn burn, uint32_t us) {
  const uint64_t ticks = diagburn::ticksFromUs(us);
  constexpr int kRuns = 101;
  std::vector<double> wall(kRuns);
  const double c0 = cpuUs();
  for (int i = 0; i < kRuns; i++) {
    const double w0 = wallUs();
    burn(ticks);
    wall[i] = wallUs() - w0;
  }
  const double c = (cpuUs() - c0) / kRuns;
  std::sort(wall.begin(), wall.end());
  const double w = wall[kRuns / 2];
  const bool ok = within10(w, us) && within10(c, us);
  std::printf("%-6s n=%4u us: median wall %8.1f us, mean cpu %8.1f us (%llu ticks): %s\n", name, us, w, c,
    (unsigned long long) ticks, ok ? "within 10 %" : "outside 10 %");
  return ok;
}

int main(int argc, char** argv) {
  (void) argc;
  const cheap_clock::Calibration& cal = cheap_clock::process();
  std::printf("clock: %s, %.4f ns per tick\n", cal.tsc ? "TSC" : "steady_clock", cal.nsPerTick);

  { // The value: a whole number of microseconds, 0 to kMaxUs; unset or empty is off.
    bool invalid = true;
    CHECK(diagburn::usFromEnv(nullptr, &invalid) == 0u && !invalid);
    CHECK(diagburn::usFromEnv("", &invalid) == 0u && !invalid);
    CHECK(diagburn::usFromEnv("300", &invalid) == 300u && !invalid);
    CHECK(diagburn::usFromEnv("000", &invalid) == 0u && !invalid);       // the guard's "=000"
    CHECK(diagburn::usFromEnv("0300", &invalid) == 300u && !invalid);
    CHECK(diagburn::usFromEnv("1000000", &invalid) == 1000000u && !invalid);
    const char* bad[] = { "1000001", "999999999", "4294967296", "18446744073709551917",
                          "-1", "+5", "1.5", "30abc", " 3", "3 ", "0x10", "1e3", "abc" };
    for (const char* v : bad) {
      invalid = false;
      const uint32_t us = diagburn::usFromEnv(v, &invalid);
      if (us != 0u || !invalid)
        std::printf("FAIL: \"%s\" read as %u, invalid %d\n", v, us, invalid);
      CHECK(us == 0u && invalid);
    }
  }

  { // Microseconds to ticks: steady_clock ns, or the TSC at its calibrated length.
    cheap_clock::Calibration ns;
    CHECK(diagburn::ticksFromUs(0u, ns) == 0u);
    CHECK(diagburn::ticksFromUs(300u, ns) == 300000u);
    CHECK(diagburn::ticksFromUs(1000000u, ns) == 1000000000u);
    cheap_clock::Calibration tsc;
    tsc.tsc       = true;
    tsc.nsPerTick = 0.4;                                             // 2.5 GHz
    CHECK(diagburn::ticksFromUs(300u, tsc) == 750000u);
    tsc.nsPerTick = 1000.0 / 24.0;                                   // 24 MHz
    CHECK(diagburn::ticksFromUs(300u, tsc) == 7200u);
    tsc.nsPerTick = 100.0;                                           // the slowest plausible: never 0
    CHECK(diagburn::ticksFromUs(0u, tsc) == 0u);
    CHECK(diagburn::ticksFromUs(1000000u, tsc) == 10000000u);
  }

  { // The burn takes n us of wall and CPU time.
    CHECK(burnsUs("spin", [] (uint64_t t) { diagburn::spin(t); }, 50u));
    CHECK(burnsUs("spin", [] (uint64_t t) { diagburn::spin(t); }, 300u));
    CHECK(burnsUs("spin", [] (uint64_t t) { diagburn::spin(t); }, 1000u));

    // Controls: the same check fails a burn that doesn't burn, and one that sleeps.
    CHECK(!burnsUs("none", [] (uint64_t) { }, 300u));
    CHECK(!burnsUs("sleep", [] (uint64_t) {
      std::this_thread::sleep_for(std::chrono::microseconds(300));
    }, 300u));
  }

  if (g_fail)
    return 1;
  if (!std::getenv("D9MT_PROFILE")) {
    setenv("D9MT_PROFILE", "1", 1);
    std::fflush(stdout);
    execv(argv[0], argv);
    std::printf("FAIL: could not re-run with D9MT_PROFILE=1\n");
    return 1;
  }
  std::printf("ok\n");
  return 0;
}
