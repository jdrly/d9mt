/* An unbound volume sampler in a programmable pixel shader (#62). The shader declares s3 as a
 * volume and samples it with no texture bound; it outputs that sample plus c0 (green). An
 * unbound slot reads zero, so the quad is green.
 * d9mt bound the argument table's 2D null texture there (the fixed-function shader's
 * convention: 2D, and cube at fragment slots 8-15), so the GPU sampled a 2D texture as a 3D one:
 * Metal's shader validation reports it as a binding of the wrong texture type. Fixed, the slot
 * gets the null 3D texture. Run it under MTL_SHADER_VALIDATION=1 as well as plainly. */
#include "t_xfer.h"

/* ps_3_0: dcl_texcoord v0; dcl_volume s3; texld r0, v0, s3; add oC0, r0, c0 */
static const DWORD volume_ps[] = {
  0xffff0300u,
  0x0200001fu, 0x80000005u, 0x900f0000u,
  0x0200001fu, 0xa0000000u, 0xa00f0803u,
  0x03000042u, 0x800f0000u, 0x90e40000u, 0xa0e40803u,
  0x03000002u, 0x800f0800u, 0x80e40000u, 0xa0e40000u,
  0x0000ffffu };

static IDirect3DPixelShader9 *ps;

static void frame(int n) {
  static const float green[4] = {0, 1, 0, 1};
  IDirect3DDevice9_SetRenderTarget(x_dev, 0, x_backBuffer);
  IDirect3DDevice9_Clear(x_dev, 0, NULL, D3DCLEAR_TARGET, 0xff000080u, 1.0f, 0);
  IDirect3DDevice9_SetPixelShader(x_dev, ps);
  IDirect3DDevice9_SetPixelShaderConstantF(x_dev, 0, green, 1);
  IDirect3DDevice9_SetTexture(x_dev, 3, NULL);
  IDirect3DDevice9_BeginScene(x_dev);
  x_draw(NULL, -0.5f, -0.5f, 0.5f, 0.5f);
  IDirect3DDevice9_EndScene(x_dev);
  if (n == 1) {
    XReadback r = x_read(x_backBuffer);
    x_expect("an unbound volume sampler reads zero", &r, 320, 240, 0xff00ff00u, 0x00ffffffu);
    x_expect("outside the quad", &r, 40, 40, 0xff000080u, 0x00ffffffu);
    x_done(&r);
    x_report("code_null_volume");
  }
}

int main(void) {
  x_init("d9mt_null_volume");
  X_CHECK(IDirect3DDevice9_CreatePixelShader(x_dev, volume_ps, &ps));
  IDirect3DDevice9_SetRenderState(x_dev, D3DRS_ALPHABLENDENABLE, FALSE);
  return x_loop(frame);
}
