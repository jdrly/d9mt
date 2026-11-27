#pragma once

// The sampler LOD bias that reaches Metal (MTLSamplerDescriptor.lodBias): the sampler key's bias,
// D3DSAMP_MIPMAPLODBIAS plus d3d9.samplerLodBias. Only Apple10 GPUs (M5) apply a sampler LOD
// bias, so on older GPUs every sampler gets 0, as before, and the descriptor's bias is never
// set. D9MT_SAMPLER_LOD_BIAS=0 turns it off on Apple10 too (docs/configuration.md).
//
// Header-only and dependency-free for the native test (test/native/sampler_lod_bias_test.cpp).
// MtlDeviceImpl picks the mode once per device and logs its line.

#include <cmath>
#include <cstdint>
#include <cstring>

namespace dxvk {

  // Metal's lodBias is S4.6 fixed point: [-16, 15.984375] in steps of 1/64.
  constexpr float kMtlLodBiasMin = -16.0f;
  constexpr float kMtlLodBiasMax = 1023.0f / 64.0f;

  enum class SamplerLodBiasMode : uint32_t { On, OffGpu, OffEnv };

  // From the GPU family and D9MT_SAMPLER_LOD_BIAS: unset, empty or `1`, the GPU decides; `0`, off.
  // Any other value is the GPU's default with `understood` false. Nothing turns it on before Apple10.
  inline SamplerLodBiasMode samplerLodBiasMode(bool apple10, const char* env, bool& understood) {
    const bool set = env != nullptr && env[0] != '\0';
    understood = !set || !std::strcmp(env, "0") || !std::strcmp(env, "1");
    if (set && !std::strcmp(env, "0"))
      return SamplerLodBiasMode::OffEnv;
    return apple10 ? SamplerLodBiasMode::On : SamplerLodBiasMode::OffGpu;
  }

  // The bias a sampler gets: 0 when off, and for NaN; otherwise the key's, clamped to Metal's
  // range and rounded to its 1/64 steps, with -0 as 0.
  inline float metalLodBias(float keyBias, bool on) {
    if (!on || std::isnan(keyBias))
      return 0.0f;
    const float b = std::fmin(std::fmax(keyBias, kMtlLodBiasMin), kMtlLodBiasMax);
    return std::round(b * 64.0f) / 64.0f + 0.0f;
  }

  // The start-up line.
  inline const char* samplerLodBiasLine(SamplerLodBiasMode mode) {
    switch (mode) {
      case SamplerLodBiasMode::On:
        return "D9MT: sampler LOD bias on (Apple10 GPU): D3DSAMP_MIPMAPLODBIAS and d3d9.samplerLodBias apply";
      case SamplerLodBiasMode::OffEnv:
        return "D9MT: sampler LOD bias off (D9MT_SAMPLER_LOD_BIAS=0): D3DSAMP_MIPMAPLODBIAS and d3d9.samplerLodBias have no effect";
      default:
        return "D9MT: sampler LOD bias off (GPU before Apple10): D3DSAMP_MIPMAPLODBIAS and d3d9.samplerLodBias have no effect";
    }
  }

}
