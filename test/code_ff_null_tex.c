/* A fixed-function stage that samples no texture (DXVK 7d248817): stage 0 PREMODULATE with
 * the diffuse colour (green), stage 1 SELECTARG1 CURRENT with no texture bound. PREMODULATE
 * multiplies stage 1's CURRENT by stage 1's texture value, which for a null texture must
 * not change it: the quad stays green. The FF shader returned (0, 0, 0, 0) for a null
 * texture (an AMD quirk upstream reverted after Commandos 3 broke), and d9mt sampled the
 * empty texture slot instead. */
#include "t_xfer.h"

struct V { float x, y, z, rhw; DWORD c; };

static void frame(int n) {
  struct V q[4] = {{160, 120, 0.5f, 1, 0xff00ff00u}, {480, 120, 0.5f, 1, 0xff00ff00u},
                   {160, 360, 0.5f, 1, 0xff00ff00u}, {480, 360, 0.5f, 1, 0xff00ff00u}};
  IDirect3DDevice9_SetRenderTarget(x_dev, 0, x_backBuffer);
  IDirect3DDevice9_Clear(x_dev, 0, NULL, D3DCLEAR_TARGET, 0xff000080u, 1.0f, 0);
  IDirect3DDevice9_SetVertexShader(x_dev, NULL);
  IDirect3DDevice9_SetPixelShader(x_dev, NULL);
  IDirect3DDevice9_SetFVF(x_dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
  IDirect3DDevice9_SetTexture(x_dev, 0, NULL);
  IDirect3DDevice9_SetTexture(x_dev, 1, NULL);
  IDirect3DDevice9_SetTextureStageState(x_dev, 0, D3DTSS_COLOROP, D3DTOP_PREMODULATE);
  IDirect3DDevice9_SetTextureStageState(x_dev, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
  IDirect3DDevice9_SetTextureStageState(x_dev, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
  IDirect3DDevice9_SetTextureStageState(x_dev, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
  IDirect3DDevice9_SetTextureStageState(x_dev, 1, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
  IDirect3DDevice9_SetTextureStageState(x_dev, 1, D3DTSS_COLORARG1, D3DTA_CURRENT);
  IDirect3DDevice9_SetTextureStageState(x_dev, 1, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
  IDirect3DDevice9_SetTextureStageState(x_dev, 1, D3DTSS_ALPHAARG1, D3DTA_CURRENT);
  IDirect3DDevice9_SetTextureStageState(x_dev, 2, D3DTSS_COLOROP, D3DTOP_DISABLE);
  IDirect3DDevice9_BeginScene(x_dev);
  IDirect3DDevice9_DrawPrimitiveUP(x_dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof(q[0]));
  IDirect3DDevice9_EndScene(x_dev);
  if (n == 1) {
    XReadback r = x_read(x_backBuffer);
    x_expect("PREMODULATE onto a stage without a texture", &r, 320, 240, 0xff00ff00u, 0x00ffffffu);
    x_expect("outside the quad", &r, 40, 40, 0xff000080u, 0x00ffffffu);
    x_done(&r);
    x_report("code_ff_null_tex");
  }
}

int main(void) {
  x_init("d9mt_ff_null_tex");
  IDirect3DDevice9_SetRenderState(x_dev, D3DRS_ALPHABLENDENABLE, FALSE);
  IDirect3DDevice9_SetRenderState(x_dev, D3DRS_LIGHTING, FALSE);
  return x_loop(frame);
}
