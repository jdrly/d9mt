/* Icon pages that a UI evicts and loads again, as SWTOR's Collections window does (field report
 * 2026-10-27: on an M4 Air the icons of a page flashed magenta for up to ~130 ms after each page
 * change; Windows and wined3d show a blank flash). Four pages of 48 64x64 icons and a cache of
 * the last three: flipping 1-2-3-4-3-2-1 evicts a page, releases its textures and creates new
 * ones, which the loader fills a few frames later (staggered per icon). Until then the window
 * draws the new, empty texture, and the frame must show the background through it (a texture
 * nothing has written is zero: blank, as DXVK and Windows show it). Once filled, each icon must
 * show its own two colours. Four kinds of icon, by slot:
 *   0  DEFAULT A8R8G8B8, filled by UpdateTexture from a SYSTEMMEM texture
 *   1  DEFAULT DXT5, the same way
 *   2  MANAGED A8R8G8B8, filled by LockRect
 *   3  DEFAULT DYNAMIC A8R8G8B8, filled by LockRect(DISCARD)
 * The device is created without D3DCREATE_MULTITHREADED, as SWTOR's is. With PK_THREAD=1 a
 * loader thread creates and fills each kind 0/1 icon's own SYSTEMMEM texture, as SWTOR's
 * streaming threads do, and the frame that finds it ready calls UpdateTexture.
 * Frames are read back (GetRenderTargetData) when PK_CHECK divides them. A failed texel is
 * garbage (an empty icon that isn't blank, or a filled one that is no icon's colour), stale
 * (another icon's colour) or late (a filled icon still blank). The first failing frame is written
 * to pk_fail.ppm in the working directory. Exit code 1 if any check failed.
 * Knobs (environment): PK_FRAMES (480, at most 600), PK_FLIP frames per page (6), PK_LOAD frames
 * before a new page's first icon is filled (2), PK_CHECK (1), PK_SPIN_US of CPU work per frame on
 * the render thread (0), PK_KINDS a mask of the kinds above (15), PK_W x PK_H the back buffer
 * (1920 x 1200: over the 8 MiB at which d9mt's early commit, G1, applies), PK_HEAVY rows of a
 * long-running pixel shader before the icons each frame (0), PK_THREAD (0), PK_WRONG (0; n fills each
 * icon with the colours of the icon n slots on: the checker's negative control, every check fails),
 * PK_CHURN noise textures per frame (0). */
#include "t_xfer.h"

#include <stdlib.h>

static const unsigned char pos_uv_vs_bytecode[] = {
#include "pos_uv_vs_bytecode.inc"
};
static const unsigned char heavy_ps_bytecode[] = {
#include "heavy_ps_bytecode.inc"
};

#define PAGES 4
#define CACHE 3
#define ICONS 48
#define ICON 64
#define BG 0x00404040u

typedef struct {
  IDirect3DTexture9 *tex;
  int kind;
  int countdown;                      /* frames until the loader may fill it; 0 = filled */
  IDirect3DTexture9 *volatile src;    /* PK_THREAD: its SYSTEMMEM texture, once filled */
  volatile LONG queued;
} Icon;

typedef struct {
  int loaded;
  int last_use;
  Icon icon[ICONS];
} Page;

static Page pages[PAGES];
static void quad_px(IDirect3DBaseTexture9 *t, int x0, int y0, int x1, int y1);
static IDirect3DTexture9 *upload_argb, *upload_dxt5;
static IDirect3DVertexShader9 *vs_texquad, *vs_pos;
static IDirect3DPixelShader9 *ps_texquad, *ps_heavy;
static int cfg_frames, cfg_flip, cfg_load, cfg_check, cfg_spin_us, cfg_kinds, cfg_w, cfg_h, cfg_heavy, cfg_thread, cfg_wrong, cfg_churn;
static int fail_garbage, fail_stale, fail_late, fail_magenta, frames_failed, printed, dumped;

/* PK_THREAD: kind 0/1 icons waiting for their SYSTEMMEM texture */
typedef struct { Icon *icon; int page, slot; } Job;
static Job jobs[PAGES * ICONS * 4];
static volatile LONG job_head, job_tail;
static CRITICAL_SECTION job_lock;
static HANDLE job_event;

