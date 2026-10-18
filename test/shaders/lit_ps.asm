ps_3_0
; bench_4k main pass: c0 times the shadow map s1 (depth compare at texcoord0.z)
; plus a quarter of the albedo s0
def c2, 0.25, 0, 0, 0
dcl_texcoord0 v0
dcl_2d s0
dcl_2d s1
texld r0, v0, s1
texld r1, v0, s0
mul r0, r0, c0
mad oC0, r1, c2.x, r0
