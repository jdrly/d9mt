#pragma once

#include <atomic>
#include <cstdint>

namespace dxvk::d9mtdbg {

  /* Live debug toggles, read from a flags file on Present (d3d9_debug.cpp).
   * The frontend keeps its own copy for state binding; the backend reads
   * this atomic where it applies a toggle itself. */
  enum Flag : uint32_t {
    OccVisible      = 1u << 0,  // occ_visible
    OccTrace        = 1u << 1,  // occ_trace
    OccSync         = 1u << 2,  // occ_sync
    NoScissor       = 1u << 3,  // no_scissor
    NoDepthBias     = 1u << 4,  // no_depthbias
    CullNone        = 1u << 5,  // cull_none
    DepthClamp      = 1u << 6,  // depth_clamp
    NoZTestBlended  = 1u << 7,  // no_ztest_blended
    NoSameViewSkip  = 1u << 8,  // no_sameview_skip
    NoClipPlanes    = 1u << 9,  // no_clipplanes
  };

  inline std::atomic<uint32_t> g_flags = { 0u };

  inline uint32_t flags() {
    return g_flags.load(std::memory_order_relaxed);
  }

}