static int env_int(const char *name, int dflt) {
  char buf[32];
  DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
  return (n > 0 && n < sizeof(buf)) ? atoi(buf) : dflt;
}

/* 565 colour of (page, slot, half): unique, never close to magenta or to the background */
static WORD c565(int page, int slot, int half) {
  const int v = page * 96 + slot * 2 + half;
  return (WORD)((4 + (v & 15)) << 11 | (8 + ((v >> 4) & 31)) << 5 | 6);
}

static DWORD x888(WORD c) {
  const DWORD r = c >> 11, g = (c >> 5) & 63, b = c & 31;
  return (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2);
}

static int close_to(DWORD got, DWORD want) {
  for (int s = 0; s < 24; s += 8) {
    const int d = (int)((got >> s) & 0xff) - (int)((want >> s) & 0xff);
    if (d < -3 || d > 3)
      return 0;
  }
  return 1;
}

static int magenta(DWORD c) {
  return ((c >> 16) & 0xff) > 0xb0 && ((c >> 8) & 0xff) < 0x60 && (c & 0xff) > 0xb0;
}

static void fill_argb(BYTE *bits, INT pitch, int page, int slot) {
  for (int y = 0; y < ICON; y++) {
    const DWORD c = 0xff000000u | x888(c565(page, slot, y >= ICON / 2));
    DWORD *row = (DWORD *)(bits + (size_t)y * pitch);
    for (int x = 0; x < ICON; x++)
      row[x] = c;
  }
}

/* solid DXT5 blocks: alpha 0xff, both endpoints the colour, all indices 0 */
static void fill_dxt5(BYTE *bits, INT pitch, int page, int slot) {
  for (int by = 0; by < ICON / 4; by++) {
    const WORD c = c565(page, slot, by >= ICON / 8);
    BYTE *row = bits + (size_t)by * pitch;
    for (int bx = 0; bx < ICON / 4; bx++) {
      BYTE *b = row + bx * 16;
      memset(b, 0, 16);
      b[0] = b[1] = 0xff;
      b[8] = b[10] = (BYTE)(c & 0xff);
      b[9] = b[11] = (BYTE)(c >> 8);
    }
  }
}

static D3DFORMAT kind_format(int kind) {
  return kind == 1 ? D3DFMT_DXT5 : D3DFMT_A8R8G8B8;
}

static void fill_source(IDirect3DTexture9 *src, int kind, int page, int slot) {
  D3DLOCKED_RECT lr;
  X_CHECK(IDirect3DTexture9_LockRect(src, 0, &lr, NULL, 0));
  (kind == 0 ? fill_argb : fill_dxt5)(lr.pBits, lr.Pitch, page, slot);
  IDirect3DTexture9_UnlockRect(src, 0);
}

static DWORD WINAPI loader(void *arg) {
  (void)arg;
  for (;;) {
    WaitForSingleObject(job_event, 5);
    for (;;) {
      Job j;
      EnterCriticalSection(&job_lock);
      const int have = job_tail != job_head;
      if (have)
        j = jobs[job_tail++ % (PAGES * ICONS * 4)];
      LeaveCriticalSection(&job_lock);
      if (!have)
        break;
      IDirect3DTexture9 *src = NULL;
      X_CHECK(IDirect3DDevice9_CreateTexture(x_dev, ICON, ICON, 1, 0, kind_format(j.icon->kind),
                                             D3DPOOL_SYSTEMMEM, &src, NULL));
      fill_source(src, j.icon->kind, j.page, j.slot);
      MemoryBarrier();
      j.icon->src = src;
    }
  }
  return 0;
}

static void create_icon(Icon *ic, int page, int slot, int countdown) {
  ic->kind = slot % 4;
  ic->countdown = countdown;
  ic->src = NULL;
  ic->queued = 0;
  switch (ic->kind) {
    case 0:
    case 1: X_CHECK(IDirect3DDevice9_CreateTexture(x_dev, ICON, ICON, 1, 0, kind_format(ic->kind), D3DPOOL_DEFAULT, &ic->tex, NULL)); break;
    case 2: X_CHECK(IDirect3DDevice9_CreateTexture(x_dev, ICON, ICON, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &ic->tex, NULL)); break;
    default: X_CHECK(IDirect3DDevice9_CreateTexture(x_dev, ICON, ICON, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &ic->tex, NULL)); break;
  }
  if (cfg_thread && ic->kind <= 1) {
    EnterCriticalSection(&job_lock);
    jobs[job_head++ % (PAGES * ICONS * 4)] = (Job){ic, page, (slot + cfg_wrong) % ICONS};
    LeaveCriticalSection(&job_lock);
    ic->queued = 1;
    SetEvent(job_event);
  }
}

