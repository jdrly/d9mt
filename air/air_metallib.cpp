#include "air_metallib.h"
#include "air_sha256.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <sys/sysctl.h>

namespace d9mt::air {

namespace {

struct ByteStream {
  std::vector<uint8_t> bytes;

  size_t tell() const { return bytes.size(); }

  void raw(const void* data, size_t size) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    bytes.insert(bytes.end(), p, p + size);
  }

  void u8(uint8_t v)   { bytes.push_back(v); }
  void u16(uint16_t v) { raw(&v, 2u); }
  void u32(uint32_t v) { raw(&v, 4u); }
  void u64(uint64_t v) { raw(&v, 8u); }

  void fourcc(const char tag[5]) { raw(tag, 4u); }

  void cstr(const std::string& s) { raw(s.c_str(), s.size() + 1u); }

  void patchU16(size_t offset, uint16_t v) {
    bytes[offset] = uint8_t(v);
    bytes[offset + 1u] = uint8_t(v >> 8);
  }
};

constexpr uint16_t kPlatformMacOS = 0x8001u;
constexpr uint8_t  kFileTypeExecutable = 0x00u;
constexpr uint8_t  kOsMacOS = 0x81u;

void writeFunctionTags(
        ByteStream&             s,
        const MetallibFunction& fn,
        const Sha256Digest&     bitcodeHash,
        uint64_t                bitcodeSize,
        uint64_t                publicMdOffset,
        uint64_t                privateMdOffset,
        const AirTarget&        t) {
  s.fourcc("NAME");
  s.u16(uint16_t(fn.name.size() + 1u));
  s.cstr(fn.name);

  s.fourcc("TYPE");
  s.u16(1u);
  s.u8(uint8_t(fn.type));

  s.fourcc("HASH");
  s.u16(uint16_t(bitcodeHash.size()));
  s.raw(bitcodeHash.data(), bitcodeHash.size());

  s.fourcc("MDSZ");
  s.u16(8u);
  s.u64(bitcodeSize);

  s.fourcc("OFFT");
  s.u16(24u);
  s.u64(publicMdOffset);
  s.u64(privateMdOffset);
  s.u64(0u);

  s.fourcc("VERS");
  s.u16(8u);
  s.u16(t.versAirMajor);
  s.u16(t.versAirMinor);
  s.u16(t.versLanguageMajor);
  s.u16(t.versLanguageMinor);

  s.fourcc("ENDT");
}

void writeVertexAttributeRecords(
        ByteStream&                                  s,
        const std::vector<MetallibVertexAttribute>&  attrs) {
  s.fourcc("VATT");
  size_t sizeFixup = s.tell();
  s.u16(0u);
  s.u16(uint16_t(attrs.size()));

  for (const auto& a : attrs) {
    s.cstr(a.name);
    s.u16(uint16_t(a.location) | uint16_t(1u << 15));
  }

  s.patchU16(sizeFixup, uint16_t(s.tell() - sizeFixup - 2u));

  s.fourcc("VATY");
  s.u16(uint16_t(2u + attrs.size()));
  s.u16(uint16_t(attrs.size()));

  for (const auto& a : attrs)
    s.u8(uint8_t(a.type));
}

void writeFunctionConstantRecord(
        ByteStream&                                  s,
        const std::vector<MetallibFunctionConstant>& constants) {
  s.fourcc("CNST");
  size_t sizeFixup = s.tell();
  s.u16(0u);
  s.u16(uint16_t(constants.size()));

  for (const auto& c : constants) {
    s.cstr(c.name);
    s.u8(uint8_t(c.type));
    s.u16(c.index);
    s.u8(1u);
  }

  s.patchU16(sizeFixup, uint16_t(s.tell() - sizeFixup - 2u));
}

/* "<major>[.<n>...]" with a major of 1 to 999; anything else is 0 (target
   macOS 26). */
unsigned parseMacOSMajor(const char* s) {
  unsigned major = 0u, digits = 0u;
  for (; *s >= '0' && *s <= '9'; s++) {
    if (++digits > 3u)
      return 0u;
    major = major * 10u + unsigned(*s - '0');
  }
  if (!digits)
    return 0u;
  while (*s == '.') {
    if (*++s < '0' || *s > '9')
      return 0u;
    while (*s >= '0' && *s <= '9')
      s++;
  }
  return *s ? 0u : major;
}

unsigned macOSMajor() {
  const char* env = std::getenv("D9MT_AIR_MACOS");
  if (env && *env)
    return parseMacOSMajor(env);
  char version[32] = {};
  size_t size = sizeof(version) - 1u;
  if (sysctlbyname("kern.osproductversion", version, &size, nullptr, 0) != 0)
    return 0u;
  return parseMacOSMajor(version);
}

}

