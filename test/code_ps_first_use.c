/* Pixel shaders at first use (C5, the app profile's psPrefetch and psIrPrefetch). The program
 * creates 16 pixel shaders besides the harness's texquad pair, each writing its own constant
 * colour, and draws with three of them: ps[0] (red, left) and ps[1] (green, right) from frame 1,
 * ps[2] (yellow, bottom) first in frame 3, so its first use comes after the device has drawn.
 * The other 13, and the texquad pixel shader, are never drawn.
 * Every arm of the two fields must draw the same frames. scripts/test-ps-first-use.sh runs this
 * program under each arm with D9MT_PROFILE=1 and counts, from the COMPILE lines, the Phase A runs
 * and the libraries d9mt made: with a field off, only the drawn shaders get them. Frame 4 sleeps
 * 1.5 s before its Present, so the work the arms with a prefetch queue at creation (17 tiny pixel
 * shaders, at lowest priority) is done and counted by that frame's end. */
#include "t_xfer.h"

#define PS_COUNT 16

static IDirect3DPixelShader9 *ps[PS_COUNT];

/* ps_3_0: def c0, r, g, b, 1; mov oC0, c0 */
static void make_ps(DWORD *code, float r, float g, float b) {
  union { float f; DWORD d; } u;
  code[0] = 0xffff0300u;
  code[1] = 0x05000051u; code[2] = 0xa00f0000u;
  u.f = r; code[3] = u.d;
  u.f = g; code[4] = u.d;
  u.f = b; code[5] = u.d;
  u.f = 1.0f; code[6] = u.d;
  code[7] = 0x02000001u; code[8] = 0x800f0800u; code[9] = 0xa0e40000u;
  code[10] = 0x0000ffffu;
}

static void quad(IDirect3DPixelShader9 *p, float x0, float y0, float x1, float y1) {
  IDirect3DDevice9_SetPixelShader(x_dev, p);
  x_draw(NULL, x0, y0, x1, y1);
}

static void frame(int n) {
  IDirect3DDevice9_SetRenderTarget(x_dev, 0, x_backBuffer);
  IDirect3DDevice9_Clear(x_dev, 0, NULL, D3DCLEAR_TARGET, 0xff000080u, 1.0f, 0);
  IDirect3DDevice9_BeginScene(x_dev);
  quad(ps[0], -0.9f, 0.1f, -0.1f, 0.9f);
  quad(ps[1], 0.1f, 0.1f, 0.9f, 0.9f);
  if (n >= 3)
    quad(ps[2], -0.4f, -0.9f, 0.4f, -0.1f);
  IDirect3DDevice9_EndScene(x_dev);
  if (n == 1 || n == 3) {
    XReadback r = x_read(x_backBuffer);
    x_expect("ps[0], drawn from frame 1", &r, 160, 120, 0xffff0000u, 0x00ffffffu);
    x_expect("ps[1], drawn from frame 1", &r, 480, 120, 0xff00ff00u, 0x00ffffffu);
    x_expect(n == 1 ? "no ps[2] before frame 3" : "ps[2], first drawn in frame 3", &r, 320, 360,
             n == 1 ? 0xff000080u : 0xffffff00u, 0x00ffffffu);
    x_done(&r);
  }
  if (n == 3)
    x_report("code_ps_first_use");
  if (n == 4)
    Sleep(1500);
}

int main(void) {
  x_init("d9mt_ps_first_use");
  for (int i = 0; i < PS_COUNT; i++) {
    DWORD code[11];
    if (i == 0)      make_ps(code, 1.0f, 0.0f, 0.0f);
    else if (i == 1) make_ps(code, 0.0f, 1.0f, 0.0f);
    else if (i == 2) make_ps(code, 1.0f, 1.0f, 0.0f);
    else             make_ps(code, (float)i / PS_COUNT, 0.25f, 0.75f);
    X_CHECK(IDirect3DDevice9_CreatePixelShader(x_dev, code, &ps[i]));
  }
  IDirect3DDevice9_SetRenderState(x_dev, D3DRS_ALPHABLENDENABLE, FALSE);
  return x_loop(frame);
}
