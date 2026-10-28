/* AMD's RESZ hack (D3DRS_POINTSIZE = RESZ_CODE) into an INTZ texture, then sampled. Every frame
 * renders two depths (left half 0.25, right half 0.75) into a 256x256 D24S8 depth surface, follows
 * AMD's sequence (the INTZ texture on sampler 0, a dummy draw with colour writes off, then the
 * RESZ code; d9mt copies the depth at the render state) and samples the texture on the backbuffer
 * without setting it again. The texture is created without D3DUSAGE_DEPTHSTENCIL, so it is a
 * sampled-only DEFAULT texture that only RESZ ever writes: with #101's rule (a never-written
 * DEFAULT texture binds as null) a RESZ that doesn't mark it written samples zero, and the dummy
 * draw has already bound it as null, so the write must also rebind it. Single-sample source: the
 * copy path, not the MSAA resolve.
 * RESZ needs an AMD vendor id: the program appends d3d9.customVendorId=1002 to DXVK_CONFIG before
 * the device exists, and checks that the adapter then reports the RESZ format.
 * Each frame is read back before Present at three points: the background and the middle of each
 * half, which must be the resolved depth as grey (64, 191). A failed point is a FAIL line (the
 * first 8 are printed); under the hash harness (d9h.h) the FAIL lines count, and run alone it
 * exits with 1 if any frame failed. */
#include "t_common.h"

#define FOURCC(a, b, c, d) ((D3DFORMAT)MAKEFOURCC(a, b, c, d))
#define RESZ_CODE 0x7fa05000u
#define BG 0x00181828u

static IDirect3DTexture9 *dummy, *intz;
static IDirect3DSurface9 *dummySurf, *ds, *backBuffer, *autoDs, *sys;
static int frames, frames_failed, printed;

static int close_to(DWORD got, DWORD want) {
  for (int s = 0; s < 24; s += 8) {
    const int d = (int)((got >> s) & 0xff) - (int)((want >> s) & 0xff);
    if (d < -3 || d > 3)
      return 0;
  }
  return 1;
}

static void check(int n) {
  static const struct { const char *what; int x, y; DWORD want; } points[] = {
      {"background", 40, 40, BG},
      {"left (depth 0.25)", 240, 240, 0x404040u},
      {"right (depth 0.75)", 400, 240, 0xbfbfbfu},
  };
  D3DLOCKED_RECT lr;
  int failed = 0;
  T_CHECK(IDirect3DDevice9_GetRenderTargetData(t_dev, backBuffer, sys));
  T_CHECK(IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY));
  for (size_t i = 0; i < sizeof(points) / sizeof(points[0]); i++) {
    const DWORD got = *(const DWORD *)((const BYTE *)lr.pBits + (size_t)points[i].y * lr.Pitch
                                       + points[i].x * 4) & 0xffffff;
    if (close_to(got, points[i].want))
      continue;
    failed = 1;
    if (printed++ < 8)
      printf("FAIL frame %d %s: got %06lx want %06lx\n", n, points[i].what, (unsigned long)got,
             (unsigned long)points[i].want);
  }
  IDirect3DSurface9_UnlockRect(sys);
  frames++;
  frames_failed += failed;
  fflush(stdout);
}

