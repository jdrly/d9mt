/* ColorFill (#14), checked against the expected pixels: whole surface and a
 * rect, on an A8R8G8B8 render-target texture, an X8R8G8B8 render-target
 * surface, R5G6B5, a lockable offscreen plain surface, a cube face and a mip
 * level; and on DXT1 (opaque, and alpha below 128: transparent black), DXT3
 * and DXT5 textures, whole and a rect that isn't block-aligned (it fills
 * every 4x4 block it touches), read back by sampling them point-filtered into
 * a render target; on DXT1 and DXT5 off-screen plain surfaces, whose blocks
 * LockRect returns byte for byte. A texture without D3DUSAGE_RENDERTARGET
 * (D3D9 doesn't allow ColorFill there) is left alone without a crash.
 * Colours are exact in R5G6B5. */
#include "t_xfer.h"

#define C_A  0xff848284u  /* 132,130,132 */
#define C_B  0xffff00ffu
#define C_C  0x7f00ff00u  /* alpha 0x7f: DXT1 transparent */
#define C_D  0x88ff0000u  /* DXT3: alpha nibble 8 -> 0x88 */
#define C_E  0x5a0000ffu  /* DXT5: alpha 0x5a */

static IDirect3DTexture9 *rtTex, *mipTex, *dxt[4], *probe;
static IDirect3DSurface9 *rtS, *x8S, *s565, *plainS, *cubeFace, *mip1, *probeS, *plainDxt1, *plainDxt5, *notRtS;
static IDirect3DTexture9 *notRt;
static IDirect3DCubeTexture9 *cube;
static BOOL has565;
static const D3DFORMAT kDxt[4] = {D3DFMT_DXT1, D3DFMT_DXT1, D3DFMT_DXT3, D3DFMT_DXT5};
static const DWORD kDxtFull[4] = {C_A, C_C, C_D, C_E};

/* What sampling a block-compressed texel filled with `c` gives (A8R8G8B8). */
static DWORD dxt_expect(int i, DWORD c) {
  const DWORD a = c >> 24;
  if (i == 0)
    return (c & 0x00ffffffu) | 0xff000000u;
  if (i == 1)
    return a < 128 ? 0u : (c | 0xff000000u);
  if (i == 2) {
    const DWORD a4 = (a * 15 + 127) / 255;
    return (c & 0x00ffffffu) | ((a4 * 17) << 24);
  }
  return c;
}

static void fill(IDirect3DSurface9 *s, const RECT *r, DWORD c) {
  X_CHECK(IDirect3DDevice9_ColorFill(x_dev, s, r, c));
}

