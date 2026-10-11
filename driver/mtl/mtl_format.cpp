#include "mtl_format.h"

namespace dxvk {

  const MtlFormatInfo* lookupFormatInfo(MtlFormat format) {
    static const MtlFormatInfo invalid = { 0u, 1u, 1u, MtlFormatFlagNone, MtlAspectColor };
    static const MtlFormatInfo b1  = { 1u, 1u, 1u, MtlFormatFlagNone, MtlAspectColor };
    static const MtlFormatInfo b2  = { 2u, 1u, 1u, MtlFormatFlagNone, MtlAspectColor };
    static const MtlFormatInfo b4  = { 4u, 1u, 1u, MtlFormatFlagNone, MtlAspectColor };
    static const MtlFormatInfo b8  = { 8u, 1u, 1u, MtlFormatFlagNone, MtlAspectColor };
    static const MtlFormatInfo b12 = { 12u, 1u, 1u, MtlFormatFlagNone, MtlAspectColor };
    static const MtlFormatInfo b16 = { 16u, 1u, 1u, MtlFormatFlagNone, MtlAspectColor };
    static const MtlFormatInfo bc1 = { 8u, 4u, 4u, MtlFormatFlagCompressed, MtlAspectColor };
    static const MtlFormatInfo bc23 = { 16u, 4u, 4u, MtlFormatFlagCompressed, MtlAspectColor };
    static const MtlFormatInfo d32s8 = { 8u, 1u, 1u,
      MtlFormatFlagDepth | MtlFormatFlagStencil, MtlAspectMask(MtlAspectDepth | MtlAspectStencil) };
    static const MtlFormatInfo d32 = { 4u, 1u, 1u, MtlFormatFlagDepth, MtlAspectDepth };
    static const MtlFormatInfo d16 = { 2u, 1u, 1u, MtlFormatFlagDepth, MtlAspectDepth };

    // Case values are raw MTLPixelFormat enum values, not arbitrary IDs.
    switch (uint32_t(format)) {
      case 0u:  return &invalid;
      case 1u: case 10u: case 11u: case 12u: case 13u: case 14u:
        return &b1;
      case 20u: case 22u: case 23u: case 24u: case 25u: case 30u: case 31u:
      case 32u: case 33u: case 40u: case 41u: case 42u: case 43u:
        return &b2;
      case 60u: case 62u: case 63u: case 64u: case 65u:
      case 70u: case 71u: case 72u: case 73u: case 74u: case 75u:
      case 80u: case 81u: case 85u: case 90u: case 91u: case 92u: case 93u:
        return &b4;
      case 100u: case 103u: case 104u: case 105u:
      case 110u: case 112u: case 113u: case 114u: case 115u:
        return &b8;
      case 123u: case 124u: case 125u: case 126u:
        return &b16;
      case 130u: case 131u: case 140u:
        return &bc1;
      case 132u: case 133u: case 134u: case 135u: case 142u:
        return &bc23;
      // Vertex-attribute-only markers (MtlFormat::RGB32Float / RGB10A2Snorm_Vtx);
      // their size feeds the stride fallback in the vertex layout.
      case 900u: return &b12;
      case 901u: return &b4;
      case 250u: return &d16;
      case 252u: return &d32;
      case 260u: case 261u: return &d32s8;
      default:  return &b4;
    }
  }

}