/* The loader's turn for this icon: returns 0 if (PK_THREAD) its source isn't ready yet. */
static int fill_icon(Icon *ic, int page, int slot) {
  D3DLOCKED_RECT lr;
  slot = (slot + cfg_wrong) % ICONS;
  switch (ic->kind) {
    case 0:
    case 1: {
      IDirect3DTexture9 *src = ic->kind == 0 ? upload_argb : upload_dxt5;
      if (cfg_thread) {
        src = ic->src;
        if (!src)
          return 0;
        MemoryBarrier();
      } else {
        fill_source(src, ic->kind, page, slot);
      }
      X_CHECK(IDirect3DDevice9_UpdateTexture(x_dev, (IDirect3DBaseTexture9 *)src, (IDirect3DBaseTexture9 *)ic->tex));
      if (cfg_thread) {
        IDirect3DTexture9_Release(src);
        ic->src = NULL;
      }
      break;
    }
    case 2:
      X_CHECK(IDirect3DTexture9_LockRect(ic->tex, 0, &lr, NULL, 0));
      fill_argb(lr.pBits, lr.Pitch, page, slot);
      IDirect3DTexture9_UnlockRect(ic->tex, 0);
      break;
    default:
      X_CHECK(IDirect3DTexture9_LockRect(ic->tex, 0, &lr, NULL, D3DLOCK_DISCARD));
      fill_argb(lr.pBits, lr.Pitch, page, slot);
      IDirect3DTexture9_UnlockRect(ic->tex, 0);
      break;
  }
  ic->countdown = 0;
  return 1;
}

static void release_page(Page *p) {
  for (int k = 0; k < ICONS; k++) {
    Icon *ic = &p->icon[k];
    /* a queued source still being made: wait for it, so it doesn't land in a freed icon */
    while (ic->queued && ic->countdown != 0 && !ic->src)
      Sleep(0);
    if (ic->src)
      IDirect3DTexture9_Release(ic->src);
    if (ic->tex)
      IDirect3DTexture9_Release(ic->tex);
    ic->src = NULL;
    ic->tex = NULL;
  }
  p->loaded = 0;
}

/* the page shown in frame n: 1 2 3 4 3 2 | 1 2 3 4 3 2 | ... (0-based here) */
static int page_of(int n) {
  static const int seq[6] = {0, 1, 2, 3, 2, 1};
  return seq[((n - 1) / cfg_flip) % 6];
}

static void show_page(int page, int n) {
  Page *p = &pages[page];
  p->last_use = n;
  if (p->loaded)
    return;
  int cached = 0, lru = -1;
  for (int i = 0; i < PAGES; i++) {
    if (!pages[i].loaded)
      continue;
    cached++;
    if (lru < 0 || pages[i].last_use < pages[lru].last_use)
      lru = i;
  }
  if (cached >= CACHE)
    release_page(&pages[lru]);
  for (int k = 0; k < ICONS; k++)
    create_icon(&p->icon[k], page, k, cfg_load + k % 3);
  p->loaded = 1;
}

/* the icon's 64x64 square in back-buffer pixels: cell (k % 8, k / 8) of 80x80 */
static int icon_x(int k) { return (k % 8) * 80 + 8; }
static int icon_y(int k) { return (k / 8) * 80 + 8; }

static void dump(const XReadback *r, int n) {
  FILE *f = fopen("pk_fail.ppm", "wb");
  if (!f)
    return;
  const UINT w = r->desc.Width < 640 ? r->desc.Width : 640, h = r->desc.Height < 480 ? r->desc.Height : 480;
  fprintf(f, "P6\n%u %u\n255\n", w, h);
  for (UINT y = 0; y < h; y++)
    for (UINT x = 0; x < w; x++) {
      const DWORD c = x_pixel(r, x, y);
      const unsigned char rgb[3] = {(unsigned char)(c >> 16), (unsigned char)(c >> 8), (unsigned char)c};
      fwrite(rgb, 1, 3, f);
    }
  fclose(f);
  printf("PK dumped frame %d to pk_fail.ppm\n", n);
}

