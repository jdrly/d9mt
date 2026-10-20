/* A4R4G4B4 / X4R4G4B4 textures and transfers (the ABGR4Unorm channel
 * rotation of neo773/d9mt-v2 PR #2, 4c62ec4 here). The texel set of t_4444.h
 * goes through
 *   - sampling, with the texquad shaders and with fixed function, from a
 *     managed texture (LockRect), a dynamic one, one filled by UpdateTexture,
 *     a render-target texture filled by UpdateSurface, and one partly
 *     rewritten through a sub-rect LockRect; X4R4G4B4 samples alpha 1 (its
 *     X nibble holds the alpha gradient);
 *   - GetRenderTargetData, and LockRect of a lockable render target: the raw
 *     16-bit texels come back unchanged;
 *   - StretchRect from 4444 to A8R8G8B8, to 4444 scaled x2 (blit path), to
 *     X4R4G4B4 (copy), from X4R4G4B4 to A8R8G8B8 (alpha 1), and from
 *     A8R8G8B8 to 4444;
 *   - Clear and ColorFill of 4444 render targets.
 * Draws into 4444 render targets are code_4444_rt (#20). The checks run in
 * the first frame; the backbuffer shows the sampled results. */
#include "t_4444.h"

static IDirect3DTexture9 *tMan, *tX, *tUpd, *tDyn, *tRtUp, *tArgbUp, *tSub;
static IDirect3DTexture9 *tOut[8];
static IDirect3DSurface9 *sOut[8];
static IDirect3DSurface9 *sLockRt, *s8to4, *s4to8, *s4to16, *s4toX, *sXto8, *sClr, *sFill, *sClrX;
static BOOL hasRtX;
static DWORD w8[N * N], wRaw[N * N], wX8[N * N], wSub[N * N], wClr[N * N], wFill[N * N];

static void frame(int n) {
  shader_draw(sOut[0], (IDirect3DBaseTexture9 *)tMan);
  IDirect3DDevice9_SetRenderTarget(x_dev, 0, sOut[1]);
  ff_draw((IDirect3DBaseTexture9 *)tMan);
  shader_draw(sOut[2], (IDirect3DBaseTexture9 *)tX);
  IDirect3DDevice9_SetRenderTarget(x_dev, 0, sOut[3]);
  ff_draw((IDirect3DBaseTexture9 *)tX);
  shader_draw(sOut[4], (IDirect3DBaseTexture9 *)tUpd);
  shader_draw(sOut[5], (IDirect3DBaseTexture9 *)tDyn);
  shader_draw(sOut[6], (IDirect3DBaseTexture9 *)tRtUp);
  shader_draw(sOut[7], (IDirect3DBaseTexture9 *)tSub);

  IDirect3DSurface9 *rtUp = x_level(tRtUp, 0), *argbUp = x_level(tArgbUp, 0);
  X_CHECK(IDirect3DDevice9_StretchRect(x_dev, rtUp, NULL, s4to8, NULL, D3DTEXF_POINT));
  X_CHECK(IDirect3DDevice9_StretchRect(x_dev, rtUp, NULL, s4to16, NULL, D3DTEXF_POINT));
  X_CHECK(IDirect3DDevice9_StretchRect(x_dev, rtUp, NULL, s4toX, NULL, D3DTEXF_NONE));
  X_CHECK(IDirect3DDevice9_StretchRect(x_dev, s4toX, NULL, sXto8, NULL, D3DTEXF_POINT));
  X_CHECK(IDirect3DDevice9_StretchRect(x_dev, argbUp, NULL, s8to4, NULL, D3DTEXF_POINT));
  IDirect3DSurface9_Release(argbUp);

  IDirect3DDevice9_SetRenderTarget(x_dev, 0, sClr);
  IDirect3DDevice9_Clear(x_dev, 0, NULL, D3DCLEAR_TARGET, 0x88ccddeeu, 1.0f, 0);
  X_CHECK(IDirect3DDevice9_ColorFill(x_dev, sFill, NULL, 0x33669911u));
  if (hasRtX) {
    IDirect3DDevice9_SetRenderTarget(x_dev, 0, sClrX);
    IDirect3DDevice9_Clear(x_dev, 0, NULL, D3DCLEAR_TARGET, 0x88ccddeeu, 1.0f, 0);
  }

  IDirect3DDevice9_SetRenderTarget(x_dev, 0, x_backBuffer);
  IDirect3DDevice9_Clear(x_dev, 0, NULL, D3DCLEAR_TARGET, 0xff202030u, 1.0f, 0);
  for (int i = 0; i < 8; i++) {
    const float x0 = -0.95f + (i % 4) * 0.48f, y0 = 0.3f - (i / 4) * 0.8f;
    x_draw((IDirect3DBaseTexture9 *)tOut[i], x0, y0, x0 + 0.44f, y0 + 0.44f);
  }
  if (n != 1) {
    IDirect3DSurface9_Release(rtUp);
    return;
  }

  static const char *const kSample[8] = {
      "sample A4R4G4B4 managed (shader)", "sample A4R4G4B4 managed (fixed function)",
      "sample X4R4G4B4 managed (shader)", "sample X4R4G4B4 managed (fixed function)",
      "sample A4R4G4B4 UpdateTexture", "sample A4R4G4B4 dynamic",
      "sample A4R4G4B4 render-target texture (UpdateSurface)", "sample A4R4G4B4 after a sub-rect LockRect"};
  const DWORD *kWant[8] = {w8, w8, wX8, wX8, w8, w8, w8, wSub};
  for (int i = 0; i < 8; i++)
    check_grid(kSample[i], sOut[i], S / N, kWant[i], 0xffffffffu);
  check_grid("GetRenderTargetData A4R4G4B4", rtUp, 1, wRaw, 0xffff);
  check_grid("LockRect of a lockable A4R4G4B4 render target", sLockRt, 1, wRaw, 0xffff);
  check_grid("StretchRect A4R4G4B4 -> A8R8G8B8", s4to8, 1, w8, 0xffffffffu);
  check_grid("StretchRect A4R4G4B4 -> A4R4G4B4 x2 (blit)", s4to16, 2, wRaw, 0xffff);
  check_grid("StretchRect A4R4G4B4 -> X4R4G4B4 (copy)", s4toX, 1, wRaw, 0x0fff);
  check_grid("StretchRect X4R4G4B4 -> A8R8G8B8", sXto8, 1, wX8, 0xffffffffu);
  check_grid("StretchRect A8R8G8B8 -> A4R4G4B4", s8to4, 1, wRaw, 0xffff);
  check_grid("Clear A4R4G4B4", sClr, 1, wClr, 0xffff);
  check_grid("ColorFill A4R4G4B4", sFill, 1, wFill, 0xffff);
  if (hasRtX)
    check_grid("Clear X4R4G4B4", sClrX, 1, wClr, 0x0fff);
  IDirect3DSurface9_Release(rtUp);
  x_report("code_4444");
}

