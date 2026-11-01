/* 4x MSAA colour and D24S8 depth-stencil resolved by StretchRect, as SWTOR does with its
 * "Very High" anti-aliasing: the paths that lose lossless compression on GPUs before
 * Apple10 (D9MT_TEX_PFV, driver/mtl/mtl_pfv.h). On those GPUs a resolve into a game's
 * texture goes through the source's resolve image and a copy; this checks that the result
 * is what a direct resolve gives. It doesn't reproduce the M4's flickering lines (a GPU
 * generation's compression hardware); run it with D9MT_TEX_PFV=msaa,resolve to take the
 * M1-M4 path on any Mac.
 *
 * Every frame, into a 64x64 4x MSAA colour surface and a 64x64 4x MSAA D24S8 surface:
 * clear to BG, depth 1, stencil 0; the left half drawn RED at depth 0.25 writing stencil 5;
 * a YELLOW triangle in the right half (edges with partly covered pixels), no depth or
 * stencil writes. Then:
 *  - the colour into tFolded right after the draws (the source's pass is still open: the
 *    resolve folds into its store action) and into tDirect after that (a resolve pass of
 *    its own);
 *  - the depth-stencil into a 64x64 D24S8 surface (the render-pass depth-stencil resolve);
 *  - with that surface as the depth-stencil (nothing writes it):
 *    - depth, into the 64x64 render-target texture tDepth cleared to BG, with ZFUNC LESS: a
 *      YELLOW quad at depth 0.1 (passes where the resolved depth is above 0.1: everywhere),
 *      then a GREEN quad at depth 0.5 (passes where it is above 0.5: the right half). Left
 *      YELLOW and right GREEN means a left depth in (0.1, 0.5] and a right one above 0.5;
 *    - stencil, into tStencil cleared to BG, no depth test: a BLUE quad with STENCILFUNC
 *      EQUAL 5 (passes where the resolved stencil is 5: the left half only).
 * The four textures are drawn into the backbuffer, and frame 1 reads them back. */
#include "t_xfer.h"

#define RED    0xffff0000u
#define GREEN  0xff00ff00u
#define BLUE   0xff0000ffu
#define YELLOW 0xffffff00u
#define BG     0xff303030u

static IDirect3DSurface9 *sMsaa, *sMsaaDs, *sDs;
static IDirect3DTexture9 *tFolded, *tDirect, *tDepth, *tStencil;
static IDirect3DTexture9 *cRed, *cGreen, *cBlue, *cYellow;
static BOOL hasMsaa;

static void quad(IDirect3DTexture9 *t, float x0, float y0, float x1, float y1, float z) {
  struct { float x, y, z, u, v; } q[4] = {
      {x0, y1, z, 0.0f, 0.0f}, {x1, y1, z, 1.0f, 0.0f},
      {x0, y0, z, 0.0f, 1.0f}, {x1, y0, z, 1.0f, 1.0f}};
  IDirect3DDevice9_SetTexture(x_dev, 0, (IDirect3DBaseTexture9 *)t);
  IDirect3DDevice9_DrawPrimitiveUP(x_dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof(q[0]));
  IDirect3DDevice9_SetTexture(x_dev, 0, NULL);
}

static void triangle(IDirect3DTexture9 *t) {
  struct { float x, y, z, u, v; } v[3] = {
      {0.0f, -1.0f, 0.5f, 0.0f, 0.0f}, {1.0f, -1.0f, 0.5f, 1.0f, 0.0f}, {1.0f, 1.0f, 0.5f, 1.0f, 1.0f}};
  IDirect3DDevice9_SetTexture(x_dev, 0, (IDirect3DBaseTexture9 *)t);
  IDirect3DDevice9_DrawPrimitiveUP(x_dev, D3DPT_TRIANGLELIST, 1, v, sizeof(v[0]));
  IDirect3DDevice9_SetTexture(x_dev, 0, NULL);
}

static void rs(D3DRENDERSTATETYPE s, DWORD v) {
  IDirect3DDevice9_SetRenderState(x_dev, s, v);
}

static void depthOff(void) {
  rs(D3DRS_ZENABLE, D3DZB_FALSE);
  rs(D3DRS_ZWRITEENABLE, FALSE);
  rs(D3DRS_STENCILENABLE, FALSE);
}

