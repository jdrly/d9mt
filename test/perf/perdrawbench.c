/* perdrawbench: CPU-bound benchmark of the per-draw vertex upload (UploadPerDrawData, #56). 320x240,
 * no vsync, 2000 DrawIndexedPrimitive per frame from D3DPOOL_SYSTEMMEM + D3DUSAGE_DYNAMIC vertex
 * buffers, which d9mt copies into an upload slice on every draw; each draw names an in-range
 * vertex window (MinVertexIndex/NumVertices) of a 768-vertex buffer of 24-byte vertices, 4 buffers
 * rotating. Prints the average CPU frame time over frames 61..frames.
 *
 *   perdrawbench.exe [frames (560)]
 *
 * The mode comes from the environment, so one binary and one argv serve both modes of a block
 * (bench-multi varies only the environment between variants):
 *   D9MT_PDRW_FLUSH unset or 0: each frame ends with Present (`RESULT perdrawbench frames= avg_ms=`).
 *   D9MT_PDRW_FLUSH=1: no Present. Each frame ends with an event query instead; d9mt submits the
 *     frame's work at the query's end (or at the next FLUSH poll), and before the next frame the
 *     bench waits until the previous frame's query has signaled, polling with D3DGETDATA_FLUSH
 *     every 20 us (as bench_draws flush does). So at most PDRW_DEPTH (2) frames of GPU work are
 *     outstanding, the uploads and draws are the same, and the drawable acquire, the present
 *     copy and the frame-latency wait are gone. The line is
 *     `RESULT perdrawbench mode=flush depth=2 frames= avg_ms=`, and `PDRW waited= polls=`
 *     follows it: the frames whose previous frame wasn't done yet, and the polls in all.
 * Any other value is a usage failure. Both modes print `MODE perdrawbench <mode>` first.
 *
 * D9MT_PDRW_CHECK=1 (validation runs only, never timed ones) adds the semantics check: an
 * occlusion query around every frame's draws, summed (`samples=`, equal in both modes when every
 * frame's draws reach the GPU), and the last frame's render target read back before its
 * Present or query (`rt_hash=` FNV-1a of the RGB, `rt_lit=` non-black pixels), as a `CHECK` line.
 * In flush mode it also presents once before the first frame and three times after the last, a
 * second apart, so that with D9MT_PROFILE d9mt's STATS line covers the flush frames' command
 * buffers and draws (STATS needs Presents). */
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bench_window.h"

struct V { float x, y, z, rhw; float psize; DWORD c; };   /* 24 bytes */
#define FVF (D3DFVF_XYZRHW | D3DFVF_PSIZE | D3DFVF_DIFFUSE)
#define NV 768
#define PDRW_DEPTH 2

/* Polls q with D3DGETDATA_FLUSH every 20 us until it signals; 0 when it did, -1 on an error or
 * after 5 s. *polls counts the polls that found it not done. */
static int wait_query(IDirect3DQuery9 *q, LONGLONG freq, unsigned long long *polls) {
  LARGE_INTEGER s, p0, p1;
  HRESULT hr;
  QueryPerformanceCounter(&s);
  while ((hr = IDirect3DQuery9_GetData(q, NULL, 0, D3DGETDATA_FLUSH)) == S_FALSE) {
    (*polls)++;
    QueryPerformanceCounter(&p0);
    do QueryPerformanceCounter(&p1); while ((p1.QuadPart - p0.QuadPart) * 50000 < freq);
    if (p1.QuadPart - s.QuadPart > 5 * freq) return -1;
  }
  return hr == S_OK ? 0 : -1;
}

