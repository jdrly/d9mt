/* LockRect(READONLY) of DEFAULT-pool plain surfaces in formats d9mt stores through the
 * format converter (L6V5U5, X8L8V8U8, A2W10V10U10) after a GPU write (UpdateSurface from a
 * SYSTEMMEM surface), #19. The image holds the converted format (8 bytes per texel), the
 * mapping buffer the D3D9 one (2 or 4): the readback copy wrote up to 4x the buffer's size
 * on the GPU (MTL_DEBUG_LAYER=1: "totalBytesUsed(2048) must be <= [destinationBuffer
 * length](512)", and the process aborts). Run with MTL_DEBUG_LAYER=1: the program must get
 * through. Reading these formats back isn't supported (d9mt logs it); their bytes are only
 * reported. A8R8G8B8 is the control and must round-trip. */
#include "t_xfer.h"

static const D3DFORMAT kFmt[4] = {D3DFMT_A8R8G8B8, D3DFMT_L6V5U5, D3DFMT_X8L8V8U8, D3DFMT_A2W10V10U10};
static const char *const kName[4] = {"A8R8G8B8", "L6V5U5", "X8L8V8U8", "A2W10V10U10"};
static const UINT kBpp[4] = {4, 2, 4, 4};

static void frame(int n) {
  if (n != 1)
    return;
  IDirect3D9 *d3d;
  IDirect3DDevice9_GetDirect3D(x_dev, &d3d);
  for (int i = 0; i < 4; i++) {
    if (FAILED(IDirect3D9_CheckDeviceFormat(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, 0,
                                            D3DRTYPE_SURFACE, kFmt[i]))) {
      printf("%s plain surfaces not supported, skipped\n", kName[i]);
      continue;
    }
    IDirect3DSurface9 *s, *sys;
    D3DLOCKED_RECT lr;
    X_CHECK(IDirect3DDevice9_CreateOffscreenPlainSurface(x_dev, 16, 16, kFmt[i], D3DPOOL_DEFAULT, &s, NULL));
    X_CHECK(IDirect3DDevice9_CreateOffscreenPlainSurface(x_dev, 16, 16, kFmt[i], D3DPOOL_SYSTEMMEM, &sys, NULL));
    X_CHECK(IDirect3DSurface9_LockRect(sys, &lr, NULL, 0));
    for (UINT y = 0; y < 16; y++)
      for (UINT b = 0; b < 16 * kBpp[i]; b++)
        ((unsigned char *)lr.pBits)[y * lr.Pitch + b] = (unsigned char)(y * 31 + b * 7 + 1);
    IDirect3DSurface9_UnlockRect(sys);
    /* a GPU write: the next lock reads the surface back */
    X_CHECK(IDirect3DDevice9_UpdateSurface(x_dev, sys, NULL, s, NULL));
    X_CHECK(IDirect3DSurface9_LockRect(s, &lr, NULL, D3DLOCK_READONLY));
    int bad = 0;
    for (UINT y = 0; y < 16; y++)
      for (UINT b = 0; b < 16 * kBpp[i]; b++)
        bad += ((unsigned char *)lr.pBits)[y * lr.Pitch + b] != (unsigned char)(y * 31 + b * 7 + 1);
    IDirect3DSurface9_UnlockRect(s);
    if (i == 0) {
      x_checks++;
      if (bad) {
        x_failed++;
        printf("FAIL %s: %d of %u bytes differ after the read-back lock\n", kName[i], bad, 16 * 16 * kBpp[i]);
      }
    } else {
      printf("%s: read back without a GPU copy (not supported), %d of %u bytes differ\n", kName[i], bad,
             16 * 16 * kBpp[i]);
    }
    IDirect3DSurface9_Release(sys);
    IDirect3DSurface9_Release(s);
  }
  IDirect3D9_Release(d3d);
  x_report("code_lock_convfmt");
}

int main(void) {
  x_init("d9mt_lock_convfmt");
  return x_loop(frame);
}
