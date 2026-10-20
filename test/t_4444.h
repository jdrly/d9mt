/* Shared setup for code_4444 and code_4444_rt: an 8x8 A4R4G4B4 texel set
 * with known values (an alpha gradient, a blue gradient, blue-only texels
 * with alpha 0 and 15, and a fixed pseudo-random fill), helpers to fill
 * textures with it, and a check of a whole 8x8 grid. A 4-bit channel n reads
 * back as n * 17 in 8 bits (+-1 allowed there); 4444 readbacks compare the
 * raw 16-bit texels exactly. Each wrong grid prints one FAIL line with its
 * first wrong texel. */
#include "t_xfer.h"

#define N 8
#define S 32  /* sampling targets: 4x4 pixels per texel, checked at the block centres */

static WORD kTex[N * N];
static DWORD kTex8[N * N];
static IDirect3DVertexShader9 *vsTex;
static IDirect3DPixelShader9 *psTex;
static IDirect3DVertexDeclaration9 *declTex;

static inline DWORD expand(WORD w) {
  return ((DWORD)((w >> 12) & 15) * 17u << 24) | ((DWORD)((w >> 8) & 15) * 17u << 16)
       | ((DWORD)((w >> 4) & 15) * 17u << 8) | (DWORD)(w & 15) * 17u;
}

static inline IDirect3D9 *t4_init(const char *name) {
  IDirect3D9 *d3d = x_init(name);
  IDirect3DDevice9_GetVertexShader(x_dev, &vsTex);
  IDirect3DDevice9_GetPixelShader(x_dev, &psTex);
  IDirect3DDevice9_GetVertexDeclaration(x_dev, &declTex);
  unsigned s = 0x1234567u;
  for (int i = 0; i < N * N; i++) {
    if (i < 16)
      kTex[i] = (WORD)((i << 12) | 0x5a3);            /* alpha gradient */
    else if (i < 32)
      kTex[i] = (WORD)(((31 - i) << 12) | (i - 16));  /* blue gradient, alpha falling */
    else {
      s = s * 1103515245u + 12345u;
      kTex[i] = (WORD)(s >> 16);
    }
  }
  kTex[32] = 0x000f;  /* blue only, alpha 0 */
  kTex[33] = 0xf00f;  /* blue only, opaque */
  kTex[34] = 0x0f00;  /* red only, alpha 0 */
  kTex[35] = 0x00f0;
  kTex[36] = 0xf000;
  kTex[37] = 0x0000;
  kTex[38] = 0xffff;
  kTex[39] = 0x1248;
  for (int i = 0; i < N * N; i++)
    kTex8[i] = expand(kTex[i]);
  return d3d;
}

static inline IDirect3DTexture9 *mk(UINT w, D3DFORMAT f, DWORD usage, D3DPOOL pool) {
  IDirect3DTexture9 *t = NULL;
  X_CHECK(IDirect3DDevice9_CreateTexture(x_dev, w, w, 1, usage, f, pool, &t, NULL));
  return t;
}

static inline void fill16(IDirect3DTexture9 *t, const WORD *data) {
  D3DLOCKED_RECT lr;
  X_CHECK(IDirect3DTexture9_LockRect(t, 0, &lr, NULL, 0));
  for (int y = 0; y < N; y++)
    memcpy((BYTE *)lr.pBits + y * lr.Pitch, data + y * N, N * 2);
  IDirect3DTexture9_UnlockRect(t, 0);
}

static inline void fill32(IDirect3DTexture9 *t, const DWORD *data) {
  D3DLOCKED_RECT lr;
  X_CHECK(IDirect3DTexture9_LockRect(t, 0, &lr, NULL, 0));
  for (int y = 0; y < N; y++)
    memcpy((BYTE *)lr.pBits + y * lr.Pitch, data + y * N, N * 4);
  IDirect3DTexture9_UnlockRect(t, 0);
}

/* Texture t over the whole of render target rt, with the texquad shaders. */
static inline void shader_draw(IDirect3DSurface9 *rt, IDirect3DBaseTexture9 *t) {
  IDirect3DDevice9_SetRenderTarget(x_dev, 0, rt);
  x_draw(t, -1.0f, -1.0f, 1.0f, 1.0f);
}

/* Texture t over the whole S x S current target, with fixed function (texture
 * colour and alpha selected), then the texquad shaders again. */
static inline void ff_draw(IDirect3DBaseTexture9 *t) {
  struct { float x, y, z, rhw, u, v; } q[4] = {
      {-0.5f, -0.5f, 0.5f, 1.0f, 0.0f, 0.0f}, {S - 0.5f, -0.5f, 0.5f, 1.0f, 1.0f, 0.0f},
      {-0.5f, S - 0.5f, 0.5f, 1.0f, 0.0f, 1.0f}, {S - 0.5f, S - 0.5f, 0.5f, 1.0f, 1.0f, 1.0f}};
  IDirect3DDevice9_SetVertexShader(x_dev, NULL);
  IDirect3DDevice9_SetPixelShader(x_dev, NULL);
  IDirect3DDevice9_SetFVF(x_dev, D3DFVF_XYZRHW | D3DFVF_TEX1);
  IDirect3DDevice9_SetTextureStageState(x_dev, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
  IDirect3DDevice9_SetTextureStageState(x_dev, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
  IDirect3DDevice9_SetTextureStageState(x_dev, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
  IDirect3DDevice9_SetTextureStageState(x_dev, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
  IDirect3DDevice9_SetTexture(x_dev, 0, t);
  IDirect3DDevice9_DrawPrimitiveUP(x_dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof(q[0]));
  IDirect3DDevice9_SetTexture(x_dev, 0, NULL);
  IDirect3DDevice9_SetVertexDeclaration(x_dev, declTex);
  IDirect3DDevice9_SetVertexShader(x_dev, vsTex);
  IDirect3DDevice9_SetPixelShader(x_dev, psTex);
}

static inline int chan_ok(DWORD got, DWORD want, DWORD mask, int bits, int tol) {
  for (int c = 0; c < 32; c += bits) {
    const DWORD m = ((1u << bits) - 1u) << c;
    if (!(mask & m))
      continue;
    const int g = (int)((got & m) >> c), w = (int)((want & m) >> c);
    if (g - w > tol || w - g > tol)
      return 0;
  }
  return 1;
}

/* The 8x8 texels of s (scale: pixels per texel) against want, masked. */
static inline void check_grid(const char *what, IDirect3DSurface9 *s, UINT scale, const DWORD *want, DWORD mask) {
  XReadback r = x_read(s);
  const int raw16 = r.desc.Format == D3DFMT_A4R4G4B4 || r.desc.Format == D3DFMT_X4R4G4B4;
  int bad = 0, first = -1;
  DWORD firstGot = 0;
  for (int i = 0; i < N * N; i++) {
    const DWORD got = x_pixel(&r, (i % N) * scale + scale / 2, (i / N) * scale + scale / 2);
    if (!chan_ok(got, want[i], mask, raw16 ? 4 : 8, raw16 ? 0 : 1)) {
      if (first < 0) {
        first = i;
        firstGot = got & mask;
      }
      bad++;
    }
  }
  x_done(&r);
  x_checks++;
  if (bad) {
    x_failed++;
    printf("FAIL %s: %d/%d texels wrong, first #%d got %08lx want %08lx (mask %08lx)\n", what, bad, N * N, first,
           (unsigned long)firstGot, (unsigned long)(want[first] & mask), (unsigned long)mask);
  } else {
    printf("ok   %s\n", what);
  }
}
