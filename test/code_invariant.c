/* code_invariant: position invariance between vertex shaders (the 0.1.8 recompile bundle,
 * "position invariance"). D3D9 games draw the same geometry with different vertex shaders across
 * passes and depth-test the later passes with EQUAL; dxbc-spirv marks the SM3 position output
 * invariant for that, and AirBuilder writes Apple's -fpreserve-invariance form for it.
 *
 * Every frame, on a D24S8 depth buffer cleared to 1:
 *   pass 1: VS A (position = dp4 of v0 with c0..c3, nothing else), ZFUNC LESS, Z write on, red;
 *   pass 2: VS B (the same position expression graph, plus unrelated outputs computed from the
 *           same input with independent arithmetic, which changes scheduling and register
 *           pressure; the PS reads them and multiplies them by 0), ZFUNC EQUAL, Z write off, green.
 * The mesh is a 64 x 48 heightfield (no pixel covered twice) under a rotated perspective
 * transform, so the depth values are far from exact. A pixel left red failed the EQUAL test.
 * On frame 3 the backbuffer is read back and the test prints
 *   "code_invariant: <n> failing pixels of <m> covered: PASS|FAIL"
 * (PASS: n == 0 and m > 0). D9MT_AIR_INVARIANT=0 turns the invariance off, as a contrast; if the
 * base (no invariance) also passes on a GPU, the test documents the guarantee there rather than
 * proving a fix. Algebraically different position formulas aren't expected to match and aren't
 * tested. An FF-then-SM3 variant (pass 1 drawn by the fixed function pipeline) comes with the
 * fixed-function batch.
 * Shader bytes: scripts/sm3_corpus.py asm (the source is in each array's comment). */
#include <windows.h>
#include <d3d9.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* vs_3_0 | dcl_position v0 | dcl_position o0 | dp4 o0.x, v0, c0 | dp4 o0.y, v0, c1 | dp4 o0.z, v0, c2 | dp4 o0.w, v0, c3 */
static const DWORD k_vs_a[] = {
  0xfffe0300, 0x0200001f, 0x80000000, 0x900f0000, 0x0200001f, 0x80000000,
  0xe00f0000, 0x03000009, 0xe0010000, 0x90e40000, 0xa0e40000, 0x03000009,
  0xe0020000, 0x90e40000, 0xa0e40001, 0x03000009, 0xe0040000, 0x90e40000,
  0xa0e40002, 0x03000009, 0xe0080000, 0x90e40000, 0xa0e40003, 0x0000ffff,
};
/* vs_3_0 | def c8, 0.37, 1.91, -0.53, 2.75 | dcl_position v0 | dcl_position o0 | dcl_texcoord0 o1 | dcl_texcoord1 o2 | dcl_texcoord2 o3 | mul r0, v0.yzxw, c8 | mad r1, v0.zxyw, c8.wzyx, r0 | dp4 o0.x, v0, c0 | mad r2, r1, r1, c8 | dp4 o0.y, v0, c1 | mul r3, r2.wzyx, r0 | dp4 o0.z, v0, c2 | mad r4, r3, c8.y, r1 | dp4 o0.w, v0, c3 | mov o1, r2 | mov o2, r3 | mov o3, r4 */
static const DWORD k_vs_b[] = {
  0xfffe0300, 0x05000051, 0xa00f0008, 0x3ebd70a4, 0x3ff47ae1, 0xbf07ae14,
  0x40300000, 0x0200001f, 0x80000000, 0x900f0000, 0x0200001f, 0x80000000,
  0xe00f0000, 0x0200001f, 0x80000005, 0xe00f0001, 0x0200001f, 0x80010005,
  0xe00f0002, 0x0200001f, 0x80020005, 0xe00f0003, 0x03000005, 0x800f0000,
  0x90c90000, 0xa0e40008, 0x04000004, 0x800f0001, 0x90d20000, 0xa01b0008,
  0x80e40000, 0x03000009, 0xe0010000, 0x90e40000, 0xa0e40000, 0x04000004,
  0x800f0002, 0x80e40001, 0x80e40001, 0xa0e40008, 0x03000009, 0xe0020000,
  0x90e40000, 0xa0e40001, 0x03000005, 0x800f0003, 0x801b0002, 0x80e40000,
  0x03000009, 0xe0040000, 0x90e40000, 0xa0e40002, 0x04000004, 0x800f0004,
  0x80e40003, 0xa0550008, 0x80e40001, 0x03000009, 0xe0080000, 0x90e40000,
  0xa0e40003, 0x02000001, 0xe00f0001, 0x80e40002, 0x02000001, 0xe00f0002,
  0x80e40003, 0x02000001, 0xe00f0003, 0x80e40004, 0x0000ffff,
};
/* ps_3_0 | mov oC0, c0 */
static const DWORD k_ps_a[] = {
  0xffff0300, 0x02000001, 0x800f0800, 0xa0e40000, 0x0000ffff,
};
/* ps_3_0 | dcl_texcoord0 v0 | dcl_texcoord1 v1 | dcl_texcoord2 v2 | mul r0, v0, c1 | mad r0, v1, c1, r0 | mad r0, v2, c1, r0 | add oC0, r0, c0 */
static const DWORD k_ps_b[] = {
  0xffff0300, 0x0200001f, 0x80000005, 0x900f0000, 0x0200001f, 0x80010005,
  0x900f0001, 0x0200001f, 0x80020005, 0x900f0002, 0x03000005, 0x800f0000,
  0x90e40000, 0xa0e40001, 0x04000004, 0x800f0000, 0x90e40001, 0xa0e40001,
  0x80e40000, 0x04000004, 0x800f0000, 0x90e40002, 0xa0e40001, 0x80e40000,
  0x03000002, 0x800f0800, 0x80e40000, 0xa0e40000, 0x0000ffff,
};

