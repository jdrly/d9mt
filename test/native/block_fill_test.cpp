// Native unit test for util::encodeSolidBlock (ColorFill of block-compressed
// surfaces, #14): every texel of the encoded block, decoded by the BC1-BC3
// rules below (written from the format specification, not from the
// encoder), is the D3DCOLOR quantised to the format.
//   clang++ -std=c++17 -I driver test/native/block_fill_test.cpp -o /tmp/bft && /tmp/bft
#include "util/util_image_math.h"

#include <cstdio>

using namespace dxvk;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

struct Texel { uint32_t r, g, b, a; };

static uint32_t expand(uint32_t v, uint32_t bits) { return (v << (8u - bits)) | (v >> (2u * bits - 8u)); }

// A colour block; `bc1`: as BC1 decodes it (3-colour mode when color0 <= color1).
static Texel decodeColor(const uint8_t* p, uint32_t i, bool bc1) {
  const uint32_t c0 = p[0] | (p[1] << 8), c1 = p[2] | (p[3] << 8);
  const uint32_t idx = (uint32_t(p[4] | (p[5] << 8) | (p[6] << 16) | (uint32_t(p[7]) << 24)) >> (2u * i)) & 3u;
  auto rgb = [] (uint32_t c) { return Texel { expand(c >> 11, 5), expand((c >> 5) & 63u, 6), expand(c & 31u, 5), 255u }; };
  const Texel t0 = rgb(c0), t1 = rgb(c1);
  if (!bc1 || c0 > c1) {
    switch (idx) {
      case 0: return t0;
      case 1: return t1;
      case 2: return Texel { (2 * t0.r + t1.r) / 3, (2 * t0.g + t1.g) / 3, (2 * t0.b + t1.b) / 3, 255u };
      default: return Texel { (t0.r + 2 * t1.r) / 3, (t0.g + 2 * t1.g) / 3, (t0.b + 2 * t1.b) / 3, 255u };
    }
  }
  switch (idx) {
    case 0: return t0;
    case 1: return t1;
    case 2: return Texel { (t0.r + t1.r) / 2, (t0.g + t1.g) / 2, (t0.b + t1.b) / 2, 255u };
    default: return Texel { 0u, 0u, 0u, 0u };
  }
}

static Texel decode(MtlFormat f, const uint8_t* p, uint32_t i) {
  if (f == MtlFormat::BC1_RGBA || f == MtlFormat::BC1_RGBA_sRGB)
    return decodeColor(p, i, true);
  Texel t = decodeColor(p + 8, i, false);
  if (f == MtlFormat::BC2_RGBA || f == MtlFormat::BC2_RGBA_sRGB) {
    t.a = expand((p[i / 2u] >> (4u * (i & 1u))) & 15u, 4);
  } else {
    const uint32_t a0 = p[0], a1 = p[1];
    uint64_t bits = 0u;
    for (uint32_t k = 0u; k < 6u; k++)
      bits |= uint64_t(p[2 + k]) << (8u * k);
    const uint32_t idx = uint32_t(bits >> (3u * i)) & 7u;
    if (idx == 0u)      t.a = a0;
    else if (idx == 1u) t.a = a1;
    else if (a0 > a1)   t.a = ((8u - idx) * a0 + (idx - 1u) * a1) / 7u;
    else if (idx < 6u)  t.a = ((6u - idx) * a0 + (idx - 1u) * a1) / 5u;
    else                t.a = idx == 6u ? 0u : 255u;
  }
  return t;
}

static uint32_t quant(uint32_t c, uint32_t bits) {
  const uint32_t max = (1u << bits) - 1u;
  return expand((c * max + 127u) / 255u, bits);
}

int main() {
  const MtlFormat formats[] = { MtlFormat::BC1_RGBA, MtlFormat::BC1_RGBA_sRGB, MtlFormat::BC2_RGBA,
                                MtlFormat::BC2_RGBA_sRGB, MtlFormat::BC3_RGBA, MtlFormat::BC3_RGBA_sRGB };
  uint32_t colors[64] = { 0x00000000u, 0xffffffffu, 0xff000000u, 0x00ffffffu, 0xffff0000u, 0xff00ff00u,
                          0xff0000ffu, 0x7f808080u, 0x80808080u, 0xfe010203u, 0x01fefdfcu, 0xffc0a060u };
  uint32_t seed = 1u;
  for (uint32_t i = 12u; i < 64u; i++) {
    seed = seed * 1664525u + 1013904223u;
    colors[i] = seed;
  }
  for (MtlFormat f : formats) {
    for (uint32_t c : colors) {
      uint8_t block[16];
      const uint32_t n = util::encodeSolidBlock(f, c, block);
      const bool bc1 = f == MtlFormat::BC1_RGBA || f == MtlFormat::BC1_RGBA_sRGB;
      CHECK(n == (bc1 ? 8u : 16u));
      const uint32_t a = c >> 24, r = (c >> 16) & 255u, g = (c >> 8) & 255u, b = c & 255u;
      Texel want = { quant(r, 5), quant(g, 6), quant(b, 5), 255u };
      if (bc1 && a < 128u)
        want = Texel { 0u, 0u, 0u, 0u };
      else if (f == MtlFormat::BC2_RGBA || f == MtlFormat::BC2_RGBA_sRGB)
        want.a = quant(a, 4);
      else if (!bc1)
        want.a = a;
      for (uint32_t i = 0u; i < 16u; i++) {
        const Texel t = decode(f, block, i);
        CHECK(t.r == want.r && t.g == want.g && t.b == want.b && t.a == want.a);
      }
    }
  }
  uint8_t block[16];
  CHECK(util::encodeSolidBlock(MtlFormat::BC4_RUnorm, 0xffffffffu, block) == 0u);
  CHECK(util::encodeSolidBlock(MtlFormat::BC5_RGUnorm, 0xffffffffu, block) == 0u);
  CHECK(util::encodeSolidBlock(MtlFormat::BGRA8Unorm, 0xffffffffu, block) == 0u);
  // Spot values: 0xff808080 -> 565 (16,32,16) = 0x8410.
  util::encodeSolidBlock(MtlFormat::BC1_RGBA, 0xff808080u, block);
  CHECK(block[0] == 0x10 && block[1] == 0x84 && block[2] == 0x10 && block[3] == 0x84 && block[4] == 0);
  util::encodeSolidBlock(MtlFormat::BC2_RGBA, 0x88ffffffu, block);
  CHECK(block[0] == 0x88 && block[7] == 0x88 && block[8] == 0xff && block[9] == 0xff);
  if (g_fail) {
    std::printf("%d check(s) failed\n", g_fail);
    return 1;
  }
  std::printf("block_fill_test: OK\n");
  return 0;
}
