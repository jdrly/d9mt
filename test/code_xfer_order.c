/* StretchRect and ColorFill interleaved with draws, clears and the
 * attachment load/store optimization (#14; the cases come from its
 * verification): a pending clear as the blit source, a ColorFill of the bound
 * render target mid-scene, a whole ColorFill over a pending clear, depth kept
 * across the pass split a blit makes, a blit into the bound render target,
 * and a ColorFill sampled in the same frame. Each must give the pixels the
 * D3D9 call order defines. */
#include "t_xfer.h"

static IDirect3DTexture9 *tR, *tG, *tB, *tW, *a, *b;
static IDirect3DSurface9 *as, *bs, *ds;

static IDirect3DTexture9 *solid(DWORD c) {
  IDirect3DTexture9 *t;
  D3DLOCKED_RECT lr;
  X_CHECK(IDirect3DDevice9_CreateTexture(x_dev, 1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, NULL));
  X_CHECK(IDirect3DTexture9_LockRect(t, 0, &lr, NULL, 0));
  *(DWORD *)lr.pBits = c;
  IDirect3DTexture9_UnlockRect(t, 0);
  return t;
}

static void quad(IDirect3DTexture9 *t, float x0, float y0, float x1, float y1, float z) {
  struct { float x, y, z, u, v; } q[4] = {{x0, y1, z, 0, 0}, {x1, y1, z, 1, 0}, {x0, y0, z, 0, 1}, {x1, y0, z, 1, 1}};
  IDirect3DDevice9_SetTexture(x_dev, 0, (IDirect3DBaseTexture9 *)t);
  IDirect3DDevice9_DrawPrimitiveUP(x_dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof(q[0]));
  IDirect3DDevice9_SetTexture(x_dev, 0, NULL);
}

static void expect(const char *what, IDirect3DSurface9 *s, UINT x, UINT y, DWORD want) {
  XReadback r = x_read(s);
  x_expect(what, &r, x, y, want, 0xffffffffu);
  x_done(&r);
}

