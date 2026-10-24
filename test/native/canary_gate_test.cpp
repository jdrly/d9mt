// Native unit test for CanaryGate (driver/backend/mtl_canary_gate.h): when a command buffer
// without a present copy gets a GPU canary encoder of its own:
//  - the first one always does (the canary's "on" line and its first mark come at once);
//  - a presenting game (a present canary armed every frame) never needs one;
//  - with the presents skipped, at most one every 250 ms, and the next one exactly at 250 ms;
//  - an own canary counts like a present one: after it, the gap starts again.
// It also models armCanary's loop over a bench_draws-like run (5000 command buffers a second,
// every 4th consulting the gate) and a game at 60 fps with three flushes a frame.
//   clang++ -std=c++17 -I driver/backend test/native/canary_gate_test.cpp -o /tmp/cgt && /tmp/cgt
#include "mtl_canary_gate.h"

#include <cstdio>

using namespace dxvk::backend;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static constexpr uint64_t ms = 1'000'000ull;

int main() {
  {
    CanaryGate g;
    CHECK(g.ownDue(0u));
    CHECK(g.ownDue(123 * ms));
  }
  {
    // with the presents skipped: own ones 250 ms apart, exactly at the boundary
    CanaryGate g;
    g.armed(1000 * ms);
    CHECK(!g.ownDue(1000 * ms));
    CHECK(!g.ownDue(1249 * ms));
    CHECK(!g.ownDue(1250 * ms - 1u));
    CHECK(g.ownDue(1250 * ms));
    CHECK(g.ownDue(5000 * ms));
  }
  {
    // a presenting game: the present canary every 16.7 ms, so no own one, ever
    CanaryGate g;
    uint64_t now = 0u;
    int own = 0;
    g.armed(now);
    for (int frame = 0; frame < 600; frame++) {
      for (int flush = 0; flush < 3; flush++) {
        now += 4 * ms;
        if (g.ownDue(now)) { own++; g.armed(now); }
      }
      now += 4 * ms;
      g.armed(now);  // the frame's present copy
    }
    CHECK(own == 0);
  }
  {
    // bench_draws present, occluded: 5000 command buffers a second, every 4th asks the gate
    // (armCanary's kCanaryEvery pre-filter), for 10 s: about 4 own encoders a second, not 1250
    CanaryGate g;
    uint64_t now = 0u;
    unsigned own = 0u, asked = 0u;
    for (unsigned cb = 0u; cb < 50000u; cb++) {
      now += 200'000ull;  // 0.2 ms
      if (cb % 4u != 0u)
        continue;
      asked++;
      if (g.ownDue(now)) { own++; g.armed(now); }
    }
    CHECK(asked == 12500u);
    CHECK(own >= 40u && own <= 41u);
  }
  {
    // the presents stop (the window gets occluded): own ones resume within 250 ms
    CanaryGate g;
    uint64_t now = 0u;
    for (int frame = 0; frame < 60; frame++) { now += 16 * ms; g.armed(now); }
    const uint64_t lastPresent = now;
    uint64_t firstOwn = 0u;
    for (int cb = 0; cb < 1000 && !firstOwn; cb++) {
      now += 1 * ms;
      if (g.ownDue(now)) { firstOwn = now; g.armed(now); }
    }
    CHECK(firstOwn == lastPresent + 250 * ms);
  }
  std::printf(g_fail ? "canary_gate_test: %d FAILED\n" : "canary_gate_test: all passed%.0d\n", g_fail);
  return g_fail ? 1 : 0;
}
