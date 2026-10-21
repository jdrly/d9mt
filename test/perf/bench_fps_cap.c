/* bench_fps_cap: frame pacing under a frame rate cap (D9MT_FPS_CAP, DXVK's d3d9.maxFrameRate,
 * or a game-style limiter in this program). A game-like loop: per frame,
 * some CPU work on the render thread (a busy loop, optionally with random jitter), a GPU load
 * (fullscreen quads with the heavy pixel shader), Present. The interval between Present
 * returns is the frame time the game sees; its distribution over the run is printed.
 *
 *   bench_fps_cap.exe [seconds] [vsync 0/1] [cpu ms] [jitter ms] [quads] [game cap fps]
 *
 * Defaults: 30 s (after 2 s of warm-up), vsync on, 8 ms of CPU work, 0 ms jitter, 8 quads,
 * no game cap. game cap > 0: the program limits itself like a game engine with a minimum frame
 * time would (Sleep(1) until the frame's time is up), for comparison with d9mt's cap.
 *
 * Output (stdout):
 *   RESULT n=<frames> fps=<avg> p1/p50/p95/p99=<ms> min/max=<ms> sd=<ms> long=<frames over 1.5x
 *     the median> cpuProc=<process CPU ms per s> cpuMain=<render thread CPU ms per s>
 *   HIST <ms bin>:<count> ...   (1 ms bins, non-empty only)
 * Not a hash test; one window (scripts/build-benches.sh's BENCH_INCLUDE can place it). */
#include "t_common.h"
#include <math.h>
#include <stdlib.h>

#define MAX_FRAMES 200000

static double qpcMs(void) {
  static LARGE_INTEGER freq;
  LARGE_INTEGER now;
  if (!freq.QuadPart)
    QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&now);
  return (double)now.QuadPart * 1000.0 / (double)freq.QuadPart;
}

static double cpuMs(HANDLE h, BOOL thread) {
  FILETIME c, e, k, u;
  BOOL ok = thread ? GetThreadTimes(h, &c, &e, &k, &u) : GetProcessTimes(h, &c, &e, &k, &u);
  if (!ok)
    return 0.0;
  const ULONGLONG kk = ((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime;
  const ULONGLONG uu = ((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime;
  return (double)(kk + uu) / 10000.0;
}

static void busy(double ms) {
  const double end = qpcMs() + ms;
  volatile unsigned x = 1u;
  while (qpcMs() < end)
    for (int i = 0; i < 200; i++) x = x * 1664525u + 1013904223u;
}

static int cmp(const void *a, const void *b) {
  const double x = *(const double *)a, y = *(const double *)b;
  return x < y ? -1 : x > y ? 1 : 0;
}

static double pct(const double *s, int n, double p) {
  int i = (int)(p / 100.0 * (n - 1) + 0.5);
  return s[i < 0 ? 0 : i >= n ? n - 1 : i];
}

int main(int argc, char **argv) {
  const double seconds = argc > 1 ? atof(argv[1]) : 30.0;
  const int vsync = argc > 2 ? atoi(argv[2]) : 1;
  const double cpuWork = argc > 3 ? atof(argv[3]) : 8.0;
  const double jitter = argc > 4 ? atof(argv[4]) : 0.0;
  const int quads = argc > 5 ? atoi(argv[5]) : 8;
  const double gameCap = argc > 6 ? atof(argv[6]) : 0.0;

  t_present_interval = vsync ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;
  t_init("bench_fps_cap", 640, 360);
  IDirect3DDevice9_SetVertexShader(t_dev, t_vs_pos);

  static double dt[MAX_FRAMES];
  int n = 0;
  unsigned seed = 4242u;
  const double t0 = qpcMs();
  double last = 0.0, frameStart = t0, cpu0 = 0.0, cpuMain0 = 0.0, tMeasure = 0.0;
  int measuring = 0;
  MSG msg;

  for (;;) {
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageA(&msg);
    }
    /* the game's CPU work for the frame */
    seed = seed * 1664525u + 1013904223u;
    busy(cpuWork + jitter * (double)(seed >> 8) / 16777216.0);

    IDirect3DDevice9_Clear(t_dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                           D3DCOLOR_XRGB((n * 3) & 255, 40, 80), 1.0f, 0);
    IDirect3DDevice9_BeginScene(t_dev);
    IDirect3DDevice9_SetPixelShader(t_dev, t_ps_heavy);
    for (int q = 0; q < quads; q++) {
      t_color(0.1f * (q & 7), 0.5f, 0.2f, 1.0f);
      t_quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.5f);
    }
    IDirect3DDevice9_EndScene(t_dev);

    if (gameCap > 0.0) { /* a game's own limiter: wait out the minimum frame time */
      while (qpcMs() - frameStart < 1000.0 / gameCap)
        Sleep(1);
    }
    frameStart = qpcMs();
    IDirect3DDevice9_Present(t_dev, NULL, NULL, NULL, NULL);
    const double now = qpcMs();

    if (!measuring && now - t0 >= 2000.0) {
      measuring = 1;
      tMeasure = now;
      cpu0 = cpuMs(GetCurrentProcess(), FALSE);
      cpuMain0 = cpuMs(GetCurrentThread(), TRUE);
    } else if (measuring && n < MAX_FRAMES) {
      dt[n++] = now - last;
    }
    last = now;
    if (measuring && now - tMeasure >= seconds * 1000.0)
      break;
  }

  const double wall = (last - tMeasure) / 1000.0;
  const double cpuP = (cpuMs(GetCurrentProcess(), FALSE) - cpu0) / wall;
  const double cpuM = (cpuMs(GetCurrentThread(), TRUE) - cpuMain0) / wall;

  double sum = 0.0, sq = 0.0;
  for (int i = 0; i < n; i++) { sum += dt[i]; sq += dt[i] * dt[i]; }
  const double mean = n ? sum / n : 0.0;
  const double sd = n ? sqrt(fmax(0.0, sq / n - mean * mean)) : 0.0;

  static double s[MAX_FRAMES];
  for (int i = 0; i < n; i++) s[i] = dt[i];
  qsort(s, (size_t)n, sizeof(double), cmp);
  const double p50 = n ? pct(s, n, 50.0) : 0.0;
  int longFrames = 0;
  for (int i = 0; i < n; i++)
    if (dt[i] > 1.5 * p50) longFrames++;

  printf("RESULT n=%d fps=%.2f p1=%.2f p50=%.2f p95=%.2f p99=%.2f min=%.2f max=%.2f sd=%.3f long=%d"
         " cpuProc=%.0f cpuMain=%.0f vsync=%d cpuWork=%.1f jitter=%.1f quads=%d gameCap=%.0f\n",
         n, n ? 1000.0 / mean : 0.0, n ? pct(s, n, 1.0) : 0.0, p50, n ? pct(s, n, 95.0) : 0.0,
         n ? pct(s, n, 99.0) : 0.0, n ? s[0] : 0.0, n ? s[n - 1] : 0.0, sd, longFrames, cpuP, cpuM,
         vsync, cpuWork, jitter, quads, gameCap);

  /* 1 ms bins */
  printf("HIST");
  for (int b = 0; b <= 200; b++) {
    int c = 0;
    for (int i = 0; i < n; i++)
      if ((int)dt[i] == b) c++;
    if (c)
      printf(" %d:%d", b, c);
  }
  printf("\n");
  fflush(stdout);
  return 0;
}