static void frame(int n) {
  RECT r = {16, 8, 48, 40};
  fill(rtS, NULL, C_A);
  fill(rtS, &r, C_B);
  fill(x8S, NULL, C_B);
  if (has565)
    fill(s565, NULL, C_A);
  fill(plainS, NULL, C_A);
  fill(plainS, &r, C_B);
  fill(cubeFace, NULL, C_B);
  fill(mip1, NULL, C_A);
  fill(plainDxt1, NULL, C_A);
  fill(plainDxt5, NULL, C_E);
  fill(notRtS, NULL, C_B);
  RECT br = {5, 5, 10, 10};  /* blocks 1-2 in x and y: texels 4..11 */
  for (int i = 0; i < 4; i++) {
    IDirect3DSurface9 *s = x_level(dxt[i], 0);
    fill(s, NULL, kDxtFull[i]);
    fill(s, &br, C_B);
    IDirect3DSurface9_Release(s);
  }

  IDirect3DDevice9_SetRenderTarget(x_dev, 0, x_backBuffer);
  IDirect3DDevice9_Clear(x_dev, 0, NULL, D3DCLEAR_TARGET, 0xff181828u, 1.0f, 0);
  IDirect3DDevice9_BeginScene(x_dev);
  x_draw((IDirect3DBaseTexture9 *)rtTex, -0.95f, 0.05f, -0.55f, 0.95f);
  for (int i = 0; i < 4; i++)
    x_draw((IDirect3DBaseTexture9 *)dxt[i], -0.5f + 0.35f * (float)i, 0.05f, -0.2f + 0.35f * (float)i, 0.45f);
  IDirect3DDevice9_EndScene(x_dev);

  if (n != 1)
    return;
  XReadback rb;
  rb = x_read(rtS);
  x_expect("rt full", &rb, 4, 4, C_A, 0xffffffffu);
  x_expect("rt full", &rb, 60, 60, C_A, 0xffffffffu);
  x_expect("rt rect", &rb, 20, 12, C_B, 0xffffffffu);
  x_expect("rt rect", &rb, 46, 38, C_B, 0xffffffffu);
  x_expect("rt rect edge", &rb, 48, 20, C_A, 0xffffffffu);
  x_expect("rt rect edge", &rb, 20, 40, C_A, 0xffffffffu);
  x_done(&rb);
  rb = x_read(x8S);
  x_expect("X8 surface", &rb, 10, 10, C_B, 0x00ffffffu);
  x_done(&rb);
  if (has565) {
    rb = x_read(s565);
    x_expect("R5G6B5", &rb, 10, 10, 0x8410, 0xffff);
    x_done(&rb);
  }
  rb = x_read(plainS);
  x_expect("plain surface", &rb, 4, 4, C_A, 0xffffffffu);
  x_expect("plain surface rect", &rb, 30, 30, C_B, 0xffffffffu);
  x_done(&rb);
  rb = x_read(cubeFace);
  x_expect("cube face", &rb, 16, 16, C_B, 0xffffffffu);
  x_done(&rb);
  rb = x_read(mip1);
  x_expect("mip 1", &rb, 16, 16, C_A, 0xffffffffu);
  x_done(&rb);

  for (int i = 0; i < 4; i++) {
    IDirect3DDevice9_SetRenderTarget(x_dev, 0, probeS);
    IDirect3DDevice9_Clear(x_dev, 0, NULL, D3DCLEAR_TARGET, 0xff102030u, 1.0f, 0);
    IDirect3DDevice9_BeginScene(x_dev);
    x_draw((IDirect3DBaseTexture9 *)dxt[i], -1.0f, -1.0f, 1.0f, 1.0f);
    IDirect3DDevice9_EndScene(x_dev);
    char what[48];
    rb = x_read(probeS);
    snprintf(what, sizeof(what), "DXT%d%s full", i == 3 ? 5 : i == 2 ? 3 : 1, i == 1 ? " alpha" : "");
    x_expect(what, &rb, 1, 1, dxt_expect(i, kDxtFull[i]), 0xffffffffu);
    x_expect(what, &rb, 14, 14, dxt_expect(i, kDxtFull[i]), 0xffffffffu);
    x_expect(what, &rb, 13, 2, dxt_expect(i, kDxtFull[i]), 0xffffffffu);
    snprintf(what, sizeof(what), "DXT%d%s rect", i == 3 ? 5 : i == 2 ? 3 : 1, i == 1 ? " alpha" : "");
    x_expect(what, &rb, 5, 5, dxt_expect(i, C_B), 0xffffffffu);
    x_expect(what, &rb, 10, 10, dxt_expect(i, C_B), 0xffffffffu);
    x_expect(what, &rb, 5, 10, dxt_expect(i, C_B), 0xffffffffu);
    x_done(&rb);
  }
  {
    /* C_A = 132,130,132 -> RGB565 0x8410: color0 = color1 = 0x8410, indices 0 */
    static const unsigned char want1[8] = {0x10, 0x84, 0x10, 0x84, 0, 0, 0, 0};
    /* C_E: alpha0 = alpha1 = 0x5a, indices 0; blue -> 0x001f */
    static const unsigned char want5[16] = {0x5a, 0x5a, 0, 0, 0, 0, 0, 0, 0x1f, 0x00, 0x1f, 0x00, 0, 0, 0, 0};
    IDirect3DSurface9 *plains[2] = {plainDxt1, plainDxt5};
    const unsigned char *wants[2] = {want1, want5};
    for (int i = 0; i < 2; i++) {
      D3DLOCKED_RECT lr;
      X_CHECK(IDirect3DSurface9_LockRect(plains[i], &lr, NULL, D3DLOCK_READONLY));
      const UINT bytes = i ? 16u : 8u;
      for (UINT by = 0; by < 4; by++) {
        for (UINT bx = 0; bx < 4; bx++) {
          const unsigned char *blk = (const unsigned char *)lr.pBits + (size_t)by * lr.Pitch + bx * bytes;
          x_checks++;
          if (memcmp(blk, wants[i], bytes) != 0) {
            x_failed++;
            printf("FAIL DXT%d plain surface block %u,%u: %02x %02x %02x %02x %02x %02x %02x %02x ...\n", i ? 5 : 1,
                   bx, by, blk[0], blk[1], blk[2], blk[3], blk[4], blk[5], blk[6], blk[7]);
          }
        }
      }
      IDirect3DSurface9_UnlockRect(plains[i]);
    }
  }
  IDirect3DDevice9_SetRenderTarget(x_dev, 0, x_backBuffer);
  printf("R5G6B5 %s\n", has565 ? "tested" : "not supported");
  x_report("code_colorfill");
}

