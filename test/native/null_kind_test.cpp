// #62: the null texture an unbound argument-table slot gets, and which slots a shader change
// rewrites. The statements under test are the production ones, cut out of mtl_context_impl.h and
// mtl_context_transfer.cpp by null_kind_extract.py (scripts/test-null-kind.sh). Fails to build or
// run on a commit without #62 (no textureDim, no per-shader kind, no rewrite on a shader change),
// and the extractor stops on one without #152's kNull3DFirstSlot.
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

static const uint64_t N2 = 0x2D, NC = 0xCB, N3 = 0x3D;

// The convention for a slot no shader declares, as the fixed-function fragment shader lays out its
// stages (#152): 2D at fragment slots 0-7, cube at 8-15, volume at 16-23; every vertex slot 2D.
// Returns the number of slots where `ctx` picks another null texture.
template <typename Context>
static int conventionMismatches(const Context& ctx, bool print) {
  int bad = 0;
  for (uint32_t slot = 0; slot < 24; slot++) {
    const uint64_t want = slot >= 16 ? N3 : slot >= 8 ? NC : N2;
    const bool ok = ctx.nullTextureId(0u, slot) == N2
                 && ctx.nullTextureId(1u, slot) == want
                 && ctx.nullTextureId(1u, slot, nullptr) == want;
    if (!ok) {
      bad++;
      if (print)
        std::printf("FAIL convention at slot %u: vertex %llx, fragment %llx, fragment with a null fn %llx (want %llx, %llx, %llx)\n",
                    slot, (unsigned long long)ctx.nullTextureId(0u, slot), (unsigned long long)ctx.nullTextureId(1u, slot),
                    (unsigned long long)ctx.nullTextureId(1u, slot, nullptr), (unsigned long long)N2,
                    (unsigned long long)want, (unsigned long long)want);
    }
  }
  return bad;
}

int main() {
  const NullKindContext ctx;

  // fixed-function shaders (no reflection): the convention
  failures += conventionMismatches(ctx, true);
  // the negative case: the same method with the first 3D slot at 17 gives fragment slot 16 the
  // null cube, and the check above must see exactly that slot
  const NullKindContextPlanted planted;
  EXPECT(conventionMismatches(planted, false) == 1, "a wrong first 3D slot must fail the convention check at one slot");
  EXPECT(planted.nullTextureId(1u, 16u) == NC && planted.nullTextureId(1u, 17u) == N3, "the planted mapping is the wrong one");

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
  EXPECT(ctx.nullTextureId(1u, 16u, &a) == N3, "fragment slot beyond the map: the convention, volume from 16");
  EXPECT(ctx.nullTextureId(1u, 23u, &a) == N3, "fragment slot 23 beyond the map: volume");
  EXPECT(ctx.nullTextureId(0u, 16u, &a) == N2, "vertex slot beyond the map: 2D");

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
