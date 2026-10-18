vs_3_0
; vertex texture fetch: x offset = s0 (R32F) at its centre, level 0
dcl_position v0
dcl_texcoord0 v1
dcl_2d s0
dcl_position o0
dcl_texcoord0 o1
def c4, 0.5, 0.5, 0, 0
texldl r0, c4, s0
mov r1, v0
add r1.x, v0.x, r0.x
mov o0, r1
mov o1.xy, v1
mov o1.zw, c5
