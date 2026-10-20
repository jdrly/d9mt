/* Draws into A4R4G4B4 / X4R4G4B4 render targets (#20). ABGR4Unorm stores
 * D3D9's alpha nibble in Metal's red, red in green, green in blue and blue in
 * alpha; sampling undoes that with a swizzle (PR #2), but a render target
 * view can't swizzle, so a draw has to write its outputs, write mask and
 * blend in the stored order. The texel set of t_4444.h (as A8R8G8B8) is
 * drawn with the texquad shaders and with fixed function, with the colour
 * write masks alpha and red|blue, blended with SRCALPHA/INVSRCALPHA (source
 * alpha 0 or 15, blue the opposite: a blend that reads blue as alpha flips)
 * and with DESTALPHA/ZERO onto destinations of alpha 0 and 15 (blue the
 * opposite), and the plain draw is sampled back. Skipped where the device
 * doesn't render to the format. The checks run in the first frame. */
#include "t_4444.h"

enum { DRAW, DRAW_FF, MASK_A, MASK_RB, BLEND, DST_A0, DST_A1, NUM_RT };

static IDirect3DTexture9 *tSrc, *tBlendSrc, *tRt[NUM_RT], *tX, *tBack;
static IDirect3DSurface9 *sRt[NUM_RT], *sX, *sBack;
static BOOL hasRt, hasRtX;
static DWORD wRaw[N * N], wMaskA[N * N], wMaskRb[N * N], wBlend[N * N], wZero[N * N];

static void clear(IDirect3DSurface9 *s, D3DCOLOR c) {
  IDirect3DDevice9_SetRenderTarget(x_dev, 0, s);
  IDirect3DDevice9_Clear(x_dev, 0, NULL, D3DCLEAR_TARGET, c, 1.0f, 0);
}

