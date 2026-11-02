// #62: the null texture an unbound argument-table slot gets, and which slots a shader change
// rewrites. The statements under test are the production ones, cut out of mtl_context_impl.h and
// mtl_context_transfer.cpp by null_kind_extract.py (scripts/test-null-kind.sh). Fails to build or
// run on a commit without #62 (no textureDim, no per-shader kind, no rewrite on a shader change).
#include <array>
#include <cstdint>
#include <cstdio>

struct MtlShaderFunction {
  static constexpr uint8_t NoTextureDim = 0xFFu;
  std::array<uint8_t, 16> textureDim;
  void* library = nullptr;
  MtlShaderFunction() { textureDim.fill(NoTextureDim); }
};

#include "null_kind_extract.h"

struct Bindings {
  uint64_t tableFnId = 0u;
  uint32_t dirtyTextures = 0u;
  std::array<int, 16> textures = { };
};

static int failures = 0;
#define EXPECT(cond, ...) do { if (!(cond)) { failures++; std::printf("FAIL %s: ", #cond); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

int main() {
  const NullKindContext ctx;
  const uint64_t N2 = 0x2D, NC = 0xCB, N3 = 0x3D;

  // fixed-function shaders (no reflection): the old convention, 2D, cube at fragment slots 8-15
  for (uint32_t slot = 0; slot < 16; slot++) {
    EXPECT(ctx.nullTextureId(0u, slot) == N2, "FF vertex slot %u", slot);
    EXPECT(ctx.nullTextureId(1u, slot) == (slot >= 8 ? NC : N2), "FF fragment slot %u", slot);
    EXPECT(ctx.nullTextureId(1u, slot, nullptr) == (slot >= 8 ? NC : N2), "null fn, fragment slot %u", slot);
  }

  // a shader's declared kinds win over the convention; undeclared slots keep it
  int libA = 0, libB = 0;
  MtlShaderFunction a;
  a.library = &libA;
  a.textureDim[3] = 2u;   // volume at fragment slot 3
  a.textureDim[9] = 0u;   // 2D at fragment slot 9 (the convention says cube)
  a.textureDim[2] = 1u;   // cube at slot 2 (the convention says 2D)
  EXPECT(ctx.nullTextureId(1u, 3u, &a) == N3, "declared volume");
  EXPECT(ctx.nullTextureId(1u, 9u, &a) == N2, "declared 2D at slot 9");
  EXPECT(ctx.nullTextureId(1u, 2u, &a) == NC, "declared cube at slot 2");
  EXPECT(ctx.nullTextureId(0u, 3u, &a) == N3, "vertex stage uses the declared kind too");
  EXPECT(ctx.nullTextureId(1u, 0u, &a) == N2, "undeclared fragment slot 0: 2D");
  EXPECT(ctx.nullTextureId(1u, 12u, &a) == NC, "undeclared fragment slot 12: cube");
  EXPECT(ctx.nullTextureId(1u, 16u, &a) == NC, "slot beyond the map: the convention");

  // the same slot, another shader declaring another kind: a shader change rewrites every slot
  MtlShaderFunction bfn;
  bfn.library = &libB;
  bfn.textureDim[3] = 1u;   // cube at slot 3 now
  Bindings b;
  const MtlShaderFunction* fn = &a;
  EXPECT(tableTextureMask(b, fn) == 0xFFFFu, "first shader: every slot");
  EXPECT(tableTextureMask(b, fn) == 0u, "same shader, nothing dirty: no slot");
  b.dirtyTextures = 1u << 5;
  EXPECT(tableTextureMask(b, fn) == (1u << 5), "same shader: only the dirty slot");
  b.dirtyTextures = 0u;
  fn = &bfn;
  EXPECT(tableTextureMask(b, fn) == 0xFFFFu, "shader change: every slot, so slot 3 is rewritten");
  EXPECT(ctx.nullTextureId(1u, 3u, fn) == NC, "slot 3 after the change: the new shader's cube");
  fn = nullptr;
  EXPECT(tableTextureMask(b, fn) == 0xFFFFu, "to fixed function: every slot");
  EXPECT(ctx.nullTextureId(1u, 3u, fn) == N2, "slot 3 under fixed function: 2D again");

  std::printf("null_kind_test: %s (%d failures)\n", failures ? "FAIL" : "OK", failures);
  return failures ? 1 : 0;
}
