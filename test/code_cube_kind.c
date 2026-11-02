/* A texture of another kind than a pixel shader's sampler declares (#150). The cube shader declares
 * s5 as a cube (dcl_cube s5), samples it in the +X direction and adds c0 (green); the 2D shader
 * declares s5 as 2D and does the same at (1, 0). Five quads, every frame:
 *   1  cube shader, s5 NULL: an unbound sampler reads zero, green;
 *   2  cube shader, s5 a 2D texture (red): a texture of the wrong kind is bound as the null cube,
 *      so it reads zero too, green;
 *   3  cube shader, s5 a cube texture (blue on every face, the control): cyan;
 *   4  2D shader, s5 the 2D texture: yellow;
 *   5  then the cube shader with that binding kept (no SetTexture in between): green.
 * d9mt encoded the 2D texture's ID in the cube sampler's argument-buffer word in 2 and 5: Metal's
 * shader validation reports "Invalid texture type MTLTextureType2D bound to shader, expected
 * MTLTextureTypeCube", and the GPU's read through it is undefined (the +X face, lod 0, keeps it
 * inside the 2D texture's one image). Run it under MTL_SHADER_VALIDATION=1 as well as plainly.
 * Checks in frames 1 and 3 (readback before Present). */
#include "t_xfer.h"

#define BG    0xff000080u
#define RED   0xffff0000u
#define BLUE  0xff0000ffu

/* ps_3_0: def c1, 1, 0, 0, 0; dcl_cube s5; mov r1, c1; texld r0, r1, s5; add oC0, r0, c0 */
static const DWORD cube_ps[] = {
  0xffff0300u,
  0x05000051u, 0xa00f0001u, 0x3f800000u, 0x00000000u, 0x00000000u, 0x00000000u,
  0x0200001fu, 0x98000000u, 0xa00f0805u,
  0x02000001u, 0x800f0001u, 0xa0e40001u,
  0x03000042u, 0x800f0000u, 0x80e40001u, 0xa0e40805u,
  0x03000002u, 0x800f0800u, 0x80e40000u, 0xa0e40000u,
  0x0000ffffu };

/* ps_3_0: the same with dcl_2d s5 */
static const DWORD flat_ps[] = {
  0xffff0300u,
  0x05000051u, 0xa00f0001u, 0x3f800000u, 0x00000000u, 0x00000000u, 0x00000000u,
  0x0200001fu, 0x90000000u, 0xa00f0805u,
  0x02000001u, 0x800f0001u, 0xa0e40001u,
  0x03000042u, 0x800f0000u, 0x80e40001u, 0xa0e40805u,
  0x03000002u, 0x800f0800u, 0x80e40000u, 0xa0e40000u,
  0x0000ffffu };

static IDirect3DPixelShader9 *ps_cube, *ps_flat;
static IDirect3DTexture9 *tex2d;
static IDirect3DCubeTexture9 *texcube;

static void fill(D3DLOCKED_RECT *lr, UINT size, DWORD colour) {
  for (UINT y = 0; y < size; y++) {
    DWORD *row = (DWORD *)((BYTE *)lr->pBits + (size_t)y * lr->Pitch);
    for (UINT x = 0; x < size; x++)
      row[x] = colour;
  }
}

/* DEFAULT-pool textures written by UpdateTexture (as code_unwritten_ab's), so neither is bound as
 * null for being unwritten. */
