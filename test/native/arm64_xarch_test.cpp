// Cross-architecture test for the driver's CPU-side routines whose code path
// depends on the architecture (DXVK_ARCH_X86 SSE/asm paths vs the portable
// code ARM64EC compiles) or whose result C++ leaves to the hardware: bit
// scans, interleaving, the x86-exact float->u32 conversion, the point-size
// encoding, the FNV/SHA-1 hashes behind shader and pipeline keys, the
// constant-buffer copy (NaN flush) and UpdateStateConstants (dirty test).
//
// Every section checks itself against an architecture-independent reference
// and prints a digest of all its results. scripts/test-arm64-port.sh builds it
// for x86_64 (run under Rosetta; the x86 code paths) and arm64 (the ARM64EC
// code paths: DXVK_ARCH_ARM64, no DXVK_ARCH_X86) and requires identical
// output. Built with the driver's float flags (-ffp-contract=off on arm64).
//
//   clang++ -std=c++17 -O2 -arch <arch> [-ffp-contract=off] -I<DXVK>/include/native/{windows,directx}
//     -Idriver/d3d9 -Ithird_party/dxbc-spirv test/native/arm64_xarch_test.cpp
//     driver/d3d9/d3d9_constant_copy.cpp driver/util/sha1/sha1_util.cpp driver/util/sha1/sha1.c

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "d3d9_constant_copy.h"
#include "d3d9_shader_analysis.h"
#include "../util/util_bit.h"
#include "../util/dxvk_hash.h"
#include "../util/sha1/sha1_util.h"

using namespace dxvk;

namespace {

int g_fail = 0;
#define CHECK(c) do { if (!(c)) { if (g_fail++ < 20) std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

// Order-sensitive digest of a stream of results (integer ops only).
struct Digest {
  uint64_t h = 0x243f6a8885a308d3ull;
  void add(uint64_t v) {
    h = (h ^ v) * 0x9e3779b97f4a7c15ull;
    h ^= h >> 29;
  }
  void bytes(const void* p, size_t n) {
    const unsigned char* b = static_cast<const unsigned char*>(p);
    for (size_t i = 0; i < n; i++)
      add(b[i]);
  }
};

void report(const char* name, const Digest& d) {
  std::printf("%-22s %016llx\n", name, (unsigned long long)d.h);
}

struct XorShift {
  uint64_t s = 0x9e3779b97f4a7c15ull;
  uint64_t next() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
  }
};

// Architecture-independent references (plain integer arithmetic).
uint32_t refPopcnt32(uint32_t v) { return bit::popcnt(v); }
uint32_t refTz32(uint32_t v) { return v ? refPopcnt32((v & (0u - v)) - 1u) : 32u; }
uint32_t refLz32(uint32_t v) {
  v |= v >> 1; v |= v >> 2; v |= v >> 4; v |= v >> 8; v |= v >> 16;
  return 32u - refPopcnt32(v);
}
uint32_t refTz64(uint64_t v) { return v ? uint32_t(bit::popcnt((v & (0ull - v)) - 1ull)) : 64u; }
uint32_t refLz64(uint64_t v) {
  v |= v >> 1; v |= v >> 2; v |= v >> 4; v |= v >> 8; v |= v >> 16; v |= v >> 32;
  return 64u - uint32_t(bit::popcnt(v));
}
uint32_t refInterleave(uint16_t x, uint16_t y) {
  uint32_t r = 0;
  for (uint32_t i = 0; i < 16; i++)
    r |= (uint32_t((x >> i) & 1u) << (2 * i)) | (uint32_t((y >> i) & 1u) << (2 * i + 1));
  return r;
}

void testBits32() {
  Digest d;
  uint64_t bad = 0;
  uint32_t v = 0;
  do {
    uint32_t tz = bit::tzcnt(v), lz = bit::lzcnt(v), pc = bit::popcnt(v);
    uint32_t bs = v ? bit::bsf(v) : 0u;  // bsf(0) is undefined
    bad += (tz != refTz32(v)) + (lz != refLz32(v)) + (v && bs != tz);
    d.add(tz | (lz << 8) | (pc << 16) | (uint64_t(bs) << 24));
  } while (++v != 0u);
  CHECK(bad == 0);
  report("bits32", d);
}