int main(void) {
  IDirect3D9 *d3d = t4_init("d9mt_4444");
  hasRtX = SUCCEEDED(IDirect3D9_CheckDeviceFormat(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8,
                                                  D3DUSAGE_RENDERTARGET, D3DRTYPE_SURFACE, D3DFMT_X4R4G4B4));
  for (int i = 0; i < N * N; i++) {
    w8[i] = kTex8[i];
    wRaw[i] = kTex[i];
    wX8[i] = kTex8[i] | 0xff000000u;
    wSub[i] = kTex8[i] & 0x00ff00ffu;  /* the sub-rect texture holds red and blue only... */
    wClr[i] = 0x8cde;                  /* 0x88ccddee */
    wFill[i] = 0x3691;                 /* 0x33669911 */
  }

  tMan = mk(N, D3DFMT_A4R4G4B4, 0, D3DPOOL_MANAGED);
  fill16(tMan, kTex);
  tX = mk(N, D3DFMT_X4R4G4B4, 0, D3DPOOL_MANAGED);
  fill16(tX, kTex);
  IDirect3DTexture9 *sys = mk(N, D3DFMT_A4R4G4B4, 0, D3DPOOL_SYSTEMMEM);
  fill16(sys, kTex);
  tUpd = mk(N, D3DFMT_A4R4G4B4, 0, D3DPOOL_DEFAULT);
  X_CHECK(IDirect3DDevice9_UpdateTexture(x_dev, (IDirect3DBaseTexture9 *)sys, (IDirect3DBaseTexture9 *)tUpd));
  tDyn = mk(N, D3DFMT_A4R4G4B4, D3DUSAGE_DYNAMIC, D3DPOOL_DEFAULT);
  fill16(tDyn, kTex);

  /* ...except texels 1..2 x 1..2, rewritten through a sub-rect lock. */
  static WORD rb[N * N];
  for (int i = 0; i < N * N; i++)
    rb[i] = (WORD)(kTex[i] & 0x0f0f);
  tSub = mk(N, D3DFMT_A4R4G4B4, 0, D3DPOOL_MANAGED);
  fill16(tSub, rb);
  D3DLOCKED_RECT lr;
  RECT sub = {1, 1, 3, 3};
  X_CHECK(IDirect3DTexture9_LockRect(tSub, 0, &lr, &sub, 0));
  for (int y = 0; y < 2; y++)
    for (int x = 0; x < 2; x++) {
      const WORD v = (WORD)(0xa009 | (x << 8) | (y << 4));
      ((WORD *)((BYTE *)lr.pBits + y * lr.Pitch))[x] = v;
      wSub[(1 + y) * N + 1 + x] = expand(v);
    }
  IDirect3DTexture9_UnlockRect(tSub, 0);

  tRtUp = mk(N, D3DFMT_A4R4G4B4, D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT);
  IDirect3DSurface9 *sysS = NULL, *rtUp = x_level(tRtUp, 0);
  X_CHECK(IDirect3DDevice9_CreateOffscreenPlainSurface(x_dev, N, N, D3DFMT_A4R4G4B4, D3DPOOL_SYSTEMMEM, &sysS, NULL));
  X_CHECK(IDirect3DSurface9_LockRect(sysS, &lr, NULL, 0));
  for (int y = 0; y < N; y++)
    memcpy((BYTE *)lr.pBits + y * lr.Pitch, kTex + y * N, N * 2);
  IDirect3DSurface9_UnlockRect(sysS);
  X_CHECK(IDirect3DDevice9_UpdateSurface(x_dev, sysS, NULL, rtUp, NULL));
  IDirect3DSurface9_Release(rtUp);

  X_CHECK(IDirect3DDevice9_CreateRenderTarget(x_dev, N, N, D3DFMT_A4R4G4B4, D3DMULTISAMPLE_NONE, 0, TRUE, &sLockRt, NULL));
  X_CHECK(IDirect3DSurface9_LockRect(sLockRt, &lr, NULL, 0));
  for (int y = 0; y < N; y++)
    memcpy((BYTE *)lr.pBits + y * lr.Pitch, kTex + y * N, N * 2);
  IDirect3DSurface9_UnlockRect(sLockRt);

  IDirect3DTexture9 *sys8 = mk(N, D3DFMT_A8R8G8B8, 0, D3DPOOL_SYSTEMMEM);
  fill32(sys8, kTex8);
  tArgbUp = mk(N, D3DFMT_A8R8G8B8, D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT);
  X_CHECK(IDirect3DDevice9_UpdateTexture(x_dev, (IDirect3DBaseTexture9 *)sys8, (IDirect3DBaseTexture9 *)tArgbUp));

  for (int i = 0; i < 8; i++) {
    tOut[i] = mk(S, D3DFMT_A8R8G8B8, D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT);
    sOut[i] = x_level(tOut[i], 0);
  }
  X_CHECK(IDirect3DDevice9_CreateRenderTarget(x_dev, N, N, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &s4to8, NULL));
  X_CHECK(IDirect3DDevice9_CreateRenderTarget(x_dev, N, N, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &sXto8, NULL));
  X_CHECK(IDirect3DDevice9_CreateRenderTarget(x_dev, 2 * N, 2 * N, D3DFMT_A4R4G4B4, D3DMULTISAMPLE_NONE, 0, FALSE, &s4to16, NULL));
  X_CHECK(IDirect3DDevice9_CreateRenderTarget(x_dev, N, N, D3DFMT_X4R4G4B4, D3DMULTISAMPLE_NONE, 0, FALSE, &s4toX, NULL));
  X_CHECK(IDirect3DDevice9_CreateRenderTarget(x_dev, N, N, D3DFMT_A4R4G4B4, D3DMULTISAMPLE_NONE, 0, FALSE, &s8to4, NULL));
  X_CHECK(IDirect3DDevice9_CreateRenderTarget(x_dev, N, N, D3DFMT_A4R4G4B4, D3DMULTISAMPLE_NONE, 0, FALSE, &sClr, NULL));
  X_CHECK(IDirect3DDevice9_CreateRenderTarget(x_dev, N, N, D3DFMT_A4R4G4B4, D3DMULTISAMPLE_NONE, 0, FALSE, &sFill, NULL));
  if (hasRtX)
    X_CHECK(IDirect3DDevice9_CreateRenderTarget(x_dev, N, N, D3DFMT_X4R4G4B4, D3DMULTISAMPLE_NONE, 0, FALSE, &sClrX, NULL));
  return x_loop(frame);
}
