/*
 * code_statecache.c - synthetic many-pipeline workload for the pipeline
 * state cache (docs/state-cache.md), shaped like SWTOR's sessions:
 *
 *  - every shader is created up front (SWTOR: ~1900 in its first 2 s);
 *  - a "loading" phase draws one fixed pipeline for a while;
 *  - the "world" phase then brings in a few new pipelines per frame (new
 *    VS/PS pairs, blend modes, render target formats) and keeps drawing
 *    them, like content coming into view.
 *
 * The shaders are generated SM3 bytecode (ALU chains on an interpolant, so
 * the Metal compiler can't fold them away); their constants come from the
 * seed, so a seed is a reproducible set of distinct shaders.
 *
 *   code_statecache.exe <seed> [pixelShaders=200] [pipelines=400] [loadMs=3000] [perFrame=4]
 *
 * Prints one summary line: world-phase frame times (avg/p50/p99/max, frames
 * over 33 and 50 ms, total time above the median) and how many pipelines
 * were drawn. d9mt's own COMPILE/STATS lines (D9MT_PROFILE) give the
 * render-thread compile waits.
 */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_TOKENS 4096
#define NUM_VS 16

static uint32_t g_rng;

static uint32_t rnd(void) {
  g_rng ^= g_rng << 13;
  g_rng ^= g_rng >> 17;
  g_rng ^= g_rng << 5;
  return g_rng;
}

static float rndf(float lo, float hi) {
  return lo + (hi - lo) * (float)(rnd() & 0xffffu) / 65535.0f;
}

static DWORD fbits(float f) {
  DWORD d;
  memcpy(&d, &f, sizeof(d));
  return d;
}

/* SM3 register tokens. */
#define DST(type, reg)  (0x80000000u | (((type) & 7u) << 28) | ((((type) >> 3) & 3u) << 11) | 0x000F0000u | (reg))
#define SRC(type, reg)  (0x80000000u | (((type) & 7u) << 28) | ((((type) >> 3) & 3u) << 11) | 0x00E40000u | (reg))
#define T_TEMP   0u
#define T_INPUT  1u
#define T_CONST  2u
#define T_OUTPUT 6u    /* D3DSPR_OUTPUT (= TEXCRDOUT) */
#define T_COLOROUT 8u

static void emitDef(DWORD *t, int *n, uint32_t reg, float a, float b, float c, float d) {
  t[(*n)++] = 0x05000051u;  /* def */
  t[(*n)++] = DST(T_CONST, reg);
  t[(*n)++] = fbits(a);
  t[(*n)++] = fbits(b);
  t[(*n)++] = fbits(c);
  t[(*n)++] = fbits(d);
}

/* vs_3_0: o0 = v0 (position), o1 (texcoord0) = v0 * c0 + c1. */
static int buildVs(DWORD *t) {
  int n = 0;
  t[n++] = 0xFFFE0300u;
  emitDef(t, &n, 0u, rndf(0.5f, 2.0f), rndf(0.5f, 2.0f), rndf(0.5f, 2.0f), 1.0f);
  emitDef(t, &n, 1u, rndf(-1.0f, 1.0f), rndf(-1.0f, 1.0f), rndf(-1.0f, 1.0f), 0.0f);
  t[n++] = 0x0200001Fu; t[n++] = 0x80000000u; t[n++] = DST(T_INPUT, 0u);   /* dcl_position v0 */
  t[n++] = 0x0200001Fu; t[n++] = 0x80000000u; t[n++] = DST(T_OUTPUT, 0u);  /* dcl_position o0 */
  t[n++] = 0x0200001Fu; t[n++] = 0x80000005u; t[n++] = DST(T_OUTPUT, 1u);  /* dcl_texcoord0 o1 */
  t[n++] = 0x02000001u; t[n++] = DST(T_OUTPUT, 0u); t[n++] = SRC(T_INPUT, 0u);   /* mov o0, v0 */
  t[n++] = 0x04000004u; t[n++] = DST(T_OUTPUT, 1u); t[n++] = SRC(T_INPUT, 0u);   /* mad o1, v0, c0, c1 */
  t[n++] = SRC(T_CONST, 0u); t[n++] = SRC(T_CONST, 1u);
  t[n++] = 0x0000FFFFu;
  return n;
}

