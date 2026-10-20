/* code_g1_hang: waits and polls on GPU progress while a pass is kept open by deferrable
 * flushes (G1, docs/g1-early-commit.md). Run with D9MT_EARLY_COMMIT=always (no idle-GPU gate)
 * against base for reference. A 3840x2160 colour + D24S8 pass gets 1500 DrawPrimitiveUP draws
 * (UP data fills 16 KiB CS chunks, so chunk-full flushes happen inside the pass), then, with
 * the pass still open:
 *   1 event query End, GetData without FLUSH polled          (base reference decides)
 *   2 event query End, GetData(FLUSH) polled
 *   3 4 managed 2560x2560 textures (26 MiB each) locked, written and drawn in the pass
 *     (staging memory throttle)
 *   4 a VB the pass draws from: Lock(DONOTWAIT) polled until it succeeds
 *   5 a dynamic texture the pass samples: LockRect(DONOTWAIT) polled until it succeeds
 *   6 occlusion query around 1500 visible draws: GetData(FLUSH) polled, samples > 0
 * Every poll gives up after 5 s ("FAIL n hang"). Prints "CHECK n ok|FAIL ..." per case and
 * "RESULT g1hang ok=<n> fail=<n>". */
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct V { float x, y, z, rhw; DWORD c; float u, v; };
#define FVF (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1)

static IDirect3DDevice9 *dev;
static LARGE_INTEGER g_f;
static int g_ok, g_fail;

static double nowMs(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return (double)t.QuadPart * 1000.0 / g_f.QuadPart; }

static void check(int n, int ok, const char *what) {
  printf("CHECK %d %s %s\n", n, ok ? "ok" : "FAIL", what);
  fflush(stdout);
  if (ok) g_ok++; else g_fail++;
}

static void quads(int n, int seed) {
  for (int i = 0; i < n; i++) {
    float x = (float)((i * 37 + seed) % 3800), y = (float)((i * 53 + seed) % 2120);
    DWORD c = 0xff000000u | (DWORD)(i * 0x010203 + seed);
    struct V q[4] = { { x, y, 0.5f, 1, c, 0, 0 }, { x + 40, y, 0.5f, 1, c, 1, 0 },
                      { x, y + 40, 0.5f, 1, c, 0, 1 }, { x + 40, y + 40, 0.5f, 1, c, 1, 1 } };
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof(q[0]));
  }
}

/* Polls an event query; flush: D3DGETDATA_FLUSH. 1: done, 0: gave up after 5 s. */
static int pollQuery(IDirect3DQuery9 *q, int flush, void *data, DWORD size) {
  double t0 = nowMs();
  HRESULT hr;
  while ((hr = IDirect3DQuery9_GetData(q, data, size, flush ? D3DGETDATA_FLUSH : 0)) == S_FALSE) {
    if (nowMs() - t0 > 5000.0) return 0;
    Sleep(0);
  }
  return hr == S_OK;
}

