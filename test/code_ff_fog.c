/* code_ff_fog: linear pixel fog with FOGSTART == FOGEND (the 0.1.8 recompile bundle, "the
 * fixed-function batch": DXVK ef81c30f + 836b6418). D3D9 gives no fog in front of the fog end and
 * full fog behind it; d9mt used to divide by zero into a scale of 0, so every pixel was fully fogged.
 *
 * One A8R8G8B8 render target, cleared to black with alpha 0; fog colour blue; LINEAR table fog,
 * start = end = 0.5; diffuse red with alpha 0x80. Three rows of three quads, each quad at depth
 * z = 0.25, 0.5 and 0.75 (rhw = 1/z, so z fog and w fog see the same depth):
 *   row 1: the fixed-function pixel stage (stage 0 selects the diffuse colour and alpha);
 *   row 2: a ps_2_0 shader (mov oC0, v0): SM1/2 pixel shaders get emulated fog;
 *   row 3: a ps_3_0 shader (the same): SM3 pixel shaders do their own fog, so none is added.
 * The oracle, read back from the render target on frame 1, the centre pixel of each quad:
 *   rows 1 and 2, z 0.25: red (no fog); z 0.5 and 0.75: blue (full fog). At z = end the ported
 *     formula gives (end - depth) * FLT_MAX = 0 * FLT_MAX = 0, a fog factor of 0 (a quad of
 *     constant z = 0.5 and rhw = 2 has depth exactly 0.5 in both fog modes);
 *   row 3: red at every depth;
 *   alpha 0x80 everywhere (fog never touches alpha).
 * Prints "code_ff_fog <row> z=<z>: <got> <ok|FAIL>" per probe and "code_ff_fog: PASS|FAIL" once.
 * On base, rows 1 and 2 at z 0.25 fail (fully fogged). The frame shown is the render target
 * stretched to the backbuffer. */
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <string.h>

/* ps_2_0 | dcl v0 | mov oC0, v0 */
static const DWORD k_ps20_color[] = {
  0xffff0200, 0x0200001f, 0x80000000, 0x900f0000, 0x02000001, 0x800f0800,
  0x90e40000, 0x0000ffff,
};
/* ps_3_0 | dcl_color0 v0 | mov oC0, v0 */
static const DWORD k_ps30_color[] = {
  0xffff0300, 0x0200001f, 0x8000000a, 0x900f0000, 0x02000001, 0x800f0800,
  0x90e40000, 0x0000ffff,
};

struct V { float x, y, z, rhw; DWORD c; };
#define FVF (D3DFVF_XYZRHW | D3DFVF_DIFFUSE)
#define W 640
#define H 480
#define RED 0x80ff0000u

static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
  return DefWindowProcA(h, m, w, l);
}

static DWORD fbits(float f) { DWORD d; memcpy(&d, &f, 4); return d; }

static const float k_z[3] = { 0.25f, 0.5f, 0.75f };

static void quad(IDirect3DDevice9 *dev, int row, int col) {
  const float x0 = 40.0f + 200.0f * col, y0 = 40.0f + 150.0f * row, z = k_z[col], rhw = 1.0f / z;
  struct V v[4] = {
    { x0, y0, z, rhw, RED }, { x0 + 160, y0, z, rhw, RED },
    { x0, y0 + 110, z, rhw, RED }, { x0 + 160, y0 + 110, z, rhw, RED },
  };
  IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, v, sizeof(v[0]));
}

