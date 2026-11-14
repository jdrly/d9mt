#pragma once

// D9MT_DIAG_BURN_US=<n> and D9MT_DIAG_BURN_CS_US=<n> (d9mt #199): a CPU cost of known size, so a
// speed guard can show it sees one. The first spins n microseconds on the thread that calls
// Present, at its entry (before the frame goes to the CS thread); the second spins n
// microseconds on the CS thread, inside EndFrame's command. Once per Present each.
//
// The spin reads the cheap clock (util_cheap_clock.h) and never sleeps, so the whole of n is CPU
// time. The device reads both values when it is created and keeps them in ticks; unset, Present
// and EndFrame test a zero and do nothing else. A developer switch: the app never sets it.
//
// Dependency-free (the cheap clock and framecap's parser aside) so it builds natively
// (test/native/diag_burn_test.cpp).

#include "d3d9_frame_cap.h"
#include "../util/util_cheap_clock.h"

#include <cstdint>

namespace dxvk::diagburn {

  // The longest burn: one second per frame.
  constexpr uint32_t kMaxUs = 1000000u;

  // The variable's value in microseconds. Unset or empty: 0, valid. A whole decimal number from
  // 0 to kMaxUs (leading zeros allowed, so "000" and "300" have the same length): that. Anything
  // else (a sign, a point, a suffix, a space, more than kMaxUs): 0, and *invalid.
  inline uint32_t usFromEnv(const char* v, bool* invalid) {
    *invalid = false;
    if (!v || !*v)
      return 0u;
    uint32_t us = 0u;
    if (!framecap::parseUint(v, us) || us > kMaxUs) {
      *invalid = true;
      return 0u;
    }
    return us;
  }

  // Microseconds in the cheap clock's ticks: the TSC at its calibrated length, or steady_clock ns.
  inline uint64_t ticksFromUs(uint32_t us, const cheap_clock::Calibration& c = cheap_clock::process()) {
    return uint64_t(double(us) * 1000.0 / c.nsPerTick + 0.5);
  }

  // Spins `ticks` of cheap_clock::now() on the calling thread. Out of line and cold: unset, the
  // callers keep only the test of `ticks`.
  [[gnu::noinline, gnu::cold]] inline void spin(uint64_t ticks) {
    const uint64_t t0 = cheap_clock::now();
    while (cheap_clock::elapsed(t0, cheap_clock::now()) < ticks) { }
  }

}
