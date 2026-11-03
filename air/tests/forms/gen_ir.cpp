/* gen_ir: hand-built dxbc-spirv IR fixtures for scripts/test-air-forms.sh.
 *
 *   gen_ir --list               the case names
 *   gen_ir <case> <out.ir>      the case's pixel shader, serialized IR
 *
 * Each case makes AirBuilder emit a known set of intrinsic forms (see
 * air/tests/forms/inventory.txt). airlib_loopback --ir feeds the file to
 * d9mtair_compile directly: no SM3 front end, no legalizeIr, so the IR here
 * must already be in the form AirBuilder takes (scalar integer ops, 2- or
 * 3-component coordinates). This also reaches forms the SM3 converter never
 * emits (ImageGather, ImageQuerySize). */
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include <ir/ir_builder.h>
#include <ir/ir_serialize.h>
#include <ir/ir_utils.h>

namespace {

using namespace dxbc_spv;

struct Gen {
  ir::Builder b;
  ir::SsaDef entry, in0, out, v, x, y, z, w, acc;

  Gen() {
    auto fn = b.add(ir::Op::Function(ir::ScalarType::eVoid));
    entry = b.add(ir::Op::EntryPoint(fn, ir::ShaderStage::ePixel));
    in0 = b.add(ir::Op::DclInput(vec(ir::ScalarType::eF32, 4u), entry, 0u, 0u));
    out = b.add(ir::Op::DclOutput(vec(ir::ScalarType::eF32, 4u), entry, 0u, 0u));
    b.add(ir::Op::Label());
    v = b.add(ir::Op::InputLoad(vec(ir::ScalarType::eF32, 4u), in0, ir::SsaDef()));
    x = ir::extractFromVector(b, v, 0u);
    y = ir::extractFromVector(b, v, 1u);
    z = ir::extractFromVector(b, v, 2u);
    w = ir::extractFromVector(b, v, 3u);
    acc = b.add(ir::Op::Constant(0.0f, 0.0f, 0.0f, 0.0f));
  }

  static ir::BasicType vec(ir::ScalarType t, uint32_t n) { return ir::BasicType(t, n); }

  /* A float vector of n components from x, y, z, w (n = 1: x). */
  ir::SsaDef coord(uint32_t n) {
    const ir::SsaDef c[4] = { x, y, z, w };
    return n == 1u ? x : ir::buildVector(b, ir::ScalarType::eF32, n, c);
  }

  /* Adds a scalar float result to the output (every component). */
  void use(ir::SsaDef scalar) {
    auto v4 = b.add(ir::Op::CompositeConstruct(vec(ir::ScalarType::eF32, 4u),
      scalar, scalar, scalar, scalar));
    useVec4(v4);
  }

  void useVec4(ir::SsaDef v4) {
    acc = b.add(ir::Op::FAdd(vec(ir::ScalarType::eF32, 4u), acc, v4));
  }

  void useU32(ir::SsaDef u) {
    use(b.add(ir::Op::ConvertItoF(ir::ScalarType::eF32, u)));
  }

  void useI32(ir::SsaDef i) {
    use(b.add(ir::Op::ConvertItoF(ir::ScalarType::eF32, i)));
  }

  ir::SsaDef texture(ir::ResourceKind kind, uint32_t slot) {
    auto dcl = b.add(ir::Op::DclSrv(ir::ScalarType::eF32, entry, 0u, slot, 1u, kind));
    return b.add(ir::Op::DescriptorLoad(ir::ScalarType::eSrv, dcl, b.makeConstant(0u)));
  }

  ir::SsaDef sampler(uint32_t slot) {
    auto dcl = b.add(ir::Op::DclSampler(entry, 0u, slot, 1u));
    return b.add(ir::Op::DescriptorLoad(ir::ScalarType::eSampler, dcl, b.makeConstant(0u)));
  }

