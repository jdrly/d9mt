/* Verifier test (perf/gpu-4k, candidate 1): vertex texture fetch through a
 * different view of an image an earlier pass rendered:
 *  - quad 1: mip level 1 of a 2-level R32F target (the pass renders the level-1
 *    surface; the vertex sampler reads level 1 via D3DSAMP_MAXMIPLEVEL=1);
 *  - quad 2: an A8R8G8B8 target rendered through its linear view, fetched
 *    through the sRGB view (D3DSAMP_SRGBTEXTURE=1 on the vertex sampler).
 * Each value changes every frame; a vertex stage that runs before the long
 * fragment phase ends reads stale data and moves the quad. Negative control:
 * without the vertex fence (resolveVertexHazard returning early) both quads
 * show the previous frame's value. */
#include "t_common.h"

static IDirect3DTexture9 *mipTex, *rgbaTex;
static IDirect3DSurface9 *mipL1, *rgbaSurf, *backBuffer, *autoDs;

static void render_value(IDirect3DSurface9 *s, float value) {
  IDirect3DDevice9_SetTexture(t_dev, D3DVERTEXTEXTURESAMPLER0, NULL);
  IDirect3DDevice9_SetRenderTarget(t_dev, 0, s);
  IDirect3DDevice9_SetDepthStencilSurface(t_dev, NULL);
  IDirect3DDevice9_SetRenderState(t_dev, D3DRS_ZENABLE, D3DZB_FALSE);
  IDirect3DDevice9_Clear(t_dev, 0, NULL, D3DCLEAR_TARGET, 0, 1.0f, 0);
  IDirect3DDevice9_SetVertexShader(t_dev, t_vs_pos);
  IDirect3DDevice9_SetPixelShader(t_dev, t_ps_heavy);
  t_color(value, 0.0f, 0.0f, 1.0f);
  t_quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.5f);
}

static void vtf_quad(IDirect3DTexture9 *t, DWORD maxMip, DWORD srgb, float y0, float y1) {
  IDirect3DDevice9_SetSamplerState(t_dev, D3DVERTEXTEXTURESAMPLER0, D3DSAMP_MAXMIPLEVEL, maxMip);
  IDirect3DDevice9_SetSamplerState(t_dev, D3DVERTEXTEXTURESAMPLER0, D3DSAMP_SRGBTEXTURE, srgb);
  IDirect3DDevice9_SetTexture(t_dev, D3DVERTEXTEXTURESAMPLER0, (IDirect3DBaseTexture9 *)t);
  IDirect3DDevice9_SetVertexShader(t_dev, t_vs_vtf);
  IDirect3DDevice9_SetPixelShader(t_dev, t_ps_color);
  t_color(1.0f, 1.0f, 1.0f, 1.0f);
  t_quad(-0.9f, y0, -0.6f, y1, 0.5f);
}

static void frame(int n) {
  IDirect3DDevice9_BeginScene(t_dev);
  render_value(mipL1, 0.1f * (float)(n % 5) + 0.1f);
  IDirect3DDevice9_SetRenderTarget(t_dev, 0, backBuffer);
  IDirect3DDevice9_SetDepthStencilSurface(t_dev, autoDs);
  IDirect3DDevice9_Clear(t_dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(16, 16, 48), 1.0f, 0);
  vtf_quad(mipTex, 1, 0, 0.1f, 0.5f);

  render_value(rgbaSurf, 0.15f * (float)((n + 2) % 5) + 0.2f);
  IDirect3DDevice9_SetRenderTarget(t_dev, 0, backBuffer);
  IDirect3DDevice9_SetDepthStencilSurface(t_dev, autoDs);
  vtf_quad(rgbaTex, 0, 1, -0.5f, -0.1f);
  IDirect3DDevice9_EndScene(t_dev);
}

int main(void) {
  t_init("d9mt_vtf_alias", 640, 480);
  IDirect3D9 *d3d = NULL;
  IDirect3DDevice9_GetDirect3D(t_dev, &d3d);
  if (FAILED(IDirect3D9_CheckDeviceFormat(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8,
                                          D3DUSAGE_RENDERTARGET | D3DUSAGE_QUERY_VERTEXTEXTURE,
                                          D3DRTYPE_TEXTURE, D3DFMT_A8R8G8B8)))
    printf("NOTE A8R8G8B8 vertex texture not reported\n");
  T_CHECK(IDirect3DDevice9_CreateTexture(t_dev, 1024, 1024, 2, D3DUSAGE_RENDERTARGET, D3DFMT_R32F,
                                         D3DPOOL_DEFAULT, &mipTex, NULL));
  T_CHECK(IDirect3DTexture9_GetSurfaceLevel(mipTex, 1, &mipL1));
  T_CHECK(IDirect3DDevice9_CreateTexture(t_dev, 1024, 1024, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                         D3DPOOL_DEFAULT, &rgbaTex, NULL));
  rgbaSurf = t_level0(rgbaTex);
  T_CHECK(IDirect3DDevice9_GetRenderTarget(t_dev, 0, &backBuffer));
  T_CHECK(IDirect3DDevice9_GetDepthStencilSurface(t_dev, &autoDs));
  IDirect3DDevice9_SetSamplerState(t_dev, D3DVERTEXTEXTURESAMPLER0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
  IDirect3DDevice9_SetSamplerState(t_dev, D3DVERTEXTEXTURESAMPLER0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
  IDirect3DDevice9_SetSamplerState(t_dev, D3DVERTEXTEXTURESAMPLER0, D3DSAMP_MIPFILTER, D3DTEXF_POINT);
  t_tex_zw(0.0f, 1.0f);
  return t_loop(frame);
}
