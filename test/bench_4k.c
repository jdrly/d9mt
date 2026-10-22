/* bench_4k: a synthetic 4K multi-pass D3D9 frame for docs/gpu-4k.md, shaped
 * like a SWTOR world frame: a 2048x2048 D24S8 shadow map with 4 cascades
 * (600 depth-only draws), a 3840x2160 main pass (1400 draws sampling the
 * shadow map, a fullscreen backdrop), a depth-only Clear and 200 more draws on
 * the same targets, fullscreen post passes ping-ponging two 3840x2160
 * targets (an even number, default 4), and a downscale into a small backbuffer (no 4K window). Vertex-
 * heavy geometry (bench_vs), ~2,200 draws per frame.
 *
 *   bench_4k.exe [frames] [post passes] [depth clears] [wait | cpu ms]
 *
 * The 4th argument (optional; default: Present only):
 *   wait    ends every frame with an event query polled with D3DGETDATA_FLUSH every 20 us until
 *           the GPU is done: the GPU idles between frames ("wait every frame", the case where
 *           postponing a flush starves it).
 *   <ms>    burns that many ms of CPU per frame before Present. Near the GPU's frame time the
 *           frame is balanced, so whether the GPU still has an earlier frame queued flips within
 *           frames (G1's backlog gate, docs/g1-early-commit.md §6).
 *
 * Defaults: 300 frames after 60 warm-up frames, 4 post passes (more: more
 * attachment bandwidth), 1 depth-only Clear (more: the main pass's draws are
 * split into that many runs, each ended by a depth-only Clear).
 *
 * Presents without vsync and prints the average frame time. When the GPU is
 * the limit (it is at 4K on any Mac) that is GPU time per frame; check with
 * a Metal System Trace (test/perf/mst_overlap.py). Not a hash test. */
#include "t_common.h"
#include "perf/bench_window.h"
#include <stdlib.h>
#include <string.h>

static const unsigned char bench_vs_bytecode[] = {
#include "bench_vs_bytecode.inc"
};
static const unsigned char post_ps_bytecode[] = {
#include "post_ps_bytecode.inc"
};
static const unsigned char lit_ps_bytecode[] = {
#include "lit_ps_bytecode.inc"
};

#define W 3840
#define H 2160
#define SHADOW 2048
#define TRIS_PER_DRAW 400
#define SHADOW_DRAWS 150  /* per cascade */
#define MAIN_DRAWS 1400
#define LATE_DRAWS 200
#define DRAWS (4 * SHADOW_DRAWS + MAIN_DRAWS + LATE_DRAWS)

static int postPasses = 4, depthClears = 1;
static IDirect3DVertexShader9 *vsBench;
static IDirect3DPixelShader9 *psPost, *psLit;
static IDirect3DVertexBuffer9 *vb;
static IDirect3DTexture9 *shadowMap, *shadowDummy, *ping, *pong, *albedo;
static IDirect3DSurface9 *shadowSurf, *shadowDummySurf, *pingSurf, *pongSurf, *depth4k, *backBuffer, *autoDs;

static float frand(unsigned *s) { *s = *s * 1664525u + 1013904223u; return (float)(*s >> 8) / 16777216.0f; }

static void build_geometry(void) {
  const UINT verts = DRAWS * TRIS_PER_DRAW * 3;
  T_CHECK(IDirect3DDevice9_CreateVertexBuffer(t_dev, verts * sizeof(struct TVertex), D3DUSAGE_WRITEONLY, 0,
                                              D3DPOOL_DEFAULT, &vb, NULL));
  struct TVertex *v = NULL;
  T_CHECK(IDirect3DVertexBuffer9_Lock(vb, 0, 0, (void **)&v, 0));
  unsigned seed = 12345u;
  for (UINT t = 0; t < DRAWS * TRIS_PER_DRAW; t++) {
    /* a small triangle (~8 px at 4K) somewhere on screen, depth 0.1..0.9 */
    const float cx = frand(&seed) * 2.0f - 1.0f, cy = frand(&seed) * 2.0f - 1.0f;
    const float z = 0.1f + 0.8f * frand(&seed), s = 0.004f;
    struct TVertex tri[3] = {
        {cx - s, cy - s, z, 0.0f, 1.0f}, {cx, cy + s, z, 0.5f, 0.0f}, {cx + s, cy - s, z, 1.0f, 1.0f}};
    memcpy(v + 3 * t, tri, sizeof(tri));
  }
  IDirect3DVertexBuffer9_Unlock(vb);
}

