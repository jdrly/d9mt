/* SM3 control flow the dxbc-spirv pin c5c1a5b9 got wrong (fixed upstream by dcd27e1 and
 * ff6673c, taken with the pin 64f078d6). Left quad: a rep loop that breaks at once, with
 * an unreachable inner loop; the CFG cleanup asserted removing it, which aborted the
 * process (built with asserts compiled out it looped forever). Right quad: r5 is used only
 * in subroutine l0, which stores it conditionally: the first of two calls stores it, the
 * second only reads it. D3D9 temps are global, so it keeps the first call's value; the old
 * pin treated r5 as local to l0 and the second call read undef. */
#include "t_xfer.h"

/* mov r0, c0.yyxw; rep i0 { break; rep i0 { add r0, r0, c0.z } }; mov oC0, r0 */
static const DWORD unreachable_ps[] = {
  0xffff0300, 0x05000051, 0xa00f0000, 0x00000000, 0x3f800000, 0x3f000000,
  0x3f800000, 0x05000030, 0xf00f0000, 0x00000004, 0x00000000, 0x00000001,
  0x00000000, 0x02000001, 0x800f0000, 0xa0c50000, 0x01000026, 0xf0e40000,
  0x0000002c, 0x01000026, 0xf0e40000, 0x03000002, 0x800f0000, 0x80e40000,
  0xa0aa0000, 0x00000027, 0x00000027, 0x02000001, 0x800f0800, 0x80e40000,
  0x0000ffff};

/* c1 = (1, 0, 1, 1); mov r2, c0.x; mov r1, c0.x; loop aL, i0(2) { add r2, r2, c0.y; call l0 };
 * mov oC0, r1; ret; label l0: if_lt r2.x, c0.z(1.5) { mov r5, c1 }; mov r1, r5; ret */
static const DWORD subroutine_ps[] = {
  0xffff0300, 0x05000051, 0xa00f0000, 0x00000000, 0x3f800000, 0x3fc00000,
  0x3e800000, 0x05000051, 0xa00f0001, 0x3f800000, 0x00000000, 0x3f800000,
  0x3f800000, 0x05000030, 0xf00f0000, 0x00000002, 0x00000000, 0x00000001,
  0x00000000, 0x02000001, 0x800f0002, 0xa0000000, 0x02000001, 0x800f0001,
  0xa0000000, 0x0200001b, 0xf0e40800, 0xf0e40000, 0x03000002, 0x800f0002,
  0x80e40002, 0xa0550000, 0x01000019, 0xa0e41000, 0x0000001d, 0x02000001,
  0x800f0800, 0x80e40001, 0x0000001c, 0x0100001e, 0xa0e41000, 0x02040029,
  0x80000002, 0xa0aa0000, 0x02000001, 0x800f0005, 0xa0e40001, 0x0000002b,
  0x02000001, 0x800f0001, 0x80e40005, 0x0000001c, 0x0000ffff};

static IDirect3DPixelShader9 *ps[2];

static void frame(int n) {
  IDirect3DDevice9_SetRenderTarget(x_dev, 0, x_backBuffer);
  IDirect3DDevice9_Clear(x_dev, 0, NULL, D3DCLEAR_TARGET, 0xff202020u, 1.0f, 0);
  IDirect3DDevice9_BeginScene(x_dev);
  IDirect3DDevice9_SetPixelShader(x_dev, ps[0]);
  x_draw(NULL, -0.9f, -0.5f, -0.1f, 0.5f);
  IDirect3DDevice9_SetPixelShader(x_dev, ps[1]);
  x_draw(NULL, 0.1f, -0.5f, 0.9f, 0.5f);
  IDirect3DDevice9_EndScene(x_dev);
  if (n == 1) {
    XReadback r = x_read(x_backBuffer);
    x_expect("unreachable inner loop", &r, 160, 240, 0xffffff00u, 0x00ffffffu);
    x_expect("subroutine temp kept between calls", &r, 480, 240, 0xffff00ffu, 0x00ffffffu);
    x_done(&r);
    x_report("code_sm3_cfg");
  }
}

int main(void) {
  x_init("d9mt_sm3_cfg");
  IDirect3DDevice9_SetRenderState(x_dev, D3DRS_ALPHABLENDENABLE, FALSE);
  X_CHECK(IDirect3DDevice9_CreatePixelShader(x_dev, unreachable_ps, &ps[0]));
  X_CHECK(IDirect3DDevice9_CreatePixelShader(x_dev, subroutine_ps, &ps[1]));
  return x_loop(frame);
}
