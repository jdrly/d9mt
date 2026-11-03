// Apple's side of the AIR built-in forms contract (scripts/test-air-forms.sh): each group makes
// Apple's compiler (xcrun metal -mmacosx-version-min=<target>.0 -O2 -S -emit-llvm) declare the
// forms inventory.txt names for it. Groups compile one at a time (this header + the group), so a
// group Apple rejects fails only its own forms. Every result reaches the output so -O2 keeps it.
#include <metal_stdlib>
using namespace metal;

struct In {
  float4 v [[user(locn0)]];
};

// group: math
fragment float4 g_math(In i [[stage_in]]) {
  float x = i.v.x;
  return float4(fast::sin(x) + fast::cos(x) + fast::exp2(x) + fast::log2(x) + fast::rsqrt(x)
                + fast::fract(x) + fast::pow(x, x) + sign(x));
}

// group: int
fragment float4 g_int(In i [[stage_in]]) {
  int a = int(i.v.x), b = int(i.v.y);
  uint c = uint(i.v.z), d = uint(i.v.w);
  return float4(float(abs(a)), float(max(a, b)), float(min(a, b)), float(max(c, d)) + float(min(c, d)));
}

// group: deriv
fragment float4 g_deriv(In i [[stage_in]]) {
  float s = dfdx(i.v.x) + dfdy(i.v.x) + dfdx(i.v.xy).x + dfdy(i.v.xy).x
          + dfdx(i.v.xyz).x + dfdy(i.v.xyz).x + dfdx(i.v).x + dfdy(i.v).x;
  return float4(s);
}

// group: discard
fragment float4 g_discard(In i [[stage_in]]) {
  if (i.v.x < 0.0f)
    discard_fragment();
  return i.v;
}

// group: sample_2d
fragment float4 g_sample_2d(In i [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {
  return t.sample(s, i.v.xy) + t.sample(s, i.v.xy, bias(i.v.w)) + t.sample(s, i.v.xy, level(i.v.w));
}

// group: sample_cube
fragment float4 g_sample_cube(In i [[stage_in]], texturecube<float> t [[texture(0)]], sampler s [[sampler(0)]]) {
  return t.sample(s, i.v.xyz) + t.sample(s, i.v.xyz, bias(i.v.w)) + t.sample(s, i.v.xyz, level(i.v.w));
}

// group: sample_3d
fragment float4 g_sample_3d(In i [[stage_in]], texture3d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {
  return t.sample(s, i.v.xyz) + t.sample(s, i.v.xyz, bias(i.v.w)) + t.sample(s, i.v.xyz, level(i.v.w));
}

// group: sample_2d_grad
fragment float4 g_sample_2d_grad(In i [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {
  return t.sample(s, i.v.xy, gradient2d(i.v.xy, i.v.zw));
}

// group: sample_cube_grad
fragment float4 g_sample_cube_grad(In i [[stage_in]], texturecube<float> t [[texture(0)]], sampler s [[sampler(0)]]) {
  return t.sample(s, i.v.xyz, gradientcube(i.v.xyz, i.v.yzw));
}

// group: sample_3d_grad
fragment float4 g_sample_3d_grad(In i [[stage_in]], texture3d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {
  return t.sample(s, i.v.xyz, gradient3d(i.v.xyz, i.v.yzw));
}

// group: compare_2d
fragment float4 g_compare_2d(In i [[stage_in]], depth2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {
  return float4(t.sample_compare(s, i.v.xy, i.v.z) + t.sample_compare(s, i.v.xy, i.v.z, level(i.v.w)));
}

// group: compare_cube
fragment float4 g_compare_cube(In i [[stage_in]], depthcube<float> t [[texture(0)]], sampler s [[sampler(0)]]) {
  return float4(t.sample_compare(s, i.v.xyz, i.v.w) + t.sample_compare(s, i.v.xyz, i.v.w, level(i.v.x)));
}

// group: compare_2d_grad
fragment float4 g_compare_2d_grad(In i [[stage_in]], depth2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {
  return float4(t.sample_compare(s, i.v.xy, i.v.z, gradient2d(i.v.xy, i.v.zw)));
}

// group: compare_cube_grad
fragment float4 g_compare_cube_grad(In i [[stage_in]], depthcube<float> t [[texture(0)]], sampler s [[sampler(0)]]) {
  return float4(t.sample_compare(s, i.v.xyz, i.v.w, gradientcube(i.v.xyz, i.v.yzw)));
}

// group: gather_2d
fragment float4 g_gather_2d(In i [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {
  return t.gather(s, i.v.xy, int2(0), component::y);
}

// group: gather_cube
fragment float4 g_gather_cube(In i [[stage_in]], texturecube<float> t [[texture(0)]], sampler s [[sampler(0)]]) {
  return t.gather(s, i.v.xyz, component::y);
}

// group: query_2d
fragment float4 g_query_2d(In i [[stage_in]], texture2d<float> t [[texture(0)]]) {
  uint l = uint(i.v.x);
  return float4(float(t.get_width(l)), float(t.get_height(l)), float(t.get_num_mip_levels()), 0.0f);
}

// group: query_cube
fragment float4 g_query_cube(In i [[stage_in]], texturecube<float> t [[texture(0)]]) {
  uint l = uint(i.v.x);
  return float4(float(t.get_width(l)), float(t.get_height(l)), float(t.get_num_mip_levels()), 0.0f);
}

// group: query_3d
fragment float4 g_query_3d(In i [[stage_in]], texture3d<float> t [[texture(0)]]) {
  uint l = uint(i.v.x);
  return float4(float(t.get_width(l)), float(t.get_height(l)), float(t.get_depth(l)), float(t.get_num_mip_levels()));
}
