/* bindbench: CPU-bound texture-rebind benchmark for the view-lifetime fix. 320x240, no vsync,
 * 5000 tiny draws per frame, each with a different one of 64 textures (every draw replaces
 * the bound view). Prints the average CPU frame time over frames 61..frames. */
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdlib.h>
#include "bench_window.h"

struct V { float x, y, z, rhw, u, v; };

int main(int argc, char **argv) {
  int frames = argc > 1 ? atoi(argv[1]) : 1060;
  if (frames <= 60) { printf("RESULT FAIL usage: frames must be more than the 60 warm-up frames\n"); return 2; }
  WNDCLASSA wc = { 0 }; wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "bb";
  RegisterClassA(&wc);
  HWND hwnd = bench_window("bb", "bindbench", 320, 240);
  IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
  D3DPRESENT_PARAMETERS pp = { 0 };
  pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferFormat = D3DFMT_X8R8G8B8;
  pp.BackBufferWidth = 320; pp.BackBufferHeight = 240; pp.hDeviceWindow = hwnd;
  pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
  IDirect3DDevice9 *dev = NULL;
  if (FAILED(IDirect3D9_CreateDevice(d3d, 0, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev))) { printf("RESULT FAIL\n"); return 1; }
  IDirect3DTexture9 *tex[64];
  for (int i = 0; i < 64; i++) {
    D3DLOCKED_RECT lr;
    IDirect3DDevice9_CreateTexture(dev, 4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex[i], NULL);
    IDirect3DTexture9_LockRect(tex[i], 0, &lr, NULL, 0);
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) ((DWORD *)((char *)lr.pBits + y * lr.Pitch))[x] = 0xff000000u | (DWORD)i * 0x030507u;
    IDirect3DTexture9_UnlockRect(tex[i], 0);
  }
  IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_TEX1);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, FALSE);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE);
  LARGE_INTEGER f, t0, t1; QueryPerformanceFrequency(&f);
  MSG msg;
  for (int n = 1; n <= frames; n++) {
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
    if (n == 61) { bench_timed_mark(); QueryPerformanceCounter(&t0); }
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xff000000, 1.0f, 0);
    IDirect3DDevice9_BeginScene(dev);
    for (int i = 0; i < 5000; i++) {
      float x = (float)(i % 80) * 4.0f, y = (float)(i / 80 % 60) * 4.0f;
      struct V q[4] = { { x, y, 0, 1, 0, 0 }, { x + 4, y, 0, 1, 1, 0 }, { x, y + 4, 0, 1, 0, 1 }, { x + 4, y + 4, 0, 1, 1, 1 } };
      IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)tex[(i * 7 + n) & 63]);
      IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof(q[0]));
    }
    IDirect3DDevice9_EndScene(dev);
    if (FAILED(IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL))) { printf("RESULT FAIL present frame=%d\n", n); return 1; }
  }
  QueryPerformanceCounter(&t1);
  bench_timed_print("bindbench");
  printf("RESULT bindbench frames=%d avg_ms=%.4f\n", frames - 60, (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart / (frames - 60));
  for (int i = 0; i < 64; i++) IDirect3DTexture9_Release(tex[i]);
  IDirect3DDevice9_Release(dev); IDirect3D9_Release(d3d);
  return 0;
}
