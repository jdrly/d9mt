/* code_ff_cube3d: cube and volume textures in fixed-function texture stages (#152, the 0.1.8
 * recompile bundle, "the fixed-function batch"). The FF fragment shader reads stage s's texture
 * from t2d[s], tcube[s] or t3d[s] by its type; d9mt bound every stage's texture at slot s only, so
 * a cube texture sampled the null cube (slots 8-15 never bound) and a volume texture went through
 * a 2D declaration (the null 2D texture since #151).
 *
 * No shaders: pre-transformed quads with one 3-component texture coordinate (0.5, 0.5, 0.5),
 * point sampling, white diffuse. Every texture is one colour:
 *   1  stage 0, a 2D texture (red), SELECTARG1 TEXTURE: red (the control: unchanged by #152);
 *   2  stage 0, a cube texture (blue on every face): blue;
 *   3  stage 0, a volume texture (green): green;
 *   4  stage 0 SELECTARG1 DIFFUSE, stage 1 the cube texture, MODULATE (TEXTURE, CURRENT): blue
 *      (stage 1's cube slot is 9: the remap is per stage);
 *   5  stage 0, the 2D texture again, after the cube and volume draws: red.
 * The oracle, read back from the backbuffer on frame 1, each quad's centre pixel exactly. Prints
 * "code_ff_cube3d <n>: <got> ok|FAIL" per quad and "code_ff_cube3d: PASS|FAIL". On base, quads 2,
 * 3 and 4 are black. Run it under MTL_SHADER_VALIDATION=1 as well as plainly: no invalid binding. */
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <string.h>

struct V { float x, y, z, rhw; DWORD c; float u, v, w; };
#define FVF (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1 | D3DFVF_TEXCOORDSIZE3(0))
#define W 640
#define H 480
#define RED   0xffff0000u
#define GREEN 0xff00ff00u
#define BLUE  0xff0000ffu
#define CLEAR 0xff101018u

static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
  return DefWindowProcA(h, m, w, l);
}

static IDirect3DDevice9 *dev;

static void quad(int n) {
  const float x0 = 20.0f + 124.0f * n;
  struct V v[4] = {
    { x0, 180, 0.5f, 1.0f, 0xffffffffu, 0.5f, 0.5f, 0.5f }, { x0 + 100, 180, 0.5f, 1.0f, 0xffffffffu, 0.5f, 0.5f, 0.5f },
    { x0, 300, 0.5f, 1.0f, 0xffffffffu, 0.5f, 0.5f, 0.5f }, { x0 + 100, 300, 0.5f, 1.0f, 0xffffffffu, 0.5f, 0.5f, 0.5f },
  };
  IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, v, sizeof(v[0]));
}

