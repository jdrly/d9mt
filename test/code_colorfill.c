/* ColorFill (#14), checked against what D3D9 on Windows gives (the D3D9
 * documentation and Wine's d3d9 tests, which run on Windows):
 *  - whole surface and a rect, on an A8R8G8B8 render-target texture, an
 *    X8R8G8B8 render-target surface, R5G6B5, a lockable off-screen plain
 *    surface, a cube face and a mip level: the pixels;
 *  - DXT1 (opaque, and alpha below 128: transparent black), DXT3 and DXT5
 *    off-screen plain surfaces, whole and a block-aligned rect: their blocks
 *    byte for byte through LockRect (DXT1/DXT5), and how they sample once
 *    copied into a texture (UpdateTexture); a 4096x4096 DXT5 surface (more
 *    than one staging block);
 *  - D3DERR_INVALIDCALL, nothing filled: a texture without
 *    D3DUSAGE_RENDERTARGET (A8R8G8B8 and DXT1), a block-compressed rect edge
 *    inside a block, rects that are empty, inverted, negative or outside.
 * Colours are exact in R5G6B5. */
#include "t_xfer.h"

#define C_A  0xff848284u  /* 132,130,132 */
#define C_B  0xffff00ffu
#define C_C  0x7f00ff00u  /* alpha 0x7f: DXT1 transparent */
#define C_D  0x88ff0000u  /* DXT3: alpha nibble 8 -> 0x88 */
#define C_E  0x5a0000ffu  /* DXT5: alpha 0x5a */