static void draws(UINT first, UINT count) {
  for (UINT i = 0; i < count; i++)
    IDirect3DDevice9_DrawPrimitive(t_dev, D3DPT_TRIANGLELIST, (first + i) * TRIS_PER_DRAW * 3, TRIS_PER_DRAW);
}

static void viewport(DWORD x, DWORD y, DWORD w, DWORD h) {
  D3DVIEWPORT9 vp = {x, y, w, h, 0.0f, 1.0f};
  IDirect3DDevice9_SetViewport(t_dev, &vp);
}

static void post(IDirect3DTexture9 *src, IDirect3DSurface9 *dst, float scale) {
  const float off[4] = {1.5f / W, 0.5f / H, -0.5f / W, 1.5f / H};
  IDirect3DDevice9_SetRenderTarget(t_dev, 0, dst);
  IDirect3DDevice9_SetTexture(t_dev, 0, (IDirect3DBaseTexture9 *)src);
  IDirect3DDevice9_SetPixelShaderConstantF(t_dev, 1, off, 1);
  t_color(scale, scale, scale, 1.0f);
  t_quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.5f);
  IDirect3DDevice9_SetTexture(t_dev, 0, NULL);
}

static void frame(int n) {
  (void)n;
  IDirect3DDevice9_BeginScene(t_dev);
  IDirect3DDevice9_SetTexture(t_dev, 0, NULL);
  IDirect3DDevice9_SetTexture(t_dev, 1, NULL);

  /* shadow cascades: depth only */
  IDirect3DDevice9_SetRenderTarget(t_dev, 0, shadowDummySurf);
  IDirect3DDevice9_SetDepthStencilSurface(t_dev, shadowSurf);
  IDirect3DDevice9_SetRenderState(t_dev, D3DRS_ZENABLE, D3DZB_TRUE);
  IDirect3DDevice9_SetRenderState(t_dev, D3DRS_ZWRITEENABLE, TRUE);
  IDirect3DDevice9_SetRenderState(t_dev, D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
  IDirect3DDevice9_SetRenderState(t_dev, D3DRS_COLORWRITEENABLE, 0);
  IDirect3DDevice9_Clear(t_dev, 0, NULL, D3DCLEAR_ZBUFFER, 0, 1.0f, 0);
  IDirect3DDevice9_SetVertexShader(t_dev, vsBench);
  IDirect3DDevice9_SetPixelShader(t_dev, t_ps_color);
  IDirect3DDevice9_SetStreamSource(t_dev, 0, vb, 0, sizeof(struct TVertex));
  for (DWORD c = 0; c < 4; c++) {
    viewport((c & 1) * (SHADOW / 2), (c >> 1) * (SHADOW / 2), SHADOW / 2, SHADOW / 2);
    draws(c * SHADOW_DRAWS, SHADOW_DRAWS);
  }
  IDirect3DDevice9_SetRenderState(t_dev, D3DRS_COLORWRITEENABLE, 0xf);

  /* main pass: shadowed geometry over a fullscreen backdrop */
  IDirect3DDevice9_SetRenderTarget(t_dev, 0, pingSurf);
  IDirect3DDevice9_SetDepthStencilSurface(t_dev, depth4k);
  IDirect3DDevice9_Clear(t_dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL,
                         D3DCOLOR_XRGB(30, 40, 60), 1.0f, 0);
  IDirect3DDevice9_SetTexture(t_dev, 0, (IDirect3DBaseTexture9 *)albedo);
  IDirect3DDevice9_SetTexture(t_dev, 1, (IDirect3DBaseTexture9 *)shadowMap);
  IDirect3DDevice9_SetPixelShader(t_dev, psLit);
  t_color(0.8f, 0.7f, 0.6f, 1.0f);
  t_tex_zw(0.5f, 1.0f);
  draws(4 * SHADOW_DRAWS, MAIN_DRAWS);
  IDirect3DDevice9_SetVertexShader(t_dev, t_vs_pos);
  t_quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.999f);

  /* first-person / UI layers: depth cleared, same targets */
  IDirect3DDevice9_SetVertexShader(t_dev, vsBench);
  IDirect3DDevice9_SetStreamSource(t_dev, 0, vb, 0, sizeof(struct TVertex));
  for (int c = 0; c < depthClears; c++) {
    const UINT first = LATE_DRAWS * c / depthClears, last = LATE_DRAWS * (c + 1) / depthClears;
    IDirect3DDevice9_Clear(t_dev, 0, NULL, D3DCLEAR_ZBUFFER, 0, 1.0f, 0);
    draws(4 * SHADOW_DRAWS + MAIN_DRAWS + first, last - first);
  }
  IDirect3DDevice9_SetTexture(t_dev, 0, NULL);
  IDirect3DDevice9_SetTexture(t_dev, 1, NULL);

  /* post: fullscreen passes ping-pong, ending in ping */
  IDirect3DDevice9_SetDepthStencilSurface(t_dev, NULL);
  IDirect3DDevice9_SetRenderState(t_dev, D3DRS_ZENABLE, D3DZB_FALSE);
  IDirect3DDevice9_SetVertexShader(t_dev, t_vs_pos);
  IDirect3DDevice9_SetPixelShader(t_dev, psPost);
  for (int i = 0; i < postPasses; i += 2) {
    post(ping, pongSurf, 0.25f);
    post(pong, pingSurf, 0.25f);
  }

  /* downscale into the backbuffer */
  IDirect3DDevice9_SetRenderTarget(t_dev, 0, backBuffer);
  IDirect3DDevice9_SetDepthStencilSurface(t_dev, autoDs);
  IDirect3DDevice9_SetPixelShader(t_dev, t_ps_sample);
  IDirect3DDevice9_SetTexture(t_dev, 0, (IDirect3DBaseTexture9 *)ping);
  t_quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.5f);
  IDirect3DDevice9_SetTexture(t_dev, 0, NULL);
  IDirect3DDevice9_EndScene(t_dev);
}

