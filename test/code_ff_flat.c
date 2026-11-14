/* code_ff_flat: D3DSHADE_FLAT in the fixed-function pipeline against D3D9's provoking vertex (the
 * 0.1.8 recompile bundle, "the fixed-function batch": the flat FF fragment shader). No shaders:
 * pre-transformed vertices, stage 0 selects the diffuse colour, every vertex its own colour:
 *   band 1, FLAT: triangle list, strip and fan, non-indexed (DrawPrimitiveUP, as SWTOR's FLAT
 *                 draws: fixed-function triangle-strip UP draws), 4 triangles each;
 *   band 2, FLAT: the same, indexed (DrawIndexedPrimitiveUP);
 *   band 3, FLAT: line list and line strip, 4 segments each;
 *   band 4, GOURAUD: the band 1 triangles again (their frames must hash as base);
 *   band 5, GOURAUD: indexed fans from index buffers (DrawIndexedPrimitive), 4 triangles each:
 *                 16-bit DEFAULT, 32-bit MANAGED and 16-bit SYSTEMMEM DYNAMIC, each with a base
 *                 vertex, a start index past indices that point at a stray vertex, and its
 *                 vertices stored in reverse (hub = index 5).
 * Oracle, read back from the first frame before it's presented, one probe pixel per primitive
 * (a triangle's centroid, a horizontal segment's midpoint):
 *   - FLAT: exactly D3D9's provoking vertex colour: the first vertex of each list or strip
 *     primitive;
 *   - FLAT, triangle fans (indexed or not): Gouraud by design until the fan index rotation lands
 *     (#170): the mean of the triangle's vertex colours, within 12 (the probe is a pixel, not the
 *     exact centroid). When #170 lands, non-indexed fans expect vertex t+1 of triangle t;
 *   - indexed fans (band 2's "indexed fan", band 5's "ib fan"): the mean, within 12. Before #187
 *     d9mt drew an indexed fan's indices as a triangle list: fan triangle 0 and another one, so
 *     1 of 4 probes was right;
 *   - GOURAUD: the mean, within 12.
 * Prints "code_ff_flat <case>: <ok>/<n>" per case and "code_ff_flat: PASS" or "code_ff_flat: FAIL"
 * once. On base (the FF FS ignored flat) the FLAT list, strip and line cases fail; that run is part
 * of the PR's evidence. */
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct V { float x, y, z, rhw; DWORD c; };
#define FVF (D3DFVF_XYZRHW | D3DFVF_DIFFUSE)
#define W 640
#define H 480

static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
  return DefWindowProcA(h, m, w, l);
}

static IDirect3DDevice9 *dev;
static int g_vertex = 0;   /* a running vertex number: every vertex its own colour */

static DWORD color_of(int k) {
  return D3DCOLOR_XRGB((k * 53 + 20) & 255, (k * 97 + 40) & 255, (k * 29 + 80) & 255);
}

static struct V vert(float x, float y) {
  struct V v = { x, y, 0.5f, 1.0f, color_of(g_vertex++) };
  return v;
}

/* A probe: a pixel and the colour it must have (flat: exact; mean: within 12). */
struct Probe { int x, y; DWORD want; int mean; };
static struct Probe probes[64];
static int nprobe;
static const char *probe_case[64];

static DWORD mean3(DWORD a, DWORD b, DWORD c) {
  DWORD r = 0;
  for (int s = 0; s < 24; s += 8)
    r |= ((((a >> s) & 255) + ((b >> s) & 255) + ((c >> s) & 255) + 1) / 3) << s;
  return r | 0xff000000u;
}

static void probe_tri(const char *name, const struct V *a, const struct V *b, const struct V *c, const struct V *prov, int mean) {
  struct Probe p = { (int)((a->x + b->x + c->x) / 3.0f), (int)((a->y + b->y + c->y) / 3.0f),
                     mean ? mean3(a->c, b->c, c->c) : (prov->c | 0xff000000u), mean };
  probe_case[nprobe] = name;
  probes[nprobe++] = p;
}

