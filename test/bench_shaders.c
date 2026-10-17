/* Shader compile benchmark for engine/driver comparisons
 * (docs/arm64-runtime.md): creates each pixel shader of a list, draws once
 * with it behind a fixed vertex shader and waits for the GPU, timing each
 * shader from CreatePixelShader to the end of its draw. Run with
 * D9MT_COMPILE_THREADS=0 D9MT_AIR_CACHE=0 D9MT_PSO_ARCHIVE=0 so every shader is
 * one synchronous compile: the front end in d3d9.dll, IR -> AIR on the unix
 * side and Metal's pipeline build. Prints
 *
 *   BENCH shaders=N total_ms=... median_ms=... p90_ms=... failed=K
 *
 *   bench_shaders.exe <list file: one SM3 pixel shader .bin path per line>
 */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const unsigned char shadertri_vs_bytecode[] = {
#include "shadertri_vs_bytecode.inc"
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

static void *read_file(const char *path, long *size) {
  FILE *f = fopen(path, "rb");
  void *data;
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  *size = ftell(f);
  fseek(f, 0, SEEK_SET);
  data = malloc(*size);
  if (fread(data, 1, *size, f) != (size_t)*size) { free(data); data = NULL; }
  fclose(f);
  return data;
}

int main(int argc, char **argv) {
  WNDCLASSA wc = {0};
  LARGE_INTEGER freq, t0, t1;
  char line[1024];
  double *ms = NULL;
  int n = 0, cap = 0, failed = 0;
  FILE *list;

  if (argc < 2 || !(list = fopen(argv[1], "r"))) { printf("FAIL usage: bench_shaders <list>\n"); return 2; }
  wc.lpfnWndProc = wnd_proc;
  wc.hInstance = GetModuleHandleA(NULL);
  wc.lpszClassName = "d9mt_bench_shaders";
  RegisterClassA(&wc);
  HWND hwnd = CreateWindowA(wc.lpszClassName, "d9mt bench shaders", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                            CW_USEDEFAULT, CW_USEDEFAULT, 320, 240, NULL, NULL, wc.hInstance, NULL);
  IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
  D3DPRESENT_PARAMETERS pp = {0};
  pp.BackBufferWidth = 256;
  pp.BackBufferHeight = 256;
  pp.BackBufferFormat = D3DFMT_X8R8G8B8;
  pp.BackBufferCount = 1;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.hDeviceWindow = hwnd;
  pp.Windowed = TRUE;
  pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
  IDirect3DDevice9 *dev = NULL;
  if (!hwnd || !d3d || FAILED(IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                     D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev)) || !dev) {
    printf("FAIL device\n");
    return 1;
  }
  IDirect3DVertexShader9 *vs = NULL;
  IDirect3DDevice9_CreateVertexShader(dev, (const DWORD *)shadertri_vs_bytecode, &vs);
  static const D3DVERTEXELEMENT9 elems[] = {
    {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
    {0, 12, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
    D3DDECL_END()};
  IDirect3DVertexDeclaration9 *decl = NULL;
  IDirect3DDevice9_CreateVertexDeclaration(dev, elems, &decl);
  static const struct Vertex verts[3] = {
    {0.0f, 0.6f, 0.5f, 0xFFFF0000}, {0.6f, -0.6f, 0.5f, 0xFF00FF00}, {-0.6f, -0.6f, 0.5f, 0xFF0000FF}};
  IDirect3DVertexBuffer9 *vb = NULL;
  void *p = NULL;
  IDirect3DDevice9_CreateVertexBuffer(dev, sizeof(verts), D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &vb, NULL);
  IDirect3DVertexBuffer9_Lock(vb, 0, sizeof(verts), &p, 0);
  memcpy(p, verts, sizeof(verts));
  IDirect3DVertexBuffer9_Unlock(vb);
  IDirect3DQuery9 *query = NULL;
  IDirect3DDevice9_CreateQuery(dev, D3DQUERYTYPE_EVENT, &query);
  if (!vs || !decl || !vb || !query) { printf("FAIL resources\n"); return 1; }
  IDirect3DDevice9_SetVertexDeclaration(dev, decl);
  IDirect3DDevice9_SetVertexShader(dev, vs);
  IDirect3DDevice9_SetStreamSource(dev, 0, vb, 0, sizeof(struct Vertex));
  QueryPerformanceFrequency(&freq);

  while (fgets(line, sizeof(line), list)) {
    long size = 0;
    void *code;
    IDirect3DPixelShader9 *ps = NULL;
    MSG msg;
    line[strcspn(line, "\r\n")] = 0;
    if (!line[0] || !(code = read_file(line, &size))) continue;
    fprintf(stderr, "SHADER %s\n", line);  /* names the one a crash happens in */
    fflush(stderr);
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
    QueryPerformanceCounter(&t0);
    if (FAILED(IDirect3DDevice9_CreatePixelShader(dev, (const DWORD *)code, &ps)) || !ps) {
      failed++;
      free(code);
      continue;
    }
    IDirect3DDevice9_SetPixelShader(dev, ps);
    IDirect3DDevice9_BeginScene(dev);
    IDirect3DDevice9_DrawPrimitive(dev, D3DPT_TRIANGLELIST, 0, 1);
    IDirect3DDevice9_EndScene(dev);
    IDirect3DQuery9_Issue(query, D3DISSUE_END);
    while (IDirect3DQuery9_GetData(query, NULL, 0, D3DGETDATA_FLUSH) == S_FALSE) { }
    QueryPerformanceCounter(&t1);
    if (n == cap) { cap = cap ? 2 * cap : 256; ms = realloc(ms, cap * sizeof(double)); }
    ms[n++] = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart;
    IDirect3DDevice9_SetPixelShader(dev, NULL);
    IDirect3DPixelShader9_Release(ps);
    free(code);
    if ((n & 31) == 0)
      IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
  }
  fclose(list);
  if (n) {
    double sum = 0.0;
    int i;
    for (i = 0; i < n; i++) sum += ms[i];
    qsort(ms, n, sizeof(double), cmp_double);
    printf("BENCH shaders=%d total_ms=%.1f median_ms=%.3f p90_ms=%.3f failed=%d\n",
           n, sum, ms[n / 2], ms[n * 9 / 10], failed);
  } else {
    printf("FAIL no shaders\n");
  }
  fflush(stdout);
  IDirect3DDevice9_Release(dev);
  IDirect3D9_Release(d3d);
  return 0;
}
