/* Depth buffers sampled as textures (docs/gpu-4k.md, candidate 2): an INTZ
 * texture read as raw depth, a D24S8 texture sampled with the depth compare
 * D3D9 applies to depth formats (shadow maps), and DF24 if the device has it.
 * Each is rendered with two depths (left/right half) in its own pass, then
 * the backbuffer samples them. */
#include "t_common.h"

#define FOURCC(a, b, c, d) ((D3DFORMAT)MAKEFOURCC(a, b, c, d))

static IDirect3DTexture9 *dummy, *intz, *d24, *df24;
static IDirect3DSurface9 *dummySurf, *backBuffer, *autoDs;

static void render_depth(IDirect3DTexture9 *t, float zl, float zr) {
  IDirect3DSurface9 *s = t_level0(t);
  IDirect3DDevice9_SetRenderTarget(t_dev, 0, dummySurf);
  IDirect3DDevice9_SetDepthStencilSurface(t_dev, s);
  IDirect3DDevice9_SetRenderState(t_dev, D3DRS_ZENABLE, D3DZB_TRUE);
  IDirect3DDevice9_SetRenderState(t_dev, D3DRS_ZWRITEENABLE, TRUE);
  IDirect3DDevice9_SetRenderState(t_dev, D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
  IDirect3DDevice9_Clear(t_dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0, 1.0f, 0);
  IDirect3DDevice9_SetPixelShader(t_dev, t_ps_color);
  t_color(1.0f, 1.0f, 1.0f, 1.0f);
  t_quad(-1.0f, -1.0f, 0.0f, 1.0f, zl);
  t_quad(0.0f, -1.0f, 1.0f, 1.0f, zr);
  IDirect3DSurface9_Release(s);
}

static void sample(IDirect3DTexture9 *t, float x0, float y0, float x1, float y1) {
  IDirect3DDevice9_SetTexture(t_dev, 0, (IDirect3DBaseTexture9 *)t);
  t_quad(x0, y0, x1, y1, 0.5f);
}

static void frame(int n) {
  (void)n;
  IDirect3DDevice9_SetTexture(t_dev, 0, NULL);
  IDirect3DDevice9_BeginScene(t_dev);
  IDirect3DDevice9_SetVertexShader(t_dev, t_vs_pos);
  t_tex_zw(0.5f, 1.0f);
  render_depth(intz, 0.25f, 0.75f);
  render_depth(d24, 0.3f, 0.7f);
  if (df24)
    render_depth(df24, 0.2f, 0.6f);

  IDirect3DDevice9_SetRenderTarget(t_dev, 0, backBuffer);
  IDirect3DDevice9_SetDepthStencilSurface(t_dev, autoDs);
  IDirect3DDevice9_SetRenderState(t_dev, D3DRS_ZENABLE, D3DZB_FALSE);
  IDirect3DDevice9_Clear(t_dev, 0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(24, 24, 40), 1.0f, 0);
  IDirect3DDevice9_SetPixelShader(t_dev, t_ps_sample);
  sample(intz, -0.95f, 0.05f, -0.05f, 0.95f);
  sample(d24, 0.05f, 0.05f, 0.95f, 0.95f);
  if (df24)
    sample(df24, -0.95f, -0.95f, -0.05f, -0.05f);
  IDirect3DDevice9_EndScene(t_dev);
}

int main(void) {
  t_init("d9mt_depthtex", 640, 480);
  IDirect3D9 *d3d = NULL;
  IDirect3DDevice9_GetDirect3D(t_dev, &d3d);
  T_CHECK(IDirect3D9_CheckDeviceFormat(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8,
                                       D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_TEXTURE, FOURCC('I', 'N', 'T', 'Z')));
  T_CHECK(IDirect3D9_CheckDeviceFormat(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8,
                                       D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_TEXTURE, D3DFMT_D24S8));
  T_CHECK(IDirect3DDevice9_CreateTexture(t_dev, 256, 256, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                         D3DPOOL_DEFAULT, &dummy, NULL));
  T_CHECK(IDirect3DDevice9_CreateTexture(t_dev, 256, 256, 1, D3DUSAGE_DEPTHSTENCIL, FOURCC('I', 'N', 'T', 'Z'),
                                         D3DPOOL_DEFAULT, &intz, NULL));
  T_CHECK(IDirect3DDevice9_CreateTexture(t_dev, 256, 256, 1, D3DUSAGE_DEPTHSTENCIL, D3DFMT_D24S8,
                                         D3DPOOL_DEFAULT, &d24, NULL));
  if (SUCCEEDED(IDirect3D9_CheckDeviceFormat(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8,
                                             D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_TEXTURE, FOURCC('D', 'F', '2', '4'))))
    T_CHECK(IDirect3DDevice9_CreateTexture(t_dev, 256, 256, 1, D3DUSAGE_DEPTHSTENCIL, FOURCC('D', 'F', '2', '4'),
                                           D3DPOOL_DEFAULT, &df24, NULL));
  printf("DF24 %s\n", df24 ? "sampled" : "not supported");
  dummySurf = t_level0(dummy);
  T_CHECK(IDirect3DDevice9_GetRenderTarget(t_dev, 0, &backBuffer));
  T_CHECK(IDirect3DDevice9_GetDepthStencilSurface(t_dev, &autoDs));
  return t_loop(frame);
}