/* 4 triangles of each kind in a band at y0 (x from x0); flat = the case's oracle. */
static void triangles(float x0, float y0, int indexed, int flat, int record) {
  static const WORD seq[16] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
  struct V v[12];
  const char *ln = indexed ? "indexed list" : "list", *sn = indexed ? "indexed strip" : "strip",
             *fn = indexed ? "indexed fan" : "fan";
  if (!flat) ln = sn = fn = "gouraud";
  /* list: triangle k = (3k, 3k+1, 3k+2), provoking 3k */
  for (int k = 0; k < 4; k++) {
    v[3 * k] = vert(x0 + 50 * k, y0); v[3 * k + 1] = vert(x0 + 50 * k + 44, y0);
    v[3 * k + 2] = vert(x0 + 50 * k + 22, y0 + 60);
    if (record) probe_tri(ln, &v[3 * k], &v[3 * k + 1], &v[3 * k + 2], &v[3 * k], !flat);
  }
  if (indexed) IDirect3DDevice9_DrawIndexedPrimitiveUP(dev, D3DPT_TRIANGLELIST, 0, 12, 4, seq, D3DFMT_INDEX16, v, sizeof(v[0]));
  else IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 4, v, sizeof(v[0]));
  /* strip: triangle i = vertices i, i+1, i+2 (D3D9: provoking i) */
  x0 += 220;
  for (int i = 0; i < 6; i++)
    v[i] = vert(x0 + 30 * i, y0 + ((i & 1) ? 60.0f : 0.0f));
  if (record)
    for (int i = 0; i < 4; i++) probe_tri(sn, &v[i], &v[i + 1], &v[i + 2], &v[i], !flat);
  if (indexed) IDirect3DDevice9_DrawIndexedPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 0, 6, 4, seq, D3DFMT_INDEX16, v, sizeof(v[0]));
  else IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 4, v, sizeof(v[0]));
  /* fan: hub 0, triangle t = (0, t+1, t+2) (D3D9: provoking t+1) */
  x0 += 200;
  v[0] = vert(x0 + 90, y0 + 64);
  for (int i = 1; i < 6; i++)
    v[i] = vert(x0 + 45.0f * (i - 1), y0);
  if (record)
    for (int t = 0; t < 4; t++)
      probe_tri(fn, &v[0], &v[t + 1], &v[t + 2], &v[t + 1], 1);   /* fans: Gouraud until #170 */
  if (indexed) IDirect3DDevice9_DrawIndexedPrimitiveUP(dev, D3DPT_TRIANGLEFAN, 0, 6, 4, seq, D3DFMT_INDEX16, v, sizeof(v[0]));
  else IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLEFAN, 4, v, sizeof(v[0]));
}

/* line list and line strip, 4 horizontal segments each (probes at their midpoints) */
static void lines(float x0, float y0, int record) {
  struct V v[8];
  for (int k = 0; k < 4; k++) {
    v[2 * k] = vert(x0 + 60 * k, y0 + 8 * k); v[2 * k + 1] = vert(x0 + 60 * k + 50, y0 + 8 * k);
    if (record) {
      struct Probe p = { (int)(x0 + 60 * k + 25), (int)(y0 + 8 * k), v[2 * k].c | 0xff000000u, 0 };
      probe_case[nprobe] = "line list"; probes[nprobe++] = p;
    }
  }
  IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_LINELIST, 4, v, sizeof(v[0]));
  /* strip: a staircase, horizontal segments 0, 2 and 4, vertical 1 and 3 */
  x0 += 260;
  for (int i = 0; i < 5; i++)
    v[i] = vert(x0 + 50.0f * ((i + 1) / 2), y0 + 12.0f * (i / 2));
  for (int s = 0; s < 4 && record; s += 2) {   /* segments 0 and 2 are horizontal: (2j, 2j+1) */
    struct Probe p = { (int)((v[s].x + v[s + 1].x) / 2), (int)v[s].y, v[s].c | 0xff000000u, 0 };
    probe_case[nprobe] = "line strip"; probes[nprobe++] = p;
  }
  IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_LINESTRIP, 4, v, sizeof(v[0]));
}