int main(int argc, char **argv) {
  const int frames = argc > 1 ? atoi(argv[1]) : 300, warm = 60;
  if (frames < 1) { printf("RESULT FAIL usage: frames must be positive\n"); return 2; }
  if (argc > 2)
    postPasses = (atoi(argv[2]) + 1) & ~1;
  if (argc > 3 && atoi(argv[3]) > 0)
    depthClears = atoi(argv[3]);
  const int waitMode = argc > 4 && !strcmp(argv[4], "wait");
  const double cpuMs = argc > 4 && !waitMode ? atof(argv[4]) : 0.0;
  t_present_interval = D3DPRESENT_INTERVAL_IMMEDIATE;
  t_init("d9mt_bench_4k", 640, 360);
  bench_client_of(t_dev, "bench_4k", 640, 360);
  T_CHECK(IDirect3DDevice9_CreateVertexShader(t_dev, (const DWORD *)bench_vs_bytecode, &vsBench));
  T_CHECK(IDirect3DDevice9_CreatePixelShader(t_dev, (const DWORD *)post_ps_bytecode, &psPost));
  T_CHECK(IDirect3DDevice9_CreatePixelShader(t_dev, (const DWORD *)lit_ps_bytecode, &psLit));
  T_CHECK(IDirect3DDevice9_CreateTexture(t_dev, SHADOW, SHADOW, 1, D3DUSAGE_DEPTHSTENCIL, D3DFMT_D24S8,
                                         D3DPOOL_DEFAULT, &shadowMap, NULL));
  T_CHECK(IDirect3DDevice9_CreateTexture(t_dev, SHADOW, SHADOW, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                         D3DPOOL_DEFAULT, &shadowDummy, NULL));
  T_CHECK(IDirect3DDevice9_CreateTexture(t_dev, W, H, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                         D3DPOOL_DEFAULT, &ping, NULL));
  T_CHECK(IDirect3DDevice9_CreateTexture(t_dev, W, H, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                         D3DPOOL_DEFAULT, &pong, NULL));
  T_CHECK(IDirect3DDevice9_CreateDepthStencilSurface(t_dev, W, H, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0,
                                                     FALSE, &depth4k, NULL));
  T_CHECK(IDirect3DDevice9_CreateTexture(t_dev, 4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &albedo, NULL));
  D3DLOCKED_RECT lr;
  T_CHECK(IDirect3DTexture9_LockRect(albedo, 0, &lr, NULL, 0));
  for (int y = 0; y < 4; y++)
    for (int x = 0; x < 4; x++)
      ((DWORD *)((char *)lr.pBits + y * lr.Pitch))[x] = ((x + y) & 1) ? 0xffc08040 : 0xff4080c0;
  IDirect3DTexture9_UnlockRect(albedo, 0);
  shadowSurf = t_level0(shadowMap);
  shadowDummySurf = t_level0(shadowDummy);
  pingSurf = t_level0(ping);
  pongSurf = t_level0(pong);
  T_CHECK(IDirect3DDevice9_GetRenderTarget(t_dev, 0, &backBuffer));
  T_CHECK(IDirect3DDevice9_GetDepthStencilSurface(t_dev, &autoDs));
  for (DWORD s = 0; s < 2; s++) {
    IDirect3DDevice9_SetSamplerState(t_dev, s, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    IDirect3DDevice9_SetSamplerState(t_dev, s, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
  }
  build_geometry();
  IDirect3DQuery9 *query = NULL;
  if (waitMode)
    T_CHECK(IDirect3DDevice9_CreateQuery(t_dev, D3DQUERYTYPE_EVENT, &query));

  LARGE_INTEGER f, t0, t1;
  QueryPerformanceFrequency(&f);
  MSG msg;
  for (int i = 0; i < warm + frames; i++) {
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageA(&msg);
    }
    if (i == warm) {
      bench_timed_mark();
      QueryPerformanceCounter(&t0);
    }
    frame(i);
    if (cpuMs > 0.0) {
      LARGE_INTEGER b0, b1;
      QueryPerformanceCounter(&b0);
      do QueryPerformanceCounter(&b1); while ((double)(b1.QuadPart - b0.QuadPart) * 1000.0 < cpuMs * (double)f.QuadPart);
    }
    if (FAILED(IDirect3DDevice9_Present(t_dev, NULL, NULL, NULL, NULL))) { printf("RESULT FAIL present frame=%d\n", i); return 1; }
    if (waitMode) {
      if (FAILED(IDirect3DQuery9_Issue(query, D3DISSUE_END))) { printf("RESULT FAIL issue frame=%d\n", i); return 1; }
      HRESULT hr;
      while ((hr = IDirect3DQuery9_GetData(query, NULL, 0, D3DGETDATA_FLUSH)) == S_FALSE) {
        LARGE_INTEGER p0, p1;
        QueryPerformanceCounter(&p0);
        do QueryPerformanceCounter(&p1); while ((p1.QuadPart - p0.QuadPart) * 50000 < f.QuadPart);
      }
      /* Only S_OK means the GPU is done: an error (e.g. D3DERR_DEVICELOST) is a failed run. */
      if (hr != S_OK) { printf("RESULT FAIL getdata hr=0x%08lx frame=%d\n", (unsigned long)hr, i); return 1; }
    }
  }
  QueryPerformanceCounter(&t1);
  const double ms = 1000.0 * (double)(t1.QuadPart - t0.QuadPart) / (double)f.QuadPart / frames;
  bench_timed_print("bench_4k");
  printf("BENCH4K frames=%d post=%d clears=%d draws=%d ms/frame=%.3f fps=%.1f mode=%s\n", frames, postPasses, depthClears,
         DRAWS + 3 + postPasses, ms, 1000.0 / ms, waitMode ? "wait" : cpuMs > 0.0 ? "balanced" : "present");
  fflush(stdout);
  return 0;
}
