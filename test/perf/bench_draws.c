/* Draw-call benchmark for engine/driver comparisons:
 * many small draws per frame, each with a vertex shader constant update, a
 * pixel shader constant update and a render state change, presented without
 * vsync. Reports the draw loop's wall time per draw (the d3d9 calls without
 * Present: what the game thread pays for them, median over the frames); the
 * whole process's CPU time per draw over all measured frames (every thread:
 * d9mt's CS and worker threads, Metal's, the emulator's; Wine on macOS only
 * has process times, from times() in 10 ms ticks, so it is taken once over
 * the whole run); and the frame time with Present (median, p99), which also
 * depends on drawable availability:
 *
 *   BENCH draws=1000 frames=600 loop_ns_per_draw=... proc_cpu_ns_per_draw=...
 *         frame_median_ms=... frame_p99_ms=... frame_median_ns=... frame_mean_ms=...
 *         cpu_tick_ms=...
 *
 * frame_median_ns is frame_median_ms finer than 1 us (its resolution is the QPC tick); frame_mean_ms
 * is the mean over the measured frames. cpu_tick_ms is the step of the process CPU clock, measured
 * at startup (0: no step seen): proc_cpu_ns_per_draw resolves no finer than one tick over
 * frames * draws.
 *
 * With "flush", each frame ends with an event query instead of Present,
 * polled with D3DGETDATA_FLUSH every 20 us (a QPC busy-wait in between, so
 * every engine and d3d9 build polls at the same rate) until the GPU is
 * done; without FLUSH a poll loop may never end (d9mt, like Windows, can
 * decline the flush). The frame time is then the CPU and GPU work alone, independent of
 * the compositor (no drawable waits, and no d9mt throttle when the window
 * isn't visible, e.g. with the display asleep). "spinflush" polls with
 * D3DGETDATA_FLUSH every time, as some games do: each such call costs d9mt
 * work on the CS thread, so it measures the polling as much as the draws.
 *
 *   bench_draws.exe [draws per frame (1000)] [frames (600)] [present|flush|spinflush]
 */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bench_window.h"

static const unsigned char shadertri_vs_bytecode[] = {
#include "shadertri_vs_bytecode.inc"
};
static const unsigned char shadertri_ps_bytecode[] = {
#include "shadertri_ps_bytecode.inc"
};

struct Vertex { float x, y, z; DWORD color; };

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
  return DefWindowProcA(hwnd, msg, wp, lp);
}

static int cmp_double(const void *a, const void *b) {
  double x = *(const double *)a, y = *(const double *)b;
  return x < y ? -1 : x > y;
}

