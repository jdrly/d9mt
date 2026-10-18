/* Verifier test (perf/gpu-4k, candidates 1 and 3): a pass split right after
 * a long fragment phase, on the same targets, so the second half's Load of
 * the attachments must wait for the first half's stores.
 *  A: rtA (1024^2) <- heavy pixel shader, value v(n); an event query with
 *     GetData(FLUSH) submits mid-pass (split by submit); a small quad; then
 *     the backbuffer samples rtA.
 *  B: rtB + its own D24S8 depth: heavy shader at z=0.5 (depth written), a
 *     depth-only Clear(ZBUFFER, 1.0) ends the pass (candidate 3 drops the
 *     depth store, keeps the colour store), a quad at z=0.7 over the left half
 *     (passes the cleared depth), the colour of the right half must survive.
 *  C: a heavy pass on rtC, then a same-size StretchRect (a copy) to rtD: the
 *     copy must wait for the pass's stores.
 * Negative controls: an end-of-pass barrier without the Fragment stage
 * (A and B load before the stores land) or without Blit|Dispatch (C copies
 * before them) changes the hashes. */
#include "t_common.h"

static IDirect3DTexture9 *rtA, *rtB, *rtC, *rtD;
static IDirect3DSurface9 *rtAS, *rtBS, *rtCS, *rtDS, *dsB, *backBuffer;
static IDirect3DQuery9 *ev;

static void frame(int n) {
  IDirect3DDevice9_BeginScene(t_dev);
  IDirect3DDevice9_SetTexture(t_dev, 0, NULL);
  /* A */
  IDirect3DDevice9_SetRenderTarget(t_dev, 0, rtAS);
  IDirect3DDevice9_SetDepthStencilSurface(t_dev, NULL);
  IDirect3DDevice9_SetRenderState(t_dev, D3DRS_ZENABLE, D3DZB_FALSE);
  IDirect3DDevice9_Clear(t_dev, 0, NULL, D3DCLEAR_TARGET, 0, 1.0f, 0);
  IDirect3DDevice9_SetVertexShader(t_dev, t_vs_pos);
  IDirect3DDevice9_SetPixelShader(t_dev, t_ps_heavy);
  t_color(0.15f * (float)(n % 5) + 0.2f, 0.5f, 0.1f, 1.0f);
  t_quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.5f);
  IDirect3DQuery9_Issue(ev, D3DISSUE_END);
  (void)IDirect3DQuery9_GetData(ev, NULL, 0, D3DGETDATA_FLUSH);
  IDirect3DDevice9_SetPixelShader(t_dev, t_ps_color);
  t_color(1.0f, 1.0f, 1.0f, 1.0f);
  t_quad(-0.1f, -0.1f, 0.1f, 0.1f, 0.5f);

  /* B */
  IDirect3DDevice9_SetRenderTarget(t_dev, 0, rtBS);
  IDirect3DDevice9_SetDepthStencilSurface(t_dev, dsB);
  IDirect3DDevice9_SetRenderState(t_dev, D3DRS_ZENABLE, D3DZB_TRUE);
  IDirect3DDevice9_SetRenderState(t_dev, D3DRS_ZWRITEENABLE, TRUE);
  IDirect3DDevice9_SetRenderState(t_dev, D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
  IDirect3DDevice9_Clear(t_dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0, 1.0f, 0);
  IDirect3DDevice9_SetPixelShader(t_dev, t_ps_heavy);
  t_color(0.1f, 0.15f * (float)((n + 2) % 5) + 0.2f, 0.6f, 1.0f);
  t_quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.5f);
  IDirect3DDevice9_Clear(t_dev, 0, NULL, D3DCLEAR_ZBUFFER, 0, 1.0f, 0);
  IDirect3DDevice9_SetPixelShader(t_dev, t_ps_color);
  t_color(1.0f, 0.5f, 0.0f, 1.0f);
  t_quad(-1.0f, -1.0f, 0.0f, 1.0f, 0.7f);
  IDirect3DDevice9_SetRenderState(t_dev, D3DRS_ZENABLE, D3DZB_FALSE);

  /* C: heavy pass on rtC, then a same-size StretchRect (copy) to rtD */
  IDirect3DDevice9_SetRenderTarget(t_dev, 0, rtCS);
  IDirect3DDevice9_SetDepthStencilSurface(t_dev, NULL);
  IDirect3DDevice9_Clear(t_dev, 0, NULL, D3DCLEAR_TARGET, 0, 1.0f, 0);
  IDirect3DDevice9_SetPixelShader(t_dev, t_ps_heavy);
  t_color(0.6f, 0.1f, 0.15f * (float)((n + 3) % 5) + 0.2f, 1.0f);
  t_quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.5f);
  IDirect3DDevice9_EndScene(t_dev);
  T_CHECK(IDirect3DDevice9_StretchRect(t_dev, rtCS, NULL, rtDS, NULL, D3DTEXF_NONE));
  IDirect3DDevice9_BeginScene(t_dev);

  IDirect3DDevice9_SetRenderTarget(t_dev, 0, backBuffer);
  IDirect3DDevice9_SetDepthStencilSurface(t_dev, NULL);
  IDirect3DDevice9_Clear(t_dev, 0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(24, 24, 40), 1.0f, 0);
  IDirect3DDevice9_SetPixelShader(t_dev, t_ps_sample);
  IDirect3DDevice9_SetTexture(t_dev, 0, (IDirect3DBaseTexture9 *)rtA);
  t_quad(-0.95f, -0.9f, -0.05f, 0.9f, 0.5f);
  IDirect3DDevice9_SetTexture(t_dev, 0, (IDirect3DBaseTexture9 *)rtB);
  t_quad(0.05f, -0.9f, 0.95f, 0.0f, 0.5f);
  IDirect3DDevice9_SetTexture(t_dev, 0, (IDirect3DBaseTexture9 *)rtD);
  t_quad(0.05f, 0.05f, 0.95f, 0.9f, 0.5f);
  IDirect3DDevice9_SetTexture(t_dev, 0, NULL);
  IDirect3DDevice9_EndScene(t_dev);
}

int main(void) {
  t_init("d9mt_split_load", 640, 480);
  T_CHECK(IDirect3DDevice9_CreateTexture(t_dev, 1024, 1024, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                         D3DPOOL_DEFAULT, &rtA, NULL));
  rtAS = t_level0(rtA);
  T_CHECK(IDirect3DDevice9_CreateTexture(t_dev, 1024, 1024, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                         D3DPOOL_DEFAULT, &rtB, NULL));
  rtBS = t_level0(rtB);
  T_CHECK(IDirect3DDevice9_CreateTexture(t_dev, 1024, 1024, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                         D3DPOOL_DEFAULT, &rtC, NULL));
  rtCS = t_level0(rtC);
  T_CHECK(IDirect3DDevice9_CreateTexture(t_dev, 1024, 1024, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                         D3DPOOL_DEFAULT, &rtD, NULL));
  rtDS = t_level0(rtD);
  T_CHECK(IDirect3DDevice9_CreateDepthStencilSurface(t_dev, 1024, 1024, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0,
                                                     FALSE, &dsB, NULL));
  T_CHECK(IDirect3DDevice9_CreateQuery(t_dev, D3DQUERYTYPE_EVENT, &ev));
  T_CHECK(IDirect3DDevice9_GetRenderTarget(t_dev, 0, &backBuffer));
  t_tex_zw(0.0f, 1.0f);
  return t_loop(frame);
}
