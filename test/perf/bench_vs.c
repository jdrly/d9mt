/* bench_vs: a vertex-bound benchmark (the 0.1.8 recompile bundle's position-invariance gate,
 * PLAN-0.1.7-codegen PR 5). About 2 M vertices a frame into a 640x360 back buffer: a 512K-vertex
 * non-indexed triangle list (tiny triangles, about a pixel each) drawn 4 times, through a
 * SWTOR-like vertex shader (a 4-bone matrix palette of 64 bones by relative constant addressing,
 * skinned normal and tangent, a view vector, shadow coordinates: 8 outputs with the position) and
 * a trivial pixel shader, so the vertex stage dominates the GPU's frame time. No vsync, no depth.
 * Prints the average frame time over frames 61..frames; when the GPU is the limit (check with a
 * Metal System Trace, test/perf/mst_overlap.py) that is GPU time per frame.
 *
 *   bench_vs.exe [frames (560)]
 *
 *   RESULT bench_vs frames=<n> verts=<per frame> avg_ms=<ms>
 *
 * Its resolution comes from an A/A run (scripts/perf-abba.sh, the same build twice) before any A/B.
 * Shader bytes: scripts/sm3_corpus.py asm (the source is in each array's comment). */
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bench_window.h"

/* vs_3_0 | def c9, 3, 1, 0, 0 | dcl_position v0 | dcl_blendweight v1 | dcl_blendindices v2 | dcl_normal v3 | dcl_texcoord0 v4 | dcl_tangent v5 | dcl_position o0 | dcl_texcoord0 o1 | dcl_texcoord1 o2 | dcl_texcoord2 o3 | dcl_texcoord3 o4 | dcl_texcoord4 o5 | dcl_texcoord5 o6 | dcl_texcoord6 o7 | mul r0, v2, c9.x | mova a0, r0 | dp4 r1.x, v0, c[a0.x+10] | dp4 r1.y, v0, c[a0.x+11] | dp4 r1.z, v0, c[a0.x+12] | mul r1.xyz, r1, v1.x | dp4 r2.x, v0, c[a0.y+10] | dp4 r2.y, v0, c[a0.y+11] | dp4 r2.z, v0, c[a0.y+12] | mad r1.xyz, r2, v1.y, r1 | dp4 r2.x, v0, c[a0.z+10] | dp4 r2.y, v0, c[a0.z+11] | dp4 r2.z, v0, c[a0.z+12] | mad r1.xyz, r2, v1.z, r1 | dp4 r2.x, v0, c[a0.w+10] | dp4 r2.y, v0, c[a0.w+11] | dp4 r2.z, v0, c[a0.w+12] | mad r1.xyz, r2, v1.w, r1 | mov r1.w, c9.y | dp4 o0.x, r1, c0 | dp4 o0.y, r1, c1 | dp4 o0.z, r1, c2 | dp4 o0.w, r1, c3 | dp3 r3.x, v3, c[a0.x+10] | dp3 r3.y, v3, c[a0.x+11] | dp3 r3.z, v3, c[a0.x+12] | mul r3.xyz, r3, v1.x | dp3 r2.x, v3, c[a0.y+10] | dp3 r2.y, v3, c[a0.y+11] | dp3 r2.z, v3, c[a0.y+12] | mad r3.xyz, r2, v1.y, r3 | nrm r4.xyz, r3 | dp3 r5.x, v5, c[a0.x+10] | dp3 r5.y, v5, c[a0.x+11] | dp3 r5.z, v5, c[a0.x+12] | nrm r6.xyz, r5 | crs r7.xyz, r4, r6 | mul r7.xyz, r7, v5.w | add r8.xyz, c4, -r1 | nrm r9.xyz, r8 | mov o1, v4 | mov o2.xyz, r4 | mov o3.xyz, r6 | mov o4.xyz, r7 | mov o5, r1 | mov o6.xyz, r9 | dp4 o7.x, r1, c5 | dp4 o7.y, r1, c6 | dp4 o7.z, r1, c7 | dp4 o7.w, r1, c8 */
static const DWORD k_skin_vs[] = {
  0xfffe0300, 0x05000051, 0xa00f0009, 0x40400000, 0x3f800000, 0x00000000,
  0x00000000, 0x0200001f, 0x80000000, 0x900f0000, 0x0200001f, 0x80000001,
  0x900f0001, 0x0200001f, 0x80000002, 0x900f0002, 0x0200001f, 0x80000003,
  0x900f0003, 0x0200001f, 0x80000005, 0x900f0004, 0x0200001f, 0x80000006,
  0x900f0005, 0x0200001f, 0x80000000, 0xe00f0000, 0x0200001f, 0x80000005,
  0xe00f0001, 0x0200001f, 0x80010005, 0xe00f0002, 0x0200001f, 0x80020005,
  0xe00f0003, 0x0200001f, 0x80030005, 0xe00f0004, 0x0200001f, 0x80040005,
  0xe00f0005, 0x0200001f, 0x80050005, 0xe00f0006, 0x0200001f, 0x80060005,
  0xe00f0007, 0x03000005, 0x800f0000, 0x90e40002, 0xa0000009, 0x0200002e,
  0xb00f0000, 0x80e40000, 0x04000009, 0x80010001, 0x90e40000, 0xa0e4200a,
  0xb0000000, 0x04000009, 0x80020001, 0x90e40000, 0xa0e4200b, 0xb0000000,
  0x04000009, 0x80040001, 0x90e40000, 0xa0e4200c, 0xb0000000, 0x03000005,
  0x80070001, 0x80e40001, 0x90000001, 0x04000009, 0x80010002, 0x90e40000,
  0xa0e4200a, 0xb0550000, 0x04000009, 0x80020002, 0x90e40000, 0xa0e4200b,
  0xb0550000, 0x04000009, 0x80040002, 0x90e40000, 0xa0e4200c, 0xb0550000,
  0x04000004, 0x80070001, 0x80e40002, 0x90550001, 0x80e40001, 0x04000009,
  0x80010002, 0x90e40000, 0xa0e4200a, 0xb0aa0000, 0x04000009, 0x80020002,
  0x90e40000, 0xa0e4200b, 0xb0aa0000, 0x04000009, 0x80040002, 0x90e40000,
  0xa0e4200c, 0xb0aa0000, 0x04000004, 0x80070001, 0x80e40002, 0x90aa0001,
  0x80e40001, 0x04000009, 0x80010002, 0x90e40000, 0xa0e4200a, 0xb0ff0000,
  0x04000009, 0x80020002, 0x90e40000, 0xa0e4200b, 0xb0ff0000, 0x04000009,
  0x80040002, 0x90e40000, 0xa0e4200c, 0xb0ff0000, 0x04000004, 0x80070001,
  0x80e40002, 0x90ff0001, 0x80e40001, 0x02000001, 0x80080001, 0xa0550009,
  0x03000009, 0xe0010000, 0x80e40001, 0xa0e40000, 0x03000009, 0xe0020000,
  0x80e40001, 0xa0e40001, 0x03000009, 0xe0040000, 0x80e40001, 0xa0e40002,
  0x03000009, 0xe0080000, 0x80e40001, 0xa0e40003, 0x04000008, 0x80010003,
  0x90e40003, 0xa0e4200a, 0xb0000000, 0x04000008, 0x80020003, 0x90e40003,
  0xa0e4200b, 0xb0000000, 0x04000008, 0x80040003, 0x90e40003, 0xa0e4200c,
  0xb0000000, 0x03000005, 0x80070003, 0x80e40003, 0x90000001, 0x04000008,
  0x80010002, 0x90e40003, 0xa0e4200a, 0xb0550000, 0x04000008, 0x80020002,
  0x90e40003, 0xa0e4200b, 0xb0550000, 0x04000008, 0x80040002, 0x90e40003,
  0xa0e4200c, 0xb0550000, 0x04000004, 0x80070003, 0x80e40002, 0x90550001,
  0x80e40003, 0x02000024, 0x80070004, 0x80e40003, 0x04000008, 0x80010005,
  0x90e40005, 0xa0e4200a, 0xb0000000, 0x04000008, 0x80020005, 0x90e40005,
  0xa0e4200b, 0xb0000000, 0x04000008, 0x80040005, 0x90e40005, 0xa0e4200c,
  0xb0000000, 0x02000024, 0x80070006, 0x80e40005, 0x03000021, 0x80070007,
  0x80e40004, 0x80e40006, 0x03000005, 0x80070007, 0x80e40007, 0x90ff0005,
  0x03000002, 0x80070008, 0xa0e40004, 0x81e40001, 0x02000024, 0x80070009,
  0x80e40008, 0x02000001, 0xe00f0001, 0x90e40004, 0x02000001, 0xe0070002,
  0x80e40004, 0x02000001, 0xe0070003, 0x80e40006, 0x02000001, 0xe0070004,
  0x80e40007, 0x02000001, 0xe00f0005, 0x80e40001, 0x02000001, 0xe0070006,
  0x80e40009, 0x03000009, 0xe0010007, 0x80e40001, 0xa0e40005, 0x03000009,
  0xe0020007, 0x80e40001, 0xa0e40006, 0x03000009, 0xe0040007, 0x80e40001,
  0xa0e40007, 0x03000009, 0xe0080007, 0x80e40001, 0xa0e40008, 0x0000ffff,
};
/* ps_3_0 | dcl_texcoord0 v0 | dcl_texcoord1 v1 | mad oC0, v1, c1, c0 */
static const DWORD k_flat_ps[] = {
  0xffff0300, 0x0200001f, 0x80000005, 0x900f0000, 0x0200001f, 0x80010005,
  0x900f0001, 0x04000004, 0x800f0800, 0x90e40001, 0xa0e40001, 0xa0e40000,
  0x0000ffff,
};

