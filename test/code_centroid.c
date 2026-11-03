/* code_centroid: centroid interpolation on a 4x MSAA target (the 0.1.8 recompile bundle,
 * "AirBuilder honours centroid and flat"). A 4x render target cleared to opaque black, blending
 * off, no sRGB, no alpha test; resolved (StretchRect) and read back (GetRenderTargetData).
 * Pre-transformed vertices; D3D9 pixel centres are at integer coordinates, which d9mt maps to
 * Metal's centres at +0.5, where Metal's standard 4x pattern puts two samples of each pixel at
 * y + 0.125 and y + 0.375.
 *   1. COLOR: a sliver triangle whose long edge runs at y = 99.95 (Metal 100.45) from x 20 to
 *      620, apex (320, 96.95). Row 100's pixel centres lie outside it, two of their samples
 *      inside. COLOR0.r is 0 on that edge and 1 at the apex, so it's negative just outside.
 *      The PS writes magenta where r < 0 or r > 1, green otherwise. Oracle: a resolved pixel
 *      with R > 0 holds an extrapolated sample. dxbc-spirv declares every COLOR input
 *      centroid; the count must be 0 here, and > 0 on base (centre sampling).
 *   2. Atlas: the same sliver 200 px lower with dcl_texcoord0_centroid, a POINT, CLAMP sampler
 *      on a 4x4 texture (inner 2x2 green, the border white), the UVs mapping the triangle onto
 *      the inner texels (its long edge at v = 0.75, the inner texels' edge). Oracle: a resolved
 *      pixel with blue > 0 holds a white (border) sample; only white has blue, and point
 *      filtering rules out blending. 0 here, > 0 on base.
 *   3. The same two draws at 1x on the backbuffer, every frame: their frames must hash as base
 *      (at 1x, centroid = centre).
 * Prints "code_centroid color: <n> pixels with an extrapolated sample", "code_centroid atlas:
 * <n> pixels with a border sample" and "code_centroid: PASS|FAIL" (PASS = both 0) once. If base
 * gives 0 for a case, its geometry doesn't test centroid and has to change.
 * Shader bytes: scripts/sm3_corpus.py assemble (the source is in each array's comment). */
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>

/* ps_3_0 | def c0, 1, 0, 1, 1 | def c1, 0, 1, 0, 1 | def c2, 1, 1, 1, 1 | dcl_color0 v0 | add r0.x, c2.x, -v0.x | min r0.x, v0.x, r0.x | cmp oC0, r0.x, c1, c0 */
static const DWORD k_ps_range[] = {
  0xffff0300, 0x05000051, 0xa00f0000, 0x3f800000, 0x00000000, 0x3f800000,
  0x3f800000, 0x05000051, 0xa00f0001, 0x00000000, 0x3f800000, 0x00000000,
  0x3f800000, 0x05000051, 0xa00f0002, 0x3f800000, 0x3f800000, 0x3f800000,
  0x3f800000, 0x0200001f, 0x8000000a, 0x900f0000, 0x03000002, 0x80010000,
  0xa0000002, 0x91000000, 0x0300000a, 0x80010000, 0x90000000, 0x80000000,
  0x04000058, 0x800f0800, 0x80000000, 0xa0e40001, 0xa0e40000, 0x0000ffff,
};
/* ps_3_0 | dcl_texcoord0_centroid v0 | dcl_2d s0 | texld r0, v0, s0 | mov oC0, r0 */
static const DWORD k_ps_atlas[] = {
  0xffff0300, 0x0200001f, 0x80000005, 0x904f0000, 0x0200001f, 0x90000000,
  0xa00f0800, 0x03000042, 0x800f0000, 0x90e40000, 0xa0e40800, 0x02000001,
  0x800f0800, 0x80e40000, 0x0000ffff,
};

struct VC { float x, y, z, rhw; DWORD c; };
struct VT { float x, y, z, rhw; float u, v; };
#define W 640
#define H 480

static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
  return DefWindowProcA(h, m, w, l);
}

static IDirect3DDevice9 *dev;
static IDirect3DPixelShader9 *psRange, *psAtlas;
static IDirect3DTexture9 *atlas;

static void draw_cases(void) {
  /* 1: COLOR0.r = 0 on the long edge, 1 at the apex */
  const struct VC c[3] = { { 20.0f, 99.95f, 0.5f, 1.0f, 0xff000000u }, { 620.0f, 99.95f, 0.5f, 1.0f, 0xff000000u },
                           { 320.0f, 96.95f, 0.5f, 1.0f, 0xffff0000u } };
  IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
  IDirect3DDevice9_SetPixelShader(dev, psRange);
  IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 1, c, sizeof(c[0]));
  /* 2: u 0.25..0.75 along x, v 0.75 on the long edge, 0.25 at the apex */
  const struct VT t[3] = { { 20.0f, 299.95f, 0.5f, 1.0f, 0.25f, 0.75f }, { 620.0f, 299.95f, 0.5f, 1.0f, 0.75f, 0.75f },
                           { 320.0f, 296.95f, 0.5f, 1.0f, 0.5f, 0.25f } };
  IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_TEX1);
  IDirect3DDevice9_SetPixelShader(dev, psAtlas);
  IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)atlas);
  IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 1, t, sizeof(t[0]));
}

