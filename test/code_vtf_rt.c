/* Vertex texture fetch of a render target rendered by the pass before
 * (docs/gpu-4k.md, candidate 1). Each frame clears a 1024x1024 R32F target,
 * renders a value into it with a long pixel shader (heavy_ps), then draws a
 * white quad whose x offset the vertex shader fetches from that target
 * (vtf_vs). The value changes every frame: a vertex stage that runs before
 * the long fragment phase has finished reads last frame's value or the clear
 * and draws the quad elsewhere. A second target and quad do the same after
 * ~120k culled draws in the same backbuffer pass. With
 * DXVK_CONFIG=d3d9.reproducibleCommandStream=True (no implicit flushes) they
 * pass 4 MiB of recorded commands, d9mt creates the pass's encoder early, and
 * the vertex fence has to split the pass (d9mt logs that once); otherwise an
 * implicit flush ends the pass first. The hashes are the same either way. */
#include "t_common.h"

#define FILLER_DRAWS 120000

static IDirect3DTexture9 *rt[2];
static IDirect3DSurface9 *rtSurf[2], *backBuffer, *autoDs;
static IDirect3DVertexBuffer9 *filler;

/* Clears target i and renders `value` into it with the long pixel shader. */
static void render_value(int i, float value) {
  IDirect3DDevice9_SetTexture(t_dev, D3DVERTEXTEXTURESAMPLER0, NULL);
  IDirect3DDevice9_SetRenderTarget(t_dev, 0, rtSurf[i]);
  IDirect3DDevice9_SetDepthStencilSurface(t_dev, NULL);
  IDirect3DDevice9_SetRenderState(t_dev, D3DRS_ZENABLE, D3DZB_FALSE);
  IDirect3DDevice9_Clear(t_dev, 0, NULL, D3DCLEAR_TARGET, 0, 1.0f, 0);
  IDirect3DDevice9_SetVertexShader(t_dev, t_vs_pos);
  IDirect3DDevice9_SetPixelShader(t_dev, t_ps_heavy);
  t_color(value, 0.0f, 0.0f, 1.0f);
  t_quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.5f);
}

/* A white quad at y0..y1 whose x offset comes from target i (vertex fetch). */
static void vtf_quad(int i, float y0, float y1) {
  IDirect3DDevice9_SetTexture(t_dev, D3DVERTEXTEXTURESAMPLER0, (IDirect3DBaseTexture9 *)rt[i]);
  IDirect3DDevice9_SetVertexShader(t_dev, t_vs_vtf);
  IDirect3DDevice9_SetPixelShader(t_dev, t_ps_color);
  t_color(1.0f, 1.0f, 1.0f, 1.0f);
  t_quad(-0.9f, y0, -0.6f, y1, 0.5f);
}

static void frame(int n) {
  IDirect3DDevice9_BeginScene(t_dev);
  render_value(0, 0.1f * (float)(n % 5) + 0.1f);

  IDirect3DDevice9_SetRenderTarget(t_dev, 0, backBuffer);
  IDirect3DDevice9_SetDepthStencilSurface(t_dev, autoDs);
  IDirect3DDevice9_Clear(t_dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(16, 16, 48), 1.0f, 0);
  vtf_quad(0, 0.1f, 0.5f);

  render_value(1, 0.1f * (float)((n + 2) % 5) + 0.1f);

  IDirect3DDevice9_SetRenderTarget(t_dev, 0, backBuffer);
  IDirect3DDevice9_SetDepthStencilSurface(t_dev, autoDs);
  IDirect3DDevice9_SetTexture(t_dev, D3DVERTEXTEXTURESAMPLER0, NULL);
  IDirect3DDevice9_SetVertexShader(t_dev, t_vs_pos);
  IDirect3DDevice9_SetPixelShader(t_dev, t_ps_color);
  IDirect3DDevice9_SetStreamSource(t_dev, 0, filler, 0, sizeof(struct TVertex));
  for (int d = 0; d < FILLER_DRAWS; d++) {
    t_color(0.0f, (float)(d & 1), 0.0f, 1.0f);
    IDirect3DDevice9_DrawPrimitive(t_dev, D3DPT_TRIANGLELIST, 0, 1);
  }
  vtf_quad(1, -0.5f, -0.1f);
  IDirect3DDevice9_EndScene(t_dev);
}

int main(void) {
  t_init("d9mt_vtf_rt", 640, 480);
  IDirect3D9 *d3d = NULL;
  IDirect3DDevice9_GetDirect3D(t_dev, &d3d);
  if (FAILED(IDirect3D9_CheckDeviceFormat(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8,
                                          D3DUSAGE_RENDERTARGET | D3DUSAGE_QUERY_VERTEXTEXTURE,
                                          D3DRTYPE_TEXTURE, D3DFMT_R32F))) {
    printf("FAIL R32F render target with vertex texture fetch not supported\n");
    return 2;
  }
  for (int i = 0; i < 2; i++) {
    T_CHECK(IDirect3DDevice9_CreateTexture(t_dev, 1024, 1024, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F,
                                           D3DPOOL_DEFAULT, &rt[i], NULL));
    rtSurf[i] = t_level0(rt[i]);
  }
  /* one zero-area triangle: culled, but recorded and encoded like any draw */
  static const struct TVertex degenerate[3] = {{0.5f, 0.5f, 0.5f, 0, 0}, {0.5f, 0.5f, 0.5f, 0, 0}, {0.5f, 0.5f, 0.5f, 0, 0}};
  T_CHECK(IDirect3DDevice9_CreateVertexBuffer(t_dev, sizeof(degenerate), D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &filler, NULL));
  void *p = NULL;
  T_CHECK(IDirect3DVertexBuffer9_Lock(filler, 0, 0, &p, 0));
  memcpy(p, degenerate, sizeof(degenerate));
  IDirect3DVertexBuffer9_Unlock(filler);
  T_CHECK(IDirect3DDevice9_GetRenderTarget(t_dev, 0, &backBuffer));
  T_CHECK(IDirect3DDevice9_GetDepthStencilSurface(t_dev, &autoDs));
  IDirect3DDevice9_SetSamplerState(t_dev, D3DVERTEXTEXTURESAMPLER0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
  IDirect3DDevice9_SetSamplerState(t_dev, D3DVERTEXTEXTURESAMPLER0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
  IDirect3DDevice9_SetSamplerState(t_dev, D3DVERTEXTEXTURESAMPLER0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
  t_tex_zw(0.0f, 1.0f);
  return t_loop(frame);
}
