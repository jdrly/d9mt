/* ThrottleAllocation at the hard staging limit with nothing recorded since the last
 * submission: each frame flushes through an event query (GetData with D3DGETDATA_FLUSH
 * until it signals), then DISCARD-locks a 100 MiB dynamic vertex buffer, more than the
 * 90 MiB of staging memory allowed in flight. The throttle asked the flush tracker for an
 * explicit flush, which it refuses when no chunk was recorded since the last one, and then
 * waited for a staging fence value no submission would ever signal: the first Lock hung
 * for good. Every frame must draw its quad from the buffer. */
#include "t_xfer.h"

#define BIG_VB_BYTES (100u << 20)

static IDirect3DVertexBuffer9 *vb;
static IDirect3DQuery9 *query;

/* The watchdog times the DISCARD Lock itself: lock_t0 is the tick the Lock began, lock_active is 1
 * while it runs. A Lock that hasn't returned after 20 s is the hang. It disarms after frame 5's
 * checks, so a slow start or a long standalone run (x_loop presents up to frame 600) never trips it. */
static volatile LONG lock_t0;
static volatile LONG lock_active;
static volatile LONG disarmed;

static DWORD WINAPI watchdog(LPVOID p) {
  (void)p;
  for (;;) {
    Sleep(250);
    if (InterlockedCompareExchange(&disarmed, 0, 0))
      return 0;
    if (!InterlockedCompareExchange(&lock_active, 0, 0))
      continue;
    const DWORD t0 = (DWORD)InterlockedCompareExchange(&lock_t0, 0, 0);
    if (GetTickCount() - t0 > 20000) {
      printf("FAIL code_throttle_flush: Lock hasn't returned in 20 s (hang in Lock)\n");
      fflush(stdout);
      ExitProcess(3);
    }
  }
}

/* DXVK_CONFIG declares d3d9.reproducibleCommandStream = True (the harness's variant sets it there). */
static BOOL repro_declared(void) {
  char cfg[1024];
  DWORD n = GetEnvironmentVariableA("DXVK_CONFIG", cfg, sizeof(cfg));
  if (n == 0 || n >= sizeof(cfg))
    return FALSE;
  const char *k = strstr(cfg, "reproducibleCommandStream");
  if (!k)
    return FALSE;
  const char *eq = strchr(k, '=');
  if (!eq)
    return FALSE;
  eq++;
  while (*eq == ' ' || *eq == '\t')
    eq++;
  return _strnicmp(eq, "true", 4) == 0;
}

static DWORD color(int n) {
  return 0xff000000u | (DWORD)(n * 40) << 16 | 0x8000u | (DWORD)(255 - n * 30);
}

static void frame(int n) {
  if (n > 5)
    return;
  IDirect3DTexture9 *t;
  D3DLOCKED_RECT lr;
  X_CHECK(IDirect3DDevice9_CreateTexture(x_dev, 1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, NULL));
  X_CHECK(IDirect3DTexture9_LockRect(t, 0, &lr, NULL, 0));
  *(DWORD *)lr.pBits = color(n);
  IDirect3DTexture9_UnlockRect(t, 0);

  IDirect3DDevice9_SetRenderTarget(x_dev, 0, x_backBuffer);
  IDirect3DDevice9_Clear(x_dev, 0, NULL, D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0);

  X_CHECK(IDirect3DQuery9_Issue(query, D3DISSUE_END));
  /* The flush before the lock is the test's precondition. d3d9.reproducibleCommandStream submits
   * only on explicit flushes, so this poll never completes before a Present: with that option
   * declared in DXVK_CONFIG the test can't run and exits 77 (SKIP, uncovered, not a pass). Without
   * it, a query that doesn't signal is a failure of its own. */
  BOOL done = FALSE;
  const DWORD poll0 = GetTickCount();
  while (IDirect3DQuery9_GetData(query, &done, sizeof(done), D3DGETDATA_FLUSH) == S_FALSE) {
    if (GetTickCount() - poll0 > 5000) {
      if (repro_declared()) {
        printf("SKIP code_throttle_flush: d3d9.reproducibleCommandStream (DXVK_CONFIG) submits only on explicit "
               "flushes, so the event query can't signal before the lock; not covered in this mode\n");
        fflush(stdout);
        ExitProcess(77);
      }
      printf("FAIL code_throttle_flush: the event query hasn't signalled in 5 s with D3DGETDATA_FLUSH (before the lock)\n");
      fflush(stdout);
      ExitProcess(4);
    }
    Sleep(0);
  }

  float *v;
  InterlockedExchange(&lock_t0, (LONG)GetTickCount());
  InterlockedExchange(&lock_active, 1);
  X_CHECK(IDirect3DVertexBuffer9_Lock(vb, 0, 0, (void **)&v, D3DLOCK_DISCARD));
  InterlockedExchange(&lock_active, 0);
  const float q[20] = {
      -0.5f, 0.5f, 0.5f, 0.0f, 0.0f, 0.5f, 0.5f, 0.5f, 1.0f, 0.0f,
      -0.5f, -0.5f, 0.5f, 0.0f, 1.0f, 0.5f, -0.5f, 0.5f, 1.0f, 1.0f};
  memcpy(v, q, sizeof(q));
  IDirect3DVertexBuffer9_Unlock(vb);

  IDirect3DDevice9_BeginScene(x_dev);
  IDirect3DDevice9_SetTexture(x_dev, 0, (IDirect3DBaseTexture9 *)t);
  IDirect3DDevice9_SetStreamSource(x_dev, 0, vb, 0, 5 * sizeof(float));
  IDirect3DDevice9_DrawPrimitive(x_dev, D3DPT_TRIANGLESTRIP, 0, 2);
  IDirect3DDevice9_SetTexture(x_dev, 0, NULL);
  IDirect3DDevice9_EndScene(x_dev);
  IDirect3DTexture9_Release(t);

  XReadback r = x_read(x_backBuffer);
  char what[32];
  snprintf(what, sizeof(what), "frame %d quad", n);
  x_expect(what, &r, 320, 240, color(n), 0x00ffffffu);
  snprintf(what, sizeof(what), "frame %d outside", n);
  x_expect(what, &r, 40, 40, 0xff000000u, 0x00ffffffu);
  x_done(&r);
  if (n == 5) {
    x_report("code_throttle_flush");
    InterlockedExchange(&disarmed, 1);
  }
}

int main(void) {
  CloseHandle(CreateThread(NULL, 0, watchdog, NULL, 0, NULL));
  x_init("d9mt_throttle_flush");
  IDirect3DDevice9_SetRenderState(x_dev, D3DRS_ALPHABLENDENABLE, FALSE);
  X_CHECK(IDirect3DDevice9_CreateVertexBuffer(x_dev, BIG_VB_BYTES, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0,
                                             D3DPOOL_DEFAULT, &vb, NULL));
  X_CHECK(IDirect3DDevice9_CreateQuery(x_dev, D3DQUERYTYPE_EVENT, &query));
  return x_loop(frame);
}
