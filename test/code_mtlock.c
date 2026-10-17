// A game streaming textures from a second thread on a device created without
// D3DCREATE_MULTITHREADED (SWTOR: behavior 0x40, HWVP only). The worker thread locks and
// fills D3DPOOL_MANAGED textures, which on Windows only touches system memory, while the
// render thread draws with them and presents. d9mt must not emit command-stream work from
// such a lock: the device lock is a no-op without D3DCREATE_MULTITHREADED, so two threads
// would append to the same CS chunk at once. 0.0.6-dev.2 did exactly that under
// D9MT_PROFILE (the RESTORE attribution tagged every LockRect into the CS stream) and SWTOR
// hung at startup with Debug Logging on.
//
// Run it with the Debug Logging environment (scripts/run-hang-test.sh): D9MT_PROFILE=1,
// D9MT_MEMSTATS=1, DXVK_LOG_LEVEL=info and a D9MT_DEBUG_FILE flags file. It presents FRAMES
// frames (default 600; argv[1] overrides), then releases the device, prints
// "code_mtlock OK frames=<n> locks=<n>" and exits 0. A hang is caught by the harness's
// timeout.
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdlib.h>

static const unsigned char texquad_vs_bytecode[] = {
#include "texquad_vs_bytecode.inc"
};
static const unsigned char texquad_ps_bytecode[] = {
#include "texquad_ps_bytecode.inc"
};

struct Vertex {
  float x, y, z;
  float u, v;
};

enum { NTEX = 16, TEX = 128, QUADS = 64 };

static IDirect3DTexture9 *g_tex[NTEX];
static volatile LONG g_stop;
static volatile LONG g_locks;

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_DESTROY) {
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcA(hwnd, msg, wp, lp);
}

// The streaming thread: lock, fill and unlock managed textures as fast as it can.
static DWORD WINAPI stream_thread(void *arg) {
  (void)arg;
  DWORD n = 0;
  while (!g_stop) {
    IDirect3DTexture9 *tex = g_tex[n % NTEX];
    D3DLOCKED_RECT lr;
    if (SUCCEEDED(IDirect3DTexture9_LockRect(tex, 0, &lr, NULL, 0))) {
      const DWORD c = 0xFF000000u | ((n * 0x3B1Du) & 0x00FFFFFFu);
      for (int y = 0; y < TEX; y++) {
        DWORD *row = (DWORD *)((BYTE *)lr.pBits + y * lr.Pitch);
        for (int x = 0; x < TEX; x++)
          row[x] = c;
      }
      IDirect3DTexture9_UnlockRect(tex, 0);
      InterlockedIncrement(&g_locks);
    }
    n++;
    if ((n & 63u) == 0u)
      Sleep(0);
  }
  return 0;
}

