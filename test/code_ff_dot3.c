/* code_ff_dot3: D3DTOP_DOTPRODUCT3 as an alpha op keeps the earlier stages' colour on a depth-only
 * target (the 0.1.8 recompile bundle, "the fixed-function batch": DXVK 8d252af1). Without a colour
 * render target d9mt replaces every stage's colour op with SELECTARG1 TFACTOR (nothing reads it),
 * but a DOTPRODUCT3 alpha op consumes the colour of the stage before it.
 *
 * Pass 1, depth only (render target 0 of format NULL, D24S8 cleared to 1), alpha test GREATER 0x80,
 * Z write on, pre-transformed quads at z 0.5, white diffuse, TFACTOR black:
 *   stage 0: colour SELECTARG1 DIFFUSE (white), alpha SELECTARG1 DIFFUSE;
 *   stage 1: colour SELECTARG1 CURRENT, alpha DOTPRODUCT3 (CURRENT, DIFFUSE):
 *            white . white saturates to 1, so the alpha test passes and depth is written. If
 *            stage 0's colour became TFACTOR (black), black . white is 0: no depth.
 *   left: that quad; right: the control, the same quad with ALPHAFUNC NEVER (never any depth).
 * Pass 2, the backbuffer with the same depth buffer, ZFUNC EQUAL, Z write off, no alpha test:
 * the same two quads in green. The oracle, read back on frame 1, each quad's centre: left green
 * (pass 1 wrote depth), right the clear colour (proves pass 2 needs pass 1's depth).
 * Prints "code_ff_dot3 dot3: <got> ok|FAIL", the same for "control", and "code_ff_dot3: PASS|FAIL".
 * On base the left quad fails. */
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <string.h>

struct V { float x, y, z, rhw; DWORD c; };
#define FVF (D3DFVF_XYZRHW | D3DFVF_DIFFUSE)
#define W 640
#define H 480
#define GREEN 0xff00ff00u
#define CLEAR 0xff101018u

static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
  return DefWindowProcA(h, m, w, l);
}

static void quad(IDirect3DDevice9 *dev, float x0, DWORD c) {
  struct V v[4] = {
    { x0, 160, 0.5f, 1.0f, c }, { x0 + 160, 160, 0.5f, 1.0f, c },
    { x0, 320, 0.5f, 1.0f, c }, { x0 + 160, 320, 0.5f, 1.0f, c },
  };
  IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, v, sizeof(v[0]));
}

static void stage(IDirect3DDevice9 *dev, DWORD s, DWORD cop, DWORD carg, DWORD aop, DWORD aarg1, DWORD aarg2) {
  IDirect3DDevice9_SetTextureStageState(dev, s, D3DTSS_COLOROP, cop);
  IDirect3DDevice9_SetTextureStageState(dev, s, D3DTSS_COLORARG1, carg);
  IDirect3DDevice9_SetTextureStageState(dev, s, D3DTSS_ALPHAOP, aop);
  IDirect3DDevice9_SetTextureStageState(dev, s, D3DTSS_ALPHAARG1, aarg1);
  IDirect3DDevice9_SetTextureStageState(dev, s, D3DTSS_ALPHAARG2, aarg2);
}