static void cases(void) {
  /* a. a pending clear (no draw) as the source of a scaled StretchRect */
  IDirect3DDevice9_SetRenderTarget(x_dev, 0, as);
  IDirect3DDevice9_Clear(x_dev, 0, NULL, D3DCLEAR_TARGET, 0xffff0000, 1, 0);
  X_CHECK(IDirect3DDevice9_StretchRect(x_dev, as, NULL, bs, NULL, D3DTEXF_LINEAR));
  expect("pending clear as blit source", bs, 10, 10, 0xffff0000);

  /* b. draw, ColorFill a rect of the bound render target mid-scene, draw more */
  IDirect3DDevice9_SetRenderTarget(x_dev, 0, as);
  IDirect3DDevice9_Clear(x_dev, 0, NULL, D3DCLEAR_TARGET, 0xff000000, 1, 0);
  IDirect3DDevice9_BeginScene(x_dev);
  quad(tG, -1, -1, 0, 1, 0.5f);
  RECT fr = {40, 0, 64, 32};
  X_CHECK(IDirect3DDevice9_ColorFill(x_dev, as, &fr, 0xff0000ff));
  quad(tW, 0, -1, 1, 0, 0.5f);
  IDirect3DDevice9_EndScene(x_dev);
  expect("fill mid-scene: draw before", as, 10, 40, 0xff00ff00);
  expect("fill mid-scene: fill", as, 50, 10, 0xff0000ff);
  expect("fill mid-scene: draw after", as, 50, 50, 0xffffffff);
  expect("fill mid-scene: untouched", as, 34, 10, 0xff000000);

  /* c. whole ColorFill of the bound render target over a pending clear, then a draw */
  IDirect3DDevice9_SetRenderTarget(x_dev, 0, as);
  IDirect3DDevice9_Clear(x_dev, 0, NULL, D3DCLEAR_TARGET, 0xffff0000, 1, 0);
  X_CHECK(IDirect3DDevice9_ColorFill(x_dev, as, NULL, 0xff0000ff));
  IDirect3DDevice9_BeginScene(x_dev);
  quad(tG, -1, -1, 0, 0, 0.5f);
  IDirect3DDevice9_EndScene(x_dev);
  expect("fill over pending clear", as, 50, 10, 0xff0000ff);
  expect("fill over pending clear: draw", as, 10, 50, 0xff00ff00);

  /* d. depth kept across the pass split of a blit: near quad, blit, fill, far quad stays hidden */
  IDirect3DDevice9_SetRenderTarget(x_dev, 0, as);
  IDirect3DDevice9_SetDepthStencilSurface(x_dev, ds);
  IDirect3DDevice9_SetRenderState(x_dev, D3DRS_ZENABLE, D3DZB_TRUE);
  IDirect3DDevice9_SetRenderState(x_dev, D3DRS_ZWRITEENABLE, TRUE);
  IDirect3DDevice9_SetRenderState(x_dev, D3DRS_ZFUNC, D3DCMP_LESS);
  IDirect3DDevice9_Clear(x_dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000, 1, 0);
  IDirect3DDevice9_BeginScene(x_dev);
  quad(tG, -1, -1, 1, 1, 0.2f);
  X_CHECK(IDirect3DDevice9_StretchRect(x_dev, as, NULL, bs, NULL, D3DTEXF_LINEAR));
  RECT cr = {0, 0, 8, 8};
  X_CHECK(IDirect3DDevice9_ColorFill(x_dev, bs, &cr, 0xffffffff));
  quad(tR, -1, -1, 1, 1, 0.8f);
  quad(tB, -1, -1, 0, 0, 0.1f);
  IDirect3DDevice9_EndScene(x_dev);
  IDirect3DDevice9_SetRenderState(x_dev, D3DRS_ZENABLE, D3DZB_FALSE);
  IDirect3DDevice9_SetDepthStencilSurface(x_dev, NULL);
  expect("depth across blit: far quad hidden", as, 50, 10, 0xff00ff00);
  expect("depth across blit: nearer quad", as, 10, 50, 0xff0000ff);
  expect("depth across blit: blit copy", bs, 20, 20, 0xff00ff00);
  expect("depth across blit: fill", bs, 2, 2, 0xffffffff);

  /* e. StretchRect into the bound render target mid-scene, then draw over part of it */
  IDirect3DDevice9_SetRenderTarget(x_dev, 0, bs);
  IDirect3DDevice9_Clear(x_dev, 0, NULL, D3DCLEAR_TARGET, 0xff000000, 1, 0);
  IDirect3DDevice9_BeginScene(x_dev);
  quad(tW, -1, -1, 1, 1, 0.5f);
  IDirect3DDevice9_SetRenderTarget(x_dev, 0, as);
  IDirect3DDevice9_Clear(x_dev, 0, NULL, D3DCLEAR_TARGET, 0xffff0000, 1, 0);
  IDirect3DDevice9_SetRenderTarget(x_dev, 0, bs);
  RECT dr = {0, 0, 16, 16};
  X_CHECK(IDirect3DDevice9_StretchRect(x_dev, as, NULL, bs, &dr, D3DTEXF_POINT));
  quad(tB, 0, -1, 1, 0, 0.5f);
  IDirect3DDevice9_EndScene(x_dev);
  expect("blit into bound target", bs, 4, 4, 0xffff0000);
  expect("blit into bound target: draw before", bs, 24, 4, 0xffffffff);
  expect("blit into bound target: draw after", bs, 24, 24, 0xff0000ff);

  /* f. ColorFill, then sample it in the same frame */
  X_CHECK(IDirect3DDevice9_ColorFill(x_dev, as, NULL, 0xff00ffff));
  IDirect3DDevice9_SetRenderTarget(x_dev, 0, bs);
  IDirect3DDevice9_BeginScene(x_dev);
  quad(a, -1, -1, 1, 1, 0.5f);
  IDirect3DDevice9_EndScene(x_dev);
  expect("fill then sample", bs, 16, 16, 0xff00ffff);
}

static void frame(int n) {
  if (n == 1) {
    cases();
    x_report("code_xfer_order");
  }
  IDirect3DDevice9_SetRenderTarget(x_dev, 0, x_backBuffer);
  IDirect3DDevice9_Clear(x_dev, 0, NULL, D3DCLEAR_TARGET, 0xff181828u, 1.0f, 0);
  IDirect3DDevice9_BeginScene(x_dev);
  quad(a, -0.95f, -0.45f, -0.05f, 0.45f, 0.5f);
  quad(b, 0.05f, -0.45f, 0.95f, 0.45f, 0.5f);
  IDirect3DDevice9_EndScene(x_dev);
}

int main(void) {
  x_init("d9mt_xfer_order");
  tR = solid(0xffff0000);
  tG = solid(0xff00ff00);
  tB = solid(0xff0000ff);
  tW = solid(0xffffffff);
  X_CHECK(IDirect3DDevice9_CreateTexture(x_dev, 64, 64, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &a, NULL));
  X_CHECK(IDirect3DDevice9_CreateTexture(x_dev, 32, 32, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &b, NULL));
  as = x_level(a, 0);
  bs = x_level(b, 0);
  X_CHECK(IDirect3DDevice9_CreateDepthStencilSurface(x_dev, 64, 64, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, FALSE, &ds, NULL));
  IDirect3DDevice9_SetRenderState(x_dev, D3DRS_ALPHABLENDENABLE, FALSE);
  return x_loop(frame);
}
