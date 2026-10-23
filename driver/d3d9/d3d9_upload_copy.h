#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace dxvk {

  // One stream's copy in D3D9DeviceEx::UploadPerDrawData: a per-draw-uploaded vertex buffer's CPU
  // copy into the draw's upload slice. Kept apart so test/native/upload_copy_test.cpp runs exactly
  // this code under AddressSanitizer and UndefinedBehaviorSanitizer.
  struct D3D9VboCopy {
    uint32_t srcOffset;
    uint32_t dstOffset;
    uint32_t copyBufferLength;
    uint32_t copyElementCount;
    uint32_t copyElementSize;
    uint32_t copyElementStride;
  };

  // elementCount vertices of srcStride bytes from srcOffset (the first vertex), copyElementSize
  // (dstStride) bytes of each, at most up to the buffer's end: a vertex the end cuts through is
  // copied as far as it goes. The part left in the buffer is measured from srcOffset (#56: from the
  // stream offset, the copy read first vertex x stride bytes past the CPU copy). The bound is
  // compared in 64 bits, so a large first vertex or count can't wrap past it.
  inline void d3d9ClampVboCopy(D3D9VboCopy& copy, uint32_t srcOffset, uint32_t elementCount,
                               uint32_t srcStride, uint32_t dstStride, uint32_t vertexBufferSize) {
    copy.srcOffset         = srcOffset;
    copy.copyElementStride = srcStride;
    copy.copyElementSize   = dstStride;
    if (srcOffset > vertexBufferSize) {
      copy.copyElementCount = elementCount;
      copy.copyBufferLength = 0u;
    } else if (uint64_t(srcOffset) + uint64_t(elementCount) * srcStride > vertexBufferSize) {
      const uint32_t bound = vertexBufferSize - srcOffset;
      copy.copyElementCount = bound / srcStride;
      copy.copyBufferLength = copy.copyElementCount * dstStride + std::min(dstStride, bound % srcStride);
    } else {
      copy.copyElementCount = elementCount;
      copy.copyBufferLength = elementCount * dstStride;
    }
  }

  // The copy itself: packed when every vertex is copied whole, else vertex by vertex, then the
  // partial last vertex if there is one.
  inline void d3d9CopyVbo(uint8_t* dst, const uint8_t* src, const D3D9VboCopy& copy) {
    if (copy.copyElementStride == copy.copyElementSize) {
      std::memcpy(dst, src, copy.copyBufferLength);
      return;
    }
    for (uint32_t j = 0; j < copy.copyElementCount; j++)
      std::memcpy(dst + j * copy.copyElementSize, src + j * copy.copyElementStride, copy.copyElementSize);
    if (copy.copyBufferLength > copy.copyElementCount * copy.copyElementSize) {
      std::memcpy(
        dst + copy.copyElementCount * copy.copyElementSize,
        src + copy.copyElementCount * copy.copyElementStride,
        copy.copyBufferLength - copy.copyElementCount * copy.copyElementSize);
    }
  }

}
