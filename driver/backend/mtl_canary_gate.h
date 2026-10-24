#pragma once

#include <cstdint>

namespace dxvk::backend {

  /**
   * \brief When a command buffer that doesn't present gets a GPU canary encoder of its own
   *
   * The present copy into the drawable carries the canary fill in its own encoder, so a game
   * that presents by copy arms a canary every frame at no extra cost. A command buffer without
   * that canary needs an encoder of its own, which costs GPU and CPU time (bench_draws present,
   * occluded, one command buffer a frame: +1.5 % with one on every 4th, RC health pass 0.1.4).
   *
   * A command buffer that presents without the copy's canary (a scaled blit, MetalFX) gets one
   * every time, so every presented frame carries a canary (MtlContextImpl::armCanary). One that
   * doesn't present (a mid-frame flush, or every one while the presents are skipped: an
   * occluded window, a benchmark) gets one on every 4th, and only once no canary of any kind
   * was armed for kOwnGapNs. So:
   *  - in a game that presents: none (every presented frame carries a canary);
   *  - otherwise: the next eligible (every-4th) submit after those 250 ms. 250 ms is the
   *    minimum spacing, not the maximum delay: that is 250 ms plus the time to the next
   *    eligible submit, as long as submissions continue (no later work: no later mark).
   *
   * The trade-off, accepted for a diagnostic that only logs: a GPU fault discards the faulting
   * command buffer and those queued after it. The canary reports a sampled command buffer that
   * completes with its mark missing (checkCanaries needs completion progress). A discarded
   * burst longer than the spacing reaches such a command buffer and is reported once it
   * completes; one whose discarded command buffers all fall between two samples (no presents;
   * it ends and the GPU recovers before the next mark) isn't. A game that presents samples
   * every frame, by copy or not. A queue that never completes again (a hang) gives no LOST
   * line at all: main ends the process then (#53, D9MT-GPU-HUNG); a build without #53 stalls.
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
