/* Shared setup for the docs/gpu-4k.md tests (code_vtf_rt, code_srgb_view,
 * code_depthtex, code_clear_split): a window, a device, the shaders from
 * test/shaders/<name>.asm and screen-space quads. Each test renders the same
 * scene every frame except where it says otherwise; d9h.h (run-hash-tests.sh)
 * hashes the backbuffer before each Present. */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <string.h>

static const unsigned char pos_uv_vs_bytecode[] = {
#include "pos_uv_vs_bytecode.inc"
};
static const unsigned char vtf_vs_bytecode[] = {
#include "vtf_vs_bytecode.inc"
};
static const unsigned char color_ps_bytecode[] = {
#include "color_ps_bytecode.inc"
};
static const unsigned char sample_ps_bytecode[] = {
#include "sample_ps_bytecode.inc"
};
static const unsigned char heavy_ps_bytecode[] = {
#include "heavy_ps_bytecode.inc"
};

struct TVertex {
  float x, y, z;
  float u, v;
};

static IDirect3DDevice9 *t_dev;
static UINT t_present_interval = D3DPRESENT_INTERVAL_ONE;  /* set before t_init */
static IDirect3DVertexDeclaration9 *t_decl;
static IDirect3DVertexShader9 *t_vs_pos, *t_vs_vtf;
static IDirect3DPixelShader9 *t_ps_color, *t_ps_sample, *t_ps_heavy;

#define T_CHECK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { \
    printf("FAIL %s: hr=%08lx (line %d)\n", #x, (unsigned long)hr_, __LINE__); fflush(stdout); ExitProcess(2); } } while (0)

static LRESULT CALLBACK t_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_DESTROY) {
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcA(hwnd, msg, wp, lp);
}

/* Window + device with a D24S8 auto depth-stencil, shaders, vertex declaration. */
static inline void t_init(const char *name, UINT w, UINT h) {
  WNDCLASSA wc = {0};
  wc.lpfnWndProc = t_wnd_proc;
  wc.hInstance = GetModuleHandleA(NULL);
  wc.lpszClassName = name;
  RegisterClassA(&wc);
  RECT rc = {0, 0, (LONG)w, (LONG)h};
  AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
  HWND hwnd = CreateWindowA(name, name, WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT,
                            CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
                            NULL, NULL, wc.hInstance, NULL);
  if (!hwnd) { printf("FAIL CreateWindow\n"); ExitProcess(2); }
  IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
  if (!d3d) { printf("FAIL Direct3DCreate9\n"); ExitProcess(2); }

  D3DPRESENT_PARAMETERS pp = {0};
  pp.BackBufferWidth = w;
  pp.BackBufferHeight = h;
  pp.BackBufferFormat = D3DFMT_X8R8G8B8;
  pp.BackBufferCount = 1;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.hDeviceWindow = hwnd;
  pp.Windowed = TRUE;
  pp.EnableAutoDepthStencil = TRUE;
  pp.AutoDepthStencilFormat = D3DFMT_D24S8;
  pp.PresentationInterval = t_present_interval;
  T_CHECK(IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                  D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &t_dev));

  T_CHECK(IDirect3DDevice9_CreateVertexShader(t_dev, (const DWORD *)pos_uv_vs_bytecode, &t_vs_pos));
  T_CHECK(IDirect3DDevice9_CreateVertexShader(t_dev, (const DWORD *)vtf_vs_bytecode, &t_vs_vtf));
  T_CHECK(IDirect3DDevice9_CreatePixelShader(t_dev, (const DWORD *)color_ps_bytecode, &t_ps_color));
  T_CHECK(IDirect3DDevice9_CreatePixelShader(t_dev, (const DWORD *)sample_ps_bytecode, &t_ps_sample));
  T_CHECK(IDirect3DDevice9_CreatePixelShader(t_dev, (const DWORD *)heavy_ps_bytecode, &t_ps_heavy));

  static const D3DVERTEXELEMENT9 elems[] = {
      {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
      {0, 12, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
      D3DDECL_END()};
  T_CHECK(IDirect3DDevice9_CreateVertexDeclaration(t_dev, elems, &t_decl));
  IDirect3DDevice9_SetVertexDeclaration(t_dev, t_decl);
  IDirect3DDevice9_SetRenderState(t_dev, D3DRS_CULLMODE, D3DCULL_NONE);
  IDirect3DDevice9_SetRenderState(t_dev, D3DRS_LIGHTING, FALSE);
  for (DWORD s = 0; s < 4; s++) {
    IDirect3DDevice9_SetSamplerState(t_dev, s, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    IDirect3DDevice9_SetSamplerState(t_dev, s, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    IDirect3DDevice9_SetSamplerState(t_dev, s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    IDirect3DDevice9_SetSamplerState(t_dev, s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    IDirect3DDevice9_SetSamplerState(t_dev, s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
  }
}

/* Quad in clip space x0..x1, y0..y1 (y up) at depth z, uv 0..1 over it. */
static inline void t_quad(float x0, float y0, float x1, float y1, float z) {
  struct TVertex v[4] = {
      {x0, y1, z, 0.0f, 0.0f}, {x1, y1, z, 1.0f, 0.0f},
      {x0, y0, z, 0.0f, 1.0f}, {x1, y0, z, 1.0f, 1.0f},
  };
  T_CHECK(IDirect3DDevice9_DrawPrimitiveUP(t_dev, D3DPT_TRIANGLESTRIP, 2, v, sizeof(v[0])));
}

static inline void t_color(float r, float g, float b, float a) {
  const float c[4] = {r, g, b, a};
  IDirect3DDevice9_SetPixelShaderConstantF(t_dev, 0, c, 1);
}

/* texcoord.zw of pos_uv_vs / vtf_vs */
static inline void t_tex_zw(float z, float w) {
  const float c[4] = {0.0f, 0.0f, z, w};
  IDirect3DDevice9_SetVertexShaderConstantF(t_dev, 5, c, 1);
}

static inline IDirect3DSurface9 *t_level0(IDirect3DTexture9 *t) {
  IDirect3DSurface9 *s = NULL;
  T_CHECK(IDirect3DTexture9_GetSurfaceLevel(t, 0, &s));
  return s;
}

/* Runs frame(n) and presents until d9h.h ends the process (or 600 frames). */
static inline int t_loop(void (*frame)(int)) {
  MSG msg;
  for (int n = 1; n <= 600; n++) {
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT)
        return 0;
      TranslateMessage(&msg);
      DispatchMessageA(&msg);
    }
    frame(n);
    IDirect3DDevice9_Present(t_dev, NULL, NULL, NULL, NULL);
  }
  return 0;
}
