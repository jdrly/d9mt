/* code_vb_relock: a static vertex buffer (D3DPOOL_DEFAULT, D3DUSAGE_WRITEONLY without DYNAMIC)
 * locked again with flags 0 after draws used it, as a game streams geometry. Each frame locks the
 * whole buffer and writes three quads (A, B, C) in this frame's colours, draws A and B, then locks
 * B's vertices alone, moves B down and colours it blue, unlocks and draws B and C again.
 *
 * The first draw of B was issued before the second lock, so it must show the old contents: B in
 * green at the top, and the blue B below it. C, outside the second lock's range, keeps its yellow.
 * The program reads the back buffer back each frame and checks one pixel of each quad.
 *
 * Which path the locks take depends on the memory profile: mapped directly below 64 GB of RAM
 * (the second lock of a used buffer waits for the CS thread or the GPU, and after two such locks
 * the buffer goes to the staging path for good), the staging path from the first lock on 64 GB
 * and up (D9MT_MEMORY_PROFILE=unlimited, run-hash-tests.sh VARIANT=unlimited). The frames are
 * the same on both. Five frames; prints "RESULT vbrelock frames=5", and "FAIL <what>" for each
 * wrong pixel. */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>

struct V { float x, y, z, rhw; DWORD c; };
#define FVF (D3DFVF_XYZRHW | D3DFVF_DIFFUSE)

#define TRY(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { \
    printf("RESULT FAIL %s: hr=%08lx (line %d)\n", #x, (unsigned long)hr_, __LINE__); fflush(stdout); return 1; } } while (0)

static void quad(struct V *v, float x0, float y0, float x1, float y1, DWORD c) {
  const float xs[4] = { x0, x1, x0, x1 }, ys[4] = { y0, y0, y1, y1 };
  for (int i = 0; i < 4; i++) {
    v[i].x = xs[i]; v[i].y = ys[i]; v[i].z = 0.5f; v[i].rhw = 1.0f; v[i].c = c;
  }
}

static int check(const D3DLOCKED_RECT *lr, int frame, const char *what, int x, int y, DWORD want) {
  const DWORD got = *(const DWORD *)((const unsigned char *)lr->pBits + (size_t)y * lr->Pitch + x * 4) & 0xffffffu;
  if (got == (want & 0xffffffu))
    return 0;
  printf("FAIL frame=%d %s at %d,%d: %06lx, want %06lx\n", frame, what, x, y,
         (unsigned long)got, (unsigned long)(want & 0xffffffu));
  return 1;
}

int main(void) {
  WNDCLASSA wc = { 0 }; wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "vbrelock";
  RegisterClassA(&wc);
  HWND hwnd = CreateWindowA("vbrelock", "vbrelock", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 340, 280, NULL, NULL, wc.hInstance, NULL);
  IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
  if (!hwnd || !d3d) { printf("RESULT FAIL window/d3d\n"); return 1; }
  D3DPRESENT_PARAMETERS pp = { 0 };
  pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferFormat = D3DFMT_X8R8G8B8;
  pp.BackBufferWidth = 320; pp.BackBufferHeight = 240; pp.hDeviceWindow = hwnd;
  pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
  IDirect3DDevice9 *dev = NULL;
  TRY(IDirect3D9_CreateDevice(d3d, 0, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev));
  IDirect3DVertexBuffer9 *vb = NULL;
  TRY(IDirect3DDevice9_CreateVertexBuffer(dev, 12 * sizeof(struct V), D3DUSAGE_WRITEONLY, FVF, D3DPOOL_DEFAULT, &vb, NULL));
  IDirect3DSurface9 *bb = NULL, *sys = NULL;
  TRY(IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb));
  TRY(IDirect3DDevice9_CreateOffscreenPlainSurface(dev, 320, 240, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, NULL));
  TRY(IDirect3DDevice9_SetFVF(dev, FVF));
  TRY(IDirect3DDevice9_SetStreamSource(dev, 0, vb, 0, sizeof(struct V)));
  TRY(IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE));
  TRY(IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, FALSE));
  TRY(IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE));
  int frames = 0, fails = 0;
  for (; frames < 5; frames++) {
    const DWORD k = (DWORD)(50 * (frames + 1));
    const DWORD red = 0xff000000u | (k << 16), green = 0xff000000u | (k << 8), blue = 0xff000000u | k;
    const DWORD yellow = 0xffffff00u;
    struct V *v = NULL;
    /* From the second frame on, the last frame's draws used the buffer. */
    TRY(IDirect3DVertexBuffer9_Lock(vb, 0, 0, (void **)&v, 0));
    quad(v + 0, 20.0f, 20.0f, 100.0f, 100.0f, red);
    quad(v + 4, 120.0f, 20.0f, 200.0f, 100.0f, green);
    quad(v + 8, 220.0f, 20.0f, 300.0f, 100.0f, yellow);
    TRY(IDirect3DVertexBuffer9_Unlock(vb));
    TRY(IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0));
    TRY(IDirect3DDevice9_BeginScene(dev));
    TRY(IDirect3DDevice9_DrawPrimitive(dev, D3DPT_TRIANGLESTRIP, 0, 2));
    TRY(IDirect3DDevice9_DrawPrimitive(dev, D3DPT_TRIANGLESTRIP, 4, 2));
    /* B's vertices again, right after a draw used them. */
    TRY(IDirect3DVertexBuffer9_Lock(vb, 4 * sizeof(struct V), 4 * sizeof(struct V), (void **)&v, 0));
    quad(v, 120.0f, 140.0f, 200.0f, 220.0f, blue);
    TRY(IDirect3DVertexBuffer9_Unlock(vb));
    TRY(IDirect3DDevice9_DrawPrimitive(dev, D3DPT_TRIANGLESTRIP, 4, 2));
    TRY(IDirect3DDevice9_DrawPrimitive(dev, D3DPT_TRIANGLESTRIP, 8, 2));
    TRY(IDirect3DDevice9_EndScene(dev));
    D3DLOCKED_RECT lr;
    TRY(IDirect3DDevice9_GetRenderTargetData(dev, bb, sys));
    TRY(IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY));
    fails += check(&lr, frames + 1, "A", 60, 60, red);
    fails += check(&lr, frames + 1, "B before the second lock", 160, 60, green);
    fails += check(&lr, frames + 1, "B after it", 160, 180, blue);
    fails += check(&lr, frames + 1, "C", 260, 60, yellow);
    TRY(IDirect3DSurface9_UnlockRect(sys));
    TRY(IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL));
  }
  printf("RESULT vbrelock frames=%d fails=%d\n", frames, fails);
  return fails ? 1 : 0;
}
