#pragma once

// Indexed triangle fans (#187). Metal has no fan primitive, so an indexed fan is drawn as the
// triangle list (i0, i1, i2), (i0, i2, i3), ...: fan triangle t is (i0, i(t+1), i(t+2)). The
// frontend expands at the API call from the index buffer's CPU copy (a DEFAULT or MANAGED
// buffer's GPU copy may still wait on a copy in the command stream when the draw is encoded),
// into an UP buffer the draw then reads as a list.

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace dxvk::fan {

  /// The triangles an indexed fan draws: `primitives` asked for, `available` indices readable
  /// from its first index. A fan of N triangles reads N + 2 indices; fewer than 3 draw nothing.
  inline uint32_t triangles(uint32_t primitives, uint64_t available) {
    if (available < 3u)
      return 0u;
    return uint32_t(std::min<uint64_t>(primitives, available - 2u));
  }

  /// Writes the list for `count` fan triangles into `dst` (3 * count indices of `indexSize`
  /// bytes, 2 or 4) from the fan's count + 2 indices at `src`. Reads nothing past them.
  inline void expand(void* dst, const void* src, uint32_t count, uint32_t indexSize) {
    auto* d = static_cast<uint8_t*>(dst);
    const auto* s = static_cast<const uint8_t*>(src);
    for (uint32_t t = 0u; t < count; t++) {
      std::memcpy(d, s, indexSize);
      std::memcpy(d + indexSize, s + (size_t(t) + 1u) * indexSize, 2u * indexSize);
      d += 3u * indexSize;
    }
  }

}
