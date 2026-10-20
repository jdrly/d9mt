/* code_lodbias: D3DSAMP_MIPMAPLODBIAS (docs/sampler-lod-bias.md). A 64x64 texture with 7 mips,
 * each a different solid colour, point-sampled (POINT mip filter) on screen-aligned quads:
 *   A 64x64 px (LOD 0)  bias  0 -> mip 0      B 64x64 px bias +1 -> mip 1
 *   C 64x64 px          bias +2 -> mip 2      D 16x16 px (LOD 2) bias 0 -> mip 2
 *   E 16x16 px          bias -1 -> mip 1
 * fixed function, then the same through an SM3 shader with texldb (bias from the coordinate's
 * w, D3D9 adds the sampler's):
 *   F 64x64 px texldb w=1, sampler bias 0 -> mip 1    G texldb w=1, sampler bias +1 -> mip 2
 *   H texldl LOD 1 + sampler bias +1: reported, not checked (Vulkan/DXVK adds the bias: mip 2;
 *     if Metal gives mip 1, that is documented, no IR fix; docs/sampler-lod-bias.md T-2)
 *   I 64x64 px, MAXMIPLEVEL 2, bias -2 -> stays at mip 2 (T-4)
 *   T-5: 200 draws, each with its own bias in 1/64 steps (200 sampler states): every quad drawn
 *     (no sampler creation failure: the log must not say "createSampler failed"). 
 * The centre of each quad is read back (GetRenderTargetData) and checked against the
 * expected mip's colour: "lodbias: N checks, M failed" plus a FAIL line per wrong quad. Base
 * fails B, C, E and G (the sampler bias never reaches Metal). */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <string.h>

static const DWORD kColors[7] = { 0xffff0000, 0xff00ff00, 0xff0000ff, 0xffffff00, 0xffff00ff, 0xff00ffff, 0xffffffff };

/* vs_3_0: dcl_position v0; dcl_texcoord v1; dcl_position o0; dcl_texcoord o1; mov o0, v0; mov o1, v1
 * (o# is D3DSPR_OUTPUT = 6) */
static const DWORD kVs[] = {
  0xFFFE0300,
  0x0200001F, 0x80000000, 0x900F0000,
  0x0200001F, 0x80000005, 0x900F0001,
  0x0200001F, 0x80000000, 0xE00F0000,
  0x0200001F, 0x80000005, 0xE00F0001,
  0x02000001, 0xE00F0000, 0x90E40000,
  0x02000001, 0xE00F0001, 0x90E40001,
  0x0000FFFF };
/* ps_3_0: dcl_texcoord v0; dcl_2d s0; texldb r0, v0, s0; mov oC0, r0 */
/* ps_3_0: dcl_texcoord v0; dcl_2d s0; texldl r0, v0, s0; mov oC0, r0 */
static const DWORD kPsL[] = {
  0xFFFF0300,
  0x0200001F, 0x80000005, 0x900F0000,
  0x0200001F, 0x90000000, 0xA00F0800,
  0x0300005F, 0x800F0000, 0x90E40000, 0xA0E40800,
  0x02000001, 0x800F0800, 0x80E40000,
  0x0000FFFF };
static const DWORD kPs[] = {
  0xFFFF0300,
  0x0200001F, 0x80000005, 0x900F0000,
  0x0200001F, 0x90000000, 0xA00F0800,
  0x03020042, 0x800F0000, 0x90E40000, 0xA0E40800,
  0x02000001, 0x800F0800, 0x80E40000,
  0x0000FFFF };

struct VT { float x, y, z, rhw, u, v; };
struct VS3 { float x, y, z, w, u, v, s, q; };

static int g_checks, g_fail;

static void quadFF(IDirect3DDevice9 *dev, float x, float y, float s) {
  struct VT q[4] = { { x - 0.5f, y - 0.5f, 0.5f, 1, 0, 0 }, { x + s - 0.5f, y - 0.5f, 0.5f, 1, 1, 0 },
                     { x - 0.5f, y + s - 0.5f, 0.5f, 1, 0, 1 }, { x + s - 0.5f, y + s - 0.5f, 0.5f, 1, 1, 1 } };
  IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof(q[0]));
}

