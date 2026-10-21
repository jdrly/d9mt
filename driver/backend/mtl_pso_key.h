#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <type_traits>

#include "../mtl/mtl_types.h"

namespace dxvk::backend {

  namespace psokey {

    // xxHash64's round, lane merge and avalanche (seed 0) over whole 8-byte words: the PSO
    // key's hash (docs/pso-key-hash.md). Four independent lanes, so a 320-byte key is ~10
    // multiplies deep instead of the 320 dependent steps of a byte-wise FNV. Little-endian
    // memcpy loads (no alignment assumption); only has to be consistent within a process.
    constexpr uint64_t P1 = 0x9e3779b185ebca87ull;
    constexpr uint64_t P2 = 0xc2b2ae3d27d4eb4full;
    constexpr uint64_t P3 = 0x165667b19e3779f9ull;
    constexpr uint64_t P4 = 0x85ebca77c2b2ae63ull;
    constexpr uint64_t P5 = 0x27d4eb2f165667c5ull;

    inline uint64_t rotl(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
    inline uint64_t mixRound(uint64_t acc, uint64_t in) { return rotl(acc + in * P2, 31) * P1; }
    inline uint64_t mergeLane(uint64_t h, uint64_t v) { return (h ^ mixRound(0u, v)) * P1 + P4; }

    inline uint64_t word(const uint8_t* p) {
      uint64_t w;
      std::memcpy(&w, p, sizeof(w));
      return w;
    }

    /// size: a multiple of 8.
    inline uint64_t hashWords(const uint8_t* p, size_t size) {
      uint64_t h = P5;
      size_t i = 0u;
      if (size >= 32u) {
        uint64_t v1 = P1 + P2, v2 = P2, v3 = 0u, v4 = 0u - P1;
        for (; i + 32u <= size; i += 32u) {
          v1 = mixRound(v1, word(p + i));
          v2 = mixRound(v2, word(p + i + 8u));
          v3 = mixRound(v3, word(p + i + 16u));
          v4 = mixRound(v4, word(p + i + 24u));
        }
        h = rotl(v1, 1) + rotl(v2, 7) + rotl(v3, 12) + rotl(v4, 18);
        h = mergeLane(mergeLane(mergeLane(mergeLane(h, v1), v2), v3), v4);
      }
      h += uint64_t(size);
      for (; i + 8u <= size; i += 8u)
        h = rotl(h ^ mixRound(0u, word(p + i)), 27) * P1 + P4;
      h ^= h >> 33;
      h *= P2;
      h ^= h >> 29;
      h *= P3;
      h ^= h >> 32;
      return h;
    }

  }

  struct PsoVertexAttr {
    uint32_t format = 0u;
    uint16_t offset = 0u;
    uint8_t  bufferIndex = 0u;
    uint8_t  pad = 0u;
  };

  struct PsoVertexLayout {
    uint16_t stride = 0u;
    uint8_t  stepFunction = 0u;
    uint8_t  pad = 0u;   // the Metal buffer index (kVertexBufferBaseIndex + stream)
  };

  struct PsoColorAttachment {
    uint32_t pixelFormat = 0u;
    uint8_t  blendEnabled = 0u;
    uint8_t  writeMask = 0xfu;
    uint8_t  rgbSrcFactor = 1u;
    uint8_t  rgbDstFactor = 0u;
    uint8_t  rgbBlendOp = 0u;
    uint8_t  alphaSrcFactor = 1u;
    uint8_t  alphaDstFactor = 0u;
    uint8_t  alphaBlendOp = 0u;
  };

  // Packed POD, hashed bytewise — must be zero-initialized before filling so
  // padding bytes don't leak into the hash.
  struct RenderPsoKey {
    uint64_t vsCookie = 0u;
    uint64_t fsCookie = 0u;

    // 20 slots: the fixed-function ubershader declares all 18 FF vertex
    // registers, and Metal requires every declared attribute to be present
    // in the vertex descriptor (unlike Vulkan, which tolerates gaps).
    PsoVertexAttr   attrs[20] = { };
    PsoVertexLayout layouts[20] = { };
    uint8_t attrCount = 0u;
    uint8_t layoutCount = 0u;
    // Explicit, so the key has no padding bytes: it is hashed and compared as raw bytes,
    // and "= {}" with default member initializers doesn't zero implicit padding.
    uint8_t padding[2] = { };

    PsoColorAttachment colors[4] = { };
    uint8_t colorCount = 0u;

    uint8_t sampleCount = 1u;
    // Always 0: build() gives Metal an unspecified topology class (SM3 vertex shaders declare
    // a point size, which an explicit class rejects), so the draw's topology must not split
    // the key: a triangle, line and point draw of the same state is one pipeline.
    uint8_t topologyClass = 0u;
    uint8_t alphaToCoverage = 0u;

