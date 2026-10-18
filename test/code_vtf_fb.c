/* Verifier test (perf/gpu-4k, candidate 1): vertex texture fetch of a render
 * target rendered by an earlier pass, by a draw that also has an unresolvable
 * render-target feedback hazard (its pixel shader samples the bound render
 * target), in a pass that has already emitted such feedback barriers
 * (EmitFeedbackLoopBarriers -> emitGraphicsBarrier, a producer barrier in the
 * open encoder). A producer barrier orders later encoders only, so it must not
 * stand in for the vertex fence of later draws in the same encoder.
 *   pass N: a tiny VTF draw of texB (so the vertex sampler holds texB, not
 *           rt0), then rt0 <- heavy pixel shader writes value v(n)
 *   pass P: RT = A, cleared; s0 = A (feedback). Three draws with blend
 *           ZERO/ONE (A unchanged). Then rt0 on the vertex sampler and a
 *           draw with blend ZERO/ZERO (black) whose x offset is fetched from
 *           rt0 - still sampling A, so still the same pass and encoder.
 *   pass Q: backbuffer <- A
 * A vertex stage that runs before pass N's fragment work finishes reads the
 * clear (0) or last frame's value and puts the black quad elsewhere.
 * VTF_NOFB=1: s0 = texB in pass P (no feedback; a plain VTF hazard).
 * The race doesn't show on an M5 Max either way; what the test checks there
 * is the mechanism: with the fix, the VTF draw in pass P takes the pass split
 * and fence (d9mt logs "pass split for a vertex fence" once, PROF vtxFence),
 * and before it (the feedback barrier started a new vertex epoch) it took
 * neither. The hashes must match main's. */
#include "t_common.h"
#include <stdlib.h>

static IDirect3DTexture9 *rt0, *texA, *texB;
static int noFb;
static IDirect3DSurface9 *rt0Surf, *surfA, *backBuffer;

static void frame(int n) {
  IDirect3DDevice9_BeginScene(t_dev);
  /* pass N */
  IDirect3DDevice9_SetTexture(t_dev, 0, NULL);
  IDirect3DDevice9_SetRenderTarget(t_dev, 0, rt0Surf);
  IDirect3DDevice9_SetDepthStencilSurface(t_dev, NULL);
  IDirect3DDevice9_SetRenderState(t_dev, D3DRS_ZENABLE, D3DZB_FALSE);
  IDirect3DDevice9_SetRenderState(t_dev, D3DRS_ALPHABLENDENABLE, FALSE);
  IDirect3DDevice9_Clear(t_dev, 0, NULL, D3DCLEAR_TARGET, 0, 1.0f, 0);
  IDirect3DDevice9_SetTexture(t_dev, D3DVERTEXTEXTURESAMPLER0, (IDirect3DBaseTexture9 *)texB);
  IDirect3DDevice9_SetVertexShader(t_dev, t_vs_vtf);
  IDirect3DDevice9_SetPixelShader(t_dev, t_ps_color);
  t_color(0.0f, 0.0f, 0.0f, 1.0f);
  t_quad(-0.01f, -0.01f, 0.01f, 0.01f, 0.5f);
  IDirect3DDevice9_SetVertexShader(t_dev, t_vs_pos);
  IDirect3DDevice9_SetPixelShader(t_dev, t_ps_heavy);
  t_color(0.1f * (float)(n % 5) + 0.1f, 0.0f, 0.0f, 1.0f);
  t_quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.5f);

  /* pass P: feedback draws first (invisible) */
  IDirect3DDevice9_SetRenderTarget(t_dev, 0, surfA);
  IDirect3DDevice9_Clear(t_dev, 0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(16, 16, 48), 1.0f, 0);
  IDirect3DDevice9_SetTexture(t_dev, 0, (IDirect3DBaseTexture9 *)(noFb ? texB : texA));
  IDirect3DDevice9_SetVertexShader(t_dev, t_vs_vtf);
  IDirect3DDevice9_SetPixelShader(t_dev, t_ps_sample);
  IDirect3DDevice9_SetRenderState(t_dev, D3DRS_ALPHABLENDENABLE, TRUE);
  IDirect3DDevice9_SetRenderState(t_dev, D3DRS_SRCBLEND, D3DBLEND_ZERO);
  IDirect3DDevice9_SetRenderState(t_dev, D3DRS_DESTBLEND, D3DBLEND_ONE);
  for (int i = 0; i < 3; i++)
    t_quad(0.2f, 0.2f, 0.4f, 0.4f, 0.5f);
  /* the VTF draw, same pass, same feedback state: a black quad */
  IDirect3DDevice9_SetTexture(t_dev, D3DVERTEXTEXTURESAMPLER0, (IDirect3DBaseTexture9 *)rt0);
  IDirect3DDevice9_SetRenderState(t_dev, D3DRS_DESTBLEND, D3DBLEND_ZERO);
  t_quad(-0.9f, -0.2f, -0.6f, 0.2f, 0.5f);
  IDirect3DDevice9_SetRenderState(t_dev, D3DRS_ALPHABLENDENABLE, FALSE);

  /* pass Q */
  IDirect3DDevice9_SetTexture(t_dev, D3DVERTEXTEXTURESAMPLER0, NULL);
  IDirect3DDevice9_SetRenderTarget(t_dev, 0, backBuffer);
  IDirect3DDevice9_SetTexture(t_dev, 0, (IDirect3DBaseTexture9 *)texA);
  IDirect3DDevice9_SetVertexShader(t_dev, t_vs_pos);
  IDirect3DDevice9_SetPixelShader(t_dev, t_ps_sample);
  t_quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.5f);
  IDirect3DDevice9_EndScene(t_dev);
}

int main(void) {
  t_init("d9mt_vtf_fb", 640, 480);
  T_CHECK(IDirect3DDevice9_CreateTexture(t_dev, 1024, 1024, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F,
                                         D3DPOOL_DEFAULT, &rt0, NULL));
  rt0Surf = t_level0(rt0);
  T_CHECK(IDirect3DDevice9_CreateTexture(t_dev, 640, 480, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                         D3DPOOL_DEFAULT, &texA, NULL));
  surfA = t_level0(texA);
  T_CHECK(IDirect3DDevice9_CreateTexture(t_dev, 64, 64, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F,
                                         D3DPOOL_DEFAULT, &texB, NULL));
  noFb = getenv("VTF_NOFB") != NULL;
  T_CHECK(IDirect3DDevice9_GetRenderTarget(t_dev, 0, &backBuffer));
  IDirect3DDevice9_SetSamplerState(t_dev, D3DVERTEXTEXTURESAMPLER0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
  IDirect3DDevice9_SetSamplerState(t_dev, D3DVERTEXTEXTURESAMPLER0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
  IDirect3DDevice9_SetSamplerState(t_dev, D3DVERTEXTEXTURESAMPLER0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
  t_tex_zw(0.0f, 1.0f);
  return t_loop(frame);
}
