/* StretchRect variants (#14), each checked against the expected pixels: a
 * 64x64 A8R8G8B8 source with four coloured quadrants, copied with POINT and
 * LINEAR filters, scaled down, up and not at all, into render-target
 * textures, render-target surfaces, an off-screen plain surface (read with
 * LockRect), a cube face, a mip level, a sub-rect and the backbuffer, converted to R5G6B5 and from X8R8G8B8 (alpha reads as 1),
 * and from a multisampled source. The quadrant centres must hold the
 * quadrant colours exactly (bilinear filtering only blends at the edges);
 * the results are drawn into the backbuffer too. */
#include "t_xfer.h"

#define RED   0xffff0000u
#define GREEN 0xff00ff00u
#define BLUE  0xff0000ffu
#define GRAY  0xff848284u  /* 132,130,132: exact in R5G6B5 */
#define BG    0xff303030u
static const DWORD kQuad[4] = {RED, GREEN, BLUE, GRAY};
static const WORD  kQuad565[4] = {0xf800, 0x07e0, 0x001f, 0x8410};

static IDirect3DTexture9 *src, *srcX8, *t32p, *t32l, *t128p, *t128l, *t64same, *t96, *t64rect, *tX8to32, *tMs32, *tMip;
static IDirect3DSurface9 *srcS, *srcX8S, *s48, *s565, *s565small, *sMsaa, *cubeFace, *mip1, *plain;
static IDirect3DCubeTexture9 *cube;
static BOOL has565, hasMsaa;

static void quadrants(IDirect3DSurface9 *s, UINT w, UINT h) {
  for (int i = 0; i < 4; i++) {
    D3DRECT r = {(i & 1) * (LONG)w / 2, (i >> 1) * (LONG)h / 2, ((i & 1) + 1) * (LONG)w / 2, ((i >> 1) + 1) * (LONG)h / 2};
    IDirect3DDevice9_SetRenderTarget(x_dev, 0, s);
    IDirect3DDevice9_Clear(x_dev, 1, &r, D3DCLEAR_TARGET, kQuad[i], 1.0f, 0);
  }
}

static void check_quads(const char *what, IDirect3DSurface9 *s, DWORD mask) {
  XReadback r = x_read(s);
  const UINT w = r.desc.Width, h = r.desc.Height;
  for (int i = 0; i < 4; i++) {
    const UINT x = (i & 1) ? 3 * w / 4 : w / 4, y = (i >> 1) ? 3 * h / 4 : h / 4;
    if (r.desc.Format == D3DFMT_R5G6B5)
      x_expect(what, &r, x, y, kQuad565[i], 0xffff);
    else
      x_expect(what, &r, x, y, kQuad[i], mask);
  }
  x_done(&r);
}

static void stretch(IDirect3DSurface9 *from, const RECT *fr, IDirect3DSurface9 *to, const RECT *tr, D3DTEXTUREFILTERTYPE f) {
  X_CHECK(IDirect3DDevice9_StretchRect(x_dev, from, fr, to, tr, f));
}

