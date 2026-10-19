/* Partial depth clears of a depth-stencil surface that isn't mip 0 / layer 0 (#17): mip 1
 * of a two-level D24S8 texture and face 3 of a D24S8 cube texture, each 64x64. Clear the
 * whole surface to depth 1, a rect of it to depth 0, then draw a white quad at depth 0.5
 * with LESSEQUAL: inside the rect it must fail the depth test (black), outside it must pass.
 * (Depth cube textures need d3d9.supportCubeDepthFormats, e.g. through DXVK_CONFIG.)
 * The partial clear applied the view's base mip/layer to a view that already starts there,
 * so it hit another subresource or none, and the quad covered the rect too. */
#include "t_xfer.h"

static IDirect3DTexture9 *white;
static IDirect3DSurface9 *rt;

static void check(const char *name, IDirect3DSurface9 *ds) {
  static const RECT rect = {16, 16, 48, 48};
  char what[64];
  X_CHECK(IDirect3DDevice9_SetRenderTarget(x_dev, 0, rt));
  X_CHECK(IDirect3DDevice9_SetDepthStencilSurface(x_dev, ds));
  X_CHECK(IDirect3DDevice9_Clear(x_dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL,
                                 0xff000000u, 1.0f, 0));
  X_CHECK(IDirect3DDevice9_Clear(x_dev, 1, (const D3DRECT *)&rect, D3DCLEAR_ZBUFFER, 0, 0.0f, 0));
  IDirect3DDevice9_BeginScene(x_dev);
  IDirect3DDevice9_SetRenderState(x_dev, D3DRS_ZENABLE, D3DZB_TRUE);
  IDirect3DDevice9_SetRenderState(x_dev, D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
  IDirect3DDevice9_SetRenderState(x_dev, D3DRS_ZWRITEENABLE, FALSE);
  x_draw((IDirect3DBaseTexture9 *)white, -1.0f, -1.0f, 1.0f, 1.0f);
  IDirect3DDevice9_SetRenderState(x_dev, D3DRS_ZENABLE, D3DZB_FALSE);
  IDirect3DDevice9_EndScene(x_dev);

  XReadback r = x_read(rt);
  snprintf(what, sizeof(what), "%s inside the cleared rect", name);
  x_expect(what, &r, 32, 32, 0xff000000u, 0x00ffffffu);
  snprintf(what, sizeof(what), "%s outside the cleared rect", name);
  x_expect(what, &r, 4, 4, 0xffffffffu, 0x00ffffffu);
  x_expect(what, &r, 60, 60, 0xffffffffu, 0x00ffffffu);
  x_done(&r);
}

static void frame(int n) {
  if (n != 1)
    return;
  D3DLOCKED_RECT lr;
  X_CHECK(IDirect3DDevice9_CreateTexture(x_dev, 1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &white, NULL));
  X_CHECK(IDirect3DTexture9_LockRect(white, 0, &lr, NULL, 0));
  *(DWORD *)lr.pBits = 0xffffffffu;
  IDirect3DTexture9_UnlockRect(white, 0);
  X_CHECK(IDirect3DDevice9_CreateRenderTarget(x_dev, 64, 64, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, NULL));

  IDirect3DTexture9 *mips;
  IDirect3DSurface9 *s;
  X_CHECK(IDirect3DDevice9_CreateTexture(x_dev, 128, 128, 2, D3DUSAGE_DEPTHSTENCIL, D3DFMT_D24S8,
                                         D3DPOOL_DEFAULT, &mips, NULL));
  X_CHECK(IDirect3DTexture9_GetSurfaceLevel(mips, 1, &s));
  check("2D mip 1", s);
  IDirect3DSurface9_Release(s);

  /* depth cube textures only with d3d9.supportCubeDepthFormats (DXVK_CONFIG) */
  IDirect3DCubeTexture9 *cube;
  if (SUCCEEDED(IDirect3DDevice9_CreateCubeTexture(x_dev, 64, 1, D3DUSAGE_DEPTHSTENCIL, D3DFMT_D24S8,
                                                   D3DPOOL_DEFAULT, &cube, NULL))) {
    X_CHECK(IDirect3DCubeTexture9_GetCubeMapSurface(cube, D3DCUBEMAP_FACE_NEGATIVE_Y, 0, &s));
    check("cube face 3", s);
    IDirect3DSurface9_Release(s);
  } else {
    printf("D24S8 cube textures not supported, cube face 3 skipped\n");
  }

  /* control: mip 0 */
  IDirect3DSurface9 *ds0;
  X_CHECK(IDirect3DDevice9_CreateDepthStencilSurface(x_dev, 64, 64, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, FALSE,
                                                     &ds0, NULL));
  check("plain surface", ds0);
  IDirect3DSurface9_Release(ds0);

  IDirect3DDevice9_SetRenderTarget(x_dev, 0, x_backBuffer);
  IDirect3DDevice9_SetDepthStencilSurface(x_dev, NULL);
  x_report("code_ds_clear_sub");
}

int main(void) {
  x_init("d9mt_ds_clear_sub");
  IDirect3DDevice9_SetRenderState(x_dev, D3DRS_ALPHABLENDENABLE, FALSE);
  return x_loop(frame);
}