AirTarget airTargetFor(unsigned macOSMajor, bool legacy) {
  AirTarget t;
  t.macOS = macOSMajor >= 27u ? 27u : 26u;
  t.legacy = legacy;
  if (legacy) {
    /* The defaults: 0.1.7's bitcode and container; VERS per #110. */
    if (t.macOS == 27u)
      t.versAirMinor = 9u;
    return t;
  }
  const uint16_t minor = t.macOS == 27u ? 9u : 8u;
  t.airMajor = 2u;
  t.airMinor = minor;
  t.languageMajor = 4u;
  t.languageMinor = t.macOS == 27u ? 1u : 0u;
  t.osMajor = uint16_t(t.macOS);
  t.osMinor = 0u;
  t.containerMajor = 2u;
  t.containerMinor = 9u;
  t.versAirMajor = t.airMajor;
  t.versAirMinor = t.airMinor;
  t.versLanguageMajor = t.languageMajor;
  t.versLanguageMinor = t.languageMinor;
  t.versionedTriple = true;
  return t;
}

const AirTarget& airTarget() {
  static const AirTarget target = [] {
    const char* env = std::getenv("D9MT_AIR_LEGACY");
    AirTarget t = airTargetFor(macOSMajor(), env && !std::strcmp(env, "1"));
    std::fprintf(stderr, "d9mt: AIR target macOS %u%s: AIR %u.%u / Metal %u.%u",
      t.macOS, t.legacy ? " (legacy, D9MT_AIR_LEGACY=1)" : "",
      t.airMajor, t.airMinor, t.languageMajor, t.languageMinor);
    if (t.versAirMajor != t.airMajor || t.versAirMinor != t.airMinor
     || t.versLanguageMajor != t.languageMajor || t.versLanguageMinor != t.languageMinor)
      std::fprintf(stderr, ", VERS %u.%u / %u.%u", t.versAirMajor, t.versAirMinor,
        t.versLanguageMajor, t.versLanguageMinor);
    std::fprintf(stderr, "\n");
    return t;
  }();
  return target;
}

std::string airTargetTriple(const AirTarget& target) {
  std::string arch = "air64";
  if (target.versionedTriple)
    arch += "_v" + std::to_string(target.airMajor) + std::to_string(target.airMinor);
  return arch + "-apple-macosx" + std::to_string(target.osMajor) + "."
    + std::to_string(target.osMinor) + ".0";
}

uint32_t airTargetCode(const AirTarget& target) {
  return (uint32_t(target.legacy) << 16)
       | (uint32_t(target.airMajor & 0xfu) << 12) | (uint32_t(target.airMinor & 0xfu) << 8)
       | (uint32_t(target.versAirMajor & 0xfu) << 4) | uint32_t(target.versAirMinor & 0xfu);
}

std::vector<uint8_t> writeMetallib(
        const void*                           bitcode,
        size_t                                bitcodeSize,
        const std::vector<MetallibFunction>&  functions,
        const AirTarget&                      target) {
  Sha256Digest hash = sha256(bitcode, bitcodeSize);

  ByteStream publicMd;
  ByteStream privateMd;
  ByteStream fnList;

  for (const auto& fn : functions) {
    ByteStream tags;
    writeFunctionTags(tags, fn, hash, bitcodeSize,
      publicMd.tell(), privateMd.tell(), target);
    fnList.u32(uint32_t(tags.tell() + 4u));
    fnList.raw(tags.bytes.data(), tags.bytes.size());

    ByteStream fnPublic;
    if (fn.type == MetallibFunctionType::Vertex && !fn.vertexAttributes.empty())
      writeVertexAttributeRecords(fnPublic, fn.vertexAttributes);
    if (!fn.functionConstants.empty())
      writeFunctionConstantRecord(fnPublic, fn.functionConstants);
    fnPublic.fourcc("ENDT");

    publicMd.u32(uint32_t(fnPublic.tell()));
    publicMd.raw(fnPublic.bytes.data(), fnPublic.bytes.size());

    privateMd.u32(4u);
    privateMd.fourcc("ENDT");
  }

  constexpr uint64_t headerSize = 88u;
  uint64_t functionListSize = fnList.tell();
  uint64_t publicMdOffset = headerSize + 4u + fnList.tell() + 4u;
  uint64_t privateMdOffset = publicMdOffset + publicMd.tell();
  uint64_t bitcodeOffset = privateMdOffset + privateMd.tell();
  uint64_t fileSize = bitcodeOffset + bitcodeSize;

  ByteStream out;
  out.bytes.reserve(size_t(fileSize));

  out.fourcc("MTLB");
  out.u16(kPlatformMacOS);
  out.u16(target.containerMajor);
  out.u16(target.containerMinor);
  out.u8(kFileTypeExecutable);
  out.u8(kOsMacOS);
  out.u16(target.osMajor);
  out.u16(target.osMinor);
  out.u64(fileSize);
  out.u64(headerSize);
  out.u64(functionListSize);
  out.u64(publicMdOffset);
  out.u64(publicMd.tell());
  out.u64(privateMdOffset);
  out.u64(privateMd.tell());
  out.u64(bitcodeOffset);
  out.u64(bitcodeSize);

  out.u32(uint32_t(functions.size()));
  out.raw(fnList.bytes.data(), fnList.bytes.size());
  out.fourcc("ENDT");
  out.raw(publicMd.bytes.data(), publicMd.bytes.size());
  out.raw(privateMd.bytes.data(), privateMd.bytes.size());
  out.raw(bitcode, bitcodeSize);

  return out.bytes;
}

}
