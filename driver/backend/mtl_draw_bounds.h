#pragma once

// How far a draw may fetch from its vertex streams (#61). Metal 4's vertex fetch has no bounds:
// a non-indexed draw past the end of a stream, or more instances than a per-instance stream
// holds, reads past the buffer. The context derives the limits from its bound streams when they
// or the layout change, and each draw clamps its counts with clamp().

#include <algorithm>
#include <cstdint>

#include "mtl_pso_key.h"

namespace dxvk::backend::drawbounds {

  constexpr uint32_t Unbounded = ~0u;

  /// One stream as a draw fetches it: the bound slice's length, the bound stride, the
  /// declaration's vertex size on that stream (the end of its last attribute) and the step
  /// function byte psoStepFunction() gives it.
  struct Stream {
    uint64_t length       = 0u;
    uint32_t stride       = 0u;
    uint32_t vertexSize   = 0u;
    uint8_t  stepFunction = 0u;
  };

  struct Limits {
    uint32_t vertices  = Unbounded;   // non-indexed draws: firstVertex + vertexCount
    uint32_t instances = Unbounded;   // every draw: firstInstance + instanceCount
  };

  /// Elements e with e * stride + vertexSize <= length: the ones that read only inside.
  inline uint64_t elements(uint64_t length, uint32_t stride, uint32_t vertexSize) {
    return length < vertexSize ? 0u : (length - vertexSize) / stride + 1u;
  }

  /// Narrows `limits` by one stream. Constant streams read element 0 only: they don't limit.
  inline void add(Limits& limits, const Stream& s) {
    if (s.stepFunction == kPsoStepConstant || !s.stride)
      return;
    const uint64_t n = elements(s.length, s.stride, s.vertexSize);
    if (s.stepFunction == kPsoStepPerVertex) {
      limits.vertices = uint32_t(std::min<uint64_t>(limits.vertices, n));
    } else {
      const uint64_t rate = uint64_t(s.stepFunction) - 1u;
      limits.instances = uint32_t(std::min<uint64_t>(limits.instances, n * rate));
    }
  }

  /// The count a draw from `first` may keep under `limit`.
  inline uint32_t clamp(uint32_t first, uint32_t count, uint32_t limit) {
    return first >= limit ? 0u : std::min(count, limit - first);
  }

}