static ULONGLONG process_cpu_100ns(void) {
  FILETIME c, e, k, u;
  GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
  return (((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime)
       + (((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime);
}

/* The process CPU clock's step: the smallest change seen while spinning for up to 50 ms. */
static double cpu_tick_ms(void) {
  LARGE_INTEGER freq, t0, t;
  ULONGLONG last = process_cpu_100ns(), step = 0;
  int changes = 0;
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&t0);
  do {
    ULONGLONG now = process_cpu_100ns();
    if (now != last) {
      if (now > last && (!step || now - last < step)) step = now - last;
      last = now;
      changes++;
    }
    QueryPerformanceCounter(&t);
  } while (changes < 8 && (t.QuadPart - t0.QuadPart) * 20 < freq.QuadPart);
  return (double)step / 10000.0;
}

int main(int argc, char **argv) {
  int draws = argc > 1 ? atoi(argv[1]) : 1000;
  int frames = argc > 2 ? atoi(argv[2]) : 600;
  if (draws < 1 || frames < 1) { printf("RESULT FAIL usage: draws and frames must be positive\n"); return 2; }
  int flush = argc > 3 && !strcmp(argv[3], "flush");
  int spinflush = argc > 3 && !strcmp(argv[3], "spinflush");
  const int warmup = 60;
  WNDCLASSA wc = {0};
  RECT rc = {0, 0, 1280, 720};
  LARGE_INTEGER freq, t0, t1;
  double *ms, *loop;
  ULONGLONG cpu0 = 0, cpu1;
  int f, i;

  wc.lpfnWndProc = wnd_proc;
  wc.hInstance = GetModuleHandleA(NULL);
  wc.lpszClassName = "d9mt_bench_draws";
  RegisterClassA(&wc);
  AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
  HWND hwnd = CreateWindowA(wc.lpszClassName, "d9mt bench draws", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                            CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
                            NULL, NULL, wc.hInstance, NULL);
  IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
  if (!hwnd || !d3d) { printf("FAIL init\n"); return 1; }
  bench_client(hwnd, "bench_draws", 1280, 720);

  D3DPRESENT_PARAMETERS pp = {0};
  pp.BackBufferWidth = 1280;
  pp.BackBufferHeight = 720;
  pp.BackBufferFormat = D3DFMT_X8R8G8B8;
  pp.BackBufferCount = 1;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.hDeviceWindow = hwnd;
  pp.Windowed = TRUE;
  pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
  IDirect3DDevice9 *dev = NULL;
  if (FAILED(IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                     D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev)) || !dev) {
    printf("FAIL device\n");
    return 1;
  }

  IDirect3DVertexShader9 *vs = NULL;
  IDirect3DPixelShader9 *ps = NULL;
  IDirect3DDevice9_CreateVertexShader(dev, (const DWORD *)shadertri_vs_bytecode, &vs);
  IDirect3DDevice9_CreatePixelShader(dev, (const DWORD *)shadertri_ps_bytecode, &ps);
  static const D3DVERTEXELEMENT9 elems[] = {
    {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
    {0, 12, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
    D3DDECL_END()};
  IDirect3DVertexDeclaration9 *decl = NULL;
  IDirect3DDevice9_CreateVertexDeclaration(dev, elems, &decl);
  static const struct Vertex verts[3] = {
    {0.0f, 0.02f, 0.5f, 0xFFFF0000}, {0.02f, -0.02f, 0.5f, 0xFF00FF00}, {-0.02f, -0.02f, 0.5f, 0xFF0000FF}};
  IDirect3DVertexBuffer9 *vb = NULL;
  void *p = NULL;
  IDirect3DDevice9_CreateVertexBuffer(dev, sizeof(verts), D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &vb, NULL);
  IDirect3DVertexBuffer9_Lock(vb, 0, sizeof(verts), &p, 0);
  memcpy(p, verts, sizeof(verts));
  IDirect3DVertexBuffer9_Unlock(vb);
  IDirect3DQuery9 *query = NULL;
  IDirect3DDevice9_CreateQuery(dev, D3DQUERYTYPE_EVENT, &query);
  if (!vs || !ps || !decl || !vb || !query) { printf("FAIL resources\n"); return 1; }

  IDirect3DDevice9_SetVertexDeclaration(dev, decl);
  IDirect3DDevice9_SetVertexShader(dev, vs);
  IDirect3DDevice9_SetPixelShader(dev, ps);
  IDirect3DDevice9_SetStreamSource(dev, 0, vb, 0, sizeof(struct Vertex));

  const double tick = cpu_tick_ms();
  ms = calloc(frames, sizeof(double));
  loop = calloc(frames, sizeof(double));
  QueryPerformanceFrequency(&freq);
  for (f = 0; f < warmup + frames; f++) {
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
    LARGE_INTEGER tl;
    if (f == warmup) { bench_timed_mark(); cpu0 = process_cpu_100ns(); }
    QueryPerformanceCounter(&t0);
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(24, 24, 40), 1.0f, 0);
    IDirect3DDevice9_BeginScene(dev);
    for (i = 0; i < draws; i++) {
      /* a 4x4 transform placing the triangle on a grid, plus a tint */
      float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
      float tint[4] = {(float)(i & 255) / 255.0f, 1.0f, (float)(f & 255) / 255.0f, 1.0f};
      m[3] = -0.95f + 1.9f * (float)(i % 40) / 40.0f;
      m[7] = -0.95f + 1.9f * (float)((i / 40) % 25) / 25.0f;
      IDirect3DDevice9_SetVertexShaderConstantF(dev, 0, m, 4);
      IDirect3DDevice9_SetPixelShaderConstantF(dev, 0, tint, 1);
      IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, (i & 1) ? D3DCULL_NONE : D3DCULL_CW);
      IDirect3DDevice9_DrawPrimitive(dev, D3DPT_TRIANGLELIST, 0, 1);
    }
    IDirect3DDevice9_EndScene(dev);
    QueryPerformanceCounter(&tl);
    if (flush || spinflush) {
      if (FAILED(IDirect3DQuery9_Issue(query, D3DISSUE_END))) { printf("RESULT FAIL issue frame=%d\n", f); return 1; }
      HRESULT hr;
      while ((hr = IDirect3DQuery9_GetData(query, NULL, 0, D3DGETDATA_FLUSH)) == S_FALSE) {
        if (!spinflush) {  /* pace the polls: 20 us */
          LARGE_INTEGER p0, p1;
          QueryPerformanceCounter(&p0);
          do QueryPerformanceCounter(&p1); while ((p1.QuadPart - p0.QuadPart) * 50000 < freq.QuadPart);
        }
      }
      /* Only S_OK means the GPU is done: an error (e.g. D3DERR_DEVICELOST) is a failed run. */
      if (hr != S_OK) { printf("RESULT FAIL getdata hr=0x%08lx frame=%d\n", (unsigned long)hr, f); return 1; }
    } else {
      if (FAILED(IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL))) { printf("RESULT FAIL present frame=%d\n", f); return 1; }
    }
    QueryPerformanceCounter(&t1);
    if (f >= warmup) {
      ms[f - warmup] = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart;
      loop[f - warmup] = (double)(tl.QuadPart - t0.QuadPart) * 1e9 / (double)freq.QuadPart / draws;
    }
  }
  cpu1 = process_cpu_100ns();
  double sum = 0.0;
  for (f = 0; f < frames; f++) sum += ms[f];
  qsort(ms, frames, sizeof(double), cmp_double);
  qsort(loop, frames, sizeof(double), cmp_double);
  bench_timed_print("bench_draws");
  printf("BENCH mode=%s draws=%d frames=%d loop_ns_per_draw=%.0f proc_cpu_ns_per_draw=%.0f frame_median_ms=%.3f frame_p99_ms=%.3f"
         " frame_median_ns=%.0f frame_mean_ms=%.4f cpu_tick_ms=%.4f\n",
         flush ? "flush" : spinflush ? "spinflush" : "present", draws, frames, loop[frames / 2], (double)(cpu1 - cpu0) * 100.0 / ((double)frames * draws),
         ms[frames / 2], ms[frames * 99 / 100], ms[frames / 2] * 1e6, sum / frames, tick);
  fflush(stdout);
  IDirect3DDevice9_Release(dev);
  IDirect3D9_Release(d3d);
  return 0;
}