int main(int argc, char **argv) {
  const int frameCount = argc > 1 ? atoi(argv[1]) : 600;

  WNDCLASSA wc = {0};
  wc.lpfnWndProc = wnd_proc;
  wc.hInstance = GetModuleHandleA(NULL);
  wc.lpszClassName = "d9mt_mtlock";
  RegisterClassA(&wc);

  RECT rc = {0, 0, 640, 480};
  AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
  HWND hwnd = CreateWindowA("d9mt_mtlock", "d9mt streaming thread", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                            CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
                            NULL, NULL, wc.hInstance, NULL);
  if (!hwnd)
    return 1;

  IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
  if (!d3d)
    return 1;

  D3DPRESENT_PARAMETERS pp = {0};
  pp.BackBufferWidth = 640;
  pp.BackBufferHeight = 480;
  pp.BackBufferFormat = D3DFMT_X8R8G8B8;
  pp.BackBufferCount = 1;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.hDeviceWindow = hwnd;
  pp.Windowed = TRUE;
  pp.EnableAutoDepthStencil = TRUE;
  pp.AutoDepthStencilFormat = D3DFMT_D24S8;
  pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

  // No D3DCREATE_MULTITHREADED, as SWTOR.
  IDirect3DDevice9 *dev = NULL;
  HRESULT hr = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                       D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
  if (FAILED(hr) || !dev) {
    printf("code_mtlock: CreateDevice failed 0x%08lx\n", (unsigned long)hr);
    return 1;
  }

  IDirect3DVertexShader9 *vs = NULL;
  IDirect3DPixelShader9 *ps = NULL;
  if (FAILED(IDirect3DDevice9_CreateVertexShader(dev, (const DWORD *)texquad_vs_bytecode, &vs))
   || FAILED(IDirect3DDevice9_CreatePixelShader(dev, (const DWORD *)texquad_ps_bytecode, &ps)))
    return 1;

  static const D3DVERTEXELEMENT9 declElems[] = {
      {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
      {0, 12, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
      D3DDECL_END()};
  IDirect3DVertexDeclaration9 *decl = NULL;
  if (FAILED(IDirect3DDevice9_CreateVertexDeclaration(dev, declElems, &decl)))
    return 1;

  // QUADS small quads in a grid, 6 vertices each.
  struct Vertex verts[QUADS * 6];
  for (int q = 0; q < QUADS; q++) {
    const float x0 = -1.0f + 2.0f * (float)(q % 8) / 8.0f, y0 = -1.0f + 2.0f * (float)(q / 8) / 8.0f;
    const float x1 = x0 + 0.24f, y1 = y0 + 0.24f;
    const struct Vertex v[6] = {
        {x0, y1, 0.5f, 0.0f, 0.0f}, {x1, y1, 0.5f, 1.0f, 0.0f}, {x1, y0, 0.5f, 1.0f, 1.0f},
        {x0, y1, 0.5f, 0.0f, 0.0f}, {x1, y0, 0.5f, 1.0f, 1.0f}, {x0, y0, 0.5f, 0.0f, 1.0f},
    };
    memcpy(&verts[q * 6], v, sizeof(v));
  }
  IDirect3DVertexBuffer9 *vb = NULL;
  if (FAILED(IDirect3DDevice9_CreateVertexBuffer(dev, sizeof(verts), D3DUSAGE_WRITEONLY, 0,
                                                 D3DPOOL_DEFAULT, &vb, NULL)))
    return 1;
  void *p = NULL;
  IDirect3DVertexBuffer9_Lock(vb, 0, sizeof(verts), &p, 0);
  memcpy(p, verts, sizeof(verts));
  IDirect3DVertexBuffer9_Unlock(vb);

  for (int i = 0; i < NTEX; i++) {
    if (FAILED(IDirect3DDevice9_CreateTexture(dev, TEX, TEX, 1, 0, D3DFMT_A8R8G8B8,
                                              D3DPOOL_MANAGED, &g_tex[i], NULL)))
      return 1;
  }

  static const float mvp[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  IDirect3DDevice9_SetVertexShaderConstantF(dev, 0, mvp, 4);
  IDirect3DDevice9_SetVertexDeclaration(dev, decl);
  IDirect3DDevice9_SetVertexShader(dev, vs);
  IDirect3DDevice9_SetPixelShader(dev, ps);
  IDirect3DDevice9_SetStreamSource(dev, 0, vb, 0, sizeof(struct Vertex));
  IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
  IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT);

  HANDLE worker = CreateThread(NULL, 0, stream_thread, NULL, 0, NULL);
  if (!worker)
    return 1;

  int frames = 0;
  const DWORD t0 = GetTickCount();
  MSG msg;
  while (frames < frameCount) {
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT)
        goto done;
      TranslateMessage(&msg);
      DispatchMessageA(&msg);
    }

    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL,
                           D3DCOLOR_XRGB(24, 24, 40), 1.0f, 0);
    IDirect3DDevice9_BeginScene(dev);
    for (int q = 0; q < QUADS; q++) {
      IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)g_tex[(q + frames) % NTEX]);
      IDirect3DDevice9_DrawPrimitive(dev, D3DPT_TRIANGLELIST, q * 6, 2);
    }
    IDirect3DDevice9_EndScene(dev);
    IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);

    if (++frames % 100 == 0) {
      printf("code_mtlock: %d frames, %ld locks, %lu ms\n", frames, (long)g_locks,
             (unsigned long)(GetTickCount() - t0));
      fflush(stdout);
    }
  }

done:
  InterlockedExchange(&g_stop, 1);
  WaitForSingleObject(worker, INFINITE);
  CloseHandle(worker);
  for (int i = 0; i < NTEX; i++)
    IDirect3DTexture9_Release(g_tex[i]);
  IDirect3DVertexBuffer9_Release(vb);
  IDirect3DVertexDeclaration9_Release(decl);
  IDirect3DVertexShader9_Release(vs);
  IDirect3DPixelShader9_Release(ps);
  IDirect3DDevice9_Release(dev);
  IDirect3D9_Release(d3d);
  printf("code_mtlock OK frames=%d locks=%ld\n", frames, (long)g_locks);
  fflush(stdout);
  return frames >= frameCount ? 0 : 2;
}