  std::vector<uint8_t> finish() {
    b.add(ir::Op::OutputStore(out, ir::SsaDef(), acc));
    b.add(ir::Op::Return());
    b.add(ir::Op::FunctionEnd());
    ir::Serializer s(b);
    std::vector<uint8_t> blob(s.computeSerializedSize());
    if (!s.serialize(blob.data(), blob.size()))
      blob.clear();
    return blob;
  }
};

using ir::ScalarType;
using ir::ResourceKind;

/* air.fast_sin/cos/exp2/log2/rsqrt/fract/pow.f32 and air.sign.f32 */
void caseMath(Gen& g, ScalarType t) {
  auto& b = g.b;
  auto in = t == ScalarType::eF32 ? g.x : b.add(ir::Op::ConvertFtoF(t, g.x));
  auto back = [&](ir::SsaDef r) {
    return t == ScalarType::eF32 ? r : b.add(ir::Op::ConvertFtoF(ScalarType::eF32, r));
  };
  g.use(back(b.add(ir::Op::FSin(t, in))));
  g.use(back(b.add(ir::Op::FCos(t, in))));
  g.use(back(b.add(ir::Op::FExp2(t, in))));
  g.use(back(b.add(ir::Op::FLog2(t, in))));
  g.use(back(b.add(ir::Op::FRsq(t, in))));
  g.use(back(b.add(ir::Op::FFract(t, in))));
  g.use(back(b.add(ir::Op::FSgn(t, in))));
  g.use(back(b.add(ir::Op::FPow(t, in, in))));
}

/* air.abs.s.i32, air.max/min.s.i32, air.max/min.u.i32 (also through the clamps) */
void caseInt(Gen& g) {
  auto& b = g.b;
  auto i = b.add(ir::Op::ConvertFtoI(ScalarType::eI32, g.x));
  auto j = b.add(ir::Op::ConvertFtoI(ScalarType::eI32, g.y));
  auto u = b.add(ir::Op::ConvertFtoI(ScalarType::eU32, g.z));
  auto v = b.add(ir::Op::ConvertFtoI(ScalarType::eU32, g.w));
  g.useI32(b.add(ir::Op::IAbs(ScalarType::eI32, i)));
  g.useI32(b.add(ir::Op::SMax(ScalarType::eI32, i, j)));
  g.useI32(b.add(ir::Op::SMin(ScalarType::eI32, i, j)));
  g.useU32(b.add(ir::Op::UMax(ScalarType::eU32, u, v)));
  g.useU32(b.add(ir::Op::UMin(ScalarType::eU32, u, v)));
  g.useI32(b.add(ir::Op::SClamp(ScalarType::eI32, i, j, b.makeConstant(int32_t(7)))));
  g.useU32(b.add(ir::Op::UClamp(ScalarType::eU32, u, v, b.makeConstant(uint32_t(7u)))));
}

/* air.dfdx/dfdy .f32, .v2f32, .v3f32, .v4f32 */
void caseDeriv(Gen& g) {
  auto& b = g.b;
  for (uint32_t n = 1u; n <= 4u; n++) {
    auto c = g.coord(n);
    for (bool y : { false, true }) {
      auto t = Gen::vec(ScalarType::eF32, n);
      auto d = b.add(y ? ir::Op::DerivY(t, c, ir::DerivativeMode::eDefault)
                       : ir::Op::DerivX(t, c, ir::DerivativeMode::eDefault));
      g.use(n == 1u ? d : ir::extractFromVector(b, d, 0u));
    }
  }
}

/* air.discard_fragment */
void caseDiscard(Gen& g) {
  g.b.add(ir::Op::Demote());
  g.use(g.x);
}

uint32_t dims(ResourceKind kind) {
  return kind == ResourceKind::eImage2D ? 2u : 3u;
}

/* air.sample_texture_<kind>.v4f32 (plain, bias and level share the form) */
void caseSample(Gen& g, ResourceKind kind) {
  auto& b = g.b;
  auto t = g.texture(kind, 0u), s = g.sampler(0u);
  auto c = g.coord(dims(kind));
  auto v4 = Gen::vec(ScalarType::eF32, 4u);
  ir::SsaDef none;
  g.useVec4(b.add(ir::Op::ImageSample(v4, t, s, none, c, none, none, none, none, none, none, none)));
  g.useVec4(b.add(ir::Op::ImageSample(v4, t, s, none, c, none, none, g.w, none, none, none, none)));
  g.useVec4(b.add(ir::Op::ImageSample(v4, t, s, none, c, none, g.w, none, none, none, none, none)));
}

/* air.sample_texture_<kind>_grad.v4f32 */
void caseSampleGrad(Gen& g, ResourceKind kind) {
  auto& b = g.b;
  auto t = g.texture(kind, 0u), s = g.sampler(0u);
  auto c = g.coord(dims(kind));
  ir::SsaDef none;
  g.useVec4(b.add(ir::Op::ImageSample(Gen::vec(ScalarType::eF32, 4u), t, s, none, c, none,
    none, none, none, c, c, none)));
}

/* air.sample_compare_depth_<kind>.f32, with grad: _grad */
void caseCompare(Gen& g, ResourceKind kind, bool grad) {
  auto& b = g.b;
  auto t = g.texture(kind, 0u), s = g.sampler(0u);
  auto c = g.coord(dims(kind));
  ir::SsaDef none;
  g.use(b.add(ir::Op::ImageSample(ScalarType::eF32, t, s, none, c, none,
    grad ? none : g.w, none, none, grad ? c : none, grad ? c : none, g.z)));
}

/* air.gather_texture_<kind>.v4f32 */
void caseGather(Gen& g, ResourceKind kind) {
  auto& b = g.b;
  auto t = g.texture(kind, 0u), s = g.sampler(0u);
  ir::SsaDef none;
  g.useVec4(b.add(ir::Op::ImageGather(Gen::vec(ScalarType::eF32, 4u), t, s, none,
    g.coord(dims(kind)), none, none, 1u)));
}

/* air.get_width/height[/depth]_texture_<kind> and air.get_num_mip_levels_texture_<kind> */
void caseQuery(Gen& g, ResourceKind kind) {
  auto& b = g.b;
  auto t = g.texture(kind, 0u);
  uint32_t n = kind == ResourceKind::eImage3D ? 3u : 2u;
  ir::Type type;
  type.addStructMember(ir::BasicType(ScalarType::eU32, n));
  type.addStructMember(ir::BasicType(ScalarType::eU32));
  auto q = b.add(ir::Op::ImageQuerySize(type, t, b.makeConstant(0u)));
  auto size = b.add(ir::Op::CompositeExtract(Gen::vec(ScalarType::eU32, n), q, b.makeConstant(0u)));
  g.useU32(ir::extractFromVector(b, size, 0u));
  g.useU32(b.add(ir::Op::CompositeExtract(ScalarType::eU32, q, b.makeConstant(1u))));
}

const std::vector<std::pair<const char*, std::function<void(Gen&)>>>& cases() {
  static const std::vector<std::pair<const char*, std::function<void(Gen&)>>> list = {
    { "math",               [](Gen& g) { caseMath(g, ScalarType::eF32); } },
    { "math_half",          [](Gen& g) { caseMath(g, ScalarType::eF16); } },
    { "int",                caseInt },
    { "deriv",              caseDeriv },
    { "discard",            caseDiscard },
    { "sample_2d",          [](Gen& g) { caseSample(g, ResourceKind::eImage2D); } },
    { "sample_cube",        [](Gen& g) { caseSample(g, ResourceKind::eImageCube); } },
    { "sample_3d",          [](Gen& g) { caseSample(g, ResourceKind::eImage3D); } },
    { "sample_2d_grad",     [](Gen& g) { caseSampleGrad(g, ResourceKind::eImage2D); } },
    { "sample_cube_grad",   [](Gen& g) { caseSampleGrad(g, ResourceKind::eImageCube); } },
    { "sample_3d_grad",     [](Gen& g) { caseSampleGrad(g, ResourceKind::eImage3D); } },
    { "compare_2d",         [](Gen& g) { caseCompare(g, ResourceKind::eImage2D, false); } },
    { "compare_cube",       [](Gen& g) { caseCompare(g, ResourceKind::eImageCube, false); } },
    { "compare_2d_grad",    [](Gen& g) { caseCompare(g, ResourceKind::eImage2D, true); } },
    { "compare_cube_grad",  [](Gen& g) { caseCompare(g, ResourceKind::eImageCube, true); } },
    { "gather_2d",          [](Gen& g) { caseGather(g, ResourceKind::eImage2D); } },
    { "gather_cube",        [](Gen& g) { caseGather(g, ResourceKind::eImageCube); } },
    { "query_2d",           [](Gen& g) { caseQuery(g, ResourceKind::eImage2D); } },
    { "query_cube",         [](Gen& g) { caseQuery(g, ResourceKind::eImageCube); } },
    { "query_3d",           [](Gen& g) { caseQuery(g, ResourceKind::eImage3D); } },
  };
  return list;
}

}

int main(int argc, char** argv) {
  if (argc == 2 && !std::strcmp(argv[1], "--list")) {
    for (const auto& c : cases())
      std::printf("%s\n", c.first);
    return 0;
  }
  if (argc != 3) {
    std::fprintf(stderr, "usage: %s --list | <case> <out.ir>\n", argv[0]);
    return 2;
  }
  for (const auto& c : cases()) {
    if (std::strcmp(c.first, argv[1]))
      continue;
    Gen g;
    c.second(g);
    std::vector<uint8_t> blob = g.finish();
    if (blob.empty()) {
      std::fprintf(stderr, "FATAL: %s: ir serialization failed\n", argv[1]);
      return 1;
    }
    std::ofstream out(argv[2], std::ios::binary);
    out.write(reinterpret_cast<const char*>(blob.data()), std::streamsize(blob.size()));
    return out ? 0 : 1;
  }
  std::fprintf(stderr, "FATAL: no case %s\n", argv[1]);
  return 2;
}