/* ps_3_0: an ALU chain of `rounds` x (mad, mul, add, frc) on texcoord0. */
static int buildPs(DWORD *t, int rounds) {
  int n = 0;
  t[n++] = 0xFFFF0300u;
  for (uint32_t c = 0u; c < 4u; c++)
    emitDef(t, &n, c, rndf(-2.0f, 2.0f), rndf(-2.0f, 2.0f), rndf(-2.0f, 2.0f), rndf(0.2f, 1.0f));
  t[n++] = 0x0200001Fu; t[n++] = 0x80000005u; t[n++] = DST(T_INPUT, 0u);   /* dcl_texcoord0 v0 */
  t[n++] = 0x04000004u; t[n++] = DST(T_TEMP, 0u); t[n++] = SRC(T_INPUT, 0u);   /* mad r0, v0, c0, c1 */
  t[n++] = SRC(T_CONST, 0u); t[n++] = SRC(T_CONST, 1u);
  for (int i = 0; i < rounds; i++) {
    t[n++] = 0x04000004u; t[n++] = DST(T_TEMP, 0u); t[n++] = SRC(T_TEMP, 0u);   /* mad r0, r0, c2, c3 */
    t[n++] = SRC(T_CONST, 2u); t[n++] = SRC(T_CONST, 3u);
    t[n++] = 0x03000005u; t[n++] = DST(T_TEMP, 1u); t[n++] = SRC(T_TEMP, 0u);   /* mul r1, r0, r0 */
    t[n++] = SRC(T_TEMP, 0u);
    t[n++] = 0x03000002u; t[n++] = DST(T_TEMP, 0u); t[n++] = SRC(T_TEMP, 0u);   /* add r0, r0, r1 */
    t[n++] = SRC(T_TEMP, 1u);
    t[n++] = 0x02000013u; t[n++] = DST(T_TEMP, 0u); t[n++] = SRC(T_TEMP, 0u);   /* frc r0, r0 */
  }
  t[n++] = 0x02000001u; t[n++] = DST(T_COLOROUT, 0u); t[n++] = SRC(T_TEMP, 0u); /* mov oC0, r0 */
  t[n++] = 0x0000FFFFu;
  return n;
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_DESTROY) {
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcA(hwnd, msg, wp, lp);
}

static int cmpDouble(const void *a, const void *b) {
  const double x = *(const double *)a, y = *(const double *)b;
  return x < y ? -1 : x > y;
}

struct Combo { int vs, ps, blend, rt; };