    // Always 0: Metal 4 pipeline descriptors have no depth/stencil formats (build() sets
    // none), so they would only split keys of the same pipeline.
    uint32_t depthFormat = 0u;
    uint32_t stencilFormat = 0u;

    // The containers' hash (RenderPsoCache::KeyHash; in memory only, not in any file or
    // signature): whole 8-byte words, docs/pso-key-hash.md.
    size_t hash() const {
      const uint64_t h = psokey::hashWords(reinterpret_cast<const uint8_t*>(this), sizeof(*this));
      return sizeof(size_t) == 8u ? size_t(h) : size_t(h ^ (h >> 32));
    }

    bool operator == (const RenderPsoKey& other) const {
      return !std::memcmp(this, &other, sizeof(*this));
    }

    /// The state cache records the key without its two cookies.
    static constexpr size_t StateOffset = 16u;

    const uint8_t* stateBytes() const {
      return reinterpret_cast<const uint8_t*>(this) + StateOffset;
    }

    /// Layout and mapping signature for the state cache file: changes
    /// whenever a field (or a sub-field) moves or kPsoKeyMappingVersion is
    /// bumped. Defined in mtl_pso_cache.cpp.
    static uint64_t signature();
  };

  static_assert(std::has_unique_object_representations_v<RenderPsoKey>,
    "RenderPsoKey is hashed and compared as raw bytes: no padding");
  static_assert(sizeof(RenderPsoKey) % 8u == 0u,
    "RenderPsoKey::hash() reads whole 8-byte words: a key that grows by a non-multiple of 8 needs a tail");

  /// Bump in the same commit as any change to what the key's derived bytes
  /// mean, so stale state cache entries are dropped instead of prewarmed:
  ///  - attributeFormat() (MtlFormat -> Metal vertex format) and
  ///    DecodeDecltype() (D3DDECLTYPE -> MtlFormat);
  ///  - the attribute order: one attribute per VS input register, in the
  ///    order BindInputLayout() builds them from the shader's inputs;
  ///  - the D3DFORMAT -> MtlFormat table (render target pixel formats);
  ///  - the blend factor/op and write mask mapping;
  ///  - kVertexBufferBaseIndex, NullStreamIdx (the null stream's binding),
  ///    and the layout's `pad` byte carrying the Metal buffer index.
  ///
  /// 3: #8 (6d625fa) deduplicates repeated semantics in a VS's input
  ///    signature, which changes BindInputLayout()'s attribute list for
  ///    vertex shaders with packed input declarations.
  /// 4: topologyClass and the depth/stencil formats are always 0 and the padding is explicit:
  ///    one pipeline was up to three keys (triangles, lines, points) and three entries.
  constexpr uint32_t kPsoKeyMappingVersion = 4u;

  /// Metal buffer indices of vertex streams: kVertexBufferBaseIndex (3) +
  /// stream 0..15, and the null stream (NullStreamIdx = 16) at 19.
  constexpr uint32_t kPsoFirstVertexBuffer = 3u;
  constexpr uint32_t kPsoLastVertexBuffer  = kPsoFirstVertexBuffer + 16u;

  /// WMTAttributeFormat values attributeFormat() produces (static_assert'ed
  /// against winemetal.h in mtl_pso_cache.cpp).
  inline bool psoAttributeFormatProduced(uint32_t f) {
    switch (f) {
      case 3u:  /* UChar4 */                 case 9u:  /* UChar4Normalized */
      case 12u: /* Char4Normalized */        case 16u: /* Short2 */
      case 18u: /* Short4 */                 case 19u: /* UShort2Normalized */
      case 21u: /* UShort4Normalized */      case 22u: /* Short2Normalized */
      case 24u: /* Short4Normalized */       case 25u: /* Half2 */
      case 27u: /* Half4 */                  case 28u: /* Float */
      case 29u: /* Float2 */                 case 30u: /* Float3 */
      case 31u: /* Float4 */                 case 40u: /* Int1010102Normalized */
      case 41u: /* UInt1010102Normalized */  case 42u: /* UChar4Normalized_BGRA */
      case 53u: /* Half */
        return true;
      default:
        return false;
    }
  }

  /// Color render target formats the D3D9 format table can produce (color
  /// and sRGB views); 0 = no target in that slot.
  inline bool psoColorFormatProduced(uint32_t f) {
    switch (MtlFormat(f)) {
      case MtlFormat::Invalid:
      case MtlFormat::R8Unorm:      case MtlFormat::R8Uint:
      case MtlFormat::R16Unorm:     case MtlFormat::R16Uint:      case MtlFormat::R16Float:
      case MtlFormat::RG8Unorm:     case MtlFormat::RG8Snorm:
      case MtlFormat::B5G6R5Unorm:  case MtlFormat::ABGR4Unorm:   case MtlFormat::BGR5A1Unorm:
      case MtlFormat::R32Uint:      case MtlFormat::R32Float:
      case MtlFormat::RG16Unorm:    case MtlFormat::RG16Snorm:    case MtlFormat::RG16Float:
      case MtlFormat::RGBA8Unorm:   case MtlFormat::RGBA8Unorm_sRGB: case MtlFormat::RGBA8Snorm:
      case MtlFormat::BGRA8Unorm:   case MtlFormat::BGRA8Unorm_sRGB:
      case MtlFormat::RGB10A2Unorm: case MtlFormat::BGR10A2Unorm: case MtlFormat::RG11B10Float:
      case MtlFormat::RG32Float:
      case MtlFormat::RGBA16Unorm:  case MtlFormat::RGBA16Snorm:  case MtlFormat::RGBA16Float:
      case MtlFormat::RGBA32Float:
        return true;
      default:
        return false;
    }
  }