void testBits64() {
  Digest d;
  uint64_t bad = 0;
  auto one = [&](uint64_t v) {
    uint32_t tz = bit::tzcnt(v), lz = bit::lzcnt(v);
    uint32_t pc = uint32_t(bit::popcnt(v));
    uint32_t bs = v ? bit::bsf(v) : 0u;
    bad += (tz != refTz64(v)) + (lz != refLz64(v)) + (v && bs != tz);
    d.add(tz | (lz << 8) | (pc << 16) | (uint64_t(bs) << 24));
  };
  for (uint32_t i = 0; i < 64; i++) {
    one(1ull << i);
    one((1ull << i) - 1ull);
    one(~((1ull << i) - 1ull));
    one(~(1ull << i));
  }
  one(0ull);
  one(~0ull);
  XorShift r;
  for (uint32_t i = 0; i < (1u << 26); i++) {
    uint64_t v = r.next();
    one(v);
    one(v >> (v & 63u));
  }
  CHECK(bad == 0);
  report("bits64", d);
}

void testInterleave() {
  Digest d;
  uint64_t bad = 0;
  uint32_t v = 0;
  do {
    uint16_t x = uint16_t(v), y = uint16_t(v >> 16);
    uint32_t r = bit::interleave(x, y);
    if ((v & 0xfffu) == 0u)
      bad += r != refInterleave(x, y);
    d.add(r);
  } while (++v != 0u);
  XorShift rr;
  for (uint32_t i = 0; i < (1u << 24); i++) {
    uint64_t w = rr.next();
    d.add(bit::interleave(uint16_t(w), uint16_t(w >> 16), uint16_t(w >> 32)));
  }
  CHECK(bad == 0);
  report("interleave", d);
}

// D9MT_U32 over every float bit pattern: the x86 build is the plain
// cast (the reference), the arm64 build the emulation.
void testCvtF32U32() {
  Digest d;
  uint32_t v = 0;
  do {
    float f;
    std::memcpy(&f, &v, 4);
    d.add(D9MT_U32(f));
  } while (++v != 0u);
  report("cvt_f32_u32", d);
}

// d3d9_device.h D3D9DeviceEx::EncodePointSize, verbatim (the class is not
// buildable natively), over every D3DRS_POINTSIZE value.
inline uint16_t EncodePointSize(DWORD Value) {
  return uint16_t(std::clamp(bit::cast<float>(Value) * 8.0f, 0.0f, 65535.0f));
}

void testPointSize() {
  Digest d;
  uint32_t v = 0;
  do {
    d.add(EncodePointSize(v));
  } while (++v != 0u);
  report("point_size", d);
}

void testHashes() {
  Digest d;
  XorShift r;
  std::vector<unsigned char> buf(4096);
  for (auto& b : buf)
    b = uint8_t(r.next());
  for (size_t len = 0; len <= 1024; len++) {
    d.add(bit::fnv1a_hash(buf.data() + (len & 7u), len));
    Sha1Hash sha = Sha1Hash::compute(buf.data() + (len & 15u), len);
    d.bytes(&sha, sizeof(sha));
    DxvkHashState hs;
    for (size_t i = 0; i < len; i += 8)
      hs.add(size_t(buf[i]) | (size_t(buf[i + 1]) << 40));
    d.add(size_t(hs));
  }
  // SHA-1 known answer ("abc")
  Sha1Hash abc = Sha1Hash::compute("abc", 3);
  CHECK(abc.toString() == "a9993e364706816aba3e25717850c26c9cd0d89d");
  report("hashes", d);
}

// Float inputs: all special classes plus random bit patterns.
const uint32_t kSpecial[] = {
  0x00000000u, 0x80000000u, 0x3f800000u, 0xbf800000u, 0x7f800000u, 0xff800000u,
  0x7fc00000u, 0xffc00000u, 0x7fa00000u, 0x7f800001u, 0xff800001u, 0x7fffffffu,
  0xffffffffu, 0x00000001u, 0x80000001u, 0x007fffffu, 0x807fffffu, 0x00800000u,
  0x7f7fffffu, 0xff7fffffu, 0x477fe000u, 0x33800000u, 0x7fa05000u,
};

uint32_t randomFloatBits(XorShift& r) {
  uint64_t k = r.next();
  if ((k & 3u) == 0u)
    return kSpecial[(k >> 8) % (sizeof(kSpecial) / sizeof(kSpecial[0]))];
  return uint32_t(k >> 32);
}

