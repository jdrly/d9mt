// Native unit test for a sampler's texture kind (driver/backend/mtl_texture_kind.h, #150) and the
// argument buffer's texture word (MtlContextImpl::flushResources, modelled here exactly: the null
// texture of the declared kind, replaced by the bound view's ID when its kind is the declared one).
//  A. the kind of every Metal texture type: 2D, cube and 3D are the declared kinds, every array,
//     multisample and buffer type is none of them;
//  B. matches: a view's kind is accepted only at a sampler that declares that kind;
//  C. the word for each declared kind (2D, cube, 3D) with the slot unbound, bound to a view of
//     each kind, and bound to an array view: the view's ID only for its own kind, else the null
//     texture of the declared kind (a 2D texture at a dcl_cube sampler gets the null cube);
//  D. a shader change over the same binding: the 2D texture a dcl_2d sampler took is replaced by
//     the null cube once a shader declaring a cube there reads the slot, and comes back after.
// Built with -DTEXTURE_KIND_TEST_OLD, the word is the old one (the bound ID, whatever its kind):
// C and D fail there, which is what this change fixes.
//   clang++ -std=c++17 -I driver/backend test/native/texture_kind_test.cpp -o /tmp/tkt && /tmp/tkt
#include "mtl_texture_kind.h"

#include <cstdio>
#include <initializer_list>

using namespace dxvk::backend;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

// MTLTextureType / WMTTextureType
enum : uint32_t { T1D = 0, T1DArray, T2D, T2DArray, T2DMS, TCube, TCubeArray, T3D, T2DMSArray, TBuffer };

static constexpr texkind::NullIds kNulls = { 0x2d00u, 0xc0beu, 0x3d00u };

// flushResources' texture word for a slot of declared kind dim (bound: a view of that kind and ID)
static uint64_t word(uint8_t dim, bool bound, uint8_t kind, uint64_t id) {
  uint64_t v = texkind::nullOf(dim, kNulls);
  if (bound) {
#ifdef TEXTURE_KIND_TEST_OLD
    (void)kind;
    v = id;
#else
    if (texkind::matches(dim, kind))
      v = id;
#endif
  }
  return v;
}

int main() {
  // A
  CHECK(texkind::ofMetalType(T2D) == texkind::k2D);
  CHECK(texkind::ofMetalType(TCube) == texkind::kCube);
  CHECK(texkind::ofMetalType(T3D) == texkind::k3D);
  for (uint32_t t : { T1D, T1DArray, T2DArray, T2DMS, TCubeArray, T2DMSArray, TBuffer })
    CHECK(texkind::ofMetalType(t) == texkind::kOther);
  CHECK(texkind::ofMetalType(0xFFFFu) == texkind::kOther);

  // B
  const uint8_t dims[] = { texkind::k2D, texkind::kCube, texkind::k3D };
  const uint8_t kinds[] = { texkind::k2D, texkind::kCube, texkind::k3D, texkind::kOther };
  for (uint8_t d : dims)
    for (uint8_t k : kinds)
      CHECK(texkind::matches(d, k) == (d == k));

  // C
  const uint64_t nullOfDim[] = { kNulls.tex2D, kNulls.cube, kNulls.tex3D };
  for (uint8_t d : dims) {
    CHECK(word(d, false, texkind::k2D, 0x1234u) == nullOfDim[d]);
    for (uint8_t k : kinds) {
      const uint64_t id = 0x1000u + k;
      CHECK(word(d, true, k, id) == (k == d ? id : nullOfDim[d]));
    }
  }
  CHECK(word(texkind::kCube, true, texkind::k2D, 0x77u) == kNulls.cube);
  CHECK(word(texkind::kCube, false, texkind::k2D, 0x77u) == kNulls.cube);
  CHECK(word(texkind::kCube, true, texkind::kCube, 0x77u) == 0x77u);

  // D: one binding (a 2D view) read by a dcl_2d shader, a dcl_cube one, then the dcl_2d one
  const uint64_t tex = 0x5555u;
  CHECK(word(texkind::k2D, true, texkind::ofMetalType(T2D), tex) == tex);
  CHECK(word(texkind::kCube, true, texkind::ofMetalType(T2D), tex) == kNulls.cube);
  CHECK(word(texkind::k2D, true, texkind::ofMetalType(T2D), tex) == tex);

  std::printf("texture_kind_test: %s (%d failed)\n", g_fail ? "FAIL" : "ok", g_fail);
  return g_fail ? 1 : 0;
}
