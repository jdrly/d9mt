#version 450
#extension GL_GOOGLE_include_directive : enable

// D3DSHADE_FLAT: the fixed-function fragment shader with flat colour inputs
// (in_Color0, in_Color1), as d9mt's ffFsMainFlat. Chosen per draw by the
// draw's effective flat (D3D9DeviceEx::EffectiveFlatShading).
#define FF_FLAT
#include "d3d9_fixed_function_frag.glsl"