static void stage(DWORD s, DWORD op, DWORD arg1, DWORD arg2) {
  IDirect3DDevice9_SetTextureStageState(dev, s, D3DTSS_COLOROP, op);
  IDirect3DDevice9_SetTextureStageState(dev, s, D3DTSS_COLORARG1, arg1);
  IDirect3DDevice9_SetTextureStageState(dev, s, D3DTSS_COLORARG2, arg2);
  IDirect3DDevice9_SetTextureStageState(dev, s, D3DTSS_ALPHAOP, op == D3DTOP_DISABLE ? D3DTOP_DISABLE : D3DTOP_SELECTARG1);
  IDirect3DDevice9_SetTextureStageState(dev, s, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
  IDirect3DDevice9_SetTextureStageState(dev, s, D3DTSS_TEXCOORDINDEX, 0);
  IDirect3DDevice9_SetSamplerState(dev, s, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
  IDirect3DDevice9_SetSamplerState(dev, s, D3DSAMP_MINFILTER, D3DTEXF_POINT);
  IDirect3DDevice9_SetSamplerState(dev, s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
}

int main(void) {
  WNDCLASSA wc = {0};
  wc.lpfnWndProc = wnd_proc;
  wc.hInstance = GetModuleHandleA(NULL);
  wc.lpszClassName = "d9mt_ff_cube3d";
  RegisterClassA(&wc);
  RECT rc = {0, 0, W, H};
  AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
  HWND hwnd = CreateWindowA("d9mt_ff_cube3d", "d9mt FF cube and volume stages", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
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

  IDirect3DTexture9 *t2d;
  IDirect3DCubeTexture9 *tcube;
  IDirect3DVolumeTexture9 *t3d;
  IDirect3DSurface9 *bb, *sys;
  D3DLOCKED_RECT lr;
  D3DLOCKED_BOX lb;
  if (FAILED(IDirect3DDevice9_CreateTexture(dev, 4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t2d, NULL))
   || FAILED(IDirect3DDevice9_CreateCubeTexture(dev, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tcube, NULL))
   || FAILED(IDirect3DDevice9_CreateVolumeTexture(dev, 4, 4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t3d, NULL))
   || FAILED(IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))
   || FAILED(IDirect3DDevice9_CreateOffscreenPlainSurface(dev, W, H, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, NULL))) {
    printf("code_ff_cube3d: FAIL (setup)\n");
    return 1;
  }
  IDirect3DTexture9_LockRect(t2d, 0, &lr, NULL, 0);
  for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) ((DWORD *)((BYTE *)lr.pBits + y * lr.Pitch))[x] = RED;
  IDirect3DTexture9_UnlockRect(t2d, 0);
  for (int f = 0; f < 6; f++) {
    IDirect3DCubeTexture9_LockRect(tcube, (D3DCUBEMAP_FACES)f, 0, &lr, NULL, 0);
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) ((DWORD *)((BYTE *)lr.pBits + y * lr.Pitch))[x] = BLUE;
    IDirect3DCubeTexture9_UnlockRect(tcube, (D3DCUBEMAP_FACES)f, 0);
  }
  IDirect3DVolumeTexture9_LockBox(t3d, 0, &lb, NULL, 0);
  for (int z = 0; z < 4; z++) for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++)
    ((DWORD *)((BYTE *)lb.pBits + z * lb.SlicePitch + y * lb.RowPitch))[x] = GREEN;
  IDirect3DVolumeTexture9_UnlockBox(t3d, 0);

  IDirect3DDevice9_SetFVF(dev, FVF);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, FALSE);

  static const DWORD want[5] = { RED, BLUE, GREEN, BLUE, RED };
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
    stage(1, D3DTOP_DISABLE, D3DTA_TEXTURE, D3DTA_CURRENT);
    stage(0, D3DTOP_SELECTARG1, D3DTA_TEXTURE, D3DTA_CURRENT);
    IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)t2d);   quad(0);
    IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)tcube); quad(1);
    IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)t3d);   quad(2);
    IDirect3DDevice9_SetTexture(dev, 0, NULL);
    stage(0, D3DTOP_SELECTARG1, D3DTA_DIFFUSE, D3DTA_CURRENT);
    stage(1, D3DTOP_MODULATE, D3DTA_TEXTURE, D3DTA_CURRENT);
    stage(2, D3DTOP_DISABLE, D3DTA_TEXTURE, D3DTA_CURRENT);
    IDirect3DDevice9_SetTexture(dev, 1, (IDirect3DBaseTexture9 *)tcube); quad(3);
    IDirect3DDevice9_SetTexture(dev, 1, NULL);
    stage(1, D3DTOP_DISABLE, D3DTA_TEXTURE, D3DTA_CURRENT);
    stage(0, D3DTOP_SELECTARG1, D3DTA_TEXTURE, D3DTA_CURRENT);
    IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)t2d);   quad(4);
    IDirect3DDevice9_SetTexture(dev, 0, NULL);
    IDirect3DDevice9_EndScene(dev);
    if (frames == 0) {
      if (FAILED(IDirect3DDevice9_GetRenderTargetData(dev, bb, sys))
       || FAILED(IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY))) {
        printf("code_ff_cube3d: FAIL (readback)\n");
      } else {
        int all = 1;
        for (int n = 0; n < 5; n++) {
          const DWORD got = ((const DWORD *)((const char *)lr.pBits + 240 * lr.Pitch))[70 + 124 * n] | 0xff000000u;
          printf("code_ff_cube3d %d: %08lx %s\n", n + 1, (unsigned long)got, got == want[n] ? "ok" : "FAIL");
          all &= got == want[n];
        }
        IDirect3DSurface9_UnlockRect(sys);
        printf("code_ff_cube3d: %s\n", all ? "PASS" : "FAIL");
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
