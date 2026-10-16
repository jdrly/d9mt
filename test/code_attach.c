// Attachment load/store optimization paths (backend/mtl_attachment_opt.h), every frame:
//  1. "shadow" pass: an R32F target with color writes off plus a depth target; the R32F target
//     is cleared to 0.5 once (frame 0) and never written again, so its clear stays unstored;
//  2. a clear-only target (green), never drawn into;
//  3. a 4x MSAA target: cleared blue, a quad sampling the clear-only target, then resolved by
//     StretchRect into a texture (the resolve the rendering pass's store can do);
//  4. the backbuffer: three quads sampling the R32F target, the clear-only target and the
//     resolved texture; every 30 frames GetRenderTargetData reads the clear-only target back.
// Expected picture: left quad (128,255,255), middle green, right green on blue (MSAA edges).
// Run with D9MT_NO_ATTACHMENT_OPT=1 for the reference: the dumps must match.
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>

static const unsigned char texquad_vs_bytecode[] = {
#include "texquad_vs_bytecode.inc"
};
static const unsigned char texquad_ps_bytecode[] = {
#include "texquad_ps_bytecode.inc"
};

struct Vertex {
  float x, y, z;
  float u, v;
};

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_DESTROY) {
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcA(hwnd, msg, wp, lp);
}

#define CHECK_HR(x) do { if (FAILED(x)) { printf("FAILED %s\n", #x); return 1; } } while (0)