  /// Blending is allowed on every produced format but the integer ones:
  /// Metal terminates the process for blending on R8/R16/R32Uint, and
  /// accepts it on the 32-bit float formats (measured at 1/4/8 samples), on
  /// which the draw path does blend.
  inline bool psoColorFormatBlendable(uint32_t f) {
    switch (MtlFormat(f)) {
      case MtlFormat::Invalid:
      case MtlFormat::R8Uint: case MtlFormat::R16Uint: case MtlFormat::R32Uint:
        return false;
      default:
        return psoColorFormatProduced(f);
    }
  }

  /**
   * \brief Checks recorded key bytes against what resolvePso() produces
   *
   * The state cache replays keys from a file, so a damaged or forged entry
   * (valid checksums, nonsense content) must never reach the compiler:
   * Metal terminates the process on invalid descriptors. Every check is an
   * invariant of the draw path: counts within the arrays, vertex formats
   * from attributeFormat(), buffer indices 3..19, every attribute's buffer
   * with a layout of nonzero stride, render target formats from the D3D9
   * format table, blending only on blendable formats, blend factors/ops and
   * write masks in range, 1/2/4/8 samples, no depth/stencil format (internal
   * pipelines only). why: a static string naming the first failed check.
   */
  inline bool psoStateValid(const uint8_t* pState, const char** pWhy = nullptr) {
    RenderPsoKey k;
    std::memcpy(reinterpret_cast<uint8_t*>(&k) + RenderPsoKey::StateOffset, pState,
      sizeof(RenderPsoKey) - RenderPsoKey::StateOffset);

    auto fail = [pWhy] (const char* why) {
      if (pWhy)
        *pWhy = why;
      return false;
    };

    if (k.attrCount > std::size(k.attrs) || k.layoutCount > std::size(k.layouts)
     || k.colorCount > std::size(k.colors))
      return fail("count");

    for (uint32_t i = 0u; i < k.layoutCount; i++) {
      const PsoVertexLayout& l = k.layouts[i];
      if (l.pad < kPsoFirstVertexBuffer || l.pad > kPsoLastVertexBuffer)
        return fail("layout buffer index");
      if (!l.stride)
        return fail("layout stride");
      if (l.stepFunction != 1u && l.stepFunction != 2u)
        return fail("layout step function");
      for (uint32_t j = 0u; j < i; j++) {
        if (k.layouts[j].pad == l.pad)
          return fail("layout buffer index twice");
      }
    }

    for (uint32_t i = 0u; i < k.attrCount; i++) {
      const PsoVertexAttr& a = k.attrs[i];
      if (!psoAttributeFormatProduced(a.format))
        return fail("vertex format");
      bool layout = false;
      for (uint32_t j = 0u; j < k.layoutCount && !layout; j++)
        layout = k.layouts[j].pad == a.bufferIndex;
      if (!layout)
        return fail("attribute buffer without layout");
    }

    for (uint32_t i = 0u; i < k.colorCount; i++) {
      const PsoColorAttachment& c = k.colors[i];
      if (!psoColorFormatProduced(c.pixelFormat))
        return fail("pixel format");
      if (c.blendEnabled > 1u || c.writeMask > 0xfu)
        return fail("blend enable or write mask");
      if (c.blendEnabled && !psoColorFormatBlendable(c.pixelFormat))
        return fail("blending on a non-blendable format");
      if (c.rgbSrcFactor > 18u || c.rgbDstFactor > 18u || c.alphaSrcFactor > 18u
       || c.alphaDstFactor > 18u || c.rgbBlendOp > 4u || c.alphaBlendOp > 4u)
        return fail("blend factor or operation");
    }

    if (k.sampleCount != 1u && k.sampleCount != 2u && k.sampleCount != 4u && k.sampleCount != 8u)
      return fail("sample count");
    if (k.topologyClass != 0u || k.alphaToCoverage > 1u)
      return fail("topology or alpha to coverage");
    if (k.padding[0] || k.padding[1])
      return fail("padding");
    if (k.depthFormat || k.stencilFormat)
      return fail("depth/stencil format");

    return true;
  }

}
