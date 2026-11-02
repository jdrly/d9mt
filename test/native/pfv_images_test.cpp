// pfv_images_test: D9MT_TEX_PFV's parsing, the GPU default and which images get
// PixelFormatView (no lossless compression), driver/mtl/mtl_pfv.h.
//   clang++ -std=c++17 -I driver/mtl test/native/pfv_images_test.cpp -o pfv && ./pfv
#include <cstdio>
#include <string>

#include "mtl_pfv.h"

using namespace dxvk;
namespace P = dxvk::MtlPfvImages;

static int fails = 0;
#define EXPECT(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static uint32_t parse(const char* v, bool* ok = nullptr) {
  bool understood = false;
  const uint32_t r = P::parse(v, understood);
  if (ok)
    *ok = understood;
  return r;
}

// Image kinds: (depth, sampleCount, resolveTarget)
struct Kind { const char* name; bool depth; uint32_t samples; bool resolve; };
static const Kind kKinds[] = {
  { "1x colour",            false, 1u, false },
  { "1x depth",             true,  1u, false },
  { "4x colour",            false, 4u, false },
  { "4x depth",             true,  4u, false },
  { "colour resolve image", false, 1u, true  },
  { "depth resolve image",  true,  1u, true  },
};

// which of kKinds get PixelFormatView, as a bit per kind
static uint32_t kinds(uint32_t pfv) {
  uint32_t r = 0u;
  for (uint32_t i = 0u; i < sizeof(kKinds) / sizeof(kKinds[0]); i++)
    if (P::wants(pfv, false, kKinds[i].depth, kKinds[i].samples, kKinds[i].resolve))
      r |= 1u << i;
  return r;
}

int main() {
  bool ok = false;

  // parsing
  EXPECT(parse("", &ok) == P::Default && ok);
  EXPECT(parse("1", &ok) == P::Every && ok);
  EXPECT(parse("1x") == P::Every);              // "a value starting with 1", as before
  EXPECT(parse("0", &ok) == 0u && ok);
  EXPECT(parse("msaa", &ok) == P::Msaa && ok);
  EXPECT(parse("resolve", &ok) == P::Resolve && ok);
  EXPECT(parse("ds", &ok) == P::Depth && ok);
  EXPECT(parse("msaa,resolve", &ok) == (P::Msaa | P::Resolve) && ok);
  EXPECT(parse("resolve,ds") == (P::Resolve | P::MsaaDepth));
  EXPECT(parse("all", &ok) == P::Default && !ok);
  EXPECT(parse("msaa,", &ok) == P::Default && !ok);
  EXPECT(parse("MSAA", &ok) == P::Default && !ok);
  EXPECT(parse("00", &ok) == P::Default && !ok);

  // the GPU's default: MSAA and resolve images before Apple10, nothing on Apple10
  EXPECT(P::effective(P::Default, true) == (P::Msaa | P::Resolve));
  EXPECT(P::effective(P::Default, false) == 0u);
  // a setting holds on every GPU
  EXPECT(P::effective(0u, true) == 0u);
  EXPECT(P::effective(P::Msaa, false) == P::Msaa);
  EXPECT(P::effective(P::Every, false) == P::Every);

  // which images: bits per kKinds index
  //   0 1x colour, 1 1x depth, 2 4x colour, 3 4x depth, 4 colour resolve, 5 depth resolve
  EXPECT(kinds(0u) == 0u);                       // M5 default and D9MT_TEX_PFV=0
  EXPECT(kinds(P::Msaa | P::Resolve) == 0x3cu);  // M1-M4 default: never a plain 1x image
  EXPECT(kinds(P::Msaa) == 0x0cu);
  EXPECT(kinds(P::Resolve) == 0x30u);
  EXPECT(kinds(P::Depth) == 0x28u);
  EXPECT(kinds(P::Every) == 0x3fu);
  // block-compressed images always get it
  EXPECT(P::wants(0u, true, false, 1u, false));

  // a resolve into a game's image goes through the resolve image when resolve images
  // are uncompressed and the game's isn't (not under Every)
  EXPECT(!P::resolveViaImage(0u, false) && !P::resolveViaImage(0u, true));
  EXPECT(P::resolveViaImage(P::Msaa | P::Resolve, false) && P::resolveViaImage(P::Msaa | P::Resolve, true));
  EXPECT(!P::resolveViaImage(P::Msaa, false) && !P::resolveViaImage(P::Msaa, true));
  EXPECT(P::resolveViaImage(P::Depth, true) && !P::resolveViaImage(P::Depth, false));
  EXPECT(!P::resolveViaImage(P::Every, false) && !P::resolveViaImage(P::Every, true));

  std::printf("pfv_images_test: %s\n", fails ? "FAIL" : "ok");
  return fails ? 1 : 0;
}
