// Benchmarks for the driver hot paths whose code depends on the architecture
// (docs/arm64ec-port.md): the constant-buffer copy (D3D9ConstantBufferCopy,
// SSE on x86, NEON otherwise), the SetShaderConstant dirty test
// (UpdateStateConstants)
// and D3D9InputSignature::find. It runs the real driver code: the SSE paths
// on x86, the NEON paths (util_bit.h) on arm64, which ARM64EC compiles too.
//
// Build like test/native/arm64_xarch_test.cpp; run the x86_64 build under
// Rosetta (today's shipping path) and the arm64 build natively (ARM64EC's).
// Prints the median ns per call of 15 runs.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "d3d9_constant_copy.h"
#include "d3d9_shader_analysis.h"
#include "../util/util_bit.h"

using namespace dxvk;

namespace {

template<typename T> inline void keep(T const& v) { asm volatile("" : : "r,m"(v) : "memory"); }
inline void clobber() { asm volatile("" : : : "memory"); }

template<typename Fn>
double bench(const char* name, uint32_t iters, Fn&& fn) {
  std::vector<double> runs;
  for (uint32_t i = 0; i < iters; i++) {  // warm-up: caches, clocks
    fn(i);
    clobber();
  }
  for (int r = 0; r < 15; r++) {
    auto t0 = std::chrono::steady_clock::now();
    for (uint32_t i = 0; i < iters; i++) {
      fn(i);
      clobber();
    }
    auto t1 = std::chrono::steady_clock::now();
    runs.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count() / iters);
  }
  std::sort(runs.begin(), runs.end());
  std::printf("  %-44s %9.1f ns\n", name, runs[runs.size() / 2]);
  return runs[runs.size() / 2];
}

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

#include "d3d9_state.h"

int main() {
#if defined(__aarch64__)
  std::printf("arm64 (NEON paths = ARM64EC's), native\n");
#else
  std::printf("x86_64 (SSE paths = the shipping x64 build's)\n");
#endif
  std::vector<Vector4> fApi(256), fOut(260);
  for (uint32_t i = 0; i < 256; i++)
    fApi[i] = Vector4(float(i), -float(i), 0.5f * float(i), 1.0f);
  std::vector<Vector4i> iApi(16), iOut(20);
  std::vector<uint32_t> bApi(32), bOut(64);

  for (uint32_t count : { 32u, 128u, 256u }) {
    uint32_t mask[8] = {};
    for (uint32_t i = 0; i < count; i++)
      mask[i / 32] |= 1u << (i % 32);
    uint32_t imask[1] = { 0xffu }, bmask[1] = { 0xffffu };
    D3D9ConstantBufferCopy copy(D3D9ConstantBufferLayout(8u, mask),
      D3D9ConstantBufferLayout(1u, imask), D3D9ConstantBufferLayout(1u, bmask));
    for (bool flush : { false, true }) {
      D3D9ConstantBufferCopyArgs a = {};
      a.floatBuffer = fOut.data();
      a.floatBufferSize = count * 16u;
      a.flushNan = flush;
      a.intBuffer = iOut.data();
      a.intBufferSize = 8u * 16u;
      a.boolBuffer = bOut.data();
      a.boolBufferSize = 16u * 4u;
      a.constFloatApi = fApi.data();
      a.constIntApi = iApi.data();
      a.constBoolApi = bApi.data();
      char name[96];
      std::snprintf(name, sizeof(name), "copyConstantData %3u f + 8 i + 16 b%s", count, flush ? " flushNan" : "");
      bench(name, 200000u, [&](uint32_t) { copy.copyConstantData(a); });
    }
  }

  FakeConsts vs = {}, ps = {};
  FakeState st = { &vs, &ps };
  for (uint32_t count : { 1u, 4u, 16u, 64u, 256u }) {
    std::vector<Vector4> src(fApi.begin(), fApi.begin() + count);
    char name[96];
    std::memcpy(vs.fConsts, src.data(), count * 16u);
    std::snprintf(name, sizeof(name), "UpdateStateConstants F x%-3u unchanged", count);
    bench(name, 1000000u, [&](uint32_t) {
      keep(UpdateStateConstants<D3D9ShaderType::VertexShader, D3D9ConstantType::Float, float>(
        &st, 0u, src[0].data, count)); });
    std::snprintf(name, sizeof(name), "UpdateStateConstants F x%-3u changed", count);
    bench(name, 1000000u, [&](uint32_t i) {
      src[0].data[0] = float(i);
      keep(UpdateStateConstants<D3D9ShaderType::VertexShader, D3D9ConstantType::Float, float>(
        &st, 0u, src[0].data, count)); });
  }

  D3D9InputSignature sig;
  for (uint32_t i = 0; i < 12u; i++) {
    dxbc_spv::sm3::Semantic s = {};
    s.usage = dxbc_spv::sm3::SemanticUsage(i % 14u);
    s.index = i / 14u;
    sig.add(s);
  }
  bench("D3D9InputSignature::find (12 entries)", 5000000u, [&](uint32_t i) {
    dxbc_spv::sm3::Semantic s = {};
    s.usage = dxbc_spv::sm3::SemanticUsage(i % 14u);
    s.index = (i >> 4) & 1u;
    keep(sig.find(s)); });
  return 0;
}