static int msaa_check(void) {
  IDirect3DSurface9 *bb, *ms, *resolved, *sys;
  D3DLOCKED_RECT lr;
  if (FAILED(IDirect3DDevice9_GetRenderTarget(dev, 0, &bb))
   || FAILED(IDirect3DDevice9_CreateRenderTarget(dev, W, H, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_4_SAMPLES, 0, FALSE, &ms, NULL))
   || FAILED(IDirect3DDevice9_CreateRenderTarget(dev, W, H, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &resolved, NULL))
   || FAILED(IDirect3DDevice9_CreateOffscreenPlainSurface(dev, W, H, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, NULL))) {
    printf("code_centroid: FAIL (4x target)\n");
    return 0;
  }
  IDirect3DDevice9_SetRenderTarget(dev, 0, ms);
  IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
  IDirect3DDevice9_BeginScene(dev);
  draw_cases();
  IDirect3DDevice9_EndScene(dev);
  IDirect3DDevice9_SetRenderTarget(dev, 0, bb);
  if (FAILED(IDirect3DDevice9_StretchRect(dev, ms, NULL, resolved, NULL, D3DTEXF_NONE))
   || FAILED(IDirect3DDevice9_GetRenderTargetData(dev, resolved, sys))
   || FAILED(IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY))) {
    printf("code_centroid: FAIL (resolve or readback)\n");
    return 0;
  }
  int color = 0, border = 0;
  for (int y = 0; y < H; y++) {
    const DWORD *row = (const DWORD *)((const char *)lr.pBits + y * lr.Pitch);
    for (int x = 0; x < W; x++) {
      if (y < 200) color += ((row[x] >> 16) & 255) > 0;   /* case 1: R > 0 */
      else border += (row[x] & 255) > 0;                   /* case 2: B > 0 */
    }
  }
  IDirect3DSurface9_UnlockRect(sys);
  printf("code_centroid color: %d pixels with an extrapolated sample\n", color);
  printf("code_centroid atlas: %d pixels with a border sample\n", border);
  printf("code_centroid: %s\n", color == 0 && border == 0 ? "PASS" : "FAIL");
  IDirect3DSurface9_Release(sys);
  IDirect3DSurface9_Release(resolved);
  IDirect3DSurface9_Release(ms);
  IDirect3DSurface9_Release(bb);
  return 1;
}

int main(void) {
  WNDCLASSA wc = {0};
  wc.lpfnWndProc = wnd_proc;
  wc.hInstance = GetModuleHandleA(NULL);
  wc.lpszClassName = "d9mt_centroid";
  RegisterClassA(&wc);
  RECT rc = {0, 0, W, H};
  AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
  HWND hwnd = CreateWindowA("d9mt_centroid", "d9mt centroid", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
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
  if (FAILED(IDirect3DDevice9_CreatePixelShader(dev, k_ps_range, &psRange))
   || FAILED(IDirect3DDevice9_CreatePixelShader(dev, k_ps_atlas, &psAtlas))
   || FAILED(IDirect3DDevice9_CreateTexture(dev, 4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &atlas, NULL))) {
    printf("code_centroid: FAIL (resources)\n");
    return 1;
  }
  D3DLOCKED_RECT lr;
  IDirect3DTexture9_LockRect(atlas, 0, &lr, NULL, 0);
  for (int y = 0; y < 4; y++)
    for (int x = 0; x < 4; x++)
      ((DWORD *)((char *)lr.pBits + y * lr.Pitch))[x] = (x == 1 || x == 2) && (y == 1 || y == 2) ? 0xff00ff00u : 0xffffffffu;
  IDirect3DTexture9_UnlockRect(atlas, 0);
  IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
  IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
  IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
  IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
  IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, FALSE);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, FALSE);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHATESTENABLE, FALSE);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_SRGBWRITEENABLE, FALSE);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_MULTISAMPLEANTIALIAS, TRUE);

  msaa_check();
  fflush(stdout);

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
    draw_cases();
    IDirect3DDevice9_EndScene(dev);
    IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
    if (++frames == 3) printf("3 frames\n");
  }
done:
  IDirect3DDevice9_Release(dev);
  IDirect3D9_Release(d3d);
  return 0;
}