int main(int argc, char **argv) {
  const uint32_t seed = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 10) : 1u;
  const int numPs     = argc > 2 ? atoi(argv[2]) : 200;
  const int numCombos = argc > 3 ? atoi(argv[3]) : 400;
  const int loadMs    = argc > 4 ? atoi(argv[4]) : 3000;
  const int perFrame  = argc > 5 ? atoi(argv[5]) : 4;
  g_rng = seed * 2654435761u + 0x9e3779b9u;
  if (!g_rng)
    g_rng = 1u;

  WNDCLASSA wc = {0};
  wc.lpfnWndProc = wnd_proc;
  wc.hInstance = GetModuleHandleA(NULL);
  wc.lpszClassName = "d9mt_statecache";
  RegisterClassA(&wc);

  RECT rc = {0, 0, 640, 480};
  AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
  HWND hwnd = CreateWindowA("d9mt_statecache", "d9mt state cache", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
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
  pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;

  IDirect3DDevice9 *dev = NULL;
  if (FAILED(IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                     D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev)) || !dev)
    return 1;

  LARGE_INTEGER freq, t0, t1;
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&t0);

  /* All shaders up front. */
  static DWORD tokens[MAX_TOKENS];
  IDirect3DVertexShader9 *vs[NUM_VS];
  IDirect3DPixelShader9 **ps = calloc((size_t)numPs, sizeof(*ps));
  for (int i = 0; i < NUM_VS; i++) {
    buildVs(tokens);
    if (FAILED(IDirect3DDevice9_CreateVertexShader(dev, tokens, &vs[i])))
      return 2;
  }
  for (int i = 0; i < numPs; i++) {
    buildPs(tokens, 40 + (int)(rnd() % 80u));
    if (FAILED(IDirect3DDevice9_CreatePixelShader(dev, tokens, &ps[i])))
      return 3;
  }
  QueryPerformanceCounter(&t1);
  const double createMs = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart;

  static const D3DVERTEXELEMENT9 declElems[] = {
      {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
      D3DDECL_END()};
  IDirect3DVertexDeclaration9 *decl = NULL;
  if (FAILED(IDirect3DDevice9_CreateVertexDeclaration(dev, declElems, &decl)))
    return 1;

  static const float verts[9] = {0.0f, 0.6f, 0.5f, 0.6f, -0.6f, 0.5f, -0.6f, -0.6f, 0.5f};
  IDirect3DVertexBuffer9 *vb = NULL;
  if (FAILED(IDirect3DDevice9_CreateVertexBuffer(dev, sizeof(verts), 0, 0, D3DPOOL_DEFAULT, &vb, NULL)))
    return 1;
  void *p = NULL;
  IDirect3DVertexBuffer9_Lock(vb, 0, sizeof(verts), &p, 0);
  memcpy(p, verts, sizeof(verts));
  IDirect3DVertexBuffer9_Unlock(vb);

  /* Offscreen targets: a second and third color format for the pipeline keys. */
  IDirect3DTexture9 *rtTex[2] = {NULL, NULL};
  IDirect3DSurface9 *rtSurf[3] = {NULL, NULL, NULL};
  IDirect3DDevice9_GetRenderTarget(dev, 0, &rtSurf[0]);
  const D3DFORMAT rtFmt[2] = {D3DFMT_A16B16G16R16F, D3DFMT_A8R8G8B8};
  for (int i = 0; i < 2; i++) {
    if (FAILED(IDirect3DDevice9_CreateTexture(dev, 256, 256, 1, D3DUSAGE_RENDERTARGET, rtFmt[i],
                                              D3DPOOL_DEFAULT, &rtTex[i], NULL)))
      return 4;
    IDirect3DTexture9_GetSurfaceLevel(rtTex[i], 0, &rtSurf[i + 1]);
  }

  /* Pipelines in order of first use: each pixel shader once with a vertex
   * shader, then again with other blend modes and targets. */
  struct Combo *combos = calloc((size_t)numCombos, sizeof(*combos));
  for (int i = 0; i < numCombos; i++) {
    combos[i].ps    = i % numPs;
    combos[i].vs    = (int)(rnd() % NUM_VS);
    combos[i].blend = (i / numPs) % 3;
    combos[i].rt    = (i / numPs + i) % 3;
  }

  IDirect3DDevice9_SetVertexDeclaration(dev, decl);
  IDirect3DDevice9_SetStreamSource(dev, 0, vb, 0, 12);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, D3DZB_FALSE);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE);

  const int maxWorldFrames = numCombos / (perFrame > 0 ? perFrame : 1) + 120;
  double *worldMs = calloc((size_t)maxWorldFrames, sizeof(double));
  int worldFrames = 0, introduced = 0, frames = 0;
  LARGE_INTEGER last, start;
  QueryPerformanceCounter(&start);
  last = start;
  int world = 0;
  MSG msg;

  for (;;) {
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT)
        goto done;
      TranslateMessage(&msg);
      DispatchMessageA(&msg);
    }

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const double sinceStart = (double)(now.QuadPart - start.QuadPart) * 1000.0 / (double)freq.QuadPart;
    if (!world && sinceStart >= loadMs)
      world = 1;

    IDirect3DDevice9_BeginScene(dev);

    if (world && introduced < numCombos)
      introduced = introduced + perFrame < numCombos ? introduced + perFrame : numCombos;

    /* Loading: one fixed pipeline. World: the newest 64 pipelines. */
    const int first = world ? (introduced > 64 ? introduced - 64 : 0) : 0;
    const int last_ = world ? introduced : 1;
    for (int rt = 0; rt < 3; rt++) {
      IDirect3DDevice9_SetRenderTarget(dev, 0, rtSurf[rt]);
      IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(16, 16, 32), 1.0f, 0);
      for (int i = first; i < last_; i++) {
        const struct Combo *c = &combos[i];
        if (c->rt != rt)
          continue;
        IDirect3DDevice9_SetVertexShader(dev, vs[c->vs]);
        IDirect3DDevice9_SetPixelShader(dev, ps[c->ps]);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, c->blend != 0);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_SRCBLEND, c->blend == 1 ? D3DBLEND_SRCALPHA : D3DBLEND_ONE);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_DESTBLEND, c->blend == 1 ? D3DBLEND_INVSRCALPHA : D3DBLEND_ONE);
        IDirect3DDevice9_DrawPrimitive(dev, D3DPT_TRIANGLELIST, 0, 1);
      }
    }
    IDirect3DDevice9_SetRenderTarget(dev, 0, rtSurf[0]);

    IDirect3DDevice9_EndScene(dev);
    IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
    frames++;

    QueryPerformanceCounter(&now);
    const double dt = (double)(now.QuadPart - last.QuadPart) * 1000.0 / (double)freq.QuadPart;
    last = now;

    if (world) {
      if (worldFrames < maxWorldFrames)
        worldMs[worldFrames++] = dt;
      if (introduced >= numCombos && worldFrames >= numCombos / perFrame + 60)
        break;
    }
  }

