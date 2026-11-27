/* code_lodbias: D3DSAMP_MIPMAPLODBIAS. A 64x64 texture with 7 mips, each a solid colour of its
 * own, point-sampled (POINT min, mag and mip filters) on screen-aligned quads, fixed function:
 *   A 64 px (LOD 0), bias 0 -> mip 0          B 64 px, bias +1 -> mip 1    C 64 px, bias +2 -> mip 2
 *   M 64 px, MAXMIPLEVEL 2, bias -2 -> mip 2  D 16 px (LOD 2), bias 0 -> mip 2
 *   E 16 px, bias -1 -> mip 1                 L 32 px (LOD 1), bias 0 -> mip 1
 *   U 32 px, bias -0.75 -> mip 0 (LOD 0.25)
 *   T 200 quads of 4 px, each with a bias of its own in 1/64 steps (200 sampler states): each drawn.
 * Only Apple10 GPUs (M5) apply a sampler LOD bias. Elsewhere, and with D9MT_SAMPLER_LOD_BIAS=0,
 * B, C, E and U sample as with bias 0 (mips 0, 0, 2 and 1).
 *
 * The centre of each quad is read back and A, M, D, L and T are checked on every Mac. The biased
 * quads print `INFO lodbias <quad>: mip N (bias applied: X, ignored: Y)`. They are checked only
 * with an argument: `applied` (an Apple10 GPU with the bias on) or `ignored` (any other case).
 * Prints `lodbias: N checks, M failed` and a FAIL line per wrong quad.
 *   code_lodbias.exe [applied|ignored] */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <string.h>

static const DWORD kColors[7] = { 0xffff0000, 0xff00ff00, 0xff0000ff, 0xffffff00, 0xffff00ff, 0xff00ffff, 0xffffffff };
static const DWORD kClear = 0xff181828;

struct VT { float x, y, z, rhw, u, v; };

static int g_checks, g_fail;

static void quad(IDirect3DDevice9 *dev, float x, float y, float s) {
  struct VT q[4] = { { x - 0.5f, y - 0.5f, 0.5f, 1, 0, 0 }, { x + s - 0.5f, y - 0.5f, 0.5f, 1, 1, 0 },
                     { x - 0.5f, y + s - 0.5f, 0.5f, 1, 0, 1 }, { x + s - 0.5f, y + s - 0.5f, 0.5f, 1, 1, 1 } };
  IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof(q[0]));
}

static void bias(IDirect3DDevice9 *dev, float b) {
  DWORD v; memcpy(&v, &b, sizeof(v));
  IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MIPMAPLODBIAS, v);
}

static DWORD pixel(const D3DLOCKED_RECT *lr, int x, int y) {
  return ((const DWORD *)((const char *)lr->pBits + y * lr->Pitch))[x] & 0xffffff;
}

static int mipOf(DWORD px) {
  for (int m = 0; m < 7; m++)
    if ((kColors[m] & 0xffffff) == px) return m;
  return -1;
}

static void check(const char *name, const D3DLOCKED_RECT *lr, int x, int y, int want) {
  const int got = mipOf(pixel(lr, x, y));
  g_checks++;
  if (got != want) {
    g_fail++;
    printf("FAIL lodbias %s: pixel %d,%d is mip %d (%06lx), want mip %d\n", name, x, y, got,
           (unsigned long)pixel(lr, x, y), want);
  }
}

/* mode: 0 report only, 1 bias applied, 2 bias ignored */
static void biased(const char *name, const D3DLOCKED_RECT *lr, int x, int y, int applied, int ignored, int mode) {
  printf("INFO lodbias %s: mip %d (bias applied: %d, ignored: %d)\n", name, mipOf(pixel(lr, x, y)), applied, ignored);
  if (mode)
    check(name, lr, x, y, mode == 1 ? applied : ignored);
}