#define GW 64
#define GH 48

struct V { float x, y, z; };

static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
  return DefWindowProcA(h, m, w, l);
}

/* row-vector 4x4: out = a * b */
static void mat_mul(double out[4][4], const double a[4][4], const double b[4][4]) {
  double t[4][4];
  for (int i = 0; i < 4; i++)
    for (int j = 0; j < 4; j++) {
      t[i][j] = 0.0;
      for (int k = 0; k < 4; k++) t[i][j] += a[i][k] * b[k][j];
    }
  memcpy(out, t, sizeof(t));
}

/* c0..c3 for dp4 o0.<i>, v0, c<i>: the columns of rotate-x * rotate-y * translate * perspective */
static void transform(float c[16]) {
  const double ax = 0.43, ay = 0.31, n = 0.5, f = 20.0, q = f / (f - n);
  const double h = 1.0 / tan(0.5 * 1.0472), w = h * 0.75;
  const double rx[4][4] = { { 1, 0, 0, 0 }, { 0, cos(ax), sin(ax), 0 }, { 0, -sin(ax), cos(ax), 0 }, { 0, 0, 0, 1 } };
  const double ry[4][4] = { { cos(ay), 0, -sin(ay), 0 }, { 0, 1, 0, 0 }, { sin(ay), 0, cos(ay), 0 }, { 0, 0, 0, 1 } };
  const double tr[4][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 0.07, -0.05, 3.2, 1 } };
  const double pr[4][4] = { { w, 0, 0, 0 }, { 0, h, 0, 0 }, { 0, 0, q, 1 }, { 0, 0, -q * n, 0 } };
  double m[4][4];
  mat_mul(m, rx, ry);
  mat_mul(m, m, tr);
  mat_mul(m, m, pr);
  for (int i = 0; i < 4; i++)
    for (int j = 0; j < 4; j++)
      c[i * 4 + j] = (float)m[j][i];
}

