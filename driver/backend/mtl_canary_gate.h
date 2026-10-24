#pragma once

#include <cstdint>

namespace dxvk::backend {

  /**
   * \brief When a command buffer without a present copy gets a GPU canary encoder of its own
   *
   * The present copy into the drawable carries the canary fill in its own encoder, so a game
   * that presents arms a canary every frame at no extra cost. A command buffer without one
   * (a mid-frame flush, or every one while the presents are skipped: an occluded window, a
   * benchmark) needs an encoder of its own, which costs GPU and CPU time (bench_draws present,
   * one command buffer a frame: +1.5 % with one on every 4th, RC health pass 0.1.4).
   *
   * So such a command buffer gets one only if no canary of any kind was armed for kOwnGapNs:
   * none in a presenting game, at most one every 250 ms otherwise. A GPU fault discards the
   * faulting command buffer and those queued after it, so the next present's canary or the
   * next own one (within 250 ms) still reports it.
   */
  class CanaryGate {

  public:

    static constexpr uint64_t kOwnGapNs = 250'000'000ull;

    /// A canary (present or own) was armed at nowNs.
    void armed(uint64_t nowNs) {
      m_lastArmedNs = nowNs;
      m_armedEver   = true;
    }

    /// Whether a command buffer without a present copy gets an encoder of its own at nowNs.
    bool ownDue(uint64_t nowNs) const {
      return !m_armedEver || nowNs - m_lastArmedNs >= kOwnGapNs;
    }

  private:

    uint64_t m_lastArmedNs = 0u;
    bool     m_armedEver   = false;

  };

}