void testConstantCopy() {
  Digest d;
  XorShift r;
  uint64_t bad = 0;
  constexpr uint32_t MaxF = 256u, MaxI = 16u, MaxB = 32u;
  std::vector<Vector4> fApi(MaxF), fOut(MaxF + 8u);
  std::vector<Vector4i> iApi(MaxI), iOut(MaxI + 8u);
  std::vector<uint32_t> bApi(MaxB), bOut(MaxB + 16u);

  for (uint32_t iter = 0; iter < 20000u; iter++) {
    for (auto& v : fApi)
      for (uint32_t j = 0; j < 4; j++) {
        uint32_t bits = randomFloatBits(r);
        std::memcpy(&v.data[j], &bits, 4);
      }
    for (auto& v : iApi)
      for (uint32_t j = 0; j < 4; j++)
        v.data[j] = int32_t(r.next());
    for (auto& b : bApi)
      b = uint32_t(r.next());

    uint32_t fMask[MaxF / 32], iMask[1] = { uint32_t(r.next()) & 0xffffu }, bMask[1] = { uint32_t(r.next()) };
    for (auto& m : fMask)
      m = uint32_t(r.next()) & uint32_t(r.next() >> 7);

    D3D9ConstantBufferLayout fl;
    if (iter & 1u) {
      fl = D3D9ConstantBufferLayout(MaxF / 32u, fMask);
    } else {
      D3D9ImmediateFloatConstant defs[4];
      for (auto& def : defs) {
        def.index = uint32_t(r.next() % MaxF);
        for (uint32_t j = 0; j < 4; j++) {
          uint32_t bits = randomFloatBits(r);
          std::memcpy(&def.value.data[j], &bits, 4);
        }
      }
      fl = D3D9ConstantBufferLayout(MaxF, MaxF / 32u, fMask, 4u, defs);
    }
    D3D9ConstantBufferCopy copy(fl, D3D9ConstantBufferLayout(1u, iMask), D3D9ConstantBufferLayout(1u, bMask));

    D3D9ConstantBufferSizes sizes = copy.getAllocationSizes(uint32_t(r.next() % (MaxF + 1u)));
    std::memset(fOut.data(), 0xcd, fOut.size() * sizeof(Vector4));
    std::memset(iOut.data(), 0xcd, iOut.size() * sizeof(Vector4i));
    std::memset(bOut.data(), 0xcd, bOut.size() * sizeof(uint32_t));

    D3D9ConstantBufferCopyArgs args = {};
    args.floatBuffer = fOut.data();
    args.floatBufferSize = sizes.floatBufferSize;
    args.floatConstantCount = sizes.floatBufferSize / sizeof(Vector4);
    args.flushNan = (iter >> 1) & 1u;
    args.intBuffer = iOut.data();
    args.intBufferSize = sizes.intBufferSize;
    args.boolBuffer = bOut.data();
    args.boolBufferSize = align(sizes.boolBufferSize, 16u);
    args.constFloatApi = fApi.data();
    args.constIntApi = iApi.data();
    args.constBoolApi = bApi.data();
    copy.copyConstantData(args);

    // reference for the NaN flush: a written NaN lane must be +0
    if (args.flushNan) {
      for (uint32_t i = 0; i < args.floatConstantCount; i++)
        for (uint32_t j = 0; j < 4; j++)
          bad += std::isnan(fOut[i].data[j]);
    }
    // Digest what shaders can read: the layouts' constants. Past the used
    // bools the paths differ by design: the SSE copy moves whole 16-byte groups
    // (up to 3 more source bools) and its pad16 stores whole 16-byte blocks,
    // where the portable path copies exactly and memsets to the end. Those
    // bytes sit past every index the shader reads.
    size_t fw = copy.getLayout(D3D9ConstantType::Float).computeConstantCount(args.floatConstantCount) * sizeof(Vector4);
    size_t iw = copy.getLayout(D3D9ConstantType::Int).computeConstantCount(0u) * sizeof(Vector4i);
    size_t bw = copy.getLayout(D3D9ConstantType::Bool).computeConstantCount(0u) * sizeof(uint32_t);
    d.bytes(fOut.data(), args.floatBufferSize);
    d.bytes(iOut.data(), args.intBufferSize);
    d.bytes(bOut.data(), bw);
    bad += fw > args.floatBufferSize || iw > args.intBufferSize;
  }
  CHECK(bad == 0);
  report("constant_copy", d);
}

