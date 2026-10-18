/* sRGB and linear views of the same images (docs/gpu-4k.md, candidate 2:
 * textures without MTLTextureUsagePixelFormatView). A 256x256 A8R8G8B8 render
 * target gets the same colour with SRGBWRITEENABLE on (left half) and off
 * (right half); StretchRect with a filter scales it into a 128x128 target
 * (the blit path, a 2D-array view); the backbuffer samples both with
 * SRGBTEXTURE on and off, plus a managed 2x2 texture, and draws one quad into
 * itself through an sRGB view of the X8R8G8B8 backbuffer. */
#include "t_common.h"

static IDirect3DTexture9 *rt, *rt2, *managed;
static IDirect3DSurface9 *rtSurf, *rt2Surf, *backBuffer;

static void sample(IDirect3DTexture9 *t, BOOL srgb, float x0, float y0, float x1, float y1) {
  IDirect3DDevice9_SetTexture(t_dev, 0, (IDirect3DBaseTexture9 *)t);
  IDirect3DDevice9_SetSamplerState(t_dev, 0, D3DSAMP_SRGBTEXTURE, srgb);
  t_quad(x0, y0, x1, y1, 0.5f);
}

static void frame(int n) {
  (void)n;
  IDirect3DDevice9_SetTexture(t_dev, 0, NULL);
  IDirect3DDevice9_SetRenderTarget(t_dev, 0, rtSurf);
  IDirect3DDevice9_SetDepthStencilSurface(t_dev, NULL);
  IDirect3DDevice9_SetRenderState(t_dev, D3DRS_ZENABLE, D3DZB_FALSE);
  IDirect3DDevice9_Clear(t_dev, 0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
  IDirect3DDevice9_BeginScene(t_dev);
  IDirect3DDevice9_SetVertexShader(t_dev, t_vs_pos);
  IDirect3DDevice9_SetPixelShader(t_dev, t_ps_color);
  t_color(0.5f, 0.25f, 0.75f, 1.0f);
  IDirect3DDevice9_SetRenderState(t_dev, D3DRS_SRGBWRITEENABLE, TRUE);
  t_quad(-1.0f, -1.0f, 0.0f, 1.0f, 0.5f);
  IDirect3DDevice9_SetRenderState(t_dev, D3DRS_SRGBWRITEENABLE, FALSE);
  t_quad(0.0f, -1.0f, 1.0f, 1.0f, 0.5f);
  IDirect3DDevice9_EndScene(t_dev);

  T_CHECK(IDirect3DDevice9_StretchRect(t_dev, rtSurf, NULL, rt2Surf, NULL, D3DTEXF_LINEAR));

  IDirect3DDevice9_SetRenderTarget(t_dev, 0, backBuffer);
  IDirect3DDevice9_Clear(t_dev, 0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(24, 24, 40), 1.0f, 0);
  IDirect3DDevice9_BeginScene(t_dev);
  IDirect3DDevice9_SetPixelShader(t_dev, t_ps_sample);
  sample(rt, TRUE, -0.95f, 0.05f, -0.05f, 0.95f);
  sample(rt, FALSE, 0.05f, 0.05f, 0.95f, 0.95f);
  sample(rt2, TRUE, -0.95f, -0.95f, -0.55f, -0.05f);
  sample(rt2, FALSE, -0.5f, -0.95f, -0.1f, -0.05f);
  sample(managed, TRUE, 0.05f, -0.95f, 0.45f, -0.05f);
  sample(managed, FALSE, 0.5f, -0.95f, 0.95f, -0.55f);
  IDirect3DDevice9_SetTexture(t_dev, 0, NULL);
  IDirect3DDevice9_SetPixelShader(t_dev, t_ps_color);
  t_color(0.5f, 0.25f, 0.75f, 1.0f);
  IDirect3DDevice9_SetRenderState(t_dev, D3DRS_SRGBWRITEENABLE, TRUE);
  t_quad(0.5f, -0.5f, 0.95f, -0.05f, 0.5f);
  IDirect3DDevice9_SetRenderState(t_dev, D3DRS_SRGBWRITEENABLE, FALSE);
  IDirect3DDevice9_EndScene(t_dev);
}

int main(void) {
  t_init("d9mt_srgb_view", 640, 480);
  T_CHECK(IDirect3DDevice9_CreateTexture(t_dev, 256, 256, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                         D3DPOOL_DEFAULT, &rt, NULL));
  T_CHECK(IDirect3DDevice9_CreateTexture(t_dev, 128, 128, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                         D3DPOOL_DEFAULT, &rt2, NULL));
  T_CHECK(IDirect3DDevice9_CreateTexture(t_dev, 2, 2, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &managed, NULL));
  D3DLOCKED_RECT lr;
  T_CHECK(IDirect3DTexture9_LockRect(managed, 0, &lr, NULL, 0));
  static const DWORD texels[4] = {0xff204080, 0xff80c0ff, 0xffff8000, 0xff408020};
  memcpy(lr.pBits, texels, 8);
  memcpy((char *)lr.pBits + lr.Pitch, texels + 2, 8);
  IDirect3DTexture9_UnlockRect(managed, 0);
  rtSurf = t_level0(rt);
  rt2Surf = t_level0(rt2);
  T_CHECK(IDirect3DDevice9_GetRenderTarget(t_dev, 0, &backBuffer));
  t_tex_zw(0.0f, 1.0f);
  return t_loop(frame);
}
