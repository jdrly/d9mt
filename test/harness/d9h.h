/* From the CrossOver 26.3 engine experiment (experiments/cx26/tests/harness/d9h.h,
 * 2026-10-17), used unchanged by scripts/run-hash-tests.sh. */
/* Force-included into d9mt's test programs (-include d9h.h): reads the backbuffer back
 * before each Present, prints an FNV-1a hash of its RGB bytes and a few sample pixels,
 * writes frame D9H_FRAMES as a PPM next to the exe, then exits. A watchdog ends the
 * process after 25 s. Engine A/B comparison harness, not part of d9mt. */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#define D9H_FRAMES 5

static int d9h_frame;
static DWORD WINAPI d9h_watchdog(LPVOID p) { (void)p; Sleep(25000); printf("D9H TIMEOUT frame=%d\n", d9h_frame); fflush(stdout); ExitProcess(3); return 0; }

static void d9h_capture(IDirect3DDevice9 *dev, int frame)
{
  IDirect3DSurface9 *bb = NULL, *sys = NULL;
  D3DSURFACE_DESC d;
  D3DLOCKED_RECT lr;
  HRESULT hr = IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
  if (FAILED(hr)) { printf("D9H frame=%d GetBackBuffer hr=%08lx\n", frame, hr); return; }
  IDirect3DSurface9_GetDesc(bb, &d);
  hr = IDirect3DDevice9_CreateOffscreenPlainSurface(dev, d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, NULL);
  if (SUCCEEDED(hr)) hr = IDirect3DDevice9_GetRenderTargetData(dev, bb, sys);
  if (SUCCEEDED(hr)) hr = IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY);
  if (FAILED(hr)) { printf("D9H frame=%d readback hr=%08lx\n", frame, hr); goto out; }
  {
    unsigned long long h = 1469598103934665603ULL;
    UINT x, y;
    for (y = 0; y < d.Height; y++) {
      const unsigned char *row = (const unsigned char *)lr.pBits + (size_t)y * lr.Pitch;
      for (x = 0; x < d.Width; x++) {
        int c;
        for (c = 0; c < 3; c++) { h ^= row[x * 4 + c]; h *= 1099511628211ULL; }
      }
    }
#define PX(X, Y) (*(const DWORD *)((const unsigned char *)lr.pBits + (size_t)(Y) * lr.Pitch + (X) * 4) & 0xffffff)
    printf("D9H frame=%d %ux%u fmt=%d hash=%016llx center=%06lx q1=%06lx q3=%06lx\n", frame, d.Width, d.Height,
           (int)d.Format, h, PX(d.Width / 2, d.Height / 2), PX(d.Width / 4, d.Height / 4), PX(3 * d.Width / 4, 3 * d.Height / 4));
    if (frame == D9H_FRAMES) {
      char path[MAX_PATH], *dot;
      FILE *f;
      GetModuleFileNameA(NULL, path, sizeof(path));
      if ((dot = strrchr(path, '.'))) strcpy(dot, ".ppm");
      if ((f = fopen(path, "wb"))) {
        fprintf(f, "P6\n%u %u\n255\n", d.Width, d.Height);
        for (y = 0; y < d.Height; y++) {
          const unsigned char *row = (const unsigned char *)lr.pBits + (size_t)y * lr.Pitch;
          for (x = 0; x < d.Width; x++) { unsigned char px[3] = { row[x * 4 + 2], row[x * 4 + 1], row[x * 4] }; fwrite(px, 1, 3, f); }
        }
        fclose(f);
      }
    }
  }
  IDirect3DSurface9_UnlockRect(sys);
out:
  if (sys) IDirect3DSurface9_Release(sys);
  IDirect3DSurface9_Release(bb);
  fflush(stdout);
}

static HRESULT d9h_present(IDirect3DDevice9 *dev, const RECT *a, const RECT *b, HWND w, const RGNDATA *r)
{
  HRESULT hr;
  if (d9h_frame == 0) CloseHandle(CreateThread(NULL, 0, d9h_watchdog, NULL, 0, NULL));
  d9h_capture(dev, ++d9h_frame);
  hr = dev->lpVtbl->Present(dev, a, b, w, r);
  if (d9h_frame >= D9H_FRAMES) { printf("D9H done\n"); fflush(stdout); ExitProcess(0); }
  return hr;
}
#undef IDirect3DDevice9_Present
#define IDirect3DDevice9_Present(p, a, b, c, d) d9h_present(p, a, b, c, d)