done:;
  double sum = 0.0, maxMs = 0.0;
  int over33 = 0, over50 = 0;
  for (int i = 0; i < worldFrames; i++) {
    sum += worldMs[i];
    if (worldMs[i] > maxMs) maxMs = worldMs[i];
    over33 += worldMs[i] > 33.4;
    over50 += worldMs[i] > 50.0;
  }
  qsort(worldMs, (size_t)worldFrames, sizeof(double), cmpDouble);
  const double p50 = worldFrames ? worldMs[worldFrames / 2] : 0.0;
  const double p99 = worldFrames ? worldMs[(worldFrames * 99) / 100] : 0.0;
  double excess = 0.0;
  for (int i = 0; i < worldFrames; i++)
    excess += worldMs[i] > p50 ? worldMs[i] - p50 : 0.0;

  printf("code_statecache OK seed=%u ps=%d vs=%d pipelines=%d createMs=%.1f frames=%d worldFrames=%d "
         "worldAvgMs=%.2f p50=%.2f p99=%.2f maxMs=%.1f over33=%d over50=%d excessMs=%.1f\n",
         seed, numPs, NUM_VS, introduced, createMs, frames, worldFrames,
         worldFrames ? sum / worldFrames : 0.0, p50, p99, maxMs, over33, over50, excess);
  fflush(stdout);

  for (int i = 0; i < 3; i++)
    if (rtSurf[i]) IDirect3DSurface9_Release(rtSurf[i]);
  for (int i = 0; i < 2; i++)
    if (rtTex[i]) IDirect3DTexture9_Release(rtTex[i]);
  IDirect3DVertexBuffer9_Release(vb);
  IDirect3DVertexDeclaration9_Release(decl);
  for (int i = 0; i < numPs; i++)
    IDirect3DPixelShader9_Release(ps[i]);
  for (int i = 0; i < NUM_VS; i++)
    IDirect3DVertexShader9_Release(vs[i]);
  IDirect3DDevice9_Release(dev);
  IDirect3D9_Release(d3d);
  DestroyWindow(hwnd);
  return 0;
}
