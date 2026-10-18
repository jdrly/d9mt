vs_3_0
; position and uv through; texcoord.zw from c5 (shadow compare reference)
dcl_position v0
dcl_texcoord0 v1
dcl_position o0
dcl_texcoord0 o1
mov o0, v0
mov o1.xy, v1
mov o1.zw, c5
