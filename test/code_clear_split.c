/* Clears between draws on the same targets (docs/gpu-4k.md, candidate 3).
 * On the backbuffer with its D24S8 depth-stencil: draw (colour, depth 0.3,
 * stencil 1), clear depth only, draw at depth 0.5 with LESS (must pass where
 * the first quad was), test stencil == 1 (must still hold the first quad),
 * clear colour only, draw at depth 0.6 with LESS (must fail where the 0.5
 * quad was), test stencil again. Each clear ends the open pass. */
#include "t_common.h"

static void stencil(BOOL on, D3DCMPFUNC func, D3DSTENCILOP pass) {
  IDirect3DDevice9_SetRenderState(t_dev, D3DRS_STENCILENABLE, on);
  IDirect3DDevice9_SetRenderState(t_dev, D3DRS_STENCILFUNC, func);
  IDirect3DDevice9_SetRenderState(t_dev, D3DRS_STENCILPASS, pass);
  IDirect3DDevice9_SetRenderState(t_dev, D3DRS_STENCILREF, 1);
}

static void depth(D3DCMPFUNC func, BOOL write) {
  IDirect3DDevice9_SetRenderState(t_dev, D3DRS_ZENABLE, D3DZB_TRUE);
  IDirect3DDevice9_SetRenderState(t_dev, D3DRS_ZFUNC, func);
  IDirect3DDevice9_SetRenderState(t_dev, D3DRS_ZWRITEENABLE, write);
}

static void frame(int n) {
  (void)n;
  IDirect3DDevice9_Clear(t_dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL,
                         D3DCOLOR_XRGB(24, 24, 40), 1.0f, 0);
  IDirect3DDevice9_BeginScene(t_dev);
  IDirect3DDevice9_SetVertexShader(t_dev, t_vs_pos);
  IDirect3DDevice9_SetPixelShader(t_dev, t_ps_color);

  depth(D3DCMP_LESS, TRUE);
  stencil(TRUE, D3DCMP_ALWAYS, D3DSTENCILOP_REPLACE);
  t_color(1.0f, 0.0f, 0.0f, 1.0f);
  t_quad(-0.8f, -0.95f, 0.2f, 0.95f, 0.3f);

  IDirect3DDevice9_Clear(t_dev, 0, NULL, D3DCLEAR_ZBUFFER, 0, 1.0f, 0);

  stencil(FALSE, D3DCMP_ALWAYS, D3DSTENCILOP_KEEP);
  t_color(0.0f, 1.0f, 0.0f, 1.0f);
  t_quad(-0.4f, -0.6f, 0.6f, 0.1f, 0.5f);

  depth(D3DCMP_ALWAYS, FALSE);
  stencil(TRUE, D3DCMP_EQUAL, D3DSTENCILOP_KEEP);
  t_color(0.0f, 0.0f, 1.0f, 1.0f);
  t_quad(-1.0f, -0.9f, 1.0f, -0.7f, 0.5f);

  IDirect3DDevice9_Clear(t_dev, 0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(64, 64, 64), 1.0f, 0);

  depth(D3DCMP_LESS, TRUE);
  stencil(FALSE, D3DCMP_ALWAYS, D3DSTENCILOP_KEEP);
  t_color(1.0f, 1.0f, 0.0f, 1.0f);
  t_quad(-1.0f, -0.5f, 1.0f, 0.0f, 0.6f);

  depth(D3DCMP_ALWAYS, FALSE);
  stencil(TRUE, D3DCMP_EQUAL, D3DSTENCILOP_KEEP);
  t_color(1.0f, 0.0f, 1.0f, 1.0f);
  t_quad(-1.0f, 0.7f, 1.0f, 0.9f, 0.5f);
  stencil(FALSE, D3DCMP_ALWAYS, D3DSTENCILOP_KEEP);
  IDirect3DDevice9_EndScene(t_dev);
}

int main(void) {
  t_init("d9mt_clear_split", 640, 480);
  t_tex_zw(0.0f, 1.0f);
  return t_loop(frame);
}
