/* bench_upload: streaming uploads inside 4K passes (G1's serial length, docs/g1-early-commit.md
 * §6; renames, staging fences and the upload ring retire per serial). Per frame, a 3840x2160
 * colour + D24S8 pass of 600 draws, each from a dynamic VB written with Lock(DISCARD) (a buffer
 * rename per draw), plus 24 managed 256x256 textures rewritten (LockRect/UnlockRect, uploaded
 * through staging) and drawn. No vsync, a small backbuffer. Prints the average frame time over
 * frames 31..frames; run with D9MT_MEMSTATS=16 for ringMiB/recycledMiB.
 *   bench_upload.exe [frames (330)] */
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct V { float x, y, z, rhw; DWORD c; float u, v; };
#define FVF (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1)

int main(int argc, char **argv) {
  int frames = argc > 1 ? atoi(argv[1]) : 330;
  if (frames <= 30) { printf("RESULT FAIL usage: frames must be more than the 30 warm-up frames\n"); return 2; }
  WNDCLASSA wc = { 0 }; wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "bup";
  RegisterClassA(&wc);
  HWND hwnd = CreateWindowA("bup", "bup", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 340, 280, NULL, NULL, wc.hInstance, NULL);
  IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
  D3DPRESENT_PARAMETERS pp = { 0 };
  pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferFormat = D3DFMT_X8R8G8B8;
  pp.BackBufferWidth = 320; pp.BackBufferHeight = 240; pp.hDeviceWindow = hwnd;
  pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
  IDirect3DDevice9 *dev = NULL;
  if (FAILED(IDirect3D9_CreateDevice(d3d, 0, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev))) { printf("RESULT FAIL device\n"); return 1; }
  IDirect3DSurface9 *rt = NULL, *ds = NULL, *bb = NULL, *bds = NULL;
  IDirect3DDevice9_CreateRenderTarget(dev, 3840, 2160, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, NULL);
  IDirect3DDevice9_CreateDepthStencilSurface(dev, 3840, 2160, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, FALSE, &ds, NULL);
  IDirect3DDevice9_GetRenderTarget(dev, 0, &bb);
  IDirect3DDevice9_GetDepthStencilSurface(dev, &bds);
  IDirect3DVertexBuffer9 *vb = NULL;
  IDirect3DDevice9_CreateVertexBuffer(dev, 4 * sizeof(struct V), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, FVF, D3DPOOL_DEFAULT, &vb, NULL);
  IDirect3DTexture9 *tex[24] = { 0 };
  for (int i = 0; i < 24; i++)
    IDirect3DDevice9_CreateTexture(dev, 256, 256, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex[i], NULL);
  if (!rt || !ds || !vb || !tex[23]) { printf("RESULT FAIL resources\n"); return 1; }
  IDirect3DDevice9_SetFVF(dev, FVF);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, TRUE);
  IDirect3DDevice9_SetStreamSource(dev, 0, vb, 0, sizeof(struct V));
  LARGE_INTEGER f, t0, t1; QueryPerformanceFrequency(&f);
  MSG msg;
  for (int n = 1; n <= frames; n++) {
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) DispatchMessageA(&msg);
    if (n == 31) QueryPerformanceCounter(&t0);
    for (int i = 0; i < 24; i++) {
      D3DLOCKED_RECT lr;
      if (SUCCEEDED(IDirect3DTexture9_LockRect(tex[i], 0, &lr, NULL, 0))) {
        for (int y = 0; y < 256; y++) memset((char *)lr.pBits + y * lr.Pitch, (n + i) & 0xff, 256 * 4);
        IDirect3DTexture9_UnlockRect(tex[i], 0);
      }
    }
    IDirect3DDevice9_SetRenderTarget(dev, 0, rt);
    IDirect3DDevice9_SetDepthStencilSurface(dev, ds);
    IDirect3DDevice9_BeginScene(dev);
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff101010, 1.0f, 0);
    for (int i = 0; i < 600; i++) {
      float x = (float)((i * 61 + n) % 3780), y = (float)((i * 29 + n) % 2100);
      DWORD c = 0xff000000u | (DWORD)(i * 0x030201 + n);
      struct V q[4] = { { x, y, 0.5f, 1, c, 0, 0 }, { x + 60, y, 0.5f, 1, c, 1, 0 },
                        { x, y + 60, 0.5f, 1, c, 0, 1 }, { x + 60, y + 60, 0.5f, 1, c, 1, 1 } };
      void *p = NULL;
      if (SUCCEEDED(IDirect3DVertexBuffer9_Lock(vb, 0, 0, &p, D3DLOCK_DISCARD))) {
        memcpy(p, q, sizeof(q));
        IDirect3DVertexBuffer9_Unlock(vb);
      }
      IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)tex[i % 24]);
      IDirect3DDevice9_DrawPrimitive(dev, D3DPT_TRIANGLESTRIP, 0, 2);
    }
    IDirect3DDevice9_SetTexture(dev, 0, NULL);
    IDirect3DDevice9_EndScene(dev);
    IDirect3DDevice9_SetRenderTarget(dev, 0, bb);
    IDirect3DDevice9_SetDepthStencilSurface(dev, bds);
    if (FAILED(IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL))) { printf("RESULT FAIL present frame=%d\n", n); return 1; }
  }
  QueryPerformanceCounter(&t1);
  printf("RESULT bench_upload frames=%d avg_ms=%.4f\n", frames - 30,
         (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart / (frames - 30));
  IDirect3DDevice9_Release(dev); IDirect3D9_Release(d3d);
  return 0;
}