/* band 5: one vertex buffer, three index buffers (see the header) */
static IDirect3DVertexBuffer9 *g_vb;
static IDirect3DIndexBuffer9 *g_ib[3];
static const int g_base[3] = { 1, 8, 15 }, g_start[3] = { 2, 1, 3 };
static struct V g_ibv[22];

static int ib_fans_setup(void) {
  static const D3DFORMAT fmt[3] = { D3DFMT_INDEX16, D3DFMT_INDEX32, D3DFMT_INDEX16 };
  static const D3DPOOL pool[3] = { D3DPOOL_DEFAULT, D3DPOOL_MANAGED, D3DPOOL_SYSTEMMEM };
  static const DWORD usage[3] = { D3DUSAGE_WRITEONLY, 0, D3DUSAGE_DYNAMIC };
  const struct V stray = { 630.0f, 470.0f, 0.5f, 1.0f, 0 };   /* every unused vertex */
  void *p;
  for (int i = 0; i < 22; i++) g_ibv[i] = stray;
  for (int f = 0; f < 3; f++) {
    const float x0 = 20.0f + 210.0f * f, y0 = 400.0f;
    for (int k = 0; k < 6; k++) {   /* fan vertex k at base + 5 - k: hub 0, rim 1..5 */
      struct V v = { k ? x0 + 45.0f * (k - 1) : x0 + 90.0f, k ? y0 : y0 + 64.0f, 0.5f, 1.0f, color_of(100 + 6 * f + k) };
      g_ibv[g_base[f] + 5 - k] = v;
    }
    DWORD ix[9];
    int n = 0;
    for (int j = 0; j < g_start[f]; j++) ix[n++] = 6;   /* skipped: the stray vertex */
    for (int k = 0; k < 6; k++) ix[n++] = 5 - k;
    const int size = fmt[f] == D3DFMT_INDEX16 ? 2 : 4;
    if (FAILED(IDirect3DDevice9_CreateIndexBuffer(dev, n * size, usage[f], fmt[f], pool[f], &g_ib[f], NULL))
     || FAILED(IDirect3DIndexBuffer9_Lock(g_ib[f], 0, 0, &p, 0)))
      return 0;
    for (int j = 0; j < n; j++) {
      if (size == 2) ((WORD *)p)[j] = (WORD)ix[j];
      else ((DWORD *)p)[j] = ix[j];
    }
    IDirect3DIndexBuffer9_Unlock(g_ib[f]);
  }
  if (FAILED(IDirect3DDevice9_CreateVertexBuffer(dev, sizeof(g_ibv), 0, FVF, D3DPOOL_MANAGED, &g_vb, NULL))
   || FAILED(IDirect3DVertexBuffer9_Lock(g_vb, 0, 0, &p, 0)))
    return 0;
  memcpy(p, g_ibv, sizeof(g_ibv));
  IDirect3DVertexBuffer9_Unlock(g_vb);
  return 1;
}

static void ib_fans(int record) {
  IDirect3DDevice9_SetStreamSource(dev, 0, g_vb, 0, sizeof(struct V));
  for (int f = 0; f < 3; f++) {
    const struct V *v = &g_ibv[g_base[f]];   /* fan vertex k is v[5 - k] */
    if (record)
      for (int t = 0; t < 4; t++) probe_tri("ib fan", &v[5], &v[4 - t], &v[3 - t], &v[4 - t], 1);
    IDirect3DDevice9_SetIndices(dev, g_ib[f]);
    IDirect3DDevice9_DrawIndexedPrimitive(dev, D3DPT_TRIANGLEFAN, g_base[f], 0, 7, g_start[f], 4);
  }
  IDirect3DDevice9_SetIndices(dev, NULL);
}

static void scene(int record) {
  g_vertex = 0;
  IDirect3DDevice9_SetRenderState(dev, D3DRS_SHADEMODE, D3DSHADE_FLAT);
  triangles(20, 20, 0, 1, record);
  triangles(20, 120, 1, 1, record);
  lines(20, 220, record);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_SHADEMODE, D3DSHADE_GOURAUD);
  triangles(20, 320, 0, 0, record);
  ib_fans(record);
}

