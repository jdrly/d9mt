#pragma once

// PROF's timed buckets (D9MT_PROFILE): the scope, the per-bucket tick totals and the 256-frame
// window. Buckets nest and are not mutually exclusive: read a sub-bucket as a breakdown of its
// parent, not as additive across the whole frame.
//
// Scopes accumulate raw ticks of the shared cheap clock (util_cheap_clock.h, d9mt #126); the
// window converts them to ms once, when it rolls over. The CS thread is the only writer.
//
// Dependency-free so the scope and window logic is unit-tested natively with a deterministic
// clock (test/native/prof_clock_test.cpp). Formatting and logging stay in mtl_profiler.h.

#include <cstdint>
#include <cstdlib>

#include "../util/util_cheap_clock.h"

namespace dxvk::backend::prof {

  enum Bucket : uint32_t {
    BeginRecording = 0,
    PrepareDraw,
    ResolvePso,
    FlushVertexBuffers,
    FlushResources,
    FlushDynamicState,
    EncodeDraw,
    FlushCmdChunk,
    Submit,
    SynchronizeWsi,
    EndFrame,
    BucketCount
  };

  constexpr uint32_t kWindowFrames = 256u;

  inline const bool g_profEnabled = std::getenv("D9MT_PROFILE") != nullptr;

  // Raw cheap-clock ticks per bucket in the current window.
  inline uint64_t g_ticks[BucketCount] = {};
  inline uint64_t g_frames = 0u;

  // The production clock; a test substitutes a deterministic one.
  struct CheapClock {
    static uint64_t now() { return cheap_clock::now(); }
  };

  template<typename Clock>
  struct BasicScope {
    uint64_t t0 = 0u;
    uint32_t bucket;
    explicit BasicScope(Bucket b) : bucket(b) {
      if (!g_profEnabled) return;
      t0 = Clock::now();
    }
    ~BasicScope() {
      if (!g_profEnabled) return;
      g_ticks[bucket] += cheap_clock::elapsed(t0, Clock::now());
    }
    BasicScope(const BasicScope&) = delete;
    BasicScope& operator=(const BasicScope&) = delete;
  };

  using Scope = BasicScope<CheapClock>;

  // Counts a frame. Every kWindowFrames-th frame, converts the window's buckets to total ms with
  // `cal`, resets them and returns true. Only called with g_profEnabled.
  inline bool takeWindow(const cheap_clock::Calibration& cal, double (&totalMs)[BucketCount]) {
    if ((++g_frames % kWindowFrames) != 0u)
      return false;
    for (uint32_t i = 0u; i < BucketCount; i++) {
      totalMs[i] = cal.toMs(g_ticks[i]);
      g_ticks[i] = 0u;
    }
    return true;
  }

}