static void frame(int n) {
  IDirect3DDevice9_SetTexture(t_dev, 0, NULL);
  IDirect3DDevice9_BeginScene(t_dev);
  IDirect3DDevice9_SetVertexShader(t_dev, t_vs_pos);
  t_tex_zw(0.5f, 1.0f);

  IDirect3DDevice9_SetRenderTarget(t_dev, 0, dummySurf);
  IDirect3DDevice9_SetDepthStencilSurface(t_dev, ds);
  IDirect3DDevice9_SetRenderState(t_dev, D3DRS_ZENABLE, D3DZB_TRUE);
  IDirect3DDevice9_SetRenderState(t_dev, D3DRS_ZWRITEENABLE, TRUE);
  IDirect3DDevice9_SetRenderState(t_dev, D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
  IDirect3DDevice9_Clear(t_dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0, 1.0f, 0);
  IDirect3DDevice9_SetPixelShader(t_dev, t_ps_color);
  t_color(1.0f, 1.0f, 1.0f, 1.0f);
  t_quad(-1.0f, -1.0f, 0.0f, 1.0f, 0.25f);
  t_quad(0.0f, -1.0f, 1.0f, 1.0f, 0.75f);

  /* AMD's sequence: the destination on sampler 0, a dummy draw, the RESZ code */
  IDirect3DDevice9_SetTexture(t_dev, 0, (IDirect3DBaseTexture9 *)intz);
  IDirect3DDevice9_SetRenderState(t_dev, D3DRS_ZENABLE, D3DZB_FALSE);
  IDirect3DDevice9_SetRenderState(t_dev, D3DRS_ZWRITEENABLE, FALSE);
  IDirect3DDevice9_SetRenderState(t_dev, D3DRS_COLORWRITEENABLE, 0);
  IDirect3DDevice9_SetPixelShader(t_dev, t_ps_sample);
  t_quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.5f);
  IDirect3DDevice9_SetRenderState(t_dev, D3DRS_POINTSIZE, RESZ_CODE);
  IDirect3DDevice9_SetRenderState(t_dev, D3DRS_POINTSIZE, 0x3f800000u);
  IDirect3DDevice9_SetRenderState(t_dev, D3DRS_COLORWRITEENABLE, 0xf);

  IDirect3DDevice9_SetRenderTarget(t_dev, 0, backBuffer);
  IDirect3DDevice9_SetDepthStencilSurface(t_dev, autoDs);
  IDirect3DDevice9_Clear(t_dev, 0, NULL, D3DCLEAR_TARGET, BG, 1.0f, 0);
  t_quad(-0.5f, -0.5f, 0.5f, 0.5f, 0.5f);
  IDirect3DDevice9_EndScene(t_dev);
  check(n);
}

int main(void) {
  char conf[1024];
  DWORD len = GetEnvironmentVariableA("DXVK_CONFIG", conf, sizeof(conf));
  if (len >= sizeof(conf) - 32) { printf("FAIL DXVK_CONFIG too long\n"); ExitProcess(2); }
  strcpy(conf + len, len ? ";d3d9.customVendorId=1002" : "d3d9.customVendorId=1002");
  SetEnvironmentVariableA("DXVK_CONFIG", conf);

  t_init("d9mt_resz", 640, 480);
  IDirect3D9 *d3d = NULL;
  IDirect3DDevice9_GetDirect3D(t_dev, &d3d);
  D3DADAPTER_IDENTIFIER9 id;
  T_CHECK(IDirect3D9_GetAdapterIdentifier(d3d, D3DADAPTER_DEFAULT, 0, &id));
  printf("vendor %04lx\n", (unsigned long)id.VendorId);
  T_CHECK(IDirect3D9_CheckDeviceFormat(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8,
                                       D3DUSAGE_RENDERTARGET, D3DRTYPE_SURFACE, FOURCC('R', 'E', 'S', 'Z')));
  T_CHECK(IDirect3DDevice9_CreateTexture(t_dev, 256, 256, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                         D3DPOOL_DEFAULT, &dummy, NULL));
  T_CHECK(IDirect3DDevice9_CreateTexture(t_dev, 256, 256, 1, 0, FOURCC('I', 'N', 'T', 'Z'),
                                         D3DPOOL_DEFAULT, &intz, NULL));
  T_CHECK(IDirect3DDevice9_CreateDepthStencilSurface(t_dev, 256, 256, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0,
                                                     FALSE, &ds, NULL));
  T_CHECK(IDirect3DDevice9_CreateOffscreenPlainSurface(t_dev, 640, 480, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM,
                                                       &sys, NULL));
  dummySurf = t_level0(dummy);
  T_CHECK(IDirect3DDevice9_GetRenderTarget(t_dev, 0, &backBuffer));
  T_CHECK(IDirect3DDevice9_GetDepthStencilSurface(t_dev, &autoDs));
  t_loop(frame);
  printf("RESZ frames=%d failed=%d\n", frames, frames_failed);
  fflush(stdout);
  ExitProcess(frames_failed ? 1 : 0);
}
