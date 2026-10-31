/* StretchRect of a sub-rect of a 4x multisampled surface without stretching (the
 * resolve fast path): the rect must land at the destination rect's offset and
 * leave every pixel outside it as it was, as on Windows. Metal resolves a whole
 * render area into (0,0) of a resolve texture of its size, so d9mt resolved the
 * whole source to (0,0) instead, and into a smaller destination (undefined in
 * Metal). Cases, from a 64x64 source with four coloured quadrants (the bottom
 * right one drawn, so the source's pass has a draw and is still open):
 *  - its top-left 32x32 into (16,24) of a 64x64 render-target texture cleared to BG;
 *  - its bottom-right 32x32 into a whole 32x32 render-target texture;
 *  - the whole source into a 64x64 texture (the whole-surface resolve, unchanged).
 * The results are drawn into the backbuffer too. */
#include "t_xfer.h"

#define RED    0xffff0000u
#define GREEN  0xff00ff00u
#define BLUE   0xff0000ffu
#define GRAY   0xff848284u
#define YELLOW 0xffffff00u
#define BG     0xff303030u
static const DWORD kQuad[4] = {RED, GREEN, BLUE, GRAY};

static IDirect3DSurface9 *sMsaa;
static IDirect3DTexture9 *tOffset, *tSmall, *tWhole, *yellow;
static BOOL hasMsaa;

static void stretch(IDirect3DSurface9 *from, const RECT *fr, IDirect3DSurface9 *to, const RECT *tr) {
  X_CHECK(IDirect3DDevice9_StretchRect(x_dev, from, fr, to, tr, D3DTEXF_NONE));
}

static void frame(int n) {
  IDirect3DSurface9 *d;
  if (hasMsaa) {
    IDirect3DDevice9_SetRenderTarget(x_dev, 0, sMsaa);
    for (int i = 0; i < 4; i++) {
      D3DRECT r = {(i & 1) * 32, (i >> 1) * 32, ((i & 1) + 1) * 32, ((i >> 1) + 1) * 32};
      IDirect3DDevice9_Clear(x_dev, 1, &r, D3DCLEAR_TARGET, kQuad[i], 1.0f, 0);
    }
    IDirect3DDevice9_BeginScene(x_dev);
    x_draw((IDirect3DBaseTexture9 *)yellow, 0.0f, -1.0f, 1.0f, 0.0f);  /* bottom-right quadrant */
    IDirect3DDevice9_EndScene(x_dev);

    d = x_level(tOffset, 0);
    IDirect3DDevice9_SetRenderTarget(x_dev, 0, d);
    IDirect3DDevice9_Clear(x_dev, 0, NULL, D3DCLEAR_TARGET, BG, 1.0f, 0);
    RECT fr = {0, 0, 32, 32}, tr = {16, 24, 48, 56};
    stretch(sMsaa, &fr, d, &tr);
    IDirect3DSurface9_Release(d);

    RECT br = {32, 32, 64, 64};
    stretch(sMsaa, &br, d = x_level(tSmall, 0), NULL);
    IDirect3DSurface9_Release(d);

    stretch(sMsaa, NULL, d = x_level(tWhole, 0), NULL);
    IDirect3DSurface9_Release(d);
  }

  IDirect3DDevice9_SetRenderTarget(x_dev, 0, x_backBuffer);
  IDirect3DDevice9_Clear(x_dev, 0, NULL, D3DCLEAR_TARGET, 0xff181828u, 1.0f, 0);
  if (hasMsaa) {
    IDirect3DDevice9_BeginScene(x_dev);
    x_draw((IDirect3DBaseTexture9 *)tOffset, -0.9f, -0.4f, -0.4f, 0.4f);
    x_draw((IDirect3DBaseTexture9 *)tSmall,  -0.25f, -0.4f, 0.25f, 0.4f);
    x_draw((IDirect3DBaseTexture9 *)tWhole,   0.4f, -0.4f, 0.9f, 0.4f);
    IDirect3DDevice9_EndScene(x_dev);
  }

  if (n != 1)
    return;
  if (hasMsaa) {
    XReadback r = x_read(d = x_level(tOffset, 0));
    x_expect("rect at its offset", &r, 18, 26, RED, 0xffffffffu);
    x_expect("rect at its offset", &r, 32, 40, RED, 0xffffffffu);
    x_expect("rect at its offset", &r, 46, 54, RED, 0xffffffffu);
    x_expect("outside the rect", &r, 4, 4, BG, 0xffffffffu);
    x_expect("outside the rect", &r, 14, 40, BG, 0xffffffffu);
    x_expect("outside the rect", &r, 40, 10, BG, 0xffffffffu);
    x_expect("outside the rect", &r, 50, 40, BG, 0xffffffffu);
    x_expect("outside the rect", &r, 40, 60, BG, 0xffffffffu);
    x_expect("outside the rect", &r, 60, 60, BG, 0xffffffffu);
    x_done(&r);
    IDirect3DSurface9_Release(d);

    r = x_read(d = x_level(tSmall, 0));
    x_expect("bottom-right into a smaller texture", &r, 2, 2, YELLOW, 0xffffffffu);
    x_expect("bottom-right into a smaller texture", &r, 16, 16, YELLOW, 0xffffffffu);
    x_expect("bottom-right into a smaller texture", &r, 29, 29, YELLOW, 0xffffffffu);
    x_done(&r);
    IDirect3DSurface9_Release(d);

    r = x_read(d = x_level(tWhole, 0));
    x_expect("whole surface", &r, 16, 16, RED, 0xffffffffu);
    x_expect("whole surface", &r, 48, 16, GREEN, 0xffffffffu);
    x_expect("whole surface", &r, 16, 48, BLUE, 0xffffffffu);
    x_expect("whole surface", &r, 48, 48, YELLOW, 0xffffffffu);
    x_done(&r);
    IDirect3DSurface9_Release(d);
  }
  printf("4x MSAA %s\n", hasMsaa ? "tested" : "not supported");
  x_report("code_msaa_rect");
}

static IDirect3DTexture9 *rt(UINT size) {
  IDirect3DTexture9 *t = NULL;
  X_CHECK(IDirect3DDevice9_CreateTexture(x_dev, size, size, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &t, NULL));
  return t;
}

int main(void) {
  IDirect3D9 *d3d = x_init("d9mt_msaa_rect");
  hasMsaa = SUCCEEDED(IDirect3D9_CheckDeviceMultiSampleType(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_A8R8G8B8,
                                                            TRUE, D3DMULTISAMPLE_4_SAMPLES, NULL));
  if (hasMsaa) {
    X_CHECK(IDirect3DDevice9_CreateRenderTarget(x_dev, 64, 64, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_4_SAMPLES, 0, FALSE, &sMsaa, NULL));
    tOffset = rt(64);
    tSmall = rt(32);
    tWhole = rt(64);
    X_CHECK(IDirect3DDevice9_CreateTexture(x_dev, 2, 2, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &yellow, NULL));
    D3DLOCKED_RECT lr;
    X_CHECK(IDirect3DTexture9_LockRect(yellow, 0, &lr, NULL, 0));
    for (int y = 0; y < 2; y++)
      for (int x = 0; x < 2; x++)
        ((DWORD *)((unsigned char *)lr.pBits + y * lr.Pitch))[x] = YELLOW;
    IDirect3DTexture9_UnlockRect(yellow, 0);
  }
  return x_loop(frame);
}