static void make_textures(void) {
  IDirect3DTexture9 *src = NULL;
  D3DLOCKED_RECT lr;
  X_CHECK(IDirect3DDevice9_CreateTexture(x_dev, 64, 64, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &src, NULL));
  X_CHECK(IDirect3DTexture9_LockRect(src, 0, &lr, NULL, 0));
  fill(&lr, 64, RED);
  IDirect3DTexture9_UnlockRect(src, 0);
  X_CHECK(IDirect3DDevice9_CreateTexture(x_dev, 64, 64, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &tex2d, NULL));
  X_CHECK(IDirect3DDevice9_UpdateTexture(x_dev, (IDirect3DBaseTexture9 *)src, (IDirect3DBaseTexture9 *)tex2d));
  IDirect3DTexture9_Release(src);

  IDirect3DCubeTexture9 *csrc = NULL;
  X_CHECK(IDirect3DDevice9_CreateCubeTexture(x_dev, 16, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &csrc, NULL));
  for (int f = 0; f < 6; f++) {
    X_CHECK(IDirect3DCubeTexture9_LockRect(csrc, (D3DCUBEMAP_FACES)f, 0, &lr, NULL, 0));
    fill(&lr, 16, BLUE);
    IDirect3DCubeTexture9_UnlockRect(csrc, (D3DCUBEMAP_FACES)f, 0);
  }
  X_CHECK(IDirect3DDevice9_CreateCubeTexture(x_dev, 16, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &texcube, NULL));
  X_CHECK(IDirect3DDevice9_UpdateTexture(x_dev, (IDirect3DBaseTexture9 *)csrc, (IDirect3DBaseTexture9 *)texcube));
  IDirect3DCubeTexture9_Release(csrc);
}

static void quad(IDirect3DPixelShader9 *ps, int set, IDirect3DBaseTexture9 *t, float x0, float x1) {
  IDirect3DDevice9_SetPixelShader(x_dev, ps);
  if (set)
    IDirect3DDevice9_SetTexture(x_dev, 5, t);
  x_draw(NULL, x0, -0.5f, x1, 0.5f);
}

static void frame(int n) {
  static const float green[4] = {0, 1, 0, 1};
  IDirect3DDevice9_SetRenderTarget(x_dev, 0, x_backBuffer);
  IDirect3DDevice9_Clear(x_dev, 0, NULL, D3DCLEAR_TARGET, BG, 1.0f, 0);
  IDirect3DDevice9_SetPixelShaderConstantF(x_dev, 0, green, 1);
  IDirect3DDevice9_BeginScene(x_dev);
  quad(ps_cube, 1, NULL, -0.95f, -0.6f);
  quad(ps_cube, 1, (IDirect3DBaseTexture9 *)tex2d, -0.55f, -0.2f);
  quad(ps_cube, 1, (IDirect3DBaseTexture9 *)texcube, -0.15f, 0.2f);
  quad(ps_flat, 1, (IDirect3DBaseTexture9 *)tex2d, 0.25f, 0.6f);
  quad(ps_cube, 0, NULL, 0.65f, 0.95f);
  IDirect3DDevice9_EndScene(x_dev);
  IDirect3DDevice9_SetTexture(x_dev, 5, NULL);

  if (n == 1 || n == 3) {
    XReadback r = x_read(x_backBuffer);
    x_expect("cube sampler, NULL", &r, 56, 240, 0xff00ff00u, 0x00ffffffu);
    x_expect("cube sampler, a 2D texture", &r, 184, 240, 0xff00ff00u, 0x00ffffffu);
    x_expect("cube sampler, a cube texture (control)", &r, 312, 240, 0xff00ffffu, 0x00ffffffu);
    x_expect("2D sampler, the 2D texture", &r, 440, 240, 0xffffff00u, 0x00ffffffu);
    x_expect("cube sampler after a shader switch that kept the 2D texture", &r, 576, 240, 0xff00ff00u, 0x00ffffffu);
    x_expect("background", &r, 320, 40, BG, 0x00ffffffu);
    x_done(&r);
    if (n == 3)
      x_report("code_cube_kind");
  }
}

int main(void) {
  x_init("d9mt_cube_kind");
  X_CHECK(IDirect3DDevice9_CreatePixelShader(x_dev, cube_ps, &ps_cube));
  X_CHECK(IDirect3DDevice9_CreatePixelShader(x_dev, flat_ps, &ps_flat));
  make_textures();
  IDirect3DDevice9_SetRenderState(x_dev, D3DRS_ALPHABLENDENABLE, FALSE);
  IDirect3DDevice9_SetSamplerState(x_dev, 5, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
  IDirect3DDevice9_SetSamplerState(x_dev, 5, D3DSAMP_MINFILTER, D3DTEXF_POINT);
  IDirect3DDevice9_SetSamplerState(x_dev, 5, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
  IDirect3DDevice9_SetSamplerState(x_dev, 5, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
  IDirect3DDevice9_SetSamplerState(x_dev, 5, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
  return x_loop(frame);
}
