ps_3_0
; texture s0 at texcoord0 (.xyzw: a depth texture compares against .z)
dcl_texcoord0 v0
dcl_2d s0
texld r0, v0, s0
mov oC0, r0