int main(void) {
  IDirect3D9 *d3d = x_init("d9mt_colorfill");
  X_CHECK(IDirect3DDevice9_CreateTexture(x_dev, 64, 64, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &rtTex, NULL));
  X_CHECK(IDirect3DDevice9_CreateTexture(x_dev, 64, 64, 2, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &mipTex, NULL));
  X_CHECK(IDirect3DDevice9_CreateTexture(x_dev, 16, 16, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &probe, NULL));
  rtS = x_level(rtTex, 0);
  mip1 = x_level(mipTex, 1);
  probeS = x_level(probe, 0);
  X_CHECK(IDirect3DDevice9_CreateRenderTarget(x_dev, 32, 32, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &x8S, NULL));
  has565 = SUCCEEDED(IDirect3D9_CheckDeviceFormat(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8,
                                                  D3DUSAGE_RENDERTARGET, D3DRTYPE_SURFACE, D3DFMT_R5G6B5));
  if (has565)
    X_CHECK(IDirect3DDevice9_CreateRenderTarget(x_dev, 32, 32, D3DFMT_R5G6B5, D3DMULTISAMPLE_NONE, 0, FALSE, &s565, NULL));
  X_CHECK(IDirect3DDevice9_CreateOffscreenPlainSurface(x_dev, 64, 64, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &plainS, NULL));
  X_CHECK(IDirect3DDevice9_CreateCubeTexture(x_dev, 32, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &cube, NULL));
  X_CHECK(IDirect3DCubeTexture9_GetCubeMapSurface(cube, D3DCUBEMAP_FACE_NEGATIVE_Z, 0, &cubeFace));
  for (int i = 0; i < 4; i++)
    X_CHECK(IDirect3DDevice9_CreateTexture(x_dev, 16, 16, 1, 0, kDxt[i], D3DPOOL_DEFAULT, &dxt[i], NULL));
  X_CHECK(IDirect3DDevice9_CreateOffscreenPlainSurface(x_dev, 16, 16, D3DFMT_DXT1, D3DPOOL_DEFAULT, &plainDxt1, NULL));
  X_CHECK(IDirect3DDevice9_CreateOffscreenPlainSurface(x_dev, 16, 16, D3DFMT_DXT5, D3DPOOL_DEFAULT, &plainDxt5, NULL));
  X_CHECK(IDirect3DDevice9_CreateTexture(x_dev, 16, 16, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &notRt, NULL));
  notRtS = x_level(notRt, 0);
  IDirect3DDevice9_SetRenderState(x_dev, D3DRS_ALPHABLENDENABLE, FALSE);
  return x_loop(frame);
}