static void frame(int n) {
  quadrants(srcS, 64, 64);
  IDirect3DDevice9_SetRenderTarget(x_dev, 0, srcX8S);
  IDirect3DDevice9_Clear(x_dev, 0, NULL, D3DCLEAR_TARGET, RED & 0x00ffffffu, 1.0f, 0);  /* alpha 0 in memory */

  IDirect3DSurface9 *d;
  stretch(srcS, NULL, d = x_level(t32p, 0), NULL, D3DTEXF_POINT);   IDirect3DSurface9_Release(d);
  stretch(srcS, NULL, d = x_level(t32l, 0), NULL, D3DTEXF_LINEAR);  IDirect3DSurface9_Release(d);
  stretch(srcS, NULL, d = x_level(t128p, 0), NULL, D3DTEXF_POINT);  IDirect3DSurface9_Release(d);
  stretch(srcS, NULL, d = x_level(t128l, 0), NULL, D3DTEXF_LINEAR); IDirect3DSurface9_Release(d);
  stretch(srcS, NULL, d = x_level(t64same, 0), NULL, D3DTEXF_LINEAR); IDirect3DSurface9_Release(d);
  stretch(srcS, NULL, s48, NULL, D3DTEXF_LINEAR);
  stretch(s48, NULL, d = x_level(t96, 0), NULL, D3DTEXF_POINT);     IDirect3DSurface9_Release(d);
  d = x_level(t64rect, 0);
  IDirect3DDevice9_SetRenderTarget(x_dev, 0, d);
  IDirect3DDevice9_Clear(x_dev, 0, NULL, D3DCLEAR_TARGET, BG, 1.0f, 0);
  RECT fr = {0, 0, 32, 32}, tr = {8, 8, 40, 40};
  stretch(srcS, &fr, d, &tr, D3DTEXF_POINT);
  IDirect3DSurface9_Release(d);
  if (has565) {
    stretch(srcS, NULL, s565, NULL, D3DTEXF_POINT);
    stretch(srcS, NULL, s565small, NULL, D3DTEXF_LINEAR);
  }
  stretch(srcX8S, NULL, d = x_level(tX8to32, 0), NULL, D3DTEXF_LINEAR); IDirect3DSurface9_Release(d);
  if (hasMsaa) {
    quadrants(sMsaa, 64, 64);
    stretch(sMsaa, NULL, d = x_level(tMs32, 0), NULL, D3DTEXF_LINEAR); IDirect3DSurface9_Release(d);
  }
  stretch(srcS, NULL, cubeFace, NULL, D3DTEXF_POINT);
  stretch(srcS, NULL, mip1, NULL, D3DTEXF_POINT);
  stretch(srcS, NULL, plain, NULL, D3DTEXF_LINEAR);

  IDirect3DDevice9_SetRenderTarget(x_dev, 0, x_backBuffer);
  IDirect3DDevice9_Clear(x_dev, 0, NULL, D3DCLEAR_TARGET, 0xff181828u, 1.0f, 0);
  RECT bbr = {400, 40, 560, 200};
  stretch(srcS, NULL, x_backBuffer, &bbr, D3DTEXF_LINEAR);
  IDirect3DDevice9_BeginScene(x_dev);
  IDirect3DBaseTexture9 *show[8] = {(IDirect3DBaseTexture9 *)t32p, (IDirect3DBaseTexture9 *)t32l,
                                    (IDirect3DBaseTexture9 *)t128p, (IDirect3DBaseTexture9 *)t128l,
                                    (IDirect3DBaseTexture9 *)t96, (IDirect3DBaseTexture9 *)t64rect,
                                    (IDirect3DBaseTexture9 *)tX8to32, (IDirect3DBaseTexture9 *)tMs32};
  for (int i = 0; i < 8; i++) {
    const float x0 = -0.95f + 0.24f * (float)(i % 4), y0 = i < 4 ? -0.45f : -0.95f;
    x_draw(show[i], x0, y0, x0 + 0.2f, y0 + 0.4f);
  }
  IDirect3DDevice9_EndScene(x_dev);

  if (n != 1)
    return;
  check_quads("down point", d = x_level(t32p, 0), 0xffffffffu);   IDirect3DSurface9_Release(d);
  check_quads("down linear", d = x_level(t32l, 0), 0xffffffffu);  IDirect3DSurface9_Release(d);
  check_quads("up point", d = x_level(t128p, 0), 0xffffffffu);    IDirect3DSurface9_Release(d);
  check_quads("up linear", d = x_level(t128l, 0), 0xffffffffu);   IDirect3DSurface9_Release(d);
  check_quads("same size", d = x_level(t64same, 0), 0xffffffffu); IDirect3DSurface9_Release(d);
  check_quads("to surface 48", s48, 0xffffffffu);
  check_quads("surface to 96", d = x_level(t96, 0), 0xffffffffu); IDirect3DSurface9_Release(d);
  {
    XReadback r = x_read(d = x_level(t64rect, 0));
    x_expect("sub-rect inside", &r, 16, 16, RED, 0xffffffffu);
    x_expect("sub-rect inside", &r, 32, 32, RED, 0xffffffffu);
    x_expect("sub-rect outside", &r, 4, 4, BG, 0xffffffffu);
    x_expect("sub-rect outside", &r, 44, 44, BG, 0xffffffffu);
    x_expect("sub-rect outside", &r, 60, 8, BG, 0xffffffffu);
    x_done(&r);
    IDirect3DSurface9_Release(d);
  }
  if (has565) {
    check_quads("to R5G6B5", s565, 0);
    check_quads("to R5G6B5 scaled", s565small, 0);
  }
  {
    XReadback r = x_read(d = x_level(tX8to32, 0));
    x_expect("X8 to A8 (alpha 1)", &r, 16, 16, RED, 0xffffffffu);
    x_done(&r);
    IDirect3DSurface9_Release(d);
  }
  if (hasMsaa) {
    check_quads("from 4x MSAA", d = x_level(tMs32, 0), 0xffffffffu);
    IDirect3DSurface9_Release(d);
  }
  check_quads("cube face", cubeFace, 0xffffffffu);
  check_quads("mip 1", mip1, 0xffffffffu);
  check_quads("off-screen plain", plain, 0xffffffffu);
  {
    XReadback r = x_read(x_backBuffer);
    for (int i = 0; i < 4; i++)
      x_expect("backbuffer rect", &r, 400 + 40 + (i & 1) * 80, 40 + 40 + (i >> 1) * 80, kQuad[i], 0x00ffffffu);
    x_expect("backbuffer outside", &r, 380, 30, 0xff181828u, 0x00ffffffu);
    x_done(&r);
  }
  printf("R5G6B5 %s, 4x MSAA %s\n", has565 ? "tested" : "not supported", hasMsaa ? "tested" : "not supported");
  x_report("code_stretch");
}

