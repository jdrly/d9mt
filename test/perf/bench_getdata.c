/* bench_getdata: GetData polling against G1's poll hooks (docs/g1-early-commit.md §4.5, §6).
 * Per frame: a 3840x2160 colour + D24S8 pass of 1500 DrawPrimitiveUP draws (chunk-full flushes
 * happen inside it), an occlusion query around its first 500 draws; at the start of the next
 * frame the previous frame's query is polled until it has a result, as games do for occlusion
 * culling (SWTOR, #22). With FLUSH or without. No vsync, presented to a small backbuffer.
 * Prints the average frame time over frames 31..frames; run with D9MT_PROFILE=1 for the
 * submits per frame (STATS cb=).
 *   bench_getdata.exe [frames (330)] [flush|noflush] */
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bench_window.h"

struct V { float x, y, z, rhw; DWORD c; };

static void quads(IDirect3DDevice9 *dev, int n, int seed) {
  for (int i = 0; i < n; i++) {
    float x = (float)((i * 37 + seed) % 3800), y = (float)((i * 53 + seed) % 2120);
    DWORD c = 0xff000000u | (DWORD)(i * 0x010203 + seed);
    struct V q[4] = { { x, y, 0.5f, 1, c }, { x + 40, y, 0.5f, 1, c }, { x, y + 40, 0.5f, 1, c }, { x + 40, y + 40, 0.5f, 1, c } };
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof(q[0]));
  }
}

int main(int argc, char **argv) {
  int frames = argc > 1 ? atoi(argv[1]) : 330;
  if (frames <= 30) { printf("RESULT FAIL usage: frames must be more than the 30 warm-up frames\n"); return 2; }
  int flush = !(argc > 2 && !strcmp(argv[2], "noflush"));
  WNDCLASSA wc = { 0 }; wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "bgd";
  RegisterClassA(&wc);
  HWND hwnd = bench_window("bgd", "bench_getdata", 320, 240);
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
  IDirect3DQuery9 *occ[2] = { NULL, NULL };
  IDirect3DDevice9_CreateQuery(dev, D3DQUERYTYPE_OCCLUSION, &occ[0]);
  IDirect3DDevice9_CreateQuery(dev, D3DQUERYTYPE_OCCLUSION, &occ[1]);
  if (!rt || !ds || !occ[0] || !occ[1]) { printf("RESULT FAIL resources\n"); return 1; }
  IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, TRUE);
  LARGE_INTEGER f, t0, t1; QueryPerformanceFrequency(&f);
  MSG msg;
  unsigned long long polls = 0;
  for (int n = 1; n <= frames; n++) {
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) DispatchMessageA(&msg);
    if (n == 31) { bench_timed_mark(); QueryPerformanceCounter(&t0); }
    if (n > 1) {
      DWORD samples = 0;
      LARGE_INTEGER p0, p1; QueryPerformanceCounter(&p0);
      HRESULT hr;
      while ((hr = IDirect3DQuery9_GetData(occ[(n - 1) & 1], &samples, sizeof(samples), flush ? D3DGETDATA_FLUSH : 0)) == S_FALSE) {
        polls++;
        QueryPerformanceCounter(&p1);
        if ((p1.QuadPart - p0.QuadPart) > f.QuadPart * 5) { printf("RESULT FAIL getdata timeout frame=%d\n", n); return 1; }
      }
      /* Only S_OK is a result: an error (e.g. D3DERR_DEVICELOST) must not count as the GPU done. */
      if (hr != S_OK) { printf("RESULT FAIL getdata hr=0x%08lx frame=%d\n", (unsigned long)hr, n); return 1; }
    }
    IDirect3DDevice9_SetRenderTarget(dev, 0, rt);
    IDirect3DDevice9_SetDepthStencilSurface(dev, ds);
    IDirect3DDevice9_BeginScene(dev);
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff101010, 1.0f, 0);
    if (FAILED(IDirect3DQuery9_Issue(occ[n & 1], D3DISSUE_BEGIN))) { printf("RESULT FAIL issue frame=%d\n", n); return 1; }
    quads(dev, 500, n);
    if (FAILED(IDirect3DQuery9_Issue(occ[n & 1], D3DISSUE_END))) { printf("RESULT FAIL issue frame=%d\n", n); return 1; }
    quads(dev, 1000, n + 7);
    IDirect3DDevice9_EndScene(dev);
    IDirect3DDevice9_SetRenderTarget(dev, 0, bb);
    IDirect3DDevice9_SetDepthStencilSurface(dev, bds);
    if (FAILED(IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL))) { printf("RESULT FAIL present frame=%d\n", n); return 1; }
  }
  QueryPerformanceCounter(&t1);
  bench_timed_print("bench_getdata");
  printf("RESULT bench_getdata mode=%s frames=%d polls_per_frame=%.1f avg_ms=%.4f\n", flush ? "flush" : "noflush",
         frames - 30, (double)polls / frames, (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart / (frames - 30));
  IDirect3DDevice9_Release(dev); IDirect3D9_Release(d3d);
  return 0;
}