static void fail(const char *cls, int n, int page, int k, int half, DWORD got, DWORD want) {
  x_failed++;
  if (magenta(got))
    fail_magenta++;
  if (printed++ < 40)
    printf("FAIL %s frame %d page %d icon %d kind %d half %d: got %06lx want %06lx\n",
           cls, n, page + 1, k, k % 4, half, (unsigned long)got, (unsigned long)want);
}

static void check(int n, int page) {
  XReadback r = x_read(x_backBuffer);
  const Page *p = &pages[page];
  const int before = x_failed;
  for (int k = 0; k < ICONS; k++) {
    const Icon *ic = &p->icon[k];
    if (!(cfg_kinds & (1 << ic->kind)))
      continue;
    for (int half = 0; half < 2; half++) {
      const UINT x = (UINT)(icon_x(k) + ICON / 2), y = (UINT)(icon_y(k) + ICON / 4 + half * ICON / 2);
      const DWORD got = x_pixel(&r, x, y) & 0x00ffffffu;
      x_checks++;
      if (ic->countdown != 0) {
        if (!close_to(got, BG)) {
          fail_garbage++;
          fail("garbage-unfilled", n, page, k, half, got, BG);
        }
        continue;
      }
      const DWORD want = x888(c565(page, k, half));
      if (close_to(got, want))
        continue;
      if (close_to(got, BG)) {
        fail_late++;
        fail("late", n, page, k, half, got, want);
        continue;
      }
      int other = 0;
      for (int pp = 0; pp < PAGES && !other; pp++)
        for (int kk = 0; kk < ICONS * 2 && !other; kk++)
          other = close_to(got, x888(c565(pp, kk / 2, kk & 1)));
      if (other) {
        fail_stale++;
        fail("stale", n, page, k, half, got, want);
      } else {
        fail_garbage++;
        fail("garbage", n, page, k, half, got, want);
      }
    }
  }
  if (x_failed != before) {
    frames_failed++;
    if (!dumped++)
      dump(&r, n);
  }
  x_done(&r);
}

/* PK_CHURN: textures of 64, 128 and 256 texels full of noise (incompressible), created, filled,
 * drawn below the icons and released in the same frame, so the GPU driver has freed memory with
 * other contents to hand to the next page's icons */
static IDirect3DTexture9 *churn_src[3];
static unsigned churn_seed = 12345u;

static void churn(void) {
  for (int i = 0; i < cfg_churn; i++) {
    const int s = i % 3, size = 64 << s;
    D3DLOCKED_RECT lr;
    if (!churn_src[s])
      X_CHECK(IDirect3DDevice9_CreateTexture(x_dev, size, size, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &churn_src[s], NULL));
    X_CHECK(IDirect3DTexture9_LockRect(churn_src[s], 0, &lr, NULL, 0));
    for (int y = 0; y < size; y++) {
      DWORD *row = (DWORD *)((BYTE *)lr.pBits + (size_t)y * lr.Pitch);
      for (int x = 0; x < size; x++)
        row[x] = (churn_seed = churn_seed * 1664525u + 1013904223u) | 0xff000000u;
    }
    IDirect3DTexture9_UnlockRect(churn_src[s], 0);
    IDirect3DTexture9 *t = NULL;
    X_CHECK(IDirect3DDevice9_CreateTexture(x_dev, size, size, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &t, NULL));
    X_CHECK(IDirect3DDevice9_UpdateTexture(x_dev, (IDirect3DBaseTexture9 *)churn_src[s], (IDirect3DBaseTexture9 *)t));
    const int x0 = (i * 37) % (cfg_w - 64);
    quad_px((IDirect3DBaseTexture9 *)t, x0, 500, x0 + 64, 564);
    IDirect3DTexture9_Release(t);
  }
}

static void spin(int us) {
  LARGE_INTEGER f, t0, t;
  QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&t0);
  do
    QueryPerformanceCounter(&t);
  while ((t.QuadPart - t0.QuadPart) * 1000000 / f.QuadPart < us);
}