int main(void) {
  WNDCLASSA wc = {0};
  wc.lpfnWndProc = wnd_proc;
  wc.hInstance = GetModuleHandleA(NULL);
  wc.lpszClassName = "d9mt_ff_dot3";
  RegisterClassA(&wc);
  RECT rc = {0, 0, W, H};
  AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
  HWND hwnd = CreateWindowA("d9mt_ff_dot3", "d9mt DOTPRODUCT3 alpha op, depth only", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                            CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, NULL, NULL, wc.hInstance, NULL);
  if (!hwnd) return 1;
  IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
  if (!d3d) return 1;
  D3DPRESENT_PARAMETERS pp = {0};
  pp.BackBufferWidth = W; pp.BackBufferHeight = H;
  pp.BackBufferFormat = D3DFMT_X8R8G8B8; pp.BackBufferCount = 1;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = hwnd;
  pp.Windowed = TRUE; pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
  pp.EnableAutoDepthStencil = TRUE; pp.AutoDepthStencilFormat = D3DFMT_D24S8;
  IDirect3DDevice9 *dev;
  if (FAILED(IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                     D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev)))
    return 1;
  IDirect3DSurface9 *bb, *nullRt, *sys;
  if (FAILED(IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))
   || FAILED(IDirect3DDevice9_CreateRenderTarget(dev, W, H, (D3DFORMAT)MAKEFOURCC('N', 'U', 'L', 'L'),
                                                 D3DMULTISAMPLE_NONE, 0, FALSE, &nullRt, NULL))
   || FAILED(IDirect3DDevice9_CreateOffscreenPlainSurface(dev, W, H, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, NULL))) {
    printf("code_ff_dot3: FAIL (setup: NULL render target)\n");
    return 1;
  }
  IDirect3DDevice9_SetFVF(dev, FVF);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_TEXTUREFACTOR, 0xff000000u);

  int frames = 0;
  MSG msg;
  for (;;) {
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) goto done;
      TranslateMessage(&msg);
      DispatchMessageA(&msg);
    }
    IDirect3DDevice9_BeginScene(dev);
    /* pass 1: depth only */
    IDirect3DDevice9_SetRenderTarget(dev, 0, nullRt);
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_ZBUFFER, 0, 1.0f, 0);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, D3DZB_TRUE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZFUNC, D3DCMP_LESS);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZWRITEENABLE, TRUE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHATESTENABLE, TRUE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHAREF, 0x80u);
    stage(dev, 0, D3DTOP_SELECTARG1, D3DTA_DIFFUSE, D3DTOP_SELECTARG1, D3DTA_DIFFUSE, D3DTA_CURRENT);
    stage(dev, 1, D3DTOP_SELECTARG1, D3DTA_CURRENT, D3DTOP_DOTPRODUCT3, D3DTA_CURRENT, D3DTA_DIFFUSE);
    stage(dev, 2, D3DTOP_DISABLE, D3DTA_CURRENT, D3DTOP_DISABLE, D3DTA_CURRENT, D3DTA_CURRENT);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHAFUNC, D3DCMP_GREATER);
    quad(dev, 80, 0xffffffffu);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHAFUNC, D3DCMP_NEVER);
    quad(dev, 400, 0xffffffffu);
    /* pass 2: colour where pass 1 wrote depth */
    IDirect3DDevice9_SetRenderTarget(dev, 0, bb);
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, CLEAR, 1.0f, 0);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHATESTENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZFUNC, D3DCMP_EQUAL);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZWRITEENABLE, FALSE);
    stage(dev, 0, D3DTOP_SELECTARG1, D3DTA_DIFFUSE, D3DTOP_SELECTARG1, D3DTA_DIFFUSE, D3DTA_CURRENT);
    stage(dev, 1, D3DTOP_DISABLE, D3DTA_CURRENT, D3DTOP_DISABLE, D3DTA_CURRENT, D3DTA_CURRENT);
    quad(dev, 80, GREEN);
    quad(dev, 400, GREEN);
    IDirect3DDevice9_EndScene(dev);
    if (frames == 0) {
      D3DLOCKED_RECT lr;
      if (FAILED(IDirect3DDevice9_GetRenderTargetData(dev, bb, sys))
       || FAILED(IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY))) {
        printf("code_ff_dot3: FAIL (readback)\n");
      } else {
        const DWORD l = ((const DWORD *)((const char *)lr.pBits + 240 * lr.Pitch))[160] | 0xff000000u;
        const DWORD r = ((const DWORD *)((const char *)lr.pBits + 240 * lr.Pitch))[480] | 0xff000000u;
        IDirect3DSurface9_UnlockRect(sys);
        printf("code_ff_dot3 dot3: %08lx %s\n", (unsigned long)l, l == GREEN ? "ok" : "FAIL");
        printf("code_ff_dot3 control: %08lx %s\n", (unsigned long)r, r == CLEAR ? "ok" : "FAIL");
        printf("code_ff_dot3: %s\n", l == GREEN && r == CLEAR ? "PASS" : "FAIL");
      }
      fflush(stdout);
    }
    IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
    if (++frames == 3) printf("3 frames\n");
  }
done:
  IDirect3DDevice9_Release(dev);
  IDirect3D9_Release(d3d);
  return 0;
}