/* frame 3: count red (pass 2 failed EQUAL) and covered pixels in the backbuffer */
static int count_pixels(IDirect3DDevice9 *dev, unsigned *failing, unsigned *covered) {
  IDirect3DSurface9 *rt = NULL, *sys = NULL;
  D3DSURFACE_DESC desc;
  D3DLOCKED_RECT lr;
  *failing = *covered = 0;
  if (FAILED(IDirect3DDevice9_GetRenderTarget(dev, 0, &rt))) return 0;
  IDirect3DSurface9_GetDesc(rt, &desc);
  if (FAILED(IDirect3DDevice9_CreateOffscreenPlainSurface(dev, desc.Width, desc.Height, desc.Format,
                                                          D3DPOOL_SYSTEMMEM, &sys, NULL))
   || FAILED(IDirect3DDevice9_GetRenderTargetData(dev, rt, sys))
   || FAILED(IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY))) {
    if (sys) IDirect3DSurface9_Release(sys);
    IDirect3DSurface9_Release(rt);
    return 0;
  }
  for (UINT y = 0; y < desc.Height; y++) {
    const DWORD *row = (const DWORD *)((const BYTE *)lr.pBits + y * lr.Pitch);
    for (UINT x = 0; x < desc.Width; x++) {
      DWORD p = row[x] & 0x00ffffffu;
      if (p == 0x00ff0000u) { (*failing)++; (*covered)++; }
      else if (p == 0x0000ff00u) (*covered)++;
    }
  }
  IDirect3DSurface9_UnlockRect(sys);
  IDirect3DSurface9_Release(sys);
  IDirect3DSurface9_Release(rt);
  return 1;
}