/* a quad over back-buffer pixels x0..x1, y0..y1 (rows down) */
static void quad_px(IDirect3DBaseTexture9 *t, int x0, int y0, int x1, int y1) {
  const float fx0 = -1.0f + 2.0f * (float)x0 / (float)cfg_w, fx1 = -1.0f + 2.0f * (float)x1 / (float)cfg_w;
  const float fy0 = 1.0f - 2.0f * (float)y1 / (float)cfg_h, fy1 = 1.0f - 2.0f * (float)y0 / (float)cfg_h;
  x_draw(t, fx0, fy0, fx1, fy1);
}

static void heavy_band(void) {
  static const float bg[4] = {0.25f, 0.25f, 0.25f, 1.0f};
  IDirect3DDevice9_SetRenderState(x_dev, D3DRS_ALPHABLENDENABLE, FALSE);
  IDirect3DDevice9_SetVertexShader(x_dev, vs_pos);
  IDirect3DDevice9_SetPixelShader(x_dev, ps_heavy);
  IDirect3DDevice9_SetPixelShaderConstantF(x_dev, 0, bg, 1);
  quad_px(NULL, 0, cfg_h - cfg_heavy, cfg_w, cfg_h);
  IDirect3DDevice9_SetVertexShader(x_dev, vs_texquad);
  IDirect3DDevice9_SetPixelShader(x_dev, ps_texquad);
  IDirect3DDevice9_SetRenderState(x_dev, D3DRS_ALPHABLENDENABLE, TRUE);
}

static void frame(int n) {
  if (n > cfg_frames)
    return;
  if (cfg_spin_us > 0)
    spin(cfg_spin_us);
  const int page = page_of(n);
  show_page(page, n);

  /* the loader's turn: the page on screen first, then the others in the cache */
  for (int i = 0; i < PAGES; i++) {
    const int pg = (page + i) % PAGES;
    if (!pages[pg].loaded)
      continue;
    for (int k = 0; k < ICONS; k++) {
      Icon *ic = &pages[pg].icon[k];
      if (ic->countdown > 1)
        ic->countdown--;
      else if (ic->countdown == 1)
        fill_icon(ic, pg, k);
    }
  }

  IDirect3DDevice9_Clear(x_dev, 0, NULL, D3DCLEAR_TARGET, 0xff000000u | BG, 1.0f, 0);
  IDirect3DDevice9_BeginScene(x_dev);
  if (cfg_heavy > 0)
    heavy_band();
  for (int k = 0; k < ICONS; k++)
    quad_px((IDirect3DBaseTexture9 *)pages[page].icon[k].tex, icon_x(k), icon_y(k), icon_x(k) + ICON, icon_y(k) + ICON);
  if (cfg_churn > 0)
    churn();
  IDirect3DDevice9_EndScene(x_dev);

  if (n % cfg_check == 0)
    check(n, page);

  if (n == cfg_frames) {
    printf("PK frames=%d flip=%d load=%d check=%d spin_us=%d kinds=%d size=%dx%d heavy=%d thread=%d: "
           "churn=%d frames_failed=%d garbage=%d stale=%d late=%d magenta=%d\n",
           cfg_frames, cfg_flip, cfg_load, cfg_check, cfg_spin_us, cfg_kinds, cfg_w, cfg_h, cfg_heavy, cfg_thread,
           cfg_churn, frames_failed, fail_garbage, fail_stale, fail_late, fail_magenta);
    x_report("code_icon_churn");
    fflush(stdout);
    ExitProcess(x_failed ? 1 : 0);
  }
}

