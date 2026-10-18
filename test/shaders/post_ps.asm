ps_3_0
; bench_4k post pass: four taps of s0 around texcoord0 (offsets c1), times c0
dcl_texcoord0 v0
dcl_2d s0
add r1.xy, v0, c1.xy
texld r0, r1, s0
add r1.xy, v0, -c1.xy
texld r2, r1, s0
add r0, r0, r2
add r1.xy, v0, c1.zw
texld r2, r1, s0
add r0, r0, r2
add r1.xy, v0, -c1.zw
texld r2, r1, s0
add r0, r0, r2
mul oC0, r0, c0