/* An SM3 quad in pixels on a W x H target, texcoord w = bias for texldb. */
static void quadSM3(IDirect3DDevice9 *dev, float x, float y, float s, float W, float H, float bias) {
  float x0 = x / W * 2 - 1, x1 = (x + s) / W * 2 - 1, y0 = 1 - y / H * 2, y1 = 1 - (y + s) / H * 2;
  struct VS3 q[4] = { { x0, y0, 0.5f, 1, 0, 0, 0, bias }, { x1, y0, 0.5f, 1, 1, 0, 0, bias },
                      { x0, y1, 0.5f, 1, 0, 1, 0, bias }, { x1, y1, 0.5f, 1, 1, 1, 0, bias } };
  IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof(q[0]));
}

static void bias(IDirect3DDevice9 *dev, float b) {
  DWORD v; memcpy(&v, &b, sizeof(v));
  IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MIPMAPLODBIAS, v);
}

static void check(const char *name, D3DLOCKED_RECT *lr, int x, int y, int mip) {
  DWORD got = ((DWORD *)((char *)lr->pBits + y * lr->Pitch))[x] & 0xffffff;
  DWORD want = kColors[mip] & 0xffffff;
  g_checks++;
  if (got != want) {
    g_fail++;
    printf("FAIL lodbias %s: pixel %d,%d = %06lx, want mip %d %06lx\n", name, x, y, (unsigned long)got, mip, (unsigned long)want);
  }
}

