/* code_topo: draws that change only the primitive topology, under one unchanged pipeline
 * (perf/topology-not-pso: a topology change no longer marks the PSO dirty). Every frame, back to
 * back with no other state change: a triangle list, a line list, a line strip, a point list, a
 * triangle fan (emulated through an index list) and a triangle strip, each in its own colour and
 * place; the order rotates per frame, so every topology follows every other. Then once more with
 * a state change between them (alpha blending on), so a PSO change and a topology change come
 * together. The frames must hash the same as base. */
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>

struct V { float x, y, z, rhw; DWORD c; };
#define FVF (D3DFVF_XYZRHW | D3DFVF_DIFFUSE)

static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
  return DefWindowProcA(h, m, w, l);
}

static IDirect3DDevice9 *dev;

static void draw(int kind, float ox, float oy, DWORD c) {
  switch (kind) {
    case 0: {   /* triangle list: 2 triangles */
      struct V v[6] = { { ox, oy, 0.5f, 1, c }, { ox + 60, oy, 0.5f, 1, c }, { ox, oy + 60, 0.5f, 1, c },
                        { ox + 70, oy + 10, 0.5f, 1, c }, { ox + 70, oy + 70, 0.5f, 1, c }, { ox + 10, oy + 70, 0.5f, 1, c } };
      IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, v, sizeof(v[0]));
      break;
    }
    case 1: {   /* line list: a grid of 6 lines */
      struct V v[12];
      for (int i = 0; i < 6; i++) {
        struct V a = { ox, oy + 12.0f * i + 0.5f, 0.5f, 1, c }, b = { ox + 70, oy + 12.0f * i + 0.5f, 0.5f, 1, c };
        v[2 * i] = a; v[2 * i + 1] = b;
      }
      IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_LINELIST, 6, v, sizeof(v[0]));
      break;
    }
    case 2: {   /* line strip: a zigzag */
      struct V v[8];
      for (int i = 0; i < 8; i++) { struct V a = { ox + 10.0f * i + 0.5f, oy + ((i & 1) ? 70.0f : 0.5f), 0.5f, 1, c }; v[i] = a; }
      IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_LINESTRIP, 7, v, sizeof(v[0]));
      break;
    }
    case 3: {   /* point list: an 8x8 dot grid */
      struct V v[64];
      for (int i = 0; i < 64; i++) { struct V a = { ox + 9.0f * (i % 8) + 0.5f, oy + 9.0f * (i / 8) + 0.5f, 0.5f, 1, c }; v[i] = a; }
      IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_POINTLIST, 64, v, sizeof(v[0]));
      break;
    }
    case 4: {   /* triangle fan: a hexagon */
      struct V v[8] = { { ox + 35, oy + 35, 0.5f, 1, c } };
      for (int i = 0; i < 7; i++) {
        static const float dx[7] = { 35, 17, -17, -35, -17, 17, 35 }, dy[7] = { 0, 30, 30, 0, -30, -30, 0 };
        struct V a = { ox + 35 + dx[i], oy + 35 + dy[i], 0.5f, 1, c }; v[i + 1] = a;
      }
      IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLEFAN, 6, v, sizeof(v[0]));
      break;
    }
    default: {  /* triangle strip: a quad */
      struct V v[4] = { { ox, oy, 0.5f, 1, c }, { ox + 70, oy, 0.5f, 1, c }, { ox, oy + 70, 0.5f, 1, c }, { ox + 70, oy + 70, 0.5f, 1, c } };
      IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, v, sizeof(v[0]));
      break;
    }
  }
}

int main(void) {
  WNDCLASSA wc = {0};
  wc.lpfnWndProc = wnd_proc;
  wc.hInstance = GetModuleHandleA(NULL);
  wc.lpszClassName = "d9mt_topo";
  RegisterClassA(&wc);
  RECT rc = {0, 0, 640, 480};
  AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
  HWND hwnd = CreateWindowA("d9mt_topo", "d9mt topology switches", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                            CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, NULL, NULL, wc.hInstance, NULL);
  if (!hwnd) return 1;
  IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
  if (!d3d) return 1;
  D3DPRESENT_PARAMETERS pp = {0};
  pp.BackBufferWidth = 640; pp.BackBufferHeight = 480;
  pp.BackBufferFormat = D3DFMT_X8R8G8B8; pp.BackBufferCount = 1;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = hwnd;
  pp.Windowed = TRUE; pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
  if (FAILED(IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                     D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev)))
    return 1;
  IDirect3DDevice9_SetFVF(dev, FVF);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, FALSE);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
  static const DWORD colors[6] = { 0xffff4040, 0xff40ff40, 0xff4040ff, 0xffffff40, 0xffff40ff, 0xff40ffff };

  int frames = 0;
  MSG msg;
  for (;;) {
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) goto done;
      TranslateMessage(&msg);
      DispatchMessageA(&msg);
    }
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(16, 16, 24), 1.0f, 0);
    IDirect3DDevice9_BeginScene(dev);
    /* Row 1: only the topology changes between draws, the order rotating per frame. */
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, FALSE);
    for (int i = 0; i < 6; i++) {
      const int kind = (i + frames) % 6;
      draw(kind, 20.0f + 100.0f * i, 40.0f, colors[kind]);
    }
    /* Row 2: the same, every other draw also toggling blending (PSO and topology change together). */
    for (int i = 0; i < 6; i++) {
      const int kind = (5 - i + frames) % 6;
      IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, (i & 1) ? TRUE : FALSE);
      draw(kind, 20.0f + 100.0f * i, 200.0f, (colors[kind] & 0x00ffffffu) | 0x90000000u);
    }
    /* Row 3: two draws of each topology back to back (an unchanged topology after a change). */
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, FALSE);
    for (int i = 0; i < 6; i++) {
      draw(i, 20.0f + 100.0f * i, 340.0f, colors[(i + 3) % 6]);
      draw(i, 30.0f + 100.0f * i, 350.0f, colors[(i + 1) % 6]);
    }
    IDirect3DDevice9_EndScene(dev);
    IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
    if (++frames == 3) printf("3 frames\n");
  }
done:
  IDirect3DDevice9_Release(dev);
  IDirect3D9_Release(d3d);
  return 0;
}
