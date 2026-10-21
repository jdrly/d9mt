/* psobench: the per-draw cost of a PSO state switch on A->B->A state. 320x240, no vsync, 3000
 * draws per frame from one static VB, every draw switches between two (or four) states:
 *   topo   : triangle strip <-> line strip. Since #33 a topology change doesn't resolve the PSO
 *            at all, so this is the floor of a state switch.
 *   blend  : SRCBLEND ONE <-> SRCALPHA (alpha blending on): every draw resolves the PSO. With
 *            the MRU's interned generations (#40) A->B->A hits the MRU; with D9MT_NO_PSOMRU=1
 *            each draw builds the full RenderPsoKey and looks it up (the key hash's cost, #34).
 *   blend2 : DESTBLEND INVSRCALPHA <-> ONE; blendop: BLENDOP ADD <-> REVSUBTRACT; blend4:
 *            SRCBLEND cycles 4 values (4 keys). Other key sets for blend, so a result that depends
 *            on where two particular keys land in the PSO cache's hash table shows as a sign flip.
 *   none   : no state change (one PSO).
 * Same draw count in every mode. Prints the average CPU frame time over frames 61..frames.
 *   psobench.exe <frames> <topo|blend|blend2|blendop|blend4|none> */
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct V { float x, y, z, rhw; DWORD c; };

int main(int argc, char **argv) {
  int frames = argc > 1 ? atoi(argv[1]) : 1060;
  if (frames <= 60) { printf("RESULT FAIL usage: frames must be more than the 60 warm-up frames\n"); return 2; }
  const char *mode = argc > 2 ? argv[2] : "blend";
  int topo = !strcmp(mode, "topo"), blend = !strcmp(mode, "blend");
  int blend2 = !strcmp(mode, "blend2"), blendop = !strcmp(mode, "blendop"), blend4 = !strcmp(mode, "blend4");
  static const DWORD src4[4] = { D3DBLEND_SRCALPHA, D3DBLEND_ONE, D3DBLEND_DESTCOLOR, D3DBLEND_INVDESTCOLOR };
  WNDCLASSA wc = { 0 }; wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "pb";
  RegisterClassA(&wc);
  HWND hwnd = CreateWindowA("pb", "pb", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 340, 280, NULL, NULL, wc.hInstance, NULL);
  IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
  D3DPRESENT_PARAMETERS pp = { 0 };
  pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferFormat = D3DFMT_X8R8G8B8;
  pp.BackBufferWidth = 320; pp.BackBufferHeight = 240; pp.hDeviceWindow = hwnd;
  pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
  IDirect3DDevice9 *dev = NULL;
  if (FAILED(IDirect3D9_CreateDevice(d3d, 0, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev))) { printf("RESULT FAIL\n"); return 1; }
  /* 80x60 grid of 4x4 quads, 4 vertices each */
  IDirect3DVertexBuffer9 *vb = NULL;
  IDirect3DDevice9_CreateVertexBuffer(dev, 4800 * 4 * sizeof(struct V), D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &vb, NULL);
  struct V *v = NULL;
  IDirect3DVertexBuffer9_Lock(vb, 0, 0, (void **)&v, 0);
  for (int i = 0; i < 4800; i++) {
    float x = (float)(i % 80) * 4.0f, y = (float)(i / 80) * 4.0f;
    DWORD c = 0x80000000u | (DWORD)i * 0x030507u;
    struct V q[4] = { { x, y, 0, 1, c }, { x + 4, y, 0, 1, c }, { x, y + 4, 0, 1, c }, { x + 4, y + 4, 0, 1, c } };
    memcpy(&v[i * 4], q, sizeof(q));
  }
  IDirect3DVertexBuffer9_Unlock(vb);
  IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
  IDirect3DDevice9_SetStreamSource(dev, 0, vb, 0, sizeof(struct V));
  IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, FALSE);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, TRUE);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
  LARGE_INTEGER f, t0, t1; QueryPerformanceFrequency(&f);
  MSG msg;
  for (int n = 1; n <= frames; n++) {
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
    if (n == 61) QueryPerformanceCounter(&t0);
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xff000000, 1.0f, 0);
    IDirect3DDevice9_BeginScene(dev);
    for (int i = 0; i < 3000; i++) {
      int q = (i * 7 + n) % 4800;
      if (blend)
        IDirect3DDevice9_SetRenderState(dev, D3DRS_SRCBLEND, (i & 1) ? D3DBLEND_ONE : D3DBLEND_SRCALPHA);
      else if (blend2)
        IDirect3DDevice9_SetRenderState(dev, D3DRS_DESTBLEND, (i & 1) ? D3DBLEND_ONE : D3DBLEND_INVSRCALPHA);
      else if (blendop)
        IDirect3DDevice9_SetRenderState(dev, D3DRS_BLENDOP, (i & 1) ? D3DBLENDOP_REVSUBTRACT : D3DBLENDOP_ADD);
      else if (blend4)
        IDirect3DDevice9_SetRenderState(dev, D3DRS_SRCBLEND, src4[i & 3]);
      if (topo && (i & 1))
        IDirect3DDevice9_DrawPrimitive(dev, D3DPT_LINESTRIP, q * 4, 3);
      else
        IDirect3DDevice9_DrawPrimitive(dev, D3DPT_TRIANGLESTRIP, q * 4, 2);
    }
    IDirect3DDevice9_EndScene(dev);
    if (FAILED(IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL))) { printf("RESULT FAIL present frame=%d\n", n); return 1; }
  }
  QueryPerformanceCounter(&t1);
  printf("RESULT psobench mode=%s frames=%d avg_ms=%.4f\n", mode, frames - 60, (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart / (frames - 60));
  IDirect3DVertexBuffer9_Release(vb);
  IDirect3DDevice9_Release(dev); IDirect3D9_Release(d3d);
  return 0;
}