int main(void) {
  WNDCLASSA wc = {0};
  wc.lpfnWndProc = wnd_proc;
  wc.hInstance = GetModuleHandleA(NULL);
  wc.lpszClassName = "d9mt_ff_fog";
  RegisterClassA(&wc);
  RECT rc = {0, 0, W, H};
  AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
  HWND hwnd = CreateWindowA("d9mt_ff_fog", "d9mt fog start == end", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                            CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, NULL, NULL, wc.hInstance, NULL);
  if (!hwnd) return 1;
  IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
  if (!d3d) return 1;
  D3DPRESENT_PARAMETERS pp = {0};
  pp.BackBufferWidth = W; pp.BackBufferHeight = H;
  pp.BackBufferFormat = D3DFMT_X8R8G8B8; pp.BackBufferCount = 1;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = hwnd;
  pp.Windowed = TRUE; pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
  IDirect3DDevice9 *dev;
  if (FAILED(IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                     D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev)))
    return 1;
  IDirect3DPixelShader9 *ps20, *ps30;
  IDirect3DSurface9 *rt, *bb, *sys;
  if (FAILED(IDirect3DDevice9_CreatePixelShader(dev, k_ps20_color, &ps20))
   || FAILED(IDirect3DDevice9_CreatePixelShader(dev, k_ps30_color, &ps30))
   || FAILED(IDirect3DDevice9_CreateRenderTarget(dev, W, H, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, NULL))
   || FAILED(IDirect3DDevice9_CreateOffscreenPlainSurface(dev, W, H, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, NULL))
   || FAILED(IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) {
    printf("code_ff_fog: FAIL (setup)\n");
    return 1;
  }
  IDirect3DDevice9_SetFVF(dev, FVF);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, FALSE);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_FOGENABLE, TRUE);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_FOGCOLOR, 0xff0000ffu);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_FOGVERTEXMODE, D3DFOG_NONE);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_FOGTABLEMODE, D3DFOG_LINEAR);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_FOGSTART, fbits(0.5f));
  IDirect3DDevice9_SetRenderState(dev, D3DRS_FOGEND, fbits(0.5f));
  IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
  IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
  IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
  IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
  IDirect3DDevice9_SetTextureStageState(dev, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);

  int frames = 0;
  MSG msg;
  for (;;) {
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) goto done;
      TranslateMessage(&msg);
      DispatchMessageA(&msg);
    }
    IDirect3DDevice9_SetRenderTarget(dev, 0, rt);
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0x00000000u, 1.0f, 0);
    IDirect3DDevice9_BeginScene(dev);
    for (int row = 0; row < 3; row++) {
      IDirect3DDevice9_SetPixelShader(dev, row == 0 ? NULL : row == 1 ? ps20 : ps30);
      for (int col = 0; col < 3; col++) quad(dev, row, col);
    }
    IDirect3DDevice9_EndScene(dev);
    if (frames == 0) {
      D3DLOCKED_RECT lr;
      int all = 1;
      if (FAILED(IDirect3DDevice9_GetRenderTargetData(dev, rt, sys))
       || FAILED(IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY))) {
        printf("code_ff_fog: FAIL (readback)\n");
      } else {
        static const char *rows[3] = { "ff", "ps_2_0", "ps_3_0" };
        for (int row = 0; row < 3; row++)
          for (int col = 0; col < 3; col++) {
            const int x = 40 + 200 * col + 80, y = 40 + 150 * row + 55;
            const DWORD got = ((const DWORD *)((const char *)lr.pBits + y * lr.Pitch))[x];
            const DWORD blue = 0x800000ffu;
            const int ok = (row == 2 || col == 0) ? got == RED : got == blue;
            printf("code_ff_fog %s z=%.2f: %08lx %s\n", rows[row], k_z[col], (unsigned long)got, ok ? "ok" : "FAIL");
            all &= ok;
          }
        IDirect3DSurface9_UnlockRect(sys);
        printf("code_ff_fog: %s\n", all ? "PASS" : "FAIL");
      }
      fflush(stdout);
    }
    IDirect3DDevice9_SetRenderTarget(dev, 0, bb);
    IDirect3DDevice9_StretchRect(dev, rt, NULL, bb, NULL, D3DTEXF_NONE);
    IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
    if (++frames == 3) printf("3 frames\n");
  }
done:
  IDirect3DDevice9_Release(dev);
  IDirect3D9_Release(d3d);
  return 0;
}
