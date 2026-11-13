#pragma once

#include <string>
#include <vector>

#include "d3d9_shader.h"
#include "d3d9_shader_analysis.h"

namespace dxvk {

  Rc<ID3D9MetalShaderBackend> createD3D9MetalShader(
    const std::string&        name,
          MtlShaderStage      stage,
    const D3D9ShaderOptions&  options,
    const D3D9ShaderAnalysis& analysis,
    const void*               bytecode);

  /// D9MT_DEPTH_BIAS_VS=1 (opt-in; off by default): the constant depth bias
  /// in the vertex shader on float depth formats (#163). Read once per
  /// process and logged once. Off: no vertex shader gets the depth-bias
  /// rewrite, so every shader's IR (and its AIR cache key) is what it is
  /// without this feature.
  bool D3D9DepthBiasVsEnabled();

}
