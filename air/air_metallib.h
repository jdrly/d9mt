#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace d9mt::air {

enum class MetallibFunctionType : uint8_t {
  Vertex = 0,
  Fragment = 1,
  Kernel = 2,
  Mesh = 7,
  Object = 8,
};

enum class MetallibAttributeType : uint8_t {
  Float = 0x03,
  Float2 = 0x04,
  Float3 = 0x05,
  Float4 = 0x06,
  Int = 0x21,
  Int2 = 0x22,
  Int3 = 0x23,
  Int4 = 0x24,
  Uint = 0x25,
  Uint2 = 0x26,
  Uint3 = 0x27,
  Uint4 = 0x28,
};

struct MetallibVertexAttribute {
  std::string name;
  uint8_t location = 0u;
  MetallibAttributeType type = MetallibAttributeType::Float4;
};

enum class MetallibConstantType : uint8_t {
  Bool = 0x35,
  Int = 0x1d,
  Uint = 0x21,
  Float = 0x03,
};

struct MetallibFunctionConstant {
  std::string name;
  MetallibConstantType type = MetallibConstantType::Uint;
  uint16_t index = 0u;
};

struct MetallibFunction {
  std::string name;
  MetallibFunctionType type = MetallibFunctionType::Vertex;
  std::vector<MetallibVertexAttribute> vertexAttributes;
  std::vector<MetallibFunctionConstant> functionConstants;
};

/* The AIR target: every version the bitcode and the metallib container
   carry. For macOS 26 and 27 it is what Apple's toolchain writes for that
   macOS (xcrun metal -mmacosx-version-min=<os>.0, then metallib):
     26: air64_v28-apple-macosx26.0.0, AIR 2.8, Metal 4.0, container 2.9,
         header OS 26.0, VERS 2.8/4.0
     27: air64_v29-apple-macosx27.0.0, AIR 2.9, Metal 4.1, container 2.9,
         header OS 27.0, VERS 2.9/4.1
   Legacy (D9MT_AIR_LEGACY=1, the support switch) is what d9mt wrote before
   (0.1.7): air64-apple-macosx15.0.0, AIR 2.7, Metal 3.0, container 2.7,
   header OS 15.0, and VERS 2.7/3.0 on 26, 2.9/3.0 on 27 (#110). */
struct AirTarget {
  uint16_t airMajor = 2u;          /* air.version, the triple's _v<maj><min> */
  uint16_t airMinor = 7u;
  uint16_t languageMajor = 3u;     /* air.language_version (Metal) */
  uint16_t languageMinor = 0u;
  uint16_t osMajor = 15u;          /* the triple's OS and the header's */
  uint16_t osMinor = 0u;
  uint16_t containerMajor = 2u;    /* the MTLB header's file version */
  uint16_t containerMinor = 7u;
  uint16_t versAirMajor = 2u;      /* every function's VERS tag */
  uint16_t versAirMinor = 7u;
  uint16_t versLanguageMajor = 3u;
  uint16_t versLanguageMinor = 0u;
  bool     versionedTriple = false; /* air64_v<maj><min>- or plain air64- */
  bool     legacy = false;
  unsigned macOS = 26u;            /* the mode: 26 or 27 */
};

/* The target for a mode: macOS 26 (also below 26 and unreadable: the
   supported minimum, and 2.8 loads on 26 and 27) or 27 (27 and later: the
   newest forms we know; an assumption for 28+, which test-air-forms.sh
   checks against that OS's toolchain). */
AirTarget airTargetFor(unsigned macOSMajor, bool legacy);

/* This process's target, decided once from D9MT_AIR_MACOS=<version> (tests)
   or kern.osproductversion, and D9MT_AIR_LEGACY=1, and logged once:
   "d9mt: AIR target macOS 26: AIR 2.8 / Metal 4.0". */
const AirTarget& airTarget();

/* The bitcode's target triple: "air64_v28-apple-macosx26.0.0". */
std::string airTargetTriple(const AirTarget& target);

/* The target's AIR cache term T (bridge/unix/air_cache.c): (legacy << 16) |
   (bitcode AIR major << 12) | (minor << 8) | (VERS AIR major << 4) | minor.
   26 = 0x2828, 27 = 0x2929, legacy 26 = 0x12727, legacy 27 = 0x12729. */
uint32_t airTargetCode(const AirTarget& target);

std::vector<uint8_t> writeMetallib(
        const void*                           bitcode,
        size_t                                bitcodeSize,
        const std::vector<MetallibFunction>&  functions,
        const AirTarget&                      target);

}