#define NV (512 * 1024 - 2)   /* a multiple of 3 */
#define DRAWS 4
#define BONES 64

struct V {
  float x, y, z;
  BYTE w[4];       /* blend weights, UBYTE4N */
  BYTE i[4];       /* blend indices, UBYTE4 */
  float n[3];
  float uv[2];
  float t[4];
};

static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
  return DefWindowProcA(h, m, w, l);
}

static unsigned rng_state = 12345u;
static float frand(void) {  /* [0, 1) */
  rng_state = rng_state * 1664525u + 1013904223u;
  return (float)(rng_state >> 8) / 16777216.0f;
}

int main(int argc, char **argv) {
  int frames = argc > 1 ? atoi(argv[1]) : 560;
  if (frames <= 60) { printf("RESULT FAIL usage: frames must be more than the 60 warm-up frames\n"); return 2; }
  WNDCLASSA wc = {0};
  wc.lpfnWndProc = wnd_proc;
  wc.hInstance = GetModuleHandleA(NULL);
  wc.lpszClassName = "d9mt_bench_vs";
  RegisterClassA(&wc);
  HWND hwnd = bench_window("d9mt_bench_vs", "bench_vs", 640, 360);
  IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
  if (!hwnd || !d3d) { printf("RESULT FAIL window\n"); return 1; }
  D3DPRESENT_PARAMETERS pp = {0};
  pp.BackBufferWidth = 640; pp.BackBufferHeight = 360;
  pp.BackBufferFormat = D3DFMT_X8R8G8B8; pp.BackBufferCount = 1;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = hwnd;
  pp.Windowed = TRUE; pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
  IDirect3DDevice9 *dev;
  if (FAILED(IDirect3D9_CreateDevice(d3d, 0, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev))) {
    printf("RESULT FAIL device\n"); return 1;
  }

  static const D3DVERTEXELEMENT9 elems[] = {
    { 0, 0,  D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION,     0 },
    { 0, 12, D3DDECLTYPE_UBYTE4N, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_BLENDWEIGHT, 0 },
    { 0, 16, D3DDECLTYPE_UBYTE4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_BLENDINDICES, 0 },
    { 0, 20, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL,       0 },
    { 0, 32, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD,     0 },
    { 0, 40, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TANGENT,      0 },
    D3DDECL_END()
  };
  IDirect3DVertexDeclaration9 *decl;
  IDirect3DVertexShader9 *vs;
  IDirect3DPixelShader9 *ps;
  IDirect3DVertexBuffer9 *vb;
  if (FAILED(IDirect3DDevice9_CreateVertexDeclaration(dev, elems, &decl))
   || FAILED(IDirect3DDevice9_CreateVertexShader(dev, k_skin_vs, &vs))
   || FAILED(IDirect3DDevice9_CreatePixelShader(dev, k_flat_ps, &ps))
   || FAILED(IDirect3DDevice9_CreateVertexBuffer(dev, NV * sizeof(struct V), D3DUSAGE_WRITEONLY, 0,
                                                 D3DPOOL_MANAGED, &vb, NULL))) {
    printf("RESULT FAIL shaders or vertex buffer\n"); return 1;
  }
  struct V *v;
  if (FAILED(IDirect3DVertexBuffer9_Lock(vb, 0, 0, (void **)&v, 0))) { printf("RESULT FAIL lock\n"); return 1; }
  /* tiny triangles (about a pixel at 640x360) spread over the target; 4 bones each, weights
   * summing to 255 */
  for (int k = 0; k < NV / 3; k++) {
    const float cx = frand() * 1.9f - 0.95f, cy = frand() * 1.9f - 0.95f;
    BYTE w0 = (BYTE)(64 + frand() * 64), w1 = (BYTE)(frand() * 64), w2 = (BYTE)(frand() * 48);
    BYTE w[4] = { w0, w1, w2, (BYTE)(255 - w0 - w1 - w2) };
    BYTE bi[4] = { (BYTE)(frand() * BONES), (BYTE)(frand() * BONES), (BYTE)(frand() * BONES), (BYTE)(frand() * BONES) };
    static const float off[3][2] = { { 0.0f, 0.0f }, { 0.0031f, 0.0f }, { 0.0f, 0.0056f } };
    for (int c = 0; c < 3; c++) {
      struct V *p = &v[k * 3 + c];
      p->x = cx + off[c][0]; p->y = cy + off[c][1]; p->z = 0.5f;
      memcpy(p->w, w, 4); memcpy(p->i, bi, 4);
      p->n[0] = 0.0f; p->n[1] = 0.6f; p->n[2] = 0.8f;
      p->uv[0] = cx; p->uv[1] = cy;
      p->t[0] = 1.0f; p->t[1] = 0.0f; p->t[2] = 0.0f; p->t[3] = 1.0f;
    }
  }
  IDirect3DVertexBuffer9_Unlock(vb);

  /* c0..c3 the view-projection (identity: the positions are already in clip space), c4 the eye,
   * c5..c8 the shadow matrix, c10.. the palette: 64 bones of 3 rows, each close to identity */
  static float c[10 + 3 * BONES][4];
  memset(c, 0, sizeof(c));
  for (int i = 0; i < 4; i++) c[i][i] = 1.0f;
  c[4][2] = -2.0f; c[4][3] = 1.0f;
  for (int i = 0; i < 4; i++) c[5 + i][i] = 0.5f;
  for (int b = 0; b < BONES; b++) {
    const float s = 0.002f * (float)(b % 7), t = 0.001f * (float)(b % 5);
    float (*m)[4] = &c[10 + 3 * b];
    m[0][0] = 1.0f; m[0][1] = s;    m[0][3] = t;
    m[1][0] = -s;   m[1][1] = 1.0f; m[1][3] = -t;
    m[2][2] = 1.0f;
  }
  static const float ps_c[2][4] = { { 0.2f, 0.4f, 0.6f, 1.0f }, { 0.01f, 0.01f, 0.01f, 0.0f } };
  IDirect3DDevice9_SetVertexShaderConstantF(dev, 0, &c[0][0], 10 + 3 * BONES);
  IDirect3DDevice9_SetPixelShaderConstantF(dev, 0, &ps_c[0][0], 2);
  IDirect3DDevice9_SetVertexDeclaration(dev, decl);
  IDirect3DDevice9_SetStreamSource(dev, 0, vb, 0, sizeof(struct V));
  IDirect3DDevice9_SetVertexShader(dev, vs);
  IDirect3DDevice9_SetPixelShader(dev, ps);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, FALSE);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);

  LARGE_INTEGER f, t0, t1;
  QueryPerformanceFrequency(&f);
  MSG msg;
  for (int n = 1; n <= frames; n++) {
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
    if (n == 61) { bench_timed_mark(); QueryPerformanceCounter(&t0); }
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xff000000, 1.0f, 0);
    IDirect3DDevice9_BeginScene(dev);
    for (int d = 0; d < DRAWS; d++) {
      if (FAILED(IDirect3DDevice9_DrawPrimitive(dev, D3DPT_TRIANGLELIST, 0, NV / 3))) {
        printf("RESULT FAIL draw frame=%d\n", n); return 1;
      }
    }
    IDirect3DDevice9_EndScene(dev);
    if (FAILED(IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL))) { printf("RESULT FAIL present frame=%d\n", n); return 1; }
  }
  QueryPerformanceCounter(&t1);
  bench_timed_print("bench_vs");
  printf("RESULT bench_vs frames=%d verts=%d avg_ms=%.4f\n", frames - 60, NV * DRAWS,
         (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart / (frames - 60));
  IDirect3DDevice9_Release(dev);
  IDirect3D9_Release(d3d);
  return 0;
}