static IDirect3DTexture9 *rt(UINT size, UINT levels, D3DFORMAT f) {
  IDirect3DTexture9 *t = NULL;
  X_CHECK(IDirect3DDevice9_CreateTexture(x_dev, size, size, levels, D3DUSAGE_RENDERTARGET, f, D3DPOOL_DEFAULT, &t, NULL));
  return t;
}

int main(void) {
  IDirect3D9 *d3d = x_init("d9mt_stretch");
  src = rt(64, 1, D3DFMT_A8R8G8B8);
  srcX8 = rt(64, 1, D3DFMT_X8R8G8B8);
  t32p = rt(32, 1, D3DFMT_A8R8G8B8);
  t32l = rt(32, 1, D3DFMT_A8R8G8B8);
  t128p = rt(128, 1, D3DFMT_A8R8G8B8);
  t128l = rt(128, 1, D3DFMT_A8R8G8B8);
  t64same = rt(64, 1, D3DFMT_A8R8G8B8);
  t96 = rt(96, 1, D3DFMT_A8R8G8B8);
  t64rect = rt(64, 1, D3DFMT_A8R8G8B8);
  tX8to32 = rt(32, 1, D3DFMT_A8R8G8B8);
  tMs32 = rt(32, 1, D3DFMT_A8R8G8B8);
  tMip = rt(64, 2, D3DFMT_A8R8G8B8);
  srcS = x_level(src, 0);
  srcX8S = x_level(srcX8, 0);
  mip1 = x_level(tMip, 1);
  X_CHECK(IDirect3DDevice9_CreateRenderTarget(x_dev, 48, 48, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &s48, NULL));
  has565 = SUCCEEDED(IDirect3D9_CheckDeviceFormat(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8,
                                                  D3DUSAGE_RENDERTARGET, D3DRTYPE_SURFACE, D3DFMT_R5G6B5));
  if (has565) {
    X_CHECK(IDirect3DDevice9_CreateRenderTarget(x_dev, 64, 64, D3DFMT_R5G6B5, D3DMULTISAMPLE_NONE, 0, FALSE, &s565, NULL));
    X_CHECK(IDirect3DDevice9_CreateRenderTarget(x_dev, 32, 32, D3DFMT_R5G6B5, D3DMULTISAMPLE_NONE, 0, FALSE, &s565small, NULL));
  }
  hasMsaa = SUCCEEDED(IDirect3D9_CheckDeviceMultiSampleType(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_A8R8G8B8,
                                                            TRUE, D3DMULTISAMPLE_4_SAMPLES, NULL));
  if (hasMsaa)
    X_CHECK(IDirect3DDevice9_CreateRenderTarget(x_dev, 64, 64, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_4_SAMPLES, 0, FALSE, &sMsaa, NULL));
  X_CHECK(IDirect3DDevice9_CreateCubeTexture(x_dev, 32, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &cube, NULL));
  X_CHECK(IDirect3DCubeTexture9_GetCubeMapSurface(cube, D3DCUBEMAP_FACE_POSITIVE_Y, 0, &cubeFace));
  X_CHECK(IDirect3DDevice9_CreateOffscreenPlainSurface(x_dev, 32, 32, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &plain, NULL));
  return x_loop(frame);
}
