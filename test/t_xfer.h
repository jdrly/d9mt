/* Shared setup for code_stretch and code_colorfill (#14): a window, a device,
 * the texquad shaders, and readbacks that compare pixels with expected
 * values. Each failed comparison prints a FAIL line (run-hash-tests.sh marks
 * the test FAIL); the checks run in the first frame, every frame renders the
 * same. Colours are chosen so that 5/6-bit formats hold them exactly. */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <string.h>

static const unsigned char texquad_vs_bytecode[] = {
#include "texquad_vs_bytecode.inc"
};
static const unsigned char texquad_ps_bytecode[] = {
#include "texquad_ps_bytecode.inc"
};

static IDirect3DDevice9 *x_dev;
static IDirect3DSurface9 *x_backBuffer;
static int x_checks, x_failed;

#define X_CHECK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { \
    printf("FAIL %s: hr=%08lx (line %d)\n", #x, (unsigned long)hr_, __LINE__); fflush(stdout); ExitProcess(2); } } while (0)

static LRESULT CALLBACK x_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_DESTROY) {
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcA(hwnd, msg, wp, lp);
}

static inline IDirect3D9 *x_init(const char *name) {
  WNDCLASSA wc = {0};
  wc.lpfnWndProc = x_wnd_proc;
  wc.hInstance = GetModuleHandleA(NULL);
  wc.lpszClassName = name;
  RegisterClassA(&wc);
  RECT rc = {0, 0, 640, 480};
  AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
  HWND hwnd = CreateWindowA(name, name, WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                            rc.right - rc.left, rc.bottom - rc.top, NULL, NULL, wc.hInstance, NULL);
  IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
  if (!hwnd || !d3d) { printf("FAIL window/d3d\n"); ExitProcess(2); }
  D3DPRESENT_PARAMETERS pp = {0};
  pp.BackBufferWidth = 640;
  pp.BackBufferHeight = 480;
  pp.BackBufferFormat = D3DFMT_X8R8G8B8;
  pp.BackBufferCount = 1;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.hDeviceWindow = hwnd;
  pp.Windowed = TRUE;
  pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
  X_CHECK(IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                  D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &x_dev));
  IDirect3DVertexShader9 *vs = NULL;
  IDirect3DPixelShader9 *ps = NULL;
  X_CHECK(IDirect3DDevice9_CreateVertexShader(x_dev, (const DWORD *)texquad_vs_bytecode, &vs));
  X_CHECK(IDirect3DDevice9_CreatePixelShader(x_dev, (const DWORD *)texquad_ps_bytecode, &ps));
  static const D3DVERTEXELEMENT9 elems[] = {
      {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
      {0, 12, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
      D3DDECL_END()};
  IDirect3DVertexDeclaration9 *decl = NULL;
  X_CHECK(IDirect3DDevice9_CreateVertexDeclaration(x_dev, elems, &decl));
  static const float mvp[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  IDirect3DDevice9_SetVertexShaderConstantF(x_dev, 0, mvp, 4);
  IDirect3DDevice9_SetVertexDeclaration(x_dev, decl);
  IDirect3DDevice9_SetVertexShader(x_dev, vs);
  IDirect3DDevice9_SetPixelShader(x_dev, ps);
  IDirect3DDevice9_SetRenderState(x_dev, D3DRS_CULLMODE, D3DCULL_NONE);
  IDirect3DDevice9_SetRenderState(x_dev, D3DRS_ZENABLE, D3DZB_FALSE);
  IDirect3DDevice9_SetSamplerState(x_dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
  IDirect3DDevice9_SetSamplerState(x_dev, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
  IDirect3DDevice9_SetSamplerState(x_dev, 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
  X_CHECK(IDirect3DDevice9_GetRenderTarget(x_dev, 0, &x_backBuffer));
  return d3d;
}

/* Draws texture t over clip-space x0..x1, y0..y1 (y up) of the current target. */
static inline void x_draw(IDirect3DBaseTexture9 *t, float x0, float y0, float x1, float y1) {
  struct { float x, y, z, u, v; } q[4] = {
      {x0, y1, 0.5f, 0.0f, 0.0f}, {x1, y1, 0.5f, 1.0f, 0.0f},
      {x0, y0, 0.5f, 0.0f, 1.0f}, {x1, y0, 0.5f, 1.0f, 1.0f}};
  IDirect3DDevice9_SetTexture(x_dev, 0, t);
  IDirect3DDevice9_DrawPrimitiveUP(x_dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof(q[0]));
  IDirect3DDevice9_SetTexture(x_dev, 0, NULL);
}

/* A copy of a render-target surface (GetRenderTargetData) or of a lockable
 * default-pool surface (LockRect), locked for reading. */
typedef struct {
  IDirect3DSurface9 *sys;
  IDirect3DSurface9 *locked;
  D3DLOCKED_RECT lr;
  D3DSURFACE_DESC desc;
} XReadback;

static inline XReadback x_read(IDirect3DSurface9 *s) {
  XReadback r = {0};
  IDirect3DSurface9_GetDesc(s, &r.desc);
  if (r.desc.Usage & D3DUSAGE_RENDERTARGET) {
    X_CHECK(IDirect3DDevice9_CreateOffscreenPlainSurface(x_dev, r.desc.Width, r.desc.Height, r.desc.Format,
                                                         D3DPOOL_SYSTEMMEM, &r.sys, NULL));
    X_CHECK(IDirect3DDevice9_GetRenderTargetData(x_dev, s, r.sys));
    r.locked = r.sys;
  } else {
    r.locked = s;
  }
  X_CHECK(IDirect3DSurface9_LockRect(r.locked, &r.lr, NULL, D3DLOCK_READONLY));
  return r;
}

static inline void x_done(XReadback *r) {
  IDirect3DSurface9_UnlockRect(r->locked);
  if (r->sys)
    IDirect3DSurface9_Release(r->sys);
}

static inline DWORD x_pixel(const XReadback *r, UINT x, UINT y) {
  const unsigned char *row = (const unsigned char *)r->lr.pBits + (size_t)y * r->lr.Pitch;
  switch (r->desc.Format) {
    case D3DFMT_R5G6B5:
    case D3DFMT_X1R5G5B5:
    case D3DFMT_A1R5G5B5:
      return ((const WORD *)row)[x];
    default:
      return ((const DWORD *)row)[x];
  }
}

/* Pixel (x, y) of r, masked, must be `want`. */
static inline void x_expect(const char *what, const XReadback *r, UINT x, UINT y, DWORD want, DWORD mask) {
  const DWORD got = x_pixel(r, x, y) & mask;
  x_checks++;
  if (got != (want & mask)) {
    x_failed++;
    printf("FAIL %s at %u,%u: got %08lx want %08lx (mask %08lx)\n", what, x, y,
           (unsigned long)got, (unsigned long)(want & mask), (unsigned long)mask);
  }
}

static inline void x_report(const char *name) {
  printf("%s: %d checks, %d failed\n", name, x_checks, x_failed);
  fflush(stdout);
}

static inline IDirect3DSurface9 *x_level(IDirect3DTexture9 *t, UINT level) {
  IDirect3DSurface9 *s = NULL;
  X_CHECK(IDirect3DTexture9_GetSurfaceLevel(t, level, &s));
  return s;
}

static inline int x_loop(void (*frame)(int)) {
  MSG msg;
  for (int n = 1; n <= 600; n++) {
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT)
        return 0;
      TranslateMessage(&msg);
      DispatchMessageA(&msg);
    }
    frame(n);
    IDirect3DDevice9_Present(x_dev, NULL, NULL, NULL, NULL);
  }
  return 0;
}