int main(int argc, char **argv) {
  int frames = argc > 1 ? atoi(argv[1]) : 560;
  if (frames <= 60) { printf("RESULT FAIL usage: frames must be more than the 60 warm-up frames\n"); return 2; }
  const char *mode = getenv("D9MT_PDRW_FLUSH"), *chk = getenv("D9MT_PDRW_CHECK");
  const int flush = mode && !strcmp(mode, "1"), check = chk && !strcmp(chk, "1");
  if (mode && !flush && strcmp(mode, "0")) { printf("RESULT FAIL usage: D9MT_PDRW_FLUSH must be 0 or 1\n"); return 2; }
  WNDCLASSA wc = { 0 }; wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "pdb";
  RegisterClassA(&wc);
  HWND hwnd = bench_window("pdb", "perdrawbench", 320, 240);
  if (flush) printf("MODE perdrawbench flush depth=%d%s\n", PDRW_DEPTH, check ? " check" : "");
  else printf("MODE perdrawbench present%s\n", check ? " check" : "");
  IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
  D3DPRESENT_PARAMETERS pp = { 0 };
  pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferFormat = D3DFMT_X8R8G8B8;
  pp.BackBufferWidth = 320; pp.BackBufferHeight = 240; pp.hDeviceWindow = hwnd;
  pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
  IDirect3DDevice9 *dev = NULL;
  if (FAILED(IDirect3D9_CreateDevice(d3d, 0, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev))) { printf("RESULT FAIL device\n"); return 1; }
  IDirect3DVertexBuffer9 *vb[4];
  IDirect3DIndexBuffer9 *ib = NULL;
  for (int b = 0; b < 4; b++) {
    struct V *v = NULL;
    if (FAILED(IDirect3DDevice9_CreateVertexBuffer(dev, NV * sizeof(struct V), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, FVF, D3DPOOL_SYSTEMMEM, &vb[b], NULL))
     || FAILED(IDirect3DVertexBuffer9_Lock(vb[b], 0, 0, (void **)&v, 0))) { printf("RESULT FAIL vertex buffer\n"); return 1; }
    for (int i = 0; i < NV; i++) {
      v[i].x = (float)(i % 32) * 10.0f; v[i].y = (float)(i / 32 % 24) * 10.0f; v[i].z = 0.5f; v[i].rhw = 1.0f;
      v[i].psize = 1.0f; v[i].c = 0xff000000u | (DWORD)(b * 0x304050 + i);
    }
    IDirect3DVertexBuffer9_Unlock(vb[b]);
  }
  {
    WORD *x = NULL;
    if (FAILED(IDirect3DDevice9_CreateIndexBuffer(dev, 6 * sizeof(WORD), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &ib, NULL))
     || FAILED(IDirect3DIndexBuffer9_Lock(ib, 0, 0, (void **)&x, 0))) { printf("RESULT FAIL index buffer\n"); return 1; }
    static const WORD idx[6] = { 0, 1, 32, 1, 33, 32 };
    for (int i = 0; i < 6; i++) x[i] = idx[i];
    IDirect3DIndexBuffer9_Unlock(ib);
  }
  /* flush mode: one event query per outstanding frame; check: one occlusion query per frame */
  IDirect3DQuery9 *ev[PDRW_DEPTH] = { 0 }, **occ = NULL;
  IDirect3DSurface9 *rt = NULL, *rb = NULL;
  for (int k = 0; flush && k < PDRW_DEPTH; k++)
    if (FAILED(IDirect3DDevice9_CreateQuery(dev, D3DQUERYTYPE_EVENT, &ev[k]))) { printf("RESULT FAIL event query\n"); return 1; }
  if (check) {
    occ = calloc(frames + 1, sizeof(*occ));
    for (int n = 1; n <= frames; n++)
      if (FAILED(IDirect3DDevice9_CreateQuery(dev, D3DQUERYTYPE_OCCLUSION, &occ[n]))) { printf("RESULT FAIL occlusion query\n"); return 1; }
    if (FAILED(IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &rt))
     || FAILED(IDirect3DDevice9_CreateOffscreenPlainSurface(dev, 320, 240, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &rb, NULL))) { printf("RESULT FAIL readback surface\n"); return 1; }
  }
  IDirect3DDevice9_SetFVF(dev, FVF);
  IDirect3DDevice9_SetIndices(dev, ib);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, FALSE);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
  LARGE_INTEGER f, t0, t1; QueryPerformanceFrequency(&f);
  MSG msg;
  unsigned long long polls = 0;
  int waited = 0;
  if (flush && check && FAILED(IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL))) { printf("RESULT FAIL check present\n"); return 1; }
  for (int n = 1; n <= frames; n++) {
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
    if (n == 61) { bench_timed_mark(); QueryPerformanceCounter(&t0); }
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xff000000, 1.0f, 0);
    IDirect3DDevice9_BeginScene(dev);
    if (check) IDirect3DQuery9_Issue(occ[n], D3DISSUE_BEGIN);
    for (int i = 0; i < 2000; i++) {
      /* an in-range window of 34 vertices (a quad's indices 0..33 from BaseVertexIndex) */
      const INT base = (i * 37 + n) % (NV - 34);
      IDirect3DDevice9_SetStreamSource(dev, 0, vb[i & 3], 0, sizeof(struct V));
      if (FAILED(IDirect3DDevice9_DrawIndexedPrimitive(dev, D3DPT_TRIANGLELIST, base, 0, 34, 0, 2))) {
        printf("RESULT FAIL draw frame=%d\n", n); return 1;
      }
    }
    if (check) IDirect3DQuery9_Issue(occ[n], D3DISSUE_END);
    IDirect3DDevice9_EndScene(dev);
    if (check && n == frames && FAILED(IDirect3DDevice9_GetRenderTargetData(dev, rt, rb))) { printf("RESULT FAIL readback\n"); return 1; }
    if (flush) {
      /* end the frame with its event query, then bound the GPU backlog: the previous frame's
       * query must have signaled before the next frame starts */
      if (FAILED(IDirect3DQuery9_Issue(ev[n % PDRW_DEPTH], D3DISSUE_END))) { printf("RESULT FAIL issue frame=%d\n", n); return 1; }
      if (n > 1) {
        const unsigned long long before = polls;
        if (wait_query(ev[(n - 1) % PDRW_DEPTH], f.QuadPart, &polls)) { printf("RESULT FAIL getdata frame=%d\n", n); return 1; }
        waited += polls != before;
      }
    } else if (FAILED(IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL))) { printf("RESULT FAIL present frame=%d\n", n); return 1; }
  }
  QueryPerformanceCounter(&t1);
  bench_timed_print("perdrawbench");
  if (flush) {
    printf("RESULT perdrawbench mode=flush depth=%d frames=%d avg_ms=%.4f\n", PDRW_DEPTH, frames - 60, (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart / (frames - 60));
    printf("PDRW waited=%d polls=%llu\n", waited, polls);
  } else {
    printf("RESULT perdrawbench frames=%d avg_ms=%.4f\n", frames - 60, (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart / (frames - 60));
  }
  if (check) {
    unsigned long long samples = 0, dummy = 0;
    for (int n = 1; n <= frames; n++) {
      DWORD s = 0;
      if (wait_query(occ[n], f.QuadPart, &dummy) || IDirect3DQuery9_GetData(occ[n], &s, sizeof(s), 0) != S_OK) { printf("CHECK FAIL occlusion frame=%d\n", n); return 1; }
      samples += s;
    }
    D3DLOCKED_RECT lr;
    unsigned int hash = 2166136261u, lit = 0;
    if (FAILED(IDirect3DSurface9_LockRect(rb, &lr, NULL, D3DLOCK_READONLY))) { printf("CHECK FAIL lock\n"); return 1; }
    for (int y = 0; y < 240; y++)
      for (int x = 0; x < 320; x++) {
        const DWORD c = ((const DWORD *)((const char *)lr.pBits + y * lr.Pitch))[x] & 0xffffffu;
        for (int k = 0; k < 3; k++) { hash ^= (c >> (8 * k)) & 0xffu; hash *= 16777619u; }
        lit += c != 0;
      }
    IDirect3DSurface9_UnlockRect(rb);
    if (flush) {  /* for d9mt's STATS: the flush frames' work into one window, a second apart */
      for (int k = 0; k < 3; k++) {
        if (wait_query(ev[frames % PDRW_DEPTH], f.QuadPart, &dummy)) { printf("CHECK FAIL idle\n"); return 1; }
        Sleep(1100);
        if (FAILED(IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL))) { printf("CHECK FAIL present\n"); return 1; }
        IDirect3DQuery9_Issue(ev[frames % PDRW_DEPTH], D3DISSUE_END);
      }
    }
    printf("CHECK perdrawbench mode=%s frames=%d draws=%d samples=%llu rt_hash=%08x rt_lit=%u\n",
           flush ? "flush" : "present", frames, frames * 2000, samples, hash, lit);
    for (int n = 1; n <= frames; n++) IDirect3DQuery9_Release(occ[n]);
    free(occ);
    IDirect3DSurface9_Release(rb); IDirect3DSurface9_Release(rt);
  }
  for (int k = 0; flush && k < PDRW_DEPTH; k++) IDirect3DQuery9_Release(ev[k]);
  for (int b = 0; b < 4; b++) IDirect3DVertexBuffer9_Release(vb[b]);
  IDirect3DIndexBuffer9_Release(ib);
  IDirect3DDevice9_Release(dev); IDirect3D9_Release(d3d);
  return 0;
}
