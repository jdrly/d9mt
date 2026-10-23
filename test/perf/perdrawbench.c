/* perdrawbench: CPU-bound benchmark of the per-draw vertex upload (UploadPerDrawData, #56). 320x240,
 * no vsync, 2000 DrawIndexedPrimitive per frame from D3DPOOL_SYSTEMMEM + D3DUSAGE_DYNAMIC vertex
 * buffers, which d9mt copies into an upload slice on every draw; each draw names an in-range
 * vertex window (MinVertexIndex/NumVertices) of a 768-vertex buffer of 24-byte vertices, 4 buffers
 * rotating. Prints the average CPU frame time over frames 61..frames. */
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdlib.h>
#include "bench_window.h"

struct V { float x, y, z, rhw; float psize; DWORD c; };   /* 24 bytes */
#define FVF (D3DFVF_XYZRHW | D3DFVF_PSIZE | D3DFVF_DIFFUSE)
#define NV 768

int main(int argc, char **argv) {
  int frames = argc > 1 ? atoi(argv[1]) : 560;
  if (frames <= 60) { printf("RESULT FAIL usage: frames must be more than the 60 warm-up frames\n"); return 2; }
  WNDCLASSA wc = { 0 }; wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "pdb";
  RegisterClassA(&wc);
  HWND hwnd = bench_window("pdb", "perdrawbench", 320, 240);
  IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
  D3DPRESENT_PARAMETERS pp = { 0 };
  pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferFormat = D3DFMT_X8R8G8B8;
  pp.BackBufferWidth = 320; pp.BackBufferHeight = 240; pp.hDeviceWindow = hwnd;
  pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
  IDirect3DDevice9 *dev = NULL;
  if (FAILED(IDirect3D9_CreateDevice(d3d, 0, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev))) { printf("RESULT FAIL device\n"); return 1; }
  IDirect3DVertexBuffer9 *vb[4];
  IDirect3DIndexBuffer9 *ib = NULL;
  for (int b = 0; b < 4; b++) {
    struct V *v = NULL;
    if (FAILED(IDirect3DDevice9_CreateVertexBuffer(dev, NV * sizeof(struct V), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, FVF, D3DPOOL_SYSTEMMEM, &vb[b], NULL))
     || FAILED(IDirect3DVertexBuffer9_Lock(vb[b], 0, 0, (void **)&v, 0))) { printf("RESULT FAIL vertex buffer\n"); return 1; }
    for (int i = 0; i < NV; i++) {
      v[i].x = (float)(i % 32) * 10.0f; v[i].y = (float)(i / 32 % 24) * 10.0f; v[i].z = 0.5f; v[i].rhw = 1.0f;
      v[i].psize = 1.0f; v[i].c = 0xff000000u | (DWORD)(b * 0x304050 + i);
    }
    IDirect3DVertexBuffer9_Unlock(vb[b]);
  }
  {
    WORD *x = NULL;
    if (FAILED(IDirect3DDevice9_CreateIndexBuffer(dev, 6 * sizeof(WORD), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &ib, NULL))
     || FAILED(IDirect3DIndexBuffer9_Lock(ib, 0, 0, (void **)&x, 0))) { printf("RESULT FAIL index buffer\n"); return 1; }
    static const WORD idx[6] = { 0, 1, 32, 1, 33, 32 };
    for (int i = 0; i < 6; i++) x[i] = idx[i];
    IDirect3DIndexBuffer9_Unlock(ib);
  }
  IDirect3DDevice9_SetFVF(dev, FVF);
  IDirect3DDevice9_SetIndices(dev, ib);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, FALSE);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
  LARGE_INTEGER f, t0, t1; QueryPerformanceFrequency(&f);
  MSG msg;
  for (int n = 1; n <= frames; n++) {
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
    if (n == 61) { bench_timed_mark(); QueryPerformanceCounter(&t0); }
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xff000000, 1.0f, 0);
    IDirect3DDevice9_BeginScene(dev);
    for (int i = 0; i < 2000; i++) {
      /* an in-range window of 34 vertices (a quad's indices 0..33 from BaseVertexIndex) */
      const INT base = (i * 37 + n) % (NV - 34);
      IDirect3DDevice9_SetStreamSource(dev, 0, vb[i & 3], 0, sizeof(struct V));
      if (FAILED(IDirect3DDevice9_DrawIndexedPrimitive(dev, D3DPT_TRIANGLELIST, base, 0, 34, 0, 2))) {
        printf("RESULT FAIL draw frame=%d\n", n); return 1;
      }
    }
    IDirect3DDevice9_EndScene(dev);
    if (FAILED(IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL))) { printf("RESULT FAIL present frame=%d\n", n); return 1; }
  }
  QueryPerformanceCounter(&t1);
  bench_timed_print("perdrawbench");
  printf("RESULT perdrawbench frames=%d avg_ms=%.4f\n", frames - 60, (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart / (frames - 60));
  for (int b = 0; b < 4; b++) IDirect3DVertexBuffer9_Release(vb[b]);
  IDirect3DIndexBuffer9_Release(ib);
  IDirect3DDevice9_Release(dev); IDirect3D9_Release(d3d);
  return 0;
}