// UpdateStateConstants (d3d9_state.h) for float and int constants.
struct FakeConsts {
  Vector4 fConsts[256];
  Vector4i iConsts[16];
  uint32_t bConsts[1];
};
struct FakeState {
  FakeConsts* vsConsts;
  FakeConsts* psConsts;
};

}

// d3d9_state.h's UpdateStateConstants, included last: it is a template over the state type.
#include "d3d9_state.h"

namespace {

void testUpdateConstants() {
  Digest d;
  XorShift r;
  uint64_t bad = 0;
  FakeConsts vs = {}, ps = {};
  FakeState st = { &vs, &ps };
  std::vector<uint32_t> src(256 * 4);
  for (uint32_t iter = 0; iter < 200000u; iter++) {
    uint32_t start = uint32_t(r.next() % 256u);
    uint32_t count = uint32_t(r.next() % (257u - start));
    FakeConsts before = vs;
    for (uint32_t i = 0; i < count * 4u; i++) {
      // mostly equal data so the dirty test sees both outcomes
      uint32_t old;
      std::memcpy(&old, &reinterpret_cast<const uint32_t*>(vs.fConsts)[start * 4u + i], 4);
      src[i] = (r.next() & 7u) ? old : randomFloatBits(r);
    }
    bool dirty = UpdateStateConstants<D3D9ShaderType::VertexShader, D3D9ConstantType::Float, float>(
      &st, start, reinterpret_cast<const float*>(src.data()), count);
    bool refDirty = std::memcmp(&before.fConsts[start], src.data(), count * 16u) != 0;
    bad += dirty != refDirty;
    bad += std::memcmp(&vs.fConsts[start], src.data(), count * 16u) != 0;
    d.add(dirty);
    d.bytes(vs.fConsts, sizeof(vs.fConsts));

    uint32_t istart = uint32_t(r.next() % 16u), icount = uint32_t(r.next() % (17u - istart));
    std::vector<int32_t> isrc(icount * 4u);
    for (auto& x : isrc)
      x = (r.next() & 3u) ? 0 : int32_t(r.next());
    d.add(UpdateStateConstants<D3D9ShaderType::PixelShader, D3D9ConstantType::Int, int>(
      &st, istart, isrc.data(), icount));
    d.bytes(ps.iConsts, sizeof(ps.iConsts));
  }
  CHECK(bad == 0);
  report("update_constants", d);
}

// D3D9InputSignature::find (SSE compare on x86, NEON on arm64/ARM64EC).
void testInputSignature() {
  Digest d;
  XorShift r;
  uint64_t bad = 0;
  for (uint32_t iter = 0; iter < 200000u; iter++) {
    D3D9InputSignature sig;
    uint32_t n = uint32_t(r.next() % 17u);
    for (uint32_t i = 0; i < n; i++) {
      dxbc_spv::sm3::Semantic s = {};
      s.usage = dxbc_spv::sm3::SemanticUsage(r.next() % 14u);
      s.index = uint32_t(r.next() % 16u);
      sig.add(s);
    }
    for (uint32_t u = 0; u < 14u; u++) {
      for (uint32_t idx = 0; idx < 16u; idx++) {
        dxbc_spv::sm3::Semantic s = {};
        s.usage = dxbc_spv::sm3::SemanticUsage(u);
        s.index = idx;
        uint32_t f = sig.find(s);
        // the SSE result: the first of the 32 bytes that matches, the count
        // byte (31) included; unused entries are 0xff, which no semantic packs to
        uint8_t packed = uint8_t(D3D9PackedSemantic(s));
        uint32_t ref = packed == sig.size() ? 31u : 32u;
        for (uint32_t i = 0; i < sig.size(); i++)
          if (uint8_t(D3D9PackedSemantic(sig.get(i))) == packed) { ref = i; break; }
        bad += f != ref;
        d.add(f);
      }
    }
  }
  CHECK(bad == 0);
  report("input_signature", d);
}

}

int main() {
  testBits32();
  testBits64();
  testInterleave();
  testCvtF32U32();
  testPointSize();
  testHashes();
  testConstantCopy();
  testUpdateConstants();
  testInputSignature();
  if (g_fail) {
    std::printf("%d check(s) failed\n", g_fail);
    return 1;
  }
  std::printf("arm64_xarch_test: all checks passed\n");
  return 0;
}
