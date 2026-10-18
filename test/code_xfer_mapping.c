/* StretchRect, ColorFill and Clear into render targets whose format has a
 * component mapping (#14): D3D9 stores A8's alpha, A8L8's luminance and
 * alpha, and A4R4G4B4/X4R4G4B4's nibbles in their own places, and the readback
 * returns them in D3D9's layout. With 0xdeadbeef (A=de R=ad G=be B=ef), what
 * Windows gives: A8 = de, A8L8 = dead (A high byte, L = red low byte),
 * A4R4G4B4 = dabe, X4R4G4B4 = xabe. The StretchRect source is an A8R8G8B8
 * render target holding 0xdeadbeef (same size: formats that aren't similar
 * take the blit path). Formats the device doesn't render to are skipped. */
#include "t_xfer.h"

#define C 0xdeadbeefu

static IDirect3DTexture9 *srcTex;
static IDirect3DSurface9 *srcS, *fillS[4], *clearS[4], *blitS[4];
static BOOL has[4];
static const D3DFORMAT kFmt[4] = {D3DFMT_A8, D3DFMT_A8L8, D3DFMT_A4R4G4B4, D3DFMT_X4R4G4B4};
static const char *const kName[4] = {"A8", "A8L8", "A4R4G4B4", "X4R4G4B4"};
static const DWORD kWant[4] = {0xde, 0xdead, 0xdabe, 0x0abe};
static const DWORD kMask[4] = {0xff, 0xffff, 0xffff, 0x0fff};

static void frame(int n) {
  X_CHECK(IDirect3DDevice9_ColorFill(x_dev, srcS, NULL, C));
  for (int i = 0; i < 4; i++) {
    if (!has[i])
      continue;
    X_CHECK(IDirect3DDevice9_ColorFill(x_dev, fillS[i], NULL, C));
    IDirect3DDevice9_SetRenderTarget(x_dev, 0, clearS[i]);
    IDirect3DDevice9_Clear(x_dev, 0, NULL, D3DCLEAR_TARGET, C, 1.0f, 0);
    X_CHECK(IDirect3DDevice9_StretchRect(x_dev, srcS, NULL, blitS[i], NULL, D3DTEXF_POINT));
  }
  IDirect3DDevice9_SetRenderTarget(x_dev, 0, x_backBuffer);
  IDirect3DDevice9_Clear(x_dev, 0, NULL, D3DCLEAR_TARGET, 0xff181828u, 1.0f, 0);
  if (n != 1)
    return;
  for (int i = 0; i < 4; i++) {
    if (!has[i]) {
      printf("%s render target not supported, skipped\n", kName[i]);
      continue;
    }
    char what[48];
    IDirect3DSurface9 *s[3] = {fillS[i], clearS[i], blitS[i]};
    const char *op[3] = {"ColorFill", "Clear", "StretchRect"};
    for (int k = 0; k < 3; k++) {
      XReadback r = x_read(s[k]);
      snprintf(what, sizeof(what), "%s into %s", op[k], kName[i]);
      x_expect(what, &r, 3, 3, kWant[i], kMask[i]);
      x_done(&r);
    }
  }
  x_report("code_xfer_mapping");
}

int main(void) {
  IDirect3D9 *d3d = x_init("d9mt_xfer_mapping");
  X_CHECK(IDirect3DDevice9_CreateTexture(x_dev, 16, 16, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &srcTex, NULL));
  srcS = x_level(srcTex, 0);
  for (int i = 0; i < 4; i++) {
    has[i] = SUCCEEDED(IDirect3D9_CheckDeviceFormat(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8,
                                                    D3DUSAGE_RENDERTARGET, D3DRTYPE_SURFACE, kFmt[i]));
    if (!has[i])
      continue;
    X_CHECK(IDirect3DDevice9_CreateRenderTarget(x_dev, 16, 16, kFmt[i], D3DMULTISAMPLE_NONE, 0, FALSE, &fillS[i], NULL));
    X_CHECK(IDirect3DDevice9_CreateRenderTarget(x_dev, 16, 16, kFmt[i], D3DMULTISAMPLE_NONE, 0, FALSE, &clearS[i], NULL));
    X_CHECK(IDirect3DDevice9_CreateRenderTarget(x_dev, 16, 16, kFmt[i], D3DMULTISAMPLE_NONE, 0, FALSE, &blitS[i], NULL));
  }
  return x_loop(frame);
}