static void frame(int n) {
  IDirect3DSurface9 *d;
  if (hasMsaa) {
    IDirect3DDevice9_SetRenderTarget(x_dev, 0, sMsaa);
    IDirect3DDevice9_SetDepthStencilSurface(x_dev, sMsaaDs);
    IDirect3DDevice9_Clear(x_dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, BG, 1.0f, 0);
    IDirect3DDevice9_BeginScene(x_dev);
    rs(D3DRS_ZENABLE, D3DZB_TRUE);
    rs(D3DRS_ZWRITEENABLE, TRUE);
    rs(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    rs(D3DRS_STENCILENABLE, TRUE);
    rs(D3DRS_STENCILFUNC, D3DCMP_ALWAYS);
    rs(D3DRS_STENCILREF, 5);
    rs(D3DRS_STENCILMASK, 0xff);
    rs(D3DRS_STENCILWRITEMASK, 0xff);
    rs(D3DRS_STENCILPASS, D3DSTENCILOP_REPLACE);
    rs(D3DRS_STENCILFAIL, D3DSTENCILOP_KEEP);
    rs(D3DRS_STENCILZFAIL, D3DSTENCILOP_KEEP);
    quad(cRed, -1.0f, -1.0f, 0.0f, 1.0f, 0.25f);
    depthOff();
    triangle(cYellow);
    IDirect3DDevice9_EndScene(x_dev);

    X_CHECK(IDirect3DDevice9_StretchRect(x_dev, sMsaa, NULL, d = x_level(tFolded, 0), NULL, D3DTEXF_NONE));
    IDirect3DSurface9_Release(d);
    X_CHECK(IDirect3DDevice9_StretchRect(x_dev, sMsaa, NULL, d = x_level(tDirect, 0), NULL, D3DTEXF_NONE));
    IDirect3DSurface9_Release(d);
    X_CHECK(IDirect3DDevice9_StretchRect(x_dev, sMsaaDs, NULL, sDs, NULL, D3DTEXF_NONE));

    IDirect3DDevice9_SetDepthStencilSurface(x_dev, sDs);
    IDirect3DDevice9_SetRenderTarget(x_dev, 0, d = x_level(tDepth, 0));
    IDirect3DSurface9_Release(d);
    IDirect3DDevice9_Clear(x_dev, 0, NULL, D3DCLEAR_TARGET, BG, 1.0f, 0);
    IDirect3DDevice9_BeginScene(x_dev);
    rs(D3DRS_ZENABLE, D3DZB_TRUE);
    rs(D3DRS_ZWRITEENABLE, FALSE);
    rs(D3DRS_ZFUNC, D3DCMP_LESS);
    rs(D3DRS_STENCILENABLE, FALSE);
    quad(cYellow, -1.0f, -1.0f, 1.0f, 1.0f, 0.1f);
    quad(cGreen, -1.0f, -1.0f, 1.0f, 1.0f, 0.5f);
    IDirect3DDevice9_EndScene(x_dev);

    IDirect3DDevice9_SetRenderTarget(x_dev, 0, d = x_level(tStencil, 0));
    IDirect3DSurface9_Release(d);
    IDirect3DDevice9_Clear(x_dev, 0, NULL, D3DCLEAR_TARGET, BG, 1.0f, 0);
    IDirect3DDevice9_BeginScene(x_dev);
    rs(D3DRS_ZENABLE, D3DZB_FALSE);
    rs(D3DRS_STENCILENABLE, TRUE);
    rs(D3DRS_STENCILFUNC, D3DCMP_EQUAL);
    rs(D3DRS_STENCILREF, 5);
    rs(D3DRS_STENCILPASS, D3DSTENCILOP_KEEP);
    quad(cBlue, -1.0f, -1.0f, 1.0f, 1.0f, 0.5f);
    depthOff();
    IDirect3DDevice9_EndScene(x_dev);
    IDirect3DDevice9_SetDepthStencilSurface(x_dev, NULL);
  }

  IDirect3DDevice9_SetRenderTarget(x_dev, 0, x_backBuffer);
  IDirect3DDevice9_Clear(x_dev, 0, NULL, D3DCLEAR_TARGET, 0xff181828u, 1.0f, 0);
  if (hasMsaa) {
    IDirect3DDevice9_BeginScene(x_dev);
    x_draw((IDirect3DBaseTexture9 *)tFolded,  -0.95f, -0.4f, -0.55f, 0.4f);
    x_draw((IDirect3DBaseTexture9 *)tDirect,  -0.45f, -0.4f, -0.05f, 0.4f);
    x_draw((IDirect3DBaseTexture9 *)tDepth,    0.05f, -0.4f,  0.45f, 0.4f);
    x_draw((IDirect3DBaseTexture9 *)tStencil,  0.55f, -0.4f,  0.95f, 0.4f);
    IDirect3DDevice9_EndScene(x_dev);
  }

  if (n != 1)
    return;
  if (hasMsaa) {
    IDirect3DTexture9 *colour[2] = {tFolded, tDirect};
    const char *name[2] = {"colour resolve, folded", "colour resolve, own pass"};
    for (int i = 0; i < 2; i++) {
      XReadback r = x_read(d = x_level(colour[i], 0));
      x_expect(name[i], &r, 4, 4, RED, 0xffffffffu);
      x_expect(name[i], &r, 28, 60, RED, 0xffffffffu);
      x_expect(name[i], &r, 36, 4, BG, 0xffffffffu);
      x_expect(name[i], &r, 60, 4, BG, 0xffffffffu);
      x_expect(name[i], &r, 60, 60, YELLOW, 0xffffffffu);
      x_done(&r);
      IDirect3DSurface9_Release(d);
    }

    XReadback r = x_read(d = x_level(tDepth, 0));
    x_expect("resolved depth 0.25 (left)", &r, 4, 4, YELLOW, 0xffffffffu);
    x_expect("resolved depth 0.25 (left)", &r, 28, 60, YELLOW, 0xffffffffu);
    x_expect("resolved depth 1 (right)", &r, 36, 4, GREEN, 0xffffffffu);
    x_expect("resolved depth 1 (right)", &r, 60, 60, GREEN, 0xffffffffu);
    x_done(&r);
    IDirect3DSurface9_Release(d);

    r = x_read(d = x_level(tStencil, 0));
    x_expect("resolved stencil 5 (left)", &r, 4, 4, BLUE, 0xffffffffu);
    x_expect("resolved stencil 5 (left)", &r, 28, 60, BLUE, 0xffffffffu);
    x_expect("resolved stencil 0 (right)", &r, 36, 4, BG, 0xffffffffu);
    x_expect("resolved stencil 0 (right)", &r, 60, 60, BG, 0xffffffffu);
    x_done(&r);
    IDirect3DSurface9_Release(d);
  }
  printf("4x MSAA %s\n", hasMsaa ? "tested" : "not supported");
  x_report("code_msaa_compress");
}

static IDirect3DTexture9 *rt(void) {
  IDirect3DTexture9 *t = NULL;
  X_CHECK(IDirect3DDevice9_CreateTexture(x_dev, 64, 64, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &t, NULL));
  return t;
}

static IDirect3DTexture9 *solid(DWORD c) {
  IDirect3DTexture9 *t = NULL;
  X_CHECK(IDirect3DDevice9_CreateTexture(x_dev, 2, 2, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, NULL));
  D3DLOCKED_RECT lr;
  X_CHECK(IDirect3DTexture9_LockRect(t, 0, &lr, NULL, 0));
  for (int y = 0; y < 2; y++)
    for (int x = 0; x < 2; x++)
      ((DWORD *)((unsigned char *)lr.pBits + y * lr.Pitch))[x] = c;
  IDirect3DTexture9_UnlockRect(t, 0);
  return t;
}

int main(void) {
  IDirect3D9 *d3d = x_init("d9mt_msaa_compress");
  hasMsaa = SUCCEEDED(IDirect3D9_CheckDeviceMultiSampleType(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_A8R8G8B8,
                                                            TRUE, D3DMULTISAMPLE_4_SAMPLES, NULL))
         && SUCCEEDED(IDirect3D9_CheckDeviceMultiSampleType(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_D24S8,
                                                            TRUE, D3DMULTISAMPLE_4_SAMPLES, NULL));
  if (hasMsaa) {
    X_CHECK(IDirect3DDevice9_CreateRenderTarget(x_dev, 64, 64, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_4_SAMPLES, 0, FALSE, &sMsaa, NULL));
    X_CHECK(IDirect3DDevice9_CreateDepthStencilSurface(x_dev, 64, 64, D3DFMT_D24S8, D3DMULTISAMPLE_4_SAMPLES, 0, FALSE, &sMsaaDs, NULL));
    X_CHECK(IDirect3DDevice9_CreateDepthStencilSurface(x_dev, 64, 64, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, FALSE, &sDs, NULL));
    tFolded = rt();
    tDirect = rt();
    tDepth = rt();
    tStencil = rt();
    cRed = solid(RED);
    cGreen = solid(GREEN);
    cBlue = solid(BLUE);
    cYellow = solid(YELLOW);
  }
  return x_loop(frame);
}
