// Native unit test for the GPU canary's sampling (driver/backend/mtl_canary_gate.h, and
// MtlContextImpl::armCanary's decision, modelled here exactly: a present copy arms its own
// canary; any other command buffer gets an encoder of its own on every 4th, and one that doesn't
// present also waits for CanaryGate).
//  A. occluded, one command buffer every 0.2 ms for 10 s (bench_draws present): ~4 own encoders
//     a second, not ~1250;
//  B. a game presenting by copy, 60 fps, three flushes a frame: no own encoder at all;
//  C. a game presenting by blit or MetalFX (no copy canary): every presenting command buffer
//     carries a canary (the old rule sampled every 4th command buffer of any kind, so a frame's
//     present only by the counter's phase); the cost stays at one encoder a frame or less;
//  D. the spacing, from every phase of the every-4th counter: the next own mark comes at the
//     next eligible submit after 250 ms (at least 250 ms, less than 250 ms + 4 submits);
//  E. a finite discarded burst without presents: one inside the spacing isn't reported (the
//     documented trade-off), one longer than the spacing plus 4 submits always is; with copy
//     presents every frame, any burst of a frame or more is.
// The policy under test is the gate's. Built with -DCANARY_GATE_TEST_OLD_POLICY, the same
// assertions run on the old every-4th rule (b0d4597): A, B and D fail there, which is what this
// change fixes.
//   clang++ -std=c++17 -I driver/backend test/native/canary_gate_test.cpp -o /tmp/cgt && /tmp/cgt
#include "mtl_canary_gate.h"

#include <cstdio>

using namespace dxvk::backend;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static constexpr uint64_t ms = 1'000'000ull;

#ifdef CANARY_GATE_TEST_OLD_POLICY
static constexpr bool kGate = false;
#else
static constexpr bool kGate = true;
#endif

// armCanary's decision for one submitted command buffer (the counter and the gate as in
// mtl_context_record.cpp). Returns whether the command buffer carries a canary mark.
struct Sampler {
  bool       gate = kGate;
  CanaryGate g;
  uint32_t   counter = 0u;
  unsigned   own = 0u;

  bool submit(uint64_t now, bool copyPresent, bool presents) {
    if (copyPresent) {         // canarySlot("present"): the copy's encoder carries it
      g.armed(now);
      return true;
    }
    if (!gate || !presents) {  // the gate's rule: a scaled present always gets one
      if (counter++ % 4u != 0u)
        return false;
      if (gate && !g.ownDue(now))
        return false;
    }
    g.armed(now);              // canarySlot(reason) for an encoder of its own
    own++;
    return true;
  }
};

int main() {
  {
    // the gate's own rules
    CanaryGate g;
    CHECK(g.ownDue(0u));
    g.armed(1000 * ms);
    CHECK(!g.ownDue(1250 * ms - 1u));
    CHECK(g.ownDue(1250 * ms));
  }
  {
    // A. occluded bench: 50000 command buffers, 0.2 ms apart, none presents
    Sampler s;
    for (uint64_t i = 0u; i < 50000u; i++)
      s.submit(i * 200'000ull, false, false);
    std::printf("A. occluded bench, 10 s: %u own encoders\n", s.own);
    CHECK(s.own >= 40u && s.own <= 41u);
  }
  {
    // B. a game presenting by copy: three flushes and a presenting command buffer a frame
    Sampler s;
    uint64_t now = 0u;
    for (int frame = 0; frame < 600; frame++) {
      for (int f = 0; f < 3; f++) { now += 4 * ms; s.submit(now, false, false); }
      now += 4 * ms;
      s.submit(now, true, true);
    }
    std::printf("B. copy presents, 600 frames: %u own encoders\n", s.own);
    CHECK(s.own <= 1u);  // the first flush, before any present: the canary's first mark at once
  }
  {
    // C. a game presenting by blit/MetalFX, three flushes a frame and then with one command
    // buffer a frame: every presenting command buffer carries a canary, at most one encoder a frame
    for (int flushes = 3; flushes >= 0; flushes -= 3) {
      Sampler s, old;
      old.gate = false;
      unsigned presented = 0u, sampled = 0u, oldSampled = 0u;
      uint64_t now = 0u;
      for (int frame = 0; frame < 600; frame++) {
        for (int f = 0; f < flushes; f++) { now += 4 * ms; s.submit(now, false, false); old.submit(now, false, false); }
        now += 4 * ms;
        presented++;
        sampled    += s.submit(now, false, true);
        oldSampled += old.submit(now, false, true);
      }
      std::printf("C. blit presents, %d flushes a frame: %u of %u presents carry a canary (old rule %u); own encoders %u (old %u)\n",
                  flushes, sampled, presented, oldSampled, s.own, old.own);
      CHECK(sampled == presented);
      CHECK(s.own <= presented + 1u);
    }
  }
  {
    // D. the spacing, from each phase of the counter: a mark at t = 0, then a command buffer
    // every 1 ms, none presents
    for (uint32_t phase = 0u; phase < 4u; phase++) {
      Sampler s;
      s.counter = 0u;
      s.submit(0u, false, false);        // the first own mark (counter 0)
      s.counter += phase;                // the counter's phase from here on
      uint64_t next = 0u;
      for (uint64_t t = 1u; t <= 2000u && !next; t++)
        if (s.submit(t * ms, false, false))
          next = t * ms;
      std::printf("D. phase %u: next own mark at %llu ms\n", phase, (unsigned long long)(next / ms));
      CHECK(next >= 250 * ms);
      CHECK(next < 254 * ms);
    }
  }
  {
    // E. a finite burst: a fault at tf discards every command buffer submitted in [tf, tf + L);
    // it's reported only if one of them carries a mark. No presents, one every 1 ms.
    auto reported = [] (uint64_t tf, uint64_t len, bool copyPresents) {
      Sampler s;
      bool seen = false;
      for (uint64_t t = 0u; t < 3000u; t++) {
        const uint64_t now = t * ms;
        const bool present = copyPresents && t % 16u == 0u;
        const bool marked = s.submit(now, present, present);
        if (marked && now >= tf && now < tf + len)
          seen = true;
      }
      return seen;
    };
    // inside the spacing, right after a mark: not reported with the gate, reported by the old rule
    const bool shortBurst = reported(10 * ms, 20 * ms, false);
    std::printf("E. a 20 ms burst 10 ms after a mark, no presents: %s\n", shortBurst ? "reported" : "not reported (the trade-off)");
    CHECK(shortBurst == !kGate);
    // longer than the spacing plus 4 command buffers: reported, wherever it starts
    for (uint64_t tf = 0u; tf < 300u; tf += 7u)
      CHECK(reported(tf * ms, 254 * ms, false));
    // with copy presents every 16 ms: any burst of 16 ms or more
    for (uint64_t tf = 0u; tf < 300u; tf += 5u)
      CHECK(reported(tf * ms, 16 * ms, true));
  }
  std::printf(g_fail ? "canary_gate_test (%s policy): %d FAILED\n" : "canary_gate_test (%s policy): all passed%.0d\n",
              kGate ? "gate" : "old every-4th", g_fail);
  return g_fail ? 1 : 0;
}