int main(void) {
  WNDCLASSA wc = {0};
  wc.lpfnWndProc = wnd_proc;
  wc.hInstance = GetModuleHandleA(NULL);
  wc.lpszClassName = "d9mt_invariant";
  RegisterClassA(&wc);
  RECT rc = {0, 0, 640, 480};
  AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
  HWND hwnd = CreateWindowA("d9mt_invariant", "d9mt position invariance", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                            CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, NULL, NULL, wc.hInstance, NULL);
  if (!hwnd) return 1;
  IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
  if (!d3d) return 1;
  D3DPRESENT_PARAMETERS pp = {0};
  pp.BackBufferWidth = 640; pp.BackBufferHeight = 480;
  pp.BackBufferFormat = D3DFMT_X8R8G8B8; pp.BackBufferCount = 1;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = hwnd;
  pp.Windowed = TRUE; pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
  pp.EnableAutoDepthStencil = TRUE; pp.AutoDepthStencilFormat = D3DFMT_D24S8;
  IDirect3DDevice9 *dev;
  if (FAILED(IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                     D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev)))
    return 1;

  static const D3DVERTEXELEMENT9 elems[] = {
    { 0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
    D3DDECL_END()
  };
  IDirect3DVertexDeclaration9 *decl;
  IDirect3DVertexShader9 *vsA, *vsB;
  IDirect3DPixelShader9 *psA, *psB;
  if (FAILED(IDirect3DDevice9_CreateVertexDeclaration(dev, elems, &decl))
   || FAILED(IDirect3DDevice9_CreateVertexShader(dev, k_vs_a, &vsA))
   || FAILED(IDirect3DDevice9_CreateVertexShader(dev, k_vs_b, &vsB))
   || FAILED(IDirect3DDevice9_CreatePixelShader(dev, k_ps_a, &psA))
   || FAILED(IDirect3DDevice9_CreatePixelShader(dev, k_ps_b, &psB))) {
    printf("FAIL: shader or declaration creation\n");
    return 1;
  }

  /* the heightfield: (GW+1) x (GH+1) vertices, two triangles per cell */
  static struct V verts[(GW + 1) * (GH + 1)];
  static WORD idx[GW * GH * 6];
  for (int j = 0; j <= GH; j++)
    for (int i = 0; i <= GW; i++) {
      float x = -1.6f + 3.2f * (float)i / GW, y = -1.2f + 2.4f * (float)j / GH;
      struct V v = { x, y, 0.25f * sinf(3.1f * x) * cosf(2.3f * y) };
      verts[j * (GW + 1) + i] = v;
    }
  int n = 0;
  for (int j = 0; j < GH; j++)
    for (int i = 0; i < GW; i++) {
      WORD a = (WORD)(j * (GW + 1) + i), b = (WORD)(a + 1), c = (WORD)(a + GW + 1), d = (WORD)(c + 1);
      idx[n++] = a; idx[n++] = b; idx[n++] = c;
      idx[n++] = c; idx[n++] = b; idx[n++] = d;
    }
  IDirect3DVertexBuffer9 *vb;
  IDirect3DIndexBuffer9 *ib;
  void *p;
  if (FAILED(IDirect3DDevice9_CreateVertexBuffer(dev, sizeof(verts), D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &vb, NULL))
   || FAILED(IDirect3DDevice9_CreateIndexBuffer(dev, sizeof(idx), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &ib, NULL)))
    return 1;
  IDirect3DVertexBuffer9_Lock(vb, 0, 0, &p, 0); memcpy(p, verts, sizeof(verts)); IDirect3DVertexBuffer9_Unlock(vb);
  IDirect3DIndexBuffer9_Lock(ib, 0, 0, &p, 0); memcpy(p, idx, sizeof(idx)); IDirect3DIndexBuffer9_Unlock(ib);

  float c[16];
  transform(c);
  static const float red[4] = { 1, 0, 0, 1 }, green[4] = { 0, 1, 0, 1 }, zero[4] = { 0, 0, 0, 0 };

  IDirect3DDevice9_SetVertexDeclaration(dev, decl);
  IDirect3DDevice9_SetStreamSource(dev, 0, vb, 0, sizeof(struct V));
  IDirect3DDevice9_SetIndices(dev, ib);
  IDirect3DDevice9_SetVertexShaderConstantF(dev, 0, c, 4);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, D3DZB_TRUE);

  int frames = 0;
  MSG msg;
  for (;;) {
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) goto done;
      TranslateMessage(&msg);
      DispatchMessageA(&msg);
    }
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(16, 16, 24), 1.0f, 0);
    IDirect3DDevice9_BeginScene(dev);
    /* pass 1 */
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZFUNC, D3DCMP_LESS);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZWRITEENABLE, TRUE);
    IDirect3DDevice9_SetVertexShader(dev, vsA);
    IDirect3DDevice9_SetPixelShader(dev, psA);
    IDirect3DDevice9_SetPixelShaderConstantF(dev, 0, red, 1);
    IDirect3DDevice9_DrawIndexedPrimitive(dev, D3DPT_TRIANGLELIST, 0, 0, (GW + 1) * (GH + 1), 0, GW * GH * 2);
    /* pass 2 */
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZFUNC, D3DCMP_EQUAL);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZWRITEENABLE, FALSE);
    IDirect3DDevice9_SetVertexShader(dev, vsB);
    IDirect3DDevice9_SetPixelShader(dev, psB);
    IDirect3DDevice9_SetPixelShaderConstantF(dev, 0, green, 1);
    IDirect3DDevice9_SetPixelShaderConstantF(dev, 1, zero, 1);
    IDirect3DDevice9_DrawIndexedPrimitive(dev, D3DPT_TRIANGLELIST, 0, 0, (GW + 1) * (GH + 1), 0, GW * GH * 2);
    IDirect3DDevice9_EndScene(dev);
    if (++frames == 3) {
      unsigned failing, covered;
      if (!count_pixels(dev, &failing, &covered)) {
        printf("code_invariant: readback failed: FAIL\n");
      } else {
        printf("code_invariant: %u failing pixels of %u covered: %s\n", failing, covered,
               failing == 0 && covered > 0 ? "PASS" : "FAIL");
      }
      fflush(stdout);
    }
    IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
  }
done:
  IDirect3DDevice9_Release(dev);
  IDirect3D9_Release(d3d);
  return 0;
}
