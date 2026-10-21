/* churnbench: texture churn like the Cartel Market opening (review of the view keep-alive fix).
 * 320x240, no vsync. Per frame: 128 new 64x64 textures (created, uploaded, drawn, released at
 * once), and 3000 draws that rebind one of 64 persistent textures. Views of the released
 * textures now die when their command buffer retires (fix) instead of at the rebind (base).
 * Prints the average CPU frame time over frames 61..frames. */
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdlib.h>

struct V { float x, y, z, rhw, u, v; };

static void quad(IDirect3DDevice9 *dev, float x, float y, float s) {
  struct V q[4] = { { x, y, 0, 1, 0, 0 }, { x + s, y, 0, 1, 1, 0 }, { x, y + s, 0, 1, 0, 1 }, { x + s, y + s, 0, 1, 1, 1 } };
  IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof(q[0]));
}

static IDirect3DTexture9 *make(IDirect3DDevice9 *dev, UINT size, DWORD c) {
  IDirect3DTexture9 *t = NULL;
  D3DLOCKED_RECT lr;
  if (FAILED(IDirect3DDevice9_CreateTexture(dev, size, size, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, NULL)))
    return NULL;
  IDirect3DTexture9_LockRect(t, 0, &lr, NULL, 0);
  for (UINT y = 0; y < size; y++)
    for (UINT x = 0; x < size; x++) ((DWORD *)((char *)lr.pBits + y * lr.Pitch))[x] = c ^ (x * 0x010101u);
  IDirect3DTexture9_UnlockRect(t, 0);
  return t;
}

int main(int argc, char **argv) {
  int frames = argc > 1 ? atoi(argv[1]) : 1060;
  WNDCLASSA wc = { 0 }; wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "cb";
  RegisterClassA(&wc);
  HWND hwnd = CreateWindowA("cb", "cb", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 340, 280, NULL, NULL, wc.hInstance, NULL);
  IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
  D3DPRESENT_PARAMETERS pp = { 0 };
  pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferFormat = D3DFMT_X8R8G8B8;
  pp.BackBufferWidth = 320; pp.BackBufferHeight = 240; pp.hDeviceWindow = hwnd;
  pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
  IDirect3DDevice9 *dev = NULL;
  if (FAILED(IDirect3D9_CreateDevice(d3d, 0, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev))) { printf("RESULT FAIL\n"); return 1; }
  IDirect3DTexture9 *tex[64];
  for (int i = 0; i < 64; i++) tex[i] = make(dev, 4, 0xff000000u | (DWORD)i * 0x030507u);
  IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_TEX1);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, FALSE);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE);
  LARGE_INTEGER f, t0, t1; QueryPerformanceFrequency(&f);
  MSG msg;
  for (int n = 1; n <= frames; n++) {
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
    if (n == 61) QueryPerformanceCounter(&t0);
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xff000000, 1.0f, 0);
    IDirect3DDevice9_BeginScene(dev);
    for (int i = 0; i < 3000; i++) {
      IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)tex[(i * 7 + n) & 63]);
      quad(dev, (float)(i % 80) * 4.0f, (float)(i / 80 % 60) * 4.0f, 4.0f);
      if (i % 24 == 0) {
        IDirect3DTexture9 *t = make(dev, 64, 0xff000000u | (DWORD)(n * 131 + i));
        if (t) {
          IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)t);
          quad(dev, (float)(i % 5) * 64.0f, (float)(i % 3) * 64.0f, 64.0f);
          IDirect3DDevice9_SetTexture(dev, 0, NULL);
          IDirect3DTexture9_Release(t);
        }
      }
    }
    IDirect3DDevice9_EndScene(dev);
    IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
  }
  QueryPerformanceCounter(&t1);
  printf("RESULT churnbench frames=%d avg_ms=%.4f\n", frames - 60, (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart / (frames - 60));
  for (int i = 0; i < 64; i++) IDirect3DTexture9_Release(tex[i]);
  IDirect3DDevice9_Release(dev); IDirect3D9_Release(d3d);
  return 0;
}