int main(int argc, char **argv) {
  const int mode = argc > 1 && !strcmp(argv[1], "applied") ? 1 : argc > 1 && !strcmp(argv[1], "ignored") ? 2 : 0;
  if (argc > 1 && !mode) { printf("FAIL usage: code_lodbias.exe [applied|ignored]\n"); return 2; }
  WNDCLASSA wc = { 0 }; wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "d9mt_lodbias";
  RegisterClassA(&wc);
  HWND hwnd = CreateWindowA("d9mt_lodbias", "d9mt lodbias", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 340, 280, NULL, NULL, wc.hInstance, NULL);
  IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
  if (!d3d) { printf("FAIL d3d\n"); return 1; }
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
    if (FAILED(IDirect3DTexture9_LockRect(tex, l, &lr, NULL, 0))) { printf("FAIL lock mip %u\n", l); return 1; }
    for (UINT y = 0; y < dim; y++) for (UINT x = 0; x < dim; x++) ((DWORD *)((char *)lr.pBits + y * lr.Pitch))[x] = kColors[l];
    IDirect3DTexture9_UnlockRect(tex, l);
  }

  IDirect3DSurface9 *bb = NULL, *sys = NULL;
  IDirect3DDevice9_GetRenderTarget(dev, 0, &bb);
  IDirect3DDevice9_CreateOffscreenPlainSurface(dev, 320, 240, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, NULL);
  if (!bb || !sys) { printf("FAIL surfaces\n"); return 1; }
  IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)tex);
  IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
  IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
  IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MIPFILTER, D3DTEXF_POINT);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, FALSE);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
  IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_TEX1);

  for (int frame = 0; frame < 5; frame++) {
    MSG msg; while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) DispatchMessageA(&msg);
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, kClear, 1.0f, 0);
    IDirect3DDevice9_BeginScene(dev);
    bias(dev, 0.0f);   quad(dev, 0, 0, 64);      /* A */
    bias(dev, 1.0f);   quad(dev, 64, 0, 64);     /* B */
    bias(dev, 2.0f);   quad(dev, 128, 0, 64);    /* C */
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MAXMIPLEVEL, 2);
    bias(dev, -2.0f);  quad(dev, 192, 0, 64);    /* M */
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MAXMIPLEVEL, 0);
    bias(dev, 0.0f);   quad(dev, 0, 80, 16);     /* D */
    bias(dev, -1.0f);  quad(dev, 32, 80, 16);    /* E */
    bias(dev, 0.0f);   quad(dev, 64, 80, 32);    /* L */
    bias(dev, -0.75f); quad(dev, 112, 80, 32);   /* U */
    for (int i = 0; i < 200; i++) {               /* T */
      bias(dev, -1.5f + (float)i / 64.0f);
      quad(dev, (float)(i % 40) * 8.0f, 160.0f + (float)(i / 40) * 8.0f, 4);
    }
    bias(dev, 0.0f);
    IDirect3DDevice9_EndScene(dev);
    if (frame == 0) {
      D3DLOCKED_RECT lr;
      if (FAILED(IDirect3DDevice9_GetRenderTargetData(dev, bb, sys))
       || FAILED(IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY))) {
        printf("FAIL lodbias readback\n");
        return 1;
      }
      check("A 64 px, bias 0", &lr, 32, 32, 0);
      check("M MAXMIPLEVEL 2, bias -2", &lr, 224, 32, 2);
      check("D 16 px, bias 0", &lr, 8, 88, 2);
      check("L 32 px, bias 0", &lr, 80, 96, 1);
      biased("B 64 px, bias +1", &lr, 96, 32, 1, 0, mode);
      biased("C 64 px, bias +2", &lr, 160, 32, 2, 0, mode);
      biased("E 16 px, bias -1", &lr, 40, 88, 1, 2, mode);
      biased("U 32 px, bias -0.75", &lr, 128, 96, 0, 1, mode);
      for (int i = 0; i < 200; i++) {
        const int x = (i % 40) * 8 + 2, y = 160 + (i / 40) * 8 + 2;
        g_checks++;
        if (pixel(&lr, x, y) == (kClear & 0xffffff)) {
          g_fail++;
          printf("FAIL lodbias T quad %d (bias %.4f) not drawn\n", i, -1.5 + i / 64.0);
        }
      }
      IDirect3DSurface9_UnlockRect(sys);
      printf("lodbias: %d checks, %d failed\n", g_checks, g_fail);
      fflush(stdout);
    }
    IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
  }
  IDirect3DSurface9_Release(sys); IDirect3DSurface9_Release(bb);
  IDirect3DTexture9_Release(tex);
  IDirect3DDevice9_Release(dev); IDirect3D9_Release(d3d);
  return g_fail ? 1 : 0;
}