static void frame(int n) {
  if (hasRt) {
    shader_draw(sRt[DRAW], (IDirect3DBaseTexture9 *)tSrc);
    IDirect3DDevice9_SetRenderTarget(x_dev, 0, sRt[DRAW_FF]);
    ff_draw((IDirect3DBaseTexture9 *)tSrc);

    clear(sRt[MASK_A], 0);
    IDirect3DDevice9_SetRenderState(x_dev, D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_ALPHA);
    x_draw((IDirect3DBaseTexture9 *)tSrc, -1.0f, -1.0f, 1.0f, 1.0f);
    IDirect3DDevice9_SetRenderState(x_dev, D3DRS_COLORWRITEENABLE, 0xf);
    clear(sRt[MASK_RB], 0);
    IDirect3DDevice9_SetRenderState(x_dev, D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_BLUE);
    x_draw((IDirect3DBaseTexture9 *)tSrc, -1.0f, -1.0f, 1.0f, 1.0f);
    IDirect3DDevice9_SetRenderState(x_dev, D3DRS_COLORWRITEENABLE, 0xf);

    clear(sRt[BLEND], 0x5599cc33u);
    IDirect3DDevice9_SetRenderState(x_dev, D3DRS_ALPHABLENDENABLE, TRUE);
    IDirect3DDevice9_SetRenderState(x_dev, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    IDirect3DDevice9_SetRenderState(x_dev, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    x_draw((IDirect3DBaseTexture9 *)tBlendSrc, -1.0f, -1.0f, 1.0f, 1.0f);
    IDirect3DDevice9_SetRenderState(x_dev, D3DRS_SRCBLEND, D3DBLEND_DESTALPHA);
    IDirect3DDevice9_SetRenderState(x_dev, D3DRS_DESTBLEND, D3DBLEND_ZERO);
    clear(sRt[DST_A0], 0x00ffffffu);
    x_draw((IDirect3DBaseTexture9 *)tSrc, -1.0f, -1.0f, 1.0f, 1.0f);
    clear(sRt[DST_A1], 0xff000000u);
    x_draw((IDirect3DBaseTexture9 *)tSrc, -1.0f, -1.0f, 1.0f, 1.0f);
    IDirect3DDevice9_SetRenderState(x_dev, D3DRS_ALPHABLENDENABLE, FALSE);

    shader_draw(sBack, (IDirect3DBaseTexture9 *)tRt[DRAW]);
  }
  if (hasRtX)
    shader_draw(sX, (IDirect3DBaseTexture9 *)tSrc);

  IDirect3DDevice9_SetRenderTarget(x_dev, 0, x_backBuffer);
  IDirect3DDevice9_Clear(x_dev, 0, NULL, D3DCLEAR_TARGET, 0xff202030u, 1.0f, 0);
  for (int i = 0; hasRt && i < NUM_RT; i++) {
    const float x0 = -0.95f + (i % 4) * 0.48f, y0 = 0.3f - (i / 4) * 0.8f;
    x_draw((IDirect3DBaseTexture9 *)tRt[i], x0, y0, x0 + 0.44f, y0 + 0.44f);
  }
  if (hasRtX)
    x_draw((IDirect3DBaseTexture9 *)tX, 0.49f, -0.5f, 0.93f, -0.06f);
  if (n != 1)
    return;

  if (hasRt) {
    check_grid("draw into A4R4G4B4 (shader)", sRt[DRAW], S / N, wRaw, 0xffff);
    check_grid("draw into A4R4G4B4 (fixed function)", sRt[DRAW_FF], S / N, wRaw, 0xffff);
    check_grid("draw into A4R4G4B4, write mask alpha", sRt[MASK_A], S / N, wMaskA, 0xffff);
    check_grid("draw into A4R4G4B4, write mask red|blue", sRt[MASK_RB], S / N, wMaskRb, 0xffff);
    check_grid("draw into A4R4G4B4, SRCALPHA/INVSRCALPHA", sRt[BLEND], S / N, wBlend, 0xffff);
    check_grid("draw into A4R4G4B4, DESTALPHA/ZERO (dest alpha 0)", sRt[DST_A0], S / N, wZero, 0xffff);
    check_grid("draw into A4R4G4B4, DESTALPHA/ZERO (dest alpha 15)", sRt[DST_A1], S / N, wRaw, 0xffff);
    check_grid("sample a drawn A4R4G4B4 render target", sBack, S / N, kTex8, 0xffffffffu);
  } else {
    printf("A4R4G4B4 render-target textures not supported, skipped\n");
  }
  if (hasRtX)
    check_grid("draw into X4R4G4B4", sX, S / N, wRaw, 0x0fff);
  else
    printf("X4R4G4B4 render-target textures not supported, skipped\n");
  x_report("code_4444_rt");
}

int main(void) {
  IDirect3D9 *d3d = t4_init("d9mt_4444_rt");
  hasRt = SUCCEEDED(IDirect3D9_CheckDeviceFormat(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8,
                                                 D3DUSAGE_RENDERTARGET, D3DRTYPE_TEXTURE, D3DFMT_A4R4G4B4));
  hasRtX = SUCCEEDED(IDirect3D9_CheckDeviceFormat(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8,
                                                  D3DUSAGE_RENDERTARGET, D3DRTYPE_TEXTURE, D3DFMT_X4R4G4B4));
  static DWORD blend8[N * N];
  for (int i = 0; i < N * N; i++) {
    const WORD b = (i & 1) ? (WORD)(0xf000 | (kTex[i] & 0x0ff0)) : (WORD)(0x000f | (kTex[i] & 0x0ff0));
    blend8[i] = expand(b);
    wBlend[i] = (b & 0xf000) ? b : 0x59c3;  /* the source where its alpha is 15, else the cleared 0x5599cc33 */
    wRaw[i] = kTex[i];
    wMaskA[i] = kTex[i] & 0xf000;
    wMaskRb[i] = kTex[i] & 0x0f0f;
    wZero[i] = 0;
  }
  tSrc = mk(N, D3DFMT_A8R8G8B8, 0, D3DPOOL_MANAGED);
  fill32(tSrc, kTex8);
  tBlendSrc = mk(N, D3DFMT_A8R8G8B8, 0, D3DPOOL_MANAGED);
  fill32(tBlendSrc, blend8);
  if (hasRt) {
    for (int i = 0; i < NUM_RT; i++) {
      tRt[i] = mk(S, D3DFMT_A4R4G4B4, D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT);
      sRt[i] = x_level(tRt[i], 0);
    }
    tBack = mk(S, D3DFMT_A8R8G8B8, D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT);
    sBack = x_level(tBack, 0);
  }
  if (hasRtX) {
    tX = mk(S, D3DFMT_X4R4G4B4, D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT);
    sX = x_level(tX, 0);
  }
  return x_loop(frame);
}