int main(void) {
  WNDCLASSA wc = { 0 }; wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "d9mt_lodbias";
  RegisterClassA(&wc);
  HWND hwnd = CreateWindowA("d9mt_lodbias", "d9mt lodbias", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 340, 280, NULL, NULL, wc.hInstance, NULL);
  IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
  D3DPRESENT_PARAMETERS pp = { 0 };
  pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferFormat = D3DFMT_X8R8G8B8;
  pp.BackBufferWidth = 320; pp.BackBufferHeight = 240; pp.hDeviceWindow = hwnd;
  pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
  IDirect3DDevice9 *dev = NULL;
  if (FAILED(IDirect3D9_CreateDevice(d3d, 0, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev))) { printf("FAIL device\n"); return 1; }
  IDirect3DTexture9 *tex = NULL;
  if (FAILED(IDirect3DDevice9_CreateTexture(dev, 64, 64, 7, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, NULL))) { printf("FAIL texture\n"); return 1; }
  for (UINT l = 0; l < 7; l++) {
    D3DLOCKED_RECT lr; UINT dim = 64u >> l;
    IDirect3DTexture9_LockRect(tex, l, &lr, NULL, 0);
    for (UINT y = 0; y < dim; y++) for (UINT x = 0; x < dim; x++) ((DWORD *)((char *)lr.pBits + y * lr.Pitch))[x] = kColors[l];
    IDirect3DTexture9_UnlockRect(tex, l);
  }
  IDirect3DVertexShader9 *vs = NULL; IDirect3DPixelShader9 *ps = NULL, *psL = NULL; IDirect3DVertexDeclaration9 *decl = NULL;
  IDirect3DDevice9_CreateVertexShader(dev, kVs, &vs);
  IDirect3DDevice9_CreatePixelShader(dev, kPs, &ps);
  IDirect3DDevice9_CreatePixelShader(dev, kPsL, &psL);
  static const D3DVERTEXELEMENT9 el[] = {
    { 0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
    { 0, 16, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 }, D3DDECL_END() };
  IDirect3DDevice9_CreateVertexDeclaration(dev, el, &decl);
  if (!vs || !ps || !psL || !decl) { printf("FAIL shaders\n"); return 1; }

  IDirect3DSurface9 *bb = NULL, *sys = NULL;
  IDirect3DDevice9_GetRenderTarget(dev, 0, &bb);
  IDirect3DDevice9_CreateOffscreenPlainSurface(dev, 320, 240, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, NULL);
  IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)tex);
  IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
  IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
  IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MIPFILTER, D3DTEXF_POINT);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, FALSE);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);

  for (int frame = 0; frame < 5; frame++) {
    MSG msg; while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) DispatchMessageA(&msg);
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xff181828, 1.0f, 0);
    IDirect3DDevice9_BeginScene(dev);
    IDirect3DDevice9_SetVertexShader(dev, NULL); IDirect3DDevice9_SetPixelShader(dev, NULL);
    IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_TEX1);
    bias(dev, 0.0f);  quadFF(dev, 0, 0, 64);      /* A */
    bias(dev, 1.0f);  quadFF(dev, 64, 0, 64);     /* B */
    bias(dev, 2.0f);  quadFF(dev, 128, 0, 64);    /* C */
    bias(dev, 0.0f);  quadFF(dev, 0, 80, 16);     /* D */
    bias(dev, -1.0f); quadFF(dev, 32, 80, 16);    /* E */
    IDirect3DDevice9_SetVertexDeclaration(dev, decl);
    IDirect3DDevice9_SetVertexShader(dev, vs); IDirect3DDevice9_SetPixelShader(dev, ps);
    bias(dev, 0.0f);  quadSM3(dev, 0, 120, 64, 320, 240, 1.0f);   /* F */
    bias(dev, 1.0f);  quadSM3(dev, 64, 120, 64, 320, 240, 1.0f);  /* G */
    IDirect3DDevice9_SetPixelShader(dev, psL);
    bias(dev, 1.0f);  quadSM3(dev, 128, 120, 64, 320, 240, 1.0f); /* H: texldl LOD 1 */
    IDirect3DDevice9_SetVertexShader(dev, NULL); IDirect3DDevice9_SetPixelShader(dev, NULL);
    IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_TEX1);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MAXMIPLEVEL, 2);
    bias(dev, -2.0f); quadFF(dev, 192, 120, 64);  /* I */
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MAXMIPLEVEL, 0);
    for (int i = 0; i < 200; i++) {               /* T-5 */
      bias(dev, -1.5f + (float)i / 64.0f);
      quadFF(dev, (float)(i % 40) * 8.0f, 200.0f + (float)(i / 40) * 8.0f, 4);
    }
    bias(dev, 0.0f);
    IDirect3DDevice9_EndScene(dev);
    if (frame == 0) {
      D3DLOCKED_RECT lr;
      IDirect3DDevice9_GetRenderTargetData(dev, bb, sys);
      if (SUCCEEDED(IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY))) {
        check("A bias 0", &lr, 32, 32, 0);
        check("B bias +1", &lr, 96, 32, 1);
        check("C bias +2", &lr, 160, 32, 2);
        check("D minified bias 0", &lr, 8, 88, 2);
        check("E minified bias -1", &lr, 40, 88, 1);
        check("F texldb +1", &lr, 32, 152, 1);
        check("G texldb +1, sampler +1", &lr, 96, 152, 2);
        check("I MAXMIPLEVEL 2, bias -2", &lr, 224, 152, 2);
        {
          DWORD h = ((DWORD *)((char *)lr.pBits + 152 * lr.Pitch))[160] & 0xffffff;
          int mip = -1;
          for (int m = 0; m < 7; m++) if ((kColors[m] & 0xffffff) == h) mip = m;
          printf("INFO lodbias texldl LOD 1 + sampler bias +1 -> mip %d (DXVK/Vulkan: 2)\n", mip);
        }
        for (int i = 0; i < 200; i++) {
          int x = (i % 40) * 8 + 2, y = 200 + (i / 40) * 8 + 2;
          DWORD px = ((DWORD *)((char *)lr.pBits + y * lr.Pitch))[x] & 0xffffff;
          g_checks++;
          if (px == 0x181828) { g_fail++; printf("FAIL lodbias T-5 quad %d (bias %.4f) not drawn\n", i, -1.5 + i / 64.0); }
        }
        IDirect3DSurface9_UnlockRect(sys);
      }
      printf("lodbias: %d checks, %d failed\n", g_checks, g_fail);
      fflush(stdout);
    }
    IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
  }
  IDirect3DDevice9_Release(dev); IDirect3D9_Release(d3d);
  return 0;
}