static IDirect3DTexture9 *rtTex, *mipTex, *probe, *notRt, *dxtTex, *dxtSys[4], *dxtCopy[4];
static IDirect3DSurface9 *rtS, *x8S, *s565, *plainS, *cubeFace, *mip1, *probeS, *notRtS, *dxtTexS, *plainDxt[4], *bigDxt5;
static IDirect3DCubeTexture9 *cube;
static BOOL has565;
static const D3DFORMAT kDxt[4] = {D3DFMT_DXT1, D3DFMT_DXT1, D3DFMT_DXT3, D3DFMT_DXT5};
static const DWORD kDxtFull[4] = {C_A, C_C, C_D, C_E};
static const char *const kDxtName[4] = {"DXT1", "DXT1 alpha", "DXT3", "DXT5"};

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
  RECT br = {4, 4, 12, 12};  /* blocks 1-2 in x and y */
  for (int i = 0; i < 4; i++) {
    fill(plainDxt[i], NULL, kDxtFull[i]);
    fill(plainDxt[i], &br, C_B);
  }

  IDirect3DDevice9_SetRenderTarget(x_dev, 0, x_backBuffer);
  IDirect3DDevice9_Clear(x_dev, 0, NULL, D3DCLEAR_TARGET, 0xff181828u, 1.0f, 0);
  IDirect3DDevice9_BeginScene(x_dev);
  x_draw((IDirect3DBaseTexture9 *)rtTex, -0.95f, 0.05f, -0.55f, 0.95f);
  IDirect3DDevice9_EndScene(x_dev);

  if (n != 1)
    return;

  /* Invalid fills: D3DERR_INVALIDCALL, and the surface keeps its contents. */
  RECT misaligned = {5, 5, 10, 10}, oob = {56, 56, 72, 72}, empty = {10, 10, 10, 20};
  RECT inverted = {20, 20, 10, 10}, negative = {-4, 0, 8, 8};
  x_expect_hr("fill texture without RENDERTARGET", IDirect3DDevice9_ColorFill(x_dev, notRtS, NULL, C_B), X_INVALIDCALL);
  x_expect_hr("fill DXT1 texture without RENDERTARGET", IDirect3DDevice9_ColorFill(x_dev, dxtTexS, NULL, C_B), X_INVALIDCALL);
  x_expect_hr("fill DXT1 rect inside blocks", IDirect3DDevice9_ColorFill(x_dev, plainDxt[0], &misaligned, C_A), X_INVALIDCALL);
  x_expect_hr("fill rect outside", IDirect3DDevice9_ColorFill(x_dev, rtS, &oob, C_D), X_INVALIDCALL);
  x_expect_hr("fill empty rect", IDirect3DDevice9_ColorFill(x_dev, rtS, &empty, C_D), X_INVALIDCALL);
  x_expect_hr("fill inverted rect", IDirect3DDevice9_ColorFill(x_dev, rtS, &inverted, C_D), X_INVALIDCALL);
  x_expect_hr("fill negative rect", IDirect3DDevice9_ColorFill(x_dev, rtS, &negative, C_D), X_INVALIDCALL);

  XReadback rb;
  rb = x_read(rtS);
  x_expect("rt full", &rb, 4, 4, C_A, 0xffffffffu);
  x_expect("rt full", &rb, 60, 60, C_A, 0xffffffffu);
  x_expect("rt rect", &rb, 20, 12, C_B, 0xffffffffu);
  x_expect("rt rect", &rb, 46, 38, C_B, 0xffffffffu);
  x_expect("rt rect edge", &rb, 48, 20, C_A, 0xffffffffu);
  x_expect("rt rect edge", &rb, 20, 40, C_A, 0xffffffffu);
  x_expect("rt after invalid rects", &rb, 2, 2, C_A, 0xffffffffu);
  x_expect("rt after invalid rects", &rb, 62, 62, C_A, 0xffffffffu);
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

  /* DXT1/DXT5 blocks byte for byte: whole fill outside the rect, C_B in it */
  {
    static const unsigned char full1[8] = {0x10, 0x84, 0x10, 0x84, 0, 0, 0, 0};  /* C_A -> 0x8410 */
    static const unsigned char rect1[8] = {0x1f, 0xf8, 0x1f, 0xf8, 0, 0, 0, 0};  /* C_B -> 0xf81f */
    static const unsigned char full5[16] = {0x5a, 0x5a, 0, 0, 0, 0, 0, 0, 0x1f, 0x00, 0x1f, 0x00, 0, 0, 0, 0};
    static const unsigned char rect5[16] = {0xff, 0xff, 0, 0, 0, 0, 0, 0, 0x1f, 0xf8, 0x1f, 0xf8, 0, 0, 0, 0};
    const int idx[2] = {0, 3};
    const unsigned char *fulls[2] = {full1, full5}, *rects[2] = {rect1, rect5};
    for (int k = 0; k < 2; k++) {
      D3DLOCKED_RECT lr;
      X_CHECK(IDirect3DSurface9_LockRect(plainDxt[idx[k]], &lr, NULL, D3DLOCK_READONLY));
      const UINT bytes = k ? 16u : 8u;
      for (UINT by = 0; by < 4; by++) {
        for (UINT bx = 0; bx < 4; bx++) {
          const BOOL inRect = bx >= 1 && bx <= 2 && by >= 1 && by <= 2;
          const unsigned char *blk = (const unsigned char *)lr.pBits + (size_t)by * lr.Pitch + bx * bytes;
          x_checks++;
          if (memcmp(blk, inRect ? rects[k] : fulls[k], bytes) != 0) {
            x_failed++;
            printf("FAIL %s plain surface block %u,%u: %02x %02x %02x %02x %02x %02x %02x %02x ...\n", kDxtName[idx[k]],
                   bx, by, blk[0], blk[1], blk[2], blk[3], blk[4], blk[5], blk[6], blk[7]);
          }
        }
      }
      IDirect3DSurface9_UnlockRect(plainDxt[idx[k]]);
    }
  }

  /* How the filled blocks sample: copied into a texture (system memory, then
   * UpdateTexture), drawn point-filtered into a 16x16 render target */
  for (int i = 0; i < 4; i++) {
    D3DLOCKED_RECT src, dst;
    X_CHECK(IDirect3DSurface9_LockRect(plainDxt[i], &src, NULL, D3DLOCK_READONLY));
    X_CHECK(IDirect3DTexture9_LockRect(dxtSys[i], 0, &dst, NULL, 0));
    const UINT rowBytes = i < 2 ? 4u * 8u : 4u * 16u;
    for (UINT by = 0; by < 4; by++)
      memcpy((char *)dst.pBits + (size_t)by * dst.Pitch, (const char *)src.pBits + (size_t)by * src.Pitch, rowBytes);
    IDirect3DTexture9_UnlockRect(dxtSys[i], 0);
    IDirect3DSurface9_UnlockRect(plainDxt[i]);
    X_CHECK(IDirect3DDevice9_UpdateTexture(x_dev, (IDirect3DBaseTexture9 *)dxtSys[i], (IDirect3DBaseTexture9 *)dxtCopy[i]));

    IDirect3DDevice9_SetRenderTarget(x_dev, 0, probeS);
    IDirect3DDevice9_Clear(x_dev, 0, NULL, D3DCLEAR_TARGET, 0xff102030u, 1.0f, 0);
    IDirect3DDevice9_BeginScene(x_dev);
    x_draw((IDirect3DBaseTexture9 *)dxtCopy[i], -1.0f, -1.0f, 1.0f, 1.0f);
    IDirect3DDevice9_EndScene(x_dev);
    char what[48];
    rb = x_read(probeS);
    snprintf(what, sizeof(what), "%s full", kDxtName[i]);
    x_expect(what, &rb, 1, 1, dxt_expect(i, kDxtFull[i]), 0xffffffffu);
    x_expect(what, &rb, 14, 14, dxt_expect(i, kDxtFull[i]), 0xffffffffu);
    x_expect(what, &rb, 13, 2, dxt_expect(i, kDxtFull[i]), 0xffffffffu);
    snprintf(what, sizeof(what), "%s rect", kDxtName[i]);
    x_expect(what, &rb, 5, 5, dxt_expect(i, C_B), 0xffffffffu);
    x_expect(what, &rb, 10, 10, dxt_expect(i, C_B), 0xffffffffu);
    x_expect(what, &rb, 5, 10, dxt_expect(i, C_B), 0xffffffffu);
    x_done(&rb);
  }

  /* 4096x4096 DXT5: 16 MiB of blocks, more than one staging block */
  {
    fill(bigDxt5, NULL, C_E);
    D3DLOCKED_RECT lr;
    X_CHECK(IDirect3DSurface9_LockRect(bigDxt5, &lr, NULL, D3DLOCK_READONLY));
    for (UINT by = 0; by < 1024; by += 97) {
      for (UINT bx = 0; bx < 1024; bx += 89) {
        const unsigned char *b = (const unsigned char *)lr.pBits + (size_t)by * lr.Pitch + (size_t)bx * 16u;
        x_checks++;
        if (b[0] != 0x5a || b[1] != 0x5a || b[8] != 0x1f || b[9] != 0x00) {
          x_failed++;
          printf("FAIL DXT5 4096 block %u,%u: %02x %02x %02x %02x\n", bx, by, b[0], b[1], b[8], b[9]);
        }
      }
    }
    const unsigned char *last = (const unsigned char *)lr.pBits + (size_t)1023 * lr.Pitch + (size_t)1023 * 16u;
    x_checks++;
    if (last[0] != 0x5a || last[8] != 0x1f) {
      x_failed++;
      printf("FAIL DXT5 4096 last block: %02x %02x\n", last[0], last[8]);
    }
    IDirect3DSurface9_UnlockRect(bigDxt5);
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
  for (int i = 0; i < 4; i++) {
    X_CHECK(IDirect3DDevice9_CreateOffscreenPlainSurface(x_dev, 16, 16, kDxt[i], D3DPOOL_DEFAULT, &plainDxt[i], NULL));
    X_CHECK(IDirect3DDevice9_CreateTexture(x_dev, 16, 16, 1, 0, kDxt[i], D3DPOOL_SYSTEMMEM, &dxtSys[i], NULL));
    X_CHECK(IDirect3DDevice9_CreateTexture(x_dev, 16, 16, 1, 0, kDxt[i], D3DPOOL_DEFAULT, &dxtCopy[i], NULL));
  }
  X_CHECK(IDirect3DDevice9_CreateOffscreenPlainSurface(x_dev, 4096, 4096, D3DFMT_DXT5, D3DPOOL_DEFAULT, &bigDxt5, NULL));
  X_CHECK(IDirect3DDevice9_CreateTexture(x_dev, 16, 16, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &notRt, NULL));
  notRtS = x_level(notRt, 0);
  X_CHECK(IDirect3DDevice9_CreateTexture(x_dev, 16, 16, 1, 0, D3DFMT_DXT1, D3DPOOL_DEFAULT, &dxtTex, NULL));
  dxtTexS = x_level(dxtTex, 0);
  IDirect3DDevice9_SetRenderState(x_dev, D3DRS_ALPHABLENDENABLE, FALSE);
  return x_loop(frame);
}
