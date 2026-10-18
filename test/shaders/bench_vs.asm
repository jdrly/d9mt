vs_3_0
; bench_4k geometry: position v0 + a small offset from 16 dependent
; rotations of v1 (vertex ALU like skinning), uv through, texcoord.zw = c5
defi i0, 16, 0, 1, 0
def c6, 0.9998, 0.0199987, 0.002, 0
dcl_position v0
dcl_texcoord0 v1
dcl_position o0
dcl_texcoord0 o1
mov r0.xy, v1
loop aL, i0
  mul r1.x, r0.x, c6.x
  mad r1.x, r0.y, -c6.y, r1.x
  mul r1.y, r0.x, c6.y
  mad r1.y, r0.y, c6.x, r1.y
  mov r0.xy, r1
endloop
mov r2, v0
mad r2.xy, r0, c6.z, v0
mov o0, r2
mov o1.xy, v1
mov o1.zw, c5