int main(void) {
  QueryPerformanceFrequency(&g_f);
  WNDCLASSA wc = { 0 }; wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "g1h";
  RegisterClassA(&wc);
  HWND hwnd = CreateWindowA("g1h", "g1h", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 340, 280, NULL, NULL, wc.hInstance, NULL);
  IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
  D3DPRESENT_PARAMETERS pp = { 0 };
  pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferFormat = D3DFMT_X8R8G8B8;
  pp.BackBufferWidth = 320; pp.BackBufferHeight = 240; pp.hDeviceWindow = hwnd;
  pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
  if (FAILED(IDirect3D9_CreateDevice(d3d, 0, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev))) { printf("RESULT FAIL device\n"); return 1; }

  IDirect3DSurface9 *rt = NULL, *ds = NULL, *bb = NULL, *bds = NULL;
  IDirect3DDevice9_CreateRenderTarget(dev, 3840, 2160, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, NULL);
  IDirect3DDevice9_CreateDepthStencilSurface(dev, 3840, 2160, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, FALSE, &ds, NULL);
  IDirect3DDevice9_GetRenderTarget(dev, 0, &bb);
  IDirect3DDevice9_GetDepthStencilSurface(dev, &bds);
  IDirect3DQuery9 *ev = NULL, *occ = NULL;
  IDirect3DDevice9_CreateQuery(dev, D3DQUERYTYPE_EVENT, &ev);
  IDirect3DDevice9_CreateQuery(dev, D3DQUERYTYPE_OCCLUSION, &occ);
  IDirect3DVertexBuffer9 *vb = NULL;
  IDirect3DDevice9_CreateVertexBuffer(dev, 4 * sizeof(struct V), 0, FVF, D3DPOOL_DEFAULT, &vb, NULL);
  IDirect3DTexture9 *dyn = NULL, *big[4] = { 0 };
  IDirect3DDevice9_CreateTexture(dev, 256, 256, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &dyn, NULL);
  for (int i = 0; i < 4; i++)
    IDirect3DDevice9_CreateTexture(dev, 2560, 2560, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &big[i], NULL);
  if (!rt || !ds || !ev || !occ || !vb || !dyn || !big[3]) { printf("RESULT FAIL resources\n"); return 1; }
  {
    void *p = NULL;
    struct V q[4] = { { 0, 0, 0.5f, 1, 0xff00ff00u, 0, 0 }, { 64, 0, 0.5f, 1, 0xff00ff00u, 1, 0 },
                      { 0, 64, 0.5f, 1, 0xff00ff00u, 0, 1 }, { 64, 64, 0.5f, 1, 0xff00ff00u, 1, 1 } };
    IDirect3DVertexBuffer9_Lock(vb, 0, 0, &p, 0); memcpy(p, q, sizeof(q)); IDirect3DVertexBuffer9_Unlock(vb);
  }
  IDirect3DDevice9_SetFVF(dev, FVF);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, TRUE);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_ZFUNC, D3DCMP_ALWAYS);

  for (int n = 1; n <= 6; n++) {
    IDirect3DDevice9_SetRenderTarget(dev, 0, rt);
    IDirect3DDevice9_SetDepthStencilSurface(dev, ds);
    IDirect3DDevice9_BeginScene(dev);
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff202020, 1.0f, 0);
    IDirect3DDevice9_SetTexture(dev, 0, NULL);
    if (n == 6) IDirect3DQuery9_Issue(occ, D3DISSUE_BEGIN);
    quads(1500, n);
    double t0 = nowMs();
    switch (n) {
      case 1:
      case 2: {
        IDirect3DQuery9_Issue(ev, D3DISSUE_END);
        quads(200, 100 + n);   /* the pass stays open */
        check(n, pollQuery(ev, n == 2, NULL, 0), n == 1 ? "event GetData without FLUSH" : "event GetData(FLUSH)");
        break;
      }
      case 3: {
        for (int i = 0; i < 4; i++) {
          D3DLOCKED_RECT lr;
          if (FAILED(IDirect3DTexture9_LockRect(big[i], 0, &lr, NULL, 0))) break;
          for (int y = 0; y < 2560; y++) memset((char *)lr.pBits + y * lr.Pitch, 0x40 + i, 2560 * 4);
          IDirect3DTexture9_UnlockRect(big[i], 0);
          IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)big[i]);
          quads(400, 300 + i);
        }
        IDirect3DDevice9_SetTexture(dev, 0, NULL);
        check(n, nowMs() - t0 < 5000.0, "4 x 26 MiB managed uploads in one pass (staging throttle)");
        break;
      }
      case 4: {
        IDirect3DDevice9_SetStreamSource(dev, 0, vb, 0, sizeof(struct V));
        for (int i = 0; i < 50; i++) IDirect3DDevice9_DrawPrimitive(dev, D3DPT_TRIANGLESTRIP, 0, 2);
        quads(300, 400);
        void *p = NULL; HRESULT hr;
        while ((hr = IDirect3DVertexBuffer9_Lock(vb, 0, 0, &p, D3DLOCK_DONOTWAIT)) == D3DERR_WASSTILLDRAWING) {
          if (nowMs() - t0 > 5000.0) break;
          Sleep(0);
        }
        if (SUCCEEDED(hr)) IDirect3DVertexBuffer9_Unlock(vb);
        check(n, SUCCEEDED(hr), "VB Lock(DONOTWAIT) polled");
        break;
      }
      case 5: {
        IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)dyn);
        quads(300, 500);
        IDirect3DDevice9_SetTexture(dev, 0, NULL);
        D3DLOCKED_RECT lr; HRESULT hr;
        while ((hr = IDirect3DTexture9_LockRect(dyn, 0, &lr, NULL, D3DLOCK_DONOTWAIT)) == D3DERR_WASSTILLDRAWING) {
          if (nowMs() - t0 > 5000.0) break;
          Sleep(0);
        }
        if (SUCCEEDED(hr)) IDirect3DTexture9_UnlockRect(dyn, 0);
        check(n, SUCCEEDED(hr), "dynamic texture LockRect(DONOTWAIT) polled");
        break;
      }
      case 6: {
        IDirect3DQuery9_Issue(occ, D3DISSUE_END);
        quads(200, 600);
        DWORD samples = 0;
        int done = pollQuery(occ, 1, &samples, sizeof(samples));
        char what[96]; snprintf(what, sizeof(what), "occlusion GetData(FLUSH) samples=%lu", (unsigned long)samples);
        check(n, done && samples > 0, what);
        break;
      }
    }
    IDirect3DDevice9_EndScene(dev);
    IDirect3DDevice9_SetRenderTarget(dev, 0, bb);
    IDirect3DDevice9_SetDepthStencilSurface(dev, bds);
    IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
  }
  /* D9MT_PROFILE writes its PROF line (unsubWait=) every 256 frames: present empty frames until one is out */
  for (int i = 0; i < 260; i++) {
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xff000000, 1.0f, 0);
    IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
  }
  printf("RESULT g1hang ok=%d fail=%d\n", g_ok, g_fail);
  fflush(stdout);
  IDirect3DDevice9_Release(dev); IDirect3D9_Release(d3d);
  return g_fail ? 1 : 0;
}
