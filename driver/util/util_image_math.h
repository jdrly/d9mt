#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>

#include "../mtl/mtl_types.h"


namespace dxvk::util {

  inline MtlExtent3D computeMipLevelExtent(MtlExtent3D size, uint32_t level) {
    size.width  = std::max(1u, size.width  >> level);
    size.height = std::max(1u, size.height >> level);
    size.depth  = std::max(1u, size.depth  >> level);
    return size;
  }

  inline MtlOrigin computeMipLevelOffset(MtlOrigin offset, uint32_t level) {
    offset.x = offset.x >> level;
    offset.y = offset.y >> level;
    offset.z = offset.z >> level;
    return offset;
  }

  inline MtlOrigin computeBlockOffset(MtlOrigin offset, MtlExtent3D blockSize) {
    return MtlOrigin {
      offset.x / int32_t(blockSize.width),
      offset.y / int32_t(blockSize.height),
      offset.z / int32_t(blockSize.depth) };
  }

  inline MtlExtent3D computeBlockCount(MtlExtent3D extent, MtlExtent3D blockSize) {
    return MtlExtent3D {
      (extent.width  + blockSize.width  - 1u) / blockSize.width,
      (extent.height + blockSize.height - 1u) / blockSize.height,
      (extent.depth  + blockSize.depth  - 1u) / blockSize.depth };
  }

  inline MtlExtent3D computeBlockExtent(MtlExtent3D blockCount, MtlExtent3D blockSize) {
    return MtlExtent3D {
      blockCount.width  * blockSize.width,
      blockCount.height * blockSize.height,
      blockCount.depth  * blockSize.depth };
  }

  inline MtlExtent3D snapExtent3D(MtlOrigin offset, MtlExtent3D extent, MtlExtent3D imageExtent) {
    return MtlExtent3D {
      std::min(extent.width,  imageExtent.width  - uint32_t(offset.x)),
      std::min(extent.height, imageExtent.height - uint32_t(offset.y)),
      std::min(extent.depth,  imageExtent.depth  - uint32_t(offset.z)) };
  }

  inline MtlExtent3D formatBlockSize(const MtlFormatInfo* info) {
    return MtlExtent3D { info ? info->blockWidth : 1u,
                         info ? info->blockHeight : 1u, 1u };
  }

  inline void packImageData(
          void*         dstBytes,
    const void*         srcBytes,
          MtlExtent3D   blockCount,
          MtlDeviceSize blockSize,
          MtlDeviceSize pitchPerRow,
          MtlDeviceSize pitchPerLayer) {
    if (!dstBytes || !srcBytes)
      return;
    auto dst = reinterpret_cast<char*>(dstBytes);
    auto src = reinterpret_cast<const char*>(srcBytes);
    const MtlDeviceSize bytesPerRow   = MtlDeviceSize(blockCount.width) * blockSize;
    for (uint32_t z = 0; z < blockCount.depth; z++) {
      for (uint32_t y = 0; y < blockCount.height; y++) {
        std::memcpy(dst, src + z * pitchPerLayer + y * pitchPerRow, size_t(bytesPerRow));
        dst += bytesPerRow;
      }
    }
  }

  // One block of a solid colour for ColorFill on a block-compressed format,
  // from the D3DCOLOR's 8-bit channels (A8R8G8B8). Returns the block size in
  // bytes (8 or 16), or 0 for a format it doesn't encode (BC4/BC5: ATI1/ATI2,
  // whose channel mapping leaves the fill colour undefined).
  //   BC1 (DXT1): color0 = color1 = RGB565, which selects the 3-colour mode,
  //     so index 0 is that colour, opaque; alpha below 128 gives index 3,
  //     transparent black, everywhere (DXT1's 1-bit alpha).
  //   BC2 (DXT2/3): 4-bit explicit alpha per texel, then the colour block.
  //   BC3 (DXT4/5): alpha0 = alpha1 = alpha with index 0, then the colour block.
  // Channels are quantised by rounding (x * max + 127) / 255; a decoder
  // expands 5/6/4 bits back by bit replication, so every texel reads back as
  // the nearest representable colour.
  inline uint32_t encodeSolidBlock(MtlFormat format, uint32_t argb, uint8_t (&block)[16]) {
    const uint32_t a = (argb >> 24) & 0xffu, r = (argb >> 16) & 0xffu;
    const uint32_t g = (argb >>  8) & 0xffu, b =  argb        & 0xffu;
    const uint16_t c565 = uint16_t((((r * 31u + 127u) / 255u) << 11)
                                 | (((g * 63u + 127u) / 255u) << 5)
                                 |  ((b * 31u + 127u) / 255u));
    auto colorBlock = [&] (uint8_t* p, uint16_t c, uint32_t indices) {
      p[0] = uint8_t(c); p[1] = uint8_t(c >> 8);   // color0
      p[2] = uint8_t(c); p[3] = uint8_t(c >> 8);   // color1
      for (uint32_t i = 0u; i < 4u; i++)
        p[4u + i] = uint8_t(indices >> (8u * i));
    };
    std::memset(block, 0, sizeof(block));
    switch (format) {
      case MtlFormat::BC1_RGBA:
      case MtlFormat::BC1_RGBA_sRGB:
        if (a < 128u)
          colorBlock(block, 0u, 0xffffffffu);
        else
          colorBlock(block, c565, 0u);
        return 8u;
      case MtlFormat::BC2_RGBA:
      case MtlFormat::BC2_RGBA_sRGB: {
        const uint32_t a4 = (a * 15u + 127u) / 255u;
        std::memset(block, int(a4 | (a4 << 4u)), 8u);
        colorBlock(block + 8u, c565, 0u);
        return 16u;
      }
      case MtlFormat::BC3_RGBA:
      case MtlFormat::BC3_RGBA_sRGB:
        block[0] = uint8_t(a);
        block[1] = uint8_t(a);
        colorBlock(block + 8u, c565, 0u);
        return 16u;
      default:
        return 0u;
    }
  }

}
