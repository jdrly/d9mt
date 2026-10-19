#pragma once

// D9MT_LATENCY_LOG: per-frame latency log (backend/mtl_latency_log.h). Off by default; when
// off, the only cost is a predictable branch per frame (Present) and per command buffer.

#include <chrono>
#include <cstdint>
#include <cstdlib>

namespace dxvk::latency {

  inline const bool g_enabled = [] {
    const char* v = std::getenv("D9MT_LATENCY_LOG");
    return v && *v && !(v[0] == '0' && !v[1]);
  }();

  inline uint64_t nowNs() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
  }

}
