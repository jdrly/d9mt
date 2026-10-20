/* User clip planes and pre-transformed vertices (DXVK 3a4c6fa3): clip plane 0 = (0, 0, 0, -1)
 * clips every vertex with w = 1. A green XYZ quad (transformed by the fixed-function vertex
 * pipeline) must be clipped away; a red XYZRHW quad must not be: clip planes don't apply
 * to pre-transformed vertices. The fixed-function vertex shader computed clip distances
 * for both. */
#include "t_xfer.h"

struct VT { float x, y, z, rhw; DWORD c; };
struct V { float x, y, z; DWORD c; };

static void frame(int n) {
  static const float plane[4] = {0.0f, 0.0f, 0.0f, -1.0f};
  struct V q[4] = {{-0.9f, 0.5f, 0.5f, 0xff00ff00u}, {-0.1f, 0.5f, 0.5f, 0xff00ff00u},
                   {-0.9f, -0.5f, 0.5f, 0xff00ff00u}, {-0.1f, -0.5f, 0.5f, 0xff00ff00u}};
  struct VT t[4] = {{352, 120, 0.5f, 1, 0xffff0000u}, {608, 120, 0.5f, 1, 0xffff0000u},
                    {352, 360, 0.5f, 1, 0xffff0000u}, {608, 360, 0.5f, 1, 0xffff0000u}};
  IDirect3DDevice9_SetRenderTarget(x_dev, 0, x_backBuffer);
  IDirect3DDevice9_Clear(x_dev, 0, NULL, D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0);
  IDirect3DDevice9_SetVertexShader(x_dev, NULL);
  IDirect3DDevice9_SetPixelShader(x_dev, NULL);
  IDirect3DDevice9_SetTexture(x_dev, 0, NULL);
  IDirect3DDevice9_SetTextureStageState(x_dev, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
  IDirect3DDevice9_SetTextureStageState(x_dev, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
  IDirect3DDevice9_SetClipPlane(x_dev, 0, plane);
  IDirect3DDevice9_SetRenderState(x_dev, D3DRS_CLIPPLANEENABLE, D3DCLIPPLANE0);
  IDirect3DDevice9_BeginScene(x_dev);
  IDirect3DDevice9_SetFVF(x_dev, D3DFVF_XYZ | D3DFVF_DIFFUSE);
  IDirect3DDevice9_DrawPrimitiveUP(x_dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof(q[0]));
  IDirect3DDevice9_SetFVF(x_dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
  IDirect3DDevice9_DrawPrimitiveUP(x_dev, D3DPT_TRIANGLESTRIP, 2, t, sizeof(t[0]));
  IDirect3DDevice9_EndScene(x_dev);
  IDirect3DDevice9_SetRenderState(x_dev, D3DRS_CLIPPLANEENABLE, 0);
  if (n == 1) {
    XReadback r = x_read(x_backBuffer);
    x_expect("XYZ quad behind clip plane 0", &r, 160, 240, 0xff000000u, 0x00ffffffu);
    x_expect("XYZRHW quad, clip planes don't apply", &r, 480, 240, 0xffff0000u, 0x00ffffffu);
    x_done(&r);
    x_report("code_ff_clip_positiont");
  }
}

int main(void) {
  x_init("d9mt_ff_clip_positiont");
  IDirect3DDevice9_SetRenderState(x_dev, D3DRS_ALPHABLENDENABLE, FALSE);
  IDirect3DDevice9_SetRenderState(x_dev, D3DRS_LIGHTING, FALSE);
  return x_loop(frame);
}