int main(void) {
  cfg_frames = env_int("PK_FRAMES", 480);
  cfg_flip = env_int("PK_FLIP", 6);
  cfg_load = env_int("PK_LOAD", 2);
  cfg_check = env_int("PK_CHECK", 1);
  cfg_spin_us = env_int("PK_SPIN_US", 0);
  cfg_kinds = env_int("PK_KINDS", 15);
  cfg_w = env_int("PK_W", 1920);
  cfg_h = env_int("PK_H", 1200);
  cfg_heavy = env_int("PK_HEAVY", 0);
  cfg_thread = env_int("PK_THREAD", 0);
  cfg_wrong = env_int("PK_WRONG", 0);
  cfg_churn = env_int("PK_CHURN", 0);
  if (cfg_frames < 1 || cfg_frames > 600) cfg_frames = 480;
  if (cfg_flip < 1) cfg_flip = 6;
  if (cfg_load < 1) cfg_load = 1;
  if (cfg_check < 1) cfg_check = 1;
  if (cfg_w < 640) cfg_w = 640;
  if (cfg_h < 480) cfg_h = 480;
  if (cfg_heavy > cfg_h - 480) cfg_heavy = cfg_h - 480;

  /* x_init's device, with a PK_W x PK_H back buffer in a 960x600 window */
  WNDCLASSA wc = {0};
  wc.lpfnWndProc = x_wnd_proc;
  wc.hInstance = GetModuleHandleA(NULL);
  wc.lpszClassName = "d9mt_icon_churn";
  RegisterClassA(&wc);
  RECT rc = {0, 0, 960, 600};
  AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
  HWND hwnd = CreateWindowA(wc.lpszClassName, wc.lpszClassName, WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT,
                            CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, NULL, NULL, wc.hInstance, NULL);
  IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
  if (!hwnd || !d3d) { printf("FAIL window/d3d\n"); ExitProcess(2); }
  D3DPRESENT_PARAMETERS pp = {0};
  pp.BackBufferWidth = (UINT)cfg_w;
  pp.BackBufferHeight = (UINT)cfg_h;
  pp.BackBufferFormat = D3DFMT_X8R8G8B8;
  pp.BackBufferCount = 1;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.hDeviceWindow = hwnd;
  pp.Windowed = TRUE;
  pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
  X_CHECK(IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                  D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &x_dev));
  X_CHECK(IDirect3DDevice9_CreateVertexShader(x_dev, (const DWORD *)texquad_vs_bytecode, &vs_texquad));
  X_CHECK(IDirect3DDevice9_CreatePixelShader(x_dev, (const DWORD *)texquad_ps_bytecode, &ps_texquad));
  X_CHECK(IDirect3DDevice9_CreateVertexShader(x_dev, (const DWORD *)pos_uv_vs_bytecode, &vs_pos));
  X_CHECK(IDirect3DDevice9_CreatePixelShader(x_dev, (const DWORD *)heavy_ps_bytecode, &ps_heavy));
  static const D3DVERTEXELEMENT9 elems[] = {
      {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
      {0, 12, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
      D3DDECL_END()};
  IDirect3DVertexDeclaration9 *decl = NULL;
  X_CHECK(IDirect3DDevice9_CreateVertexDeclaration(x_dev, elems, &decl));
  static const float mvp[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  IDirect3DDevice9_SetVertexShaderConstantF(x_dev, 0, mvp, 4);
  IDirect3DDevice9_SetVertexDeclaration(x_dev, decl);
  IDirect3DDevice9_SetVertexShader(x_dev, vs_texquad);
  IDirect3DDevice9_SetPixelShader(x_dev, ps_texquad);
  IDirect3DDevice9_SetRenderState(x_dev, D3DRS_CULLMODE, D3DCULL_NONE);
  IDirect3DDevice9_SetRenderState(x_dev, D3DRS_ZENABLE, D3DZB_FALSE);
  IDirect3DDevice9_SetSamplerState(x_dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
  IDirect3DDevice9_SetSamplerState(x_dev, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
  IDirect3DDevice9_SetSamplerState(x_dev, 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
  X_CHECK(IDirect3DDevice9_GetRenderTarget(x_dev, 0, &x_backBuffer));

  X_CHECK(IDirect3DDevice9_CreateTexture(x_dev, ICON, ICON, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &upload_argb, NULL));
  X_CHECK(IDirect3DDevice9_CreateTexture(x_dev, ICON, ICON, 1, 0, D3DFMT_DXT5, D3DPOOL_SYSTEMMEM, &upload_dxt5, NULL));
  IDirect3DDevice9_SetRenderState(x_dev, D3DRS_ALPHABLENDENABLE, TRUE);
  IDirect3DDevice9_SetRenderState(x_dev, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
  IDirect3DDevice9_SetRenderState(x_dev, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);

  if (cfg_thread) {
    InitializeCriticalSection(&job_lock);
    job_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    CreateThread(NULL, 0, loader, NULL, 0, NULL);
  }
  return x_loop(frame);
}
