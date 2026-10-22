/* bench_window.h: the benches' window, with a client area of exactly the back buffer's size
 * (AdjustWindowRect). Only then does the presenter copy each frame into the drawable
 * (d3d9_swapchain.cpp); a client of any other size (a 340x280 window holds a 332x246 client)
 * makes every present a scaling blit: an extra PSO and a GPU pass that aren't the code under
 * test. bench_client() prints the client area the window got, `CLIENT <bench> <W>x<H>`, with
 * `MISMATCH backbuffer=<W>x<H>` when it isn't the back buffer's size; scripts/perf-abba.sh
 * carries it into each run's line. */
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>

static inline void bench_client(HWND hwnd, const char *bench, UINT w, UINT h) {
  RECT rc;
  if (!hwnd || !GetClientRect(hwnd, &rc)) {
    printf("CLIENT %s unknown\n", bench);
  } else if ((UINT)(rc.right - rc.left) == w && (UINT)(rc.bottom - rc.top) == h) {
    printf("CLIENT %s %ux%u\n", bench, w, h);
  } else {
    printf("CLIENT %s %ldx%ld MISMATCH backbuffer=%ux%u: every present takes the scaling blit\n",
           bench, (long)(rc.right - rc.left), (long)(rc.bottom - rc.top), w, h);
  }
  fflush(stdout);
}

/* The same for a window created elsewhere (t_common.h's t_init): the device's focus window. */
static inline void bench_client_of(IDirect3DDevice9 *dev, const char *bench, UINT w, UINT h) {
  D3DDEVICE_CREATION_PARAMETERS cp;
  bench_client(dev && SUCCEEDED(IDirect3DDevice9_GetCreationParameters(dev, &cp)) ? cp.hFocusWindow : NULL,
               bench, w, h);
}

/* A visible WS_OVERLAPPEDWINDOW of the registered class cls whose client area is w x h. */
static inline HWND bench_window(const char *cls, const char *bench, UINT w, UINT h) {
  RECT rc = { 0, 0, (LONG)w, (LONG)h };
  AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
  HWND hwnd = CreateWindowA(cls, cls, WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, rc.right - rc.left,
                            rc.bottom - rc.top, NULL, NULL, GetModuleHandleA(NULL), NULL);
  bench_client(hwnd, bench, w, h);
  return hwnd;
}
