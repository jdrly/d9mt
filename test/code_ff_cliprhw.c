/* code_ff_cliprhw: user clip planes don't apply to pre-transformed vertices (the 0.1.8 recompile
 * bundle, "the fixed-function batch": DXVK 3a4c6fa3). One user clip plane, (0, 0, 0, -1), rejects
 * every point (the distance is -1 everywhere), enabled with D3DRS_CLIPPLANEENABLE = 1:
 *   left:  a pre-transformed (XYZRHW) green quad: D3D9 doesn't clip it, so it must be visible;
 *   right: the control, an untransformed (XYZ, identity transforms) green quad: clipped away,
 *          which proves the plane is active.
 * The oracle, read back from the backbuffer on frame 1, each quad's centre pixel: left green,
 * right the clear colour. Prints "code_ff_cliprhw rhw: <got> ok|FAIL", the same for "xyz", and
 * "code_ff_cliprhw: PASS|FAIL". On base the left quad is clipped too. */
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <string.h>

struct VT { float x, y, z, rhw; DWORD c; };
struct VX { float x, y, z; DWORD c; };
#define W 640
#define H 480
#define GREEN 0xff00ff00u
#define CLEAR 0xff101018u

static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
  return DefWindowProcA(h, m, w, l);
}

int main(void) {
  WNDCLASSA wc = {0};
  wc.lpfnWndProc = wnd_proc;
  wc.hInstance = GetModuleHandleA(NULL);
  wc.lpszClassName = "d9mt_ff_cliprhw";
  RegisterClassA(&wc);
  RECT rc = {0, 0, W, H};
  AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
  HWND hwnd = CreateWindowA("d9mt_ff_cliprhw", "d9mt clip planes and RHW", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
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
  IDirect3DSurface9 *bb, *sys;
  if (FAILED(IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))
   || FAILED(IDirect3DDevice9_CreateOffscreenPlainSurface(dev, W, H, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, NULL))) {
    printf("code_ff_cliprhw: FAIL (setup)\n");
    return 1;
  }
  static const float plane[4] = { 0.0f, 0.0f, 0.0f, -1.0f };
  IDirect3DDevice9_SetClipPlane(dev, 0, plane);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_CLIPPLANEENABLE, 1u);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, FALSE);
  IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
  IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
  IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
  IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
  IDirect3DDevice9_SetTextureStageState(dev, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);

  /* left half: screen pixels 80..240 x 160..320 */
  static const struct VT rhw[4] = {
    { 80, 160, 0.5f, 1.0f, GREEN }, { 240, 160, 0.5f, 1.0f, GREEN },
    { 80, 320, 0.5f, 1.0f, GREEN }, { 240, 320, 0.5f, 1.0f, GREEN },
  };
  /* right half in clip space (identity world, view, projection): x 0.25..0.75, y -0.33..0.33 */
  static const struct VX xyz[4] = {
    { 0.25f, 0.33f, 0.5f, GREEN }, { 0.75f, 0.33f, 0.5f, GREEN },
    { 0.25f, -0.33f, 0.5f, GREEN }, { 0.75f, -0.33f, 0.5f, GREEN },
  };

  int frames = 0;
  MSG msg;
  for (;;) {
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) goto done;
      TranslateMessage(&msg);
      DispatchMessageA(&msg);
    }
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, CLEAR, 1.0f, 0);
    IDirect3DDevice9_BeginScene(dev);
    IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, rhw, sizeof(rhw[0]));
    IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZ | D3DFVF_DIFFUSE);
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, xyz, sizeof(xyz[0]));
    IDirect3DDevice9_EndScene(dev);
    if (frames == 0) {
      D3DLOCKED_RECT lr;
      if (FAILED(IDirect3DDevice9_GetRenderTargetData(dev, bb, sys))
       || FAILED(IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY))) {
        printf("code_ff_cliprhw: FAIL (readback)\n");
      } else {
        const DWORD l = ((const DWORD *)((const char *)lr.pBits + 240 * lr.Pitch))[160] | 0xff000000u;
        const DWORD r = ((const DWORD *)((const char *)lr.pBits + 240 * lr.Pitch))[480] | 0xff000000u;
        IDirect3DSurface9_UnlockRect(sys);
        printf("code_ff_cliprhw rhw: %08lx %s\n", (unsigned long)l, l == GREEN ? "ok" : "FAIL");
        printf("code_ff_cliprhw xyz: %08lx %s\n", (unsigned long)r, r == CLEAR ? "ok" : "FAIL");
        printf("code_ff_cliprhw: %s\n", l == GREEN && r == CLEAR ? "PASS" : "FAIL");
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
