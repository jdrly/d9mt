/* A fixed-function stage whose op reads a texture that isn't there (review of
 * fix/ff-unbound-slots M-1): stage 0 SELECTARG1 TEXTURE (colour and alpha) with no texture
 * bound, and MODULATE TEXTURE, DIFFUSE, on green diffuse quads. With null resources in the
 * unbound slots, the result must not change from base, where the empty slot read as 0.
 * Measured on base (2a1bc00): both quads stay green, i.e. this FF path doesn't use a null
 * stage's texture value for these ops (only D3DTOP_PREMODULATE observes it:
 * port/ff-null-texture's code_ff_null_tex). The test pins that: green, base = fix, with or
 * without that port (which changes the null value to (1, 1, 1, 1)). */
#include "t_xfer.h"

struct V { float x, y, z, rhw; DWORD c; };

static void stage0(DWORD op, DWORD arg1, DWORD arg2) {
  IDirect3DDevice9_SetTextureStageState(x_dev, 0, D3DTSS_COLOROP, op);
  IDirect3DDevice9_SetTextureStageState(x_dev, 0, D3DTSS_COLORARG1, arg1);
  IDirect3DDevice9_SetTextureStageState(x_dev, 0, D3DTSS_COLORARG2, arg2);
  IDirect3DDevice9_SetTextureStageState(x_dev, 0, D3DTSS_ALPHAOP, op);
  IDirect3DDevice9_SetTextureStageState(x_dev, 0, D3DTSS_ALPHAARG1, arg1);
  IDirect3DDevice9_SetTextureStageState(x_dev, 0, D3DTSS_ALPHAARG2, arg2);
}

static void frame(int n) {
  struct V a[4] = {{40, 120, 0.5f, 1, 0xff00ff00u}, {280, 120, 0.5f, 1, 0xff00ff00u},
                   {40, 360, 0.5f, 1, 0xff00ff00u}, {280, 360, 0.5f, 1, 0xff00ff00u}};
  struct V b[4] = {{360, 120, 0.5f, 1, 0xff00ff00u}, {600, 120, 0.5f, 1, 0xff00ff00u},
                   {360, 360, 0.5f, 1, 0xff00ff00u}, {600, 360, 0.5f, 1, 0xff00ff00u}};
  IDirect3DDevice9_SetRenderTarget(x_dev, 0, x_backBuffer);
  IDirect3DDevice9_Clear(x_dev, 0, NULL, D3DCLEAR_TARGET, 0xff000080u, 1.0f, 0);
  IDirect3DDevice9_SetVertexShader(x_dev, NULL);
  IDirect3DDevice9_SetPixelShader(x_dev, NULL);
  IDirect3DDevice9_SetFVF(x_dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
  IDirect3DDevice9_SetTexture(x_dev, 0, NULL);
  IDirect3DDevice9_SetTextureStageState(x_dev, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
  IDirect3DDevice9_BeginScene(x_dev);
  stage0(D3DTOP_SELECTARG1, D3DTA_TEXTURE, D3DTA_DIFFUSE);
  IDirect3DDevice9_DrawPrimitiveUP(x_dev, D3DPT_TRIANGLESTRIP, 2, a, sizeof(a[0]));
  stage0(D3DTOP_MODULATE, D3DTA_TEXTURE, D3DTA_DIFFUSE);
  IDirect3DDevice9_DrawPrimitiveUP(x_dev, D3DPT_TRIANGLESTRIP, 2, b, sizeof(b[0]));
  IDirect3DDevice9_EndScene(x_dev);
  if (n == 1) {
    XReadback r = x_read(x_backBuffer);
    x_expect("SELECTARG1 TEXTURE without a texture", &r, 160, 240, 0xff00ff00u, 0x00ffffffu);
    x_expect("MODULATE TEXTURE, DIFFUSE without a texture", &r, 480, 240, 0xff00ff00u, 0x00ffffffu);
    x_expect("outside the quads", &r, 320, 40, 0xff000080u, 0x00ffffffu);
    x_done(&r);
    x_report("code_ff_null_sample");
  }
}

int main(void) {
  x_init("d9mt_ff_null_sample");
  IDirect3DDevice9_SetRenderState(x_dev, D3DRS_ALPHABLENDENABLE, FALSE);
  IDirect3DDevice9_SetRenderState(x_dev, D3DRS_LIGHTING, FALSE);
  return x_loop(frame);
}