int main(void) {
  WNDCLASSA wc = {0};
  wc.lpfnWndProc = wnd_proc;
  wc.hInstance = GetModuleHandleA(NULL);
  wc.lpszClassName = "d9mt_attach";
  RegisterClassA(&wc);

  RECT rc = {0, 0, 800, 600};
  AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
  HWND hwnd = CreateWindowA("d9mt_attach", "d9mt attachment load/store", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                            CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
                            NULL, NULL, wc.hInstance, NULL);
  if (!hwnd)
    return 1;

  IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
  if (!d3d)
    return 1;

  D3DPRESENT_PARAMETERS pp = {0};
  pp.BackBufferWidth = 800;
  pp.BackBufferHeight = 600;
  pp.BackBufferFormat = D3DFMT_X8R8G8B8;
  pp.BackBufferCount = 1;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.hDeviceWindow = hwnd;
  pp.Windowed = TRUE;
  pp.EnableAutoDepthStencil = TRUE;
  pp.AutoDepthStencilFormat = D3DFMT_D24S8;
  pp.Flags = D3DPRESENTFLAG_DISCARD_DEPTHSTENCIL;
  pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;

  IDirect3DDevice9 *dev = NULL;
  CHECK_HR(IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                   D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev));

  IDirect3DVertexShader9 *vs = NULL;
  IDirect3DPixelShader9 *ps = NULL;
  CHECK_HR(IDirect3DDevice9_CreateVertexShader(dev, (const DWORD *)texquad_vs_bytecode, &vs));
  CHECK_HR(IDirect3DDevice9_CreatePixelShader(dev, (const DWORD *)texquad_ps_bytecode, &ps));

  static const D3DVERTEXELEMENT9 declElems[] = {
      {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
      {0, 12, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
      D3DDECL_END()};
  IDirect3DVertexDeclaration9 *decl = NULL;
  CHECK_HR(IDirect3DDevice9_CreateVertexDeclaration(dev, declElems, &decl));

  // quads: 0 full target, 1 left, 2 middle, 3 right
  static const float xs[4][2] = {{-0.8f, 0.8f}, {-0.95f, -0.35f}, {-0.3f, 0.3f}, {0.35f, 0.95f}};
  struct Vertex verts[24];
  for (int q = 0; q < 4; q++) {
    float x0 = xs[q][0], x1 = xs[q][1], y0 = q ? -0.4f : -0.8f, y1 = q ? 0.4f : 0.8f;
    struct Vertex v[6] = {{x0, y1, 0.5f, 0, 0}, {x1, y1, 0.5f, 1, 0}, {x1, y0, 0.5f, 1, 1},
                          {x0, y1, 0.5f, 0, 0}, {x1, y0, 0.5f, 1, 1}, {x0, y0, 0.5f, 0, 1}};
    memcpy(&verts[q * 6], v, sizeof(v));
  }
  IDirect3DVertexBuffer9 *vb = NULL;
  CHECK_HR(IDirect3DDevice9_CreateVertexBuffer(dev, sizeof(verts), 0, 0, D3DPOOL_DEFAULT, &vb, NULL));
  void *p = NULL;
  IDirect3DVertexBuffer9_Lock(vb, 0, sizeof(verts), &p, 0);
  memcpy(p, verts, sizeof(verts));
  IDirect3DVertexBuffer9_Unlock(vb);

  IDirect3DTexture9 *shadowTex = NULL, *clearTex = NULL, *resolveTex = NULL;
  CHECK_HR(IDirect3DDevice9_CreateTexture(dev, 256, 256, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F,
                                          D3DPOOL_DEFAULT, &shadowTex, NULL));
  CHECK_HR(IDirect3DDevice9_CreateTexture(dev, 800, 600, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                          D3DPOOL_DEFAULT, &clearTex, NULL));
  CHECK_HR(IDirect3DDevice9_CreateTexture(dev, 800, 600, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                          D3DPOOL_DEFAULT, &resolveTex, NULL));
  IDirect3DSurface9 *shadowSurf, *clearSurf, *resolveSurf, *shadowDepth, *msaa, *readback, *backBuffer, *autoDepth;
  IDirect3DTexture9_GetSurfaceLevel(shadowTex, 0, &shadowSurf);
  IDirect3DTexture9_GetSurfaceLevel(clearTex, 0, &clearSurf);
  IDirect3DTexture9_GetSurfaceLevel(resolveTex, 0, &resolveSurf);
  CHECK_HR(IDirect3DDevice9_CreateDepthStencilSurface(dev, 256, 256, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0,
                                                      FALSE, &shadowDepth, NULL));
  CHECK_HR(IDirect3DDevice9_CreateRenderTarget(dev, 800, 600, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_4_SAMPLES, 0,
                                               FALSE, &msaa, NULL));
  CHECK_HR(IDirect3DDevice9_CreateOffscreenPlainSurface(dev, 800, 600, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM,
                                                        &readback, NULL));
  IDirect3DDevice9_GetRenderTarget(dev, 0, &backBuffer);
  IDirect3DDevice9_GetDepthStencilSurface(dev, &autoDepth);

  static const float mvp[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  IDirect3DDevice9_SetVertexShaderConstantF(dev, 0, mvp, 4);
  IDirect3DDevice9_SetVertexDeclaration(dev, decl);
  IDirect3DDevice9_SetVertexShader(dev, vs);
  IDirect3DDevice9_SetPixelShader(dev, ps);
  IDirect3DDevice9_SetStreamSource(dev, 0, vb, 0, sizeof(struct Vertex));
  IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
  IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);

  int frames = 0;
  MSG msg;
  for (;;) {
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT)
        goto done;
      TranslateMessage(&msg);
      DispatchMessageA(&msg);
    }

    IDirect3DDevice9_BeginScene(dev);

    // 1. shadow pass: color writes off, depth only
    IDirect3DDevice9_SetTexture(dev, 0, NULL);
    IDirect3DDevice9_SetRenderTarget(dev, 0, shadowSurf);
    IDirect3DDevice9_SetDepthStencilSurface(dev, shadowDepth);
    if (frames == 0)
      IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, 128, 0, 0), 1.0f, 0);
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0, 1.0f, 0);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_COLORWRITEENABLE, 0);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, TRUE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZWRITEENABLE, TRUE);
    IDirect3DDevice9_DrawPrimitive(dev, D3DPT_TRIANGLELIST, 0, 2);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_COLORWRITEENABLE, 0xf);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, FALSE);

    // 2. clear-only target
    IDirect3DDevice9_SetDepthStencilSurface(dev, NULL);
    IDirect3DDevice9_SetRenderTarget(dev, 0, clearSurf);
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 200, 0), 1.0f, 0);

    // 3. MSAA target, resolved by StretchRect
    IDirect3DDevice9_SetRenderTarget(dev, 0, msaa);
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 200), 1.0f, 0);
    IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)clearTex);
    IDirect3DDevice9_DrawPrimitive(dev, D3DPT_TRIANGLELIST, 0, 2);
    IDirect3DDevice9_StretchRect(dev, msaa, NULL, resolveSurf, NULL, D3DTEXF_NONE);

    // 4. backbuffer
    IDirect3DDevice9_SetRenderTarget(dev, 0, backBuffer);
    IDirect3DDevice9_SetDepthStencilSurface(dev, autoDepth);
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(24, 24, 40), 1.0f, 0);
    IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)shadowTex);
    IDirect3DDevice9_DrawPrimitive(dev, D3DPT_TRIANGLELIST, 6, 2);
    IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)clearTex);
    IDirect3DDevice9_DrawPrimitive(dev, D3DPT_TRIANGLELIST, 12, 2);
    IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)resolveTex);
    IDirect3DDevice9_DrawPrimitive(dev, D3DPT_TRIANGLELIST, 18, 2);

    IDirect3DDevice9_EndScene(dev);

    if (frames % 30 == 5) {
      D3DLOCKED_RECT lr;
      if (SUCCEEDED(IDirect3DDevice9_GetRenderTargetData(dev, clearSurf, readback))
       && SUCCEEDED(IDirect3DSurface9_LockRect(readback, &lr, NULL, D3DLOCK_READONLY))) {
        DWORD px = ((DWORD *)lr.pBits)[300 * (lr.Pitch / 4) + 400];
        printf("frame %d readback 0x%08lx %s\n", frames, (unsigned long)px,
               (px & 0xffffff) == 0x00c800 ? "OK" : "WRONG");
        fflush(stdout);
        IDirect3DSurface9_UnlockRect(readback);
      }
    }

    IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
    frames++;
  }

done:
  IDirect3DDevice9_Release(dev);
  IDirect3D9_Release(d3d);
  return 0;
}
