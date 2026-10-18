ps_3_0
; c0, after ~10k dependent ALU iterations (a long fragment phase); the
; loop's value is floor(frac(x)) = 0, which the compiler can't prove
defi i0, 255, 0, 1, 0
defi i1, 40, 0, 1, 0
def c1, 1.37, 0.11, 0, 0
dcl_texcoord0 v0
mov r1, v0.x
rep i1
  loop aL, i0
    mad r1.x, r1.x, c1.x, c1.y
    frc r1.x, r1.x
  endloop
endrep
frc r2.x, r1.x
add r2.x, r1.x, -r2.x
add r0, c0, r2.x
mov oC0, r0
