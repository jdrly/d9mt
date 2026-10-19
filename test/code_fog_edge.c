/* Linear fog with FOGSTART == FOGEND (DXVK ef81c30f + 836b6418): fixed function, table
 * (pixel) fog, start = end = 0.5, red fog, green quads at depth 0.25 and 0.75. In front of
 * the fog distance a pixel is not fogged (green), behind it fully fogged (red). The fog
 * scale 1 / (end - start) was set to 0 for start == end, which fogged everything. */
#include "t_xfer.h"

struct V { float x, y, z; DWORD c; };

static void quad(float x0, float x1, float z) {
  struct V q[4] = {{x0, 0.5f, z, 0xff00ff00u}, {x1, 0.5f, z, 0xff00ff00u},
                   {x0, -0.5f, z, 0xff00ff00u}, {x1, -0.5f, z, 0xff00ff00u}};
  IDirect3DDevice9_DrawPrimitiveUP(x_dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof(q[0]));
}

static void frame(int n) {
  const float edge = 0.5f;
  IDirect3DDevice9_SetRenderTarget(x_dev, 0, x_backBuffer);
  IDirect3DDevice9_Clear(x_dev, 0, NULL, D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0);
  IDirect3DDevice9_SetVertexShader(x_dev, NULL);
  IDirect3DDevice9_SetPixelShader(x_dev, NULL);
  IDirect3DDevice9_SetFVF(x_dev, D3DFVF_XYZ | D3DFVF_DIFFUSE);
  IDirect3DDevice9_SetRenderState(x_dev, D3DRS_LIGHTING, FALSE);
  IDirect3DDevice9_SetRenderState(x_dev, D3DRS_FOGENABLE, TRUE);
  IDirect3DDevice9_SetRenderState(x_dev, D3DRS_FOGCOLOR, 0xffff0000u);
  IDirect3DDevice9_SetRenderState(x_dev, D3DRS_FOGTABLEMODE, D3DFOG_LINEAR);
  IDirect3DDevice9_SetRenderState(x_dev, D3DRS_FOGVERTEXMODE, D3DFOG_NONE);
  IDirect3DDevice9_SetRenderState(x_dev, D3DRS_FOGSTART, *(const DWORD *)&edge);
  IDirect3DDevice9_SetRenderState(x_dev, D3DRS_FOGEND, *(const DWORD *)&edge);
  IDirect3DDevice9_SetTextureStageState(x_dev, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
  IDirect3DDevice9_SetTextureStageState(x_dev, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
  IDirect3DDevice9_BeginScene(x_dev);
  quad(-0.9f, -0.1f, 0.25f);
  quad(0.1f, 0.9f, 0.75f);
  IDirect3DDevice9_EndScene(x_dev);
  IDirect3DDevice9_SetRenderState(x_dev, D3DRS_FOGENABLE, FALSE);
  if (n == 1) {
    XReadback r = x_read(x_backBuffer);
    x_expect("depth 0.25, in front of the fog", &r, 160, 240, 0xff00ff00u, 0x00ffffffu);
    x_expect("depth 0.75, behind the fog", &r, 480, 240, 0xffff0000u, 0x00ffffffu);
    x_done(&r);
    x_report("code_fog_edge");
  }
}

int main(void) {
  x_init("d9mt_fog_edge");
  IDirect3DDevice9_SetRenderState(x_dev, D3DRS_ALPHABLENDENABLE, FALSE);
  return x_loop(frame);
}