static int close_enough(DWORD a, DWORD b, int tol) {
  for (int s = 0; s < 24; s += 8)
    if (abs((int)((a >> s) & 255) - (int)((b >> s) & 255)) > tol) return 0;
  return 1;
}

static void check(void) {
  IDirect3DSurface9 *bb, *sys;
  D3DLOCKED_RECT lr;
  if (FAILED(IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))
   || FAILED(IDirect3DDevice9_CreateOffscreenPlainSurface(dev, W, H, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, NULL))
   || FAILED(IDirect3DDevice9_GetRenderTargetData(dev, bb, sys))
   || FAILED(IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY))) {
    printf("code_ff_flat: FAIL (readback)\n");
    return;
  }
  static const char *cases[] = { "list", "strip", "fan", "indexed list", "indexed strip", "indexed fan",
                                 "line list", "line strip", "gouraud", "ib fan" };
  int all = 1;
  for (int c = 0; c < 10; c++) {
    int n = 0, ok = 0;
    for (int i = 0; i < nprobe; i++) {
      if (strcmp(probe_case[i], cases[c])) continue;
      const DWORD got = ((const DWORD *)((const char *)lr.pBits + probes[i].y * lr.Pitch))[probes[i].x] | 0xff000000u;
      const int good = probes[i].mean ? close_enough(got, probes[i].want, 12) : got == probes[i].want;
      n++; ok += good;
      if (!good) printf("  %s probe (%d,%d): want %08lx got %08lx\n", cases[c], probes[i].x, probes[i].y,
                        (unsigned long)probes[i].want, (unsigned long)got);
    }
    printf("code_ff_flat %s: %d/%d\n", cases[c], ok, n);
    all &= n > 0 && ok == n;
  }
  printf("code_ff_flat: %s\n", all ? "PASS" : "FAIL");
  IDirect3DSurface9_UnlockRect(sys);
  IDirect3DSurface9_Release(sys);
  IDirect3DSurface9_Release(bb);
}

int main(void) {
  WNDCLASSA wc = {0};
  wc.lpfnWndProc = wnd_proc;
  wc.hInstance = GetModuleHandleA(NULL);
  wc.lpszClassName = "d9mt_ff_flat";
  RegisterClassA(&wc);
  RECT rc = {0, 0, W, H};
  AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
  HWND hwnd = CreateWindowA("d9mt_ff_flat", "d9mt fixed-function flat shading", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                            CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, NULL, NULL, wc.hInstance, NULL);
  if (!hwnd) return 1;
  IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
  if (!d3d) return 1;
  D3DPRESENT_PARAMETERS pp = {0};
  pp.BackBufferWidth = W; pp.BackBufferHeight = H;
  pp.BackBufferFormat = D3DFMT_X8R8G8B8; pp.BackBufferCount = 1;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = hwnd;
  pp.Windowed = TRUE; pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
  if (FAILED(IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                     D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev)))
    return 1;
  IDirect3DDevice9_SetFVF(dev, FVF);
  /* the FF pixel stage: stage 0 selects the diffuse colour (no texture), alpha too */
  IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
  IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
  IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
  IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
  IDirect3DDevice9_SetTextureStageState(dev, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, FALSE);
  if (!ib_fans_setup()) {
    printf("code_ff_flat: FAIL (index or vertex buffer setup)\n");
    return 1;
  }

  int frames = 0;
  MSG msg;
  for (;;) {
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) goto done;
      TranslateMessage(&msg);
      DispatchMessageA(&msg);
    }
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
    IDirect3DDevice9_BeginScene(dev);
    scene(frames == 0);
    IDirect3DDevice9_EndScene(dev);
    if (frames == 0) {
      check();
      fflush(stdout);
    }
    IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
    if (++frames == 3) printf("3 frames\n");
  }
done:
  for (int f = 0; f < 3; f++) IDirect3DIndexBuffer9_Release(g_ib[f]);
  IDirect3DVertexBuffer9_Release(g_vb);
  IDirect3DDevice9_Release(dev);
  IDirect3D9_Release(d3d);
  return 0;
}
