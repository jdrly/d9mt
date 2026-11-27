/* C4: the flush cap (the app profile's flushCap: the `swtor` app profile, or D9MT_FLUSH_CAP=1).
 * With the cap, weak flush hints no longer submit a command buffer mid-frame; without it they do.
 * Where the submits land must not change any pixel. CF_MSAA (code_flush_hints_msaa.c) draws the
 * scene into a 4x render target and 4x D24S8 surfaces and resolves it into the back buffer with
 * StretchRect.
 *
 * Every frame, in the scene pass (GREY), each weak hint d9mt's front end sends:
 *  H1. 1200 draws of 16x16 cells in the left half (each cell drawn twice, the second colour
 *      kept), which fill CS chunks: chunk-full hints;
 *  H2. another depth-stencil surface set, a cell drawn with it, and the scene's set again;
 *  H3. a dynamic texture locked with DISCARD and drawn, four times, a colour each time;
 *  H4. a dynamic vertex buffer locked with DISCARD and drawn, four times, at a place each time;
 *  then a render target texture drawn BLUE and sampled into a cell (strong hints), and an event
 *  query polled with D3DGETDATA_FLUSH (gives up after 5 s).
 * Frame 1 reads the back buffer back and prints a FAIL line for each pixel that differs. */
#include "t_common.h"

#ifndef CF_NAME
#define CF_NAME "d9mt_flush_hints"
#endif

#define W 640
#define H 480
#define RED     0xff0000u
#define GREEN   0x00ff00u
#define BLUE    0x0000ffu
#define YELLOW  0xffff00u
#define MAGENTA 0xff00ffu
#define CYAN    0x00ffffu
#define GREY    0x404040u
#define BG      0x181828u

static const unsigned kPalette[5] = { RED, GREEN, BLUE, YELLOW, MAGENTA };
static const unsigned kDyn[4] = { RED, GREEN, YELLOW, CYAN };

static IDirect3DSurface9 *backBuffer, *autoDs, *sceneRt, *sceneDs, *ds2, *sys, *rttS;
static IDirect3DTexture9 *dynTex, *rttTex;
static IDirect3DVertexBuffer9 *dynVb;
static IDirect3DQuery9 *event;
static int failed;

static void rs(D3DRENDERSTATETYPE s, DWORD v) { IDirect3DDevice9_SetRenderState(t_dev, s, v); }

static void colour(unsigned rgb) {
  t_color((float)((rgb >> 16) & 255) / 255.0f, (float)((rgb >> 8) & 255) / 255.0f, (float)(rgb & 255) / 255.0f, 1.0f);
}

static float cx(int x) { return (float)x / (W / 2) - 1.0f; }
static float cy(int y) { return 1.0f - (float)y / (H / 2); }

/* a cell of the back buffer, in pixels */
static void cell(int x, int y, int w, int h) {
  t_quad(cx(x), cy(y + h), cx(x + w), cy(y), 0.5f);
}

static unsigned gridColour(int i) { return kPalette[(i * 37 + 11) % 5]; }

static void frame(int n) {
  IDirect3DDevice9_BeginScene(t_dev);
  IDirect3DDevice9_SetVertexShader(t_dev, t_vs_pos);
  IDirect3DDevice9_SetPixelShader(t_dev, t_ps_color);
  IDirect3DDevice9_SetVertexDeclaration(t_dev, t_decl);
  IDirect3DDevice9_SetStreamSource(t_dev, 0, NULL, 0, 0);
  IDirect3DDevice9_SetTexture(t_dev, 0, NULL);
  rs(D3DRS_ZENABLE, D3DZB_TRUE);
  rs(D3DRS_ZWRITEENABLE, FALSE);
  rs(D3DRS_ZFUNC, D3DCMP_ALWAYS);
  rs(D3DRS_STENCILENABLE, FALSE);
  rs(D3DRS_CULLMODE, D3DCULL_NONE);

  IDirect3DDevice9_SetRenderTarget(t_dev, 0, sceneRt);
  IDirect3DDevice9_SetDepthStencilSurface(t_dev, sceneDs);
  IDirect3DDevice9_Clear(t_dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0xff000000u | BG, 1.0f, 0);
  colour(GREY);
  t_quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.5f);

  /* H1: chunk-full hints */
  for (int i = 0; i < 600; i++) {
    const int x = (i % 20) * 16, y = (i / 20) * 16;
    colour(gridColour(i + 1));
    cell(x, y, 16, 16);
    colour(gridColour(i));
    cell(x, y, 16, 16);
  }

  /* H2: another depth-stencil surface and back */
  IDirect3DDevice9_SetDepthStencilSurface(t_dev, ds2);
  colour(CYAN);
  cell(340, 20, 60, 60);
  IDirect3DDevice9_SetDepthStencilSurface(t_dev, sceneDs);
  colour(MAGENTA);
  cell(420, 20, 60, 60);

  /* H3: a dynamic texture updated and drawn four times */
  IDirect3DDevice9_SetPixelShader(t_dev, t_ps_sample);
  for (int k = 0; k < 4; k++) {
    D3DLOCKED_RECT lr;
    T_CHECK(IDirect3DTexture9_LockRect(dynTex, 0, &lr, NULL, D3DLOCK_DISCARD));
    for (int y = 0; y < 4; y++)
      for (int x = 0; x < 4; x++)
        ((DWORD *)((char *)lr.pBits + y * lr.Pitch))[x] = 0xff000000u | kDyn[k];
    T_CHECK(IDirect3DTexture9_UnlockRect(dynTex, 0));
    IDirect3DDevice9_SetTexture(t_dev, 0, (IDirect3DBaseTexture9 *)dynTex);
    cell(340 + k * 70, 100, 60, 60);
  }
  IDirect3DDevice9_SetTexture(t_dev, 0, NULL);
  IDirect3DDevice9_SetPixelShader(t_dev, t_ps_color);

  /* H4: a dynamic vertex buffer locked and drawn four times */
  for (int k = 0; k < 4; k++) {
    struct TVertex *v;
    const float x0 = cx(340 + k * 70), x1 = cx(400 + k * 70), y0 = cy(240), y1 = cy(180);
    T_CHECK(IDirect3DVertexBuffer9_Lock(dynVb, 0, 4 * sizeof(struct TVertex), (void **)&v, D3DLOCK_DISCARD));
    v[0] = (struct TVertex){ x0, y1, 0.5f, 0.0f, 0.0f };
    v[1] = (struct TVertex){ x1, y1, 0.5f, 1.0f, 0.0f };
    v[2] = (struct TVertex){ x0, y0, 0.5f, 0.0f, 1.0f };
    v[3] = (struct TVertex){ x1, y0, 0.5f, 1.0f, 1.0f };
    T_CHECK(IDirect3DVertexBuffer9_Unlock(dynVb));
    colour(kDyn[3 - k]);
    IDirect3DDevice9_SetStreamSource(t_dev, 0, dynVb, 0, sizeof(struct TVertex));
    T_CHECK(IDirect3DDevice9_DrawPrimitive(t_dev, D3DPT_TRIANGLESTRIP, 0, 2));
  }
  IDirect3DDevice9_SetStreamSource(t_dev, 0, NULL, 0, 0);

  /* a render target texture drawn and sampled (strong hints) */
  IDirect3DDevice9_SetRenderTarget(t_dev, 0, rttS);
  IDirect3DDevice9_SetDepthStencilSurface(t_dev, NULL);
  colour(BLUE);
  t_quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.5f);
  IDirect3DDevice9_SetRenderTarget(t_dev, 0, sceneRt);
  IDirect3DDevice9_SetDepthStencilSurface(t_dev, sceneDs);
  IDirect3DDevice9_SetPixelShader(t_dev, t_ps_sample);
  IDirect3DDevice9_SetTexture(t_dev, 0, (IDirect3DBaseTexture9 *)rttTex);
  cell(340, 260, 60, 60);
  IDirect3DDevice9_SetTexture(t_dev, 0, NULL);
  IDirect3DDevice9_SetPixelShader(t_dev, t_ps_color);

  /* an event query polled with FLUSH */
  T_CHECK(IDirect3DQuery9_Issue(event, D3DISSUE_END));
  {
    const DWORD start = GetTickCount();
    HRESULT hr;
    while ((hr = IDirect3DQuery9_GetData(event, NULL, 0, D3DGETDATA_FLUSH)) == S_FALSE) {
      if (GetTickCount() - start > 5000u) {
        printf("FAIL event query: not done after 5 s of GetData(FLUSH)\n");
        failed++;
        break;
      }
    }
    if (FAILED(hr)) { printf("FAIL event query: GetData hr=%08lx\n", (unsigned long)hr); failed++; }
  }
  IDirect3DDevice9_EndScene(t_dev);

  if (sceneRt != backBuffer)
    T_CHECK(IDirect3DDevice9_StretchRect(t_dev, sceneRt, NULL, backBuffer, NULL, D3DTEXF_NONE));

  if (n == 1) {
    struct Probe { int x, y; unsigned rgb; const char *what; } px[24];
    int np = 0;
    static const int gridProbes[] = { 0, 1, 19, 20, 299, 300, 451, 599 };
    char what[16][48];
    for (int g = 0; g < 8; g++) {
      const int i = gridProbes[g];
      snprintf(what[g], sizeof(what[g]), "H1: grid cell %d", i);
      px[np++] = (struct Probe){ (i % 20) * 16 + 8, (i / 20) * 16 + 8, gridColour(i), what[g] };
    }
    px[np++] = (struct Probe){ 370,  50, CYAN,    "H2: drawn with the other depth-stencil" };
    px[np++] = (struct Probe){ 450,  50, MAGENTA, "H2: drawn after the scene's again" };
    for (int k = 0; k < 4; k++) {
      snprintf(what[8 + k], sizeof(what[8 + k]), "H3: texture update %d", k);
      px[np++] = (struct Probe){ 370 + k * 70, 130, kDyn[k], what[8 + k] };
    }
    for (int k = 0; k < 4; k++) {
      snprintf(what[12 + k], sizeof(what[12 + k]), "H4: vertex buffer lock %d", k);
      px[np++] = (struct Probe){ 370 + k * 70, 210, kDyn[3 - k], what[12 + k] };
    }
    px[np++] = (struct Probe){ 370, 290, BLUE, "the render target texture" };
    px[np++] = (struct Probe){ 560, 400, GREY, "the scene away from the cells" };

    D3DLOCKED_RECT lr;
    T_CHECK(IDirect3DDevice9_GetRenderTargetData(t_dev, backBuffer, sys));
    T_CHECK(IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY));
    for (int i = 0; i < np; i++) {
      const unsigned got = *(const DWORD *)((const char *)lr.pBits + (size_t)px[i].y * lr.Pitch + px[i].x * 4) & 0xffffffu;
      printf("CF pixel %d,%d %06x (want %06x) %s\n", px[i].x, px[i].y, got, px[i].rgb, px[i].what);
      if (got != px[i].rgb) { printf("FAIL pixel %d,%d: %06x, want %06x (%s)\n", px[i].x, px[i].y, got, px[i].rgb, px[i].what); failed++; }
    }
    IDirect3DSurface9_UnlockRect(sys);
    printf("CF %s: %d failed\n", CF_NAME, failed);
    fflush(stdout);
  }
}

int main(void) {
  t_init(CF_NAME, W, H);
  T_CHECK(IDirect3DDevice9_GetBackBuffer(t_dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer));
  T_CHECK(IDirect3DDevice9_GetDepthStencilSurface(t_dev, &autoDs));
#ifdef CF_MSAA
  T_CHECK(IDirect3DDevice9_CreateRenderTarget(t_dev, W, H, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_4_SAMPLES, 0, FALSE, &sceneRt, NULL));
  T_CHECK(IDirect3DDevice9_CreateDepthStencilSurface(t_dev, W, H, D3DFMT_D24S8, D3DMULTISAMPLE_4_SAMPLES, 0, FALSE, &sceneDs, NULL));
  T_CHECK(IDirect3DDevice9_CreateDepthStencilSurface(t_dev, W, H, D3DFMT_D24S8, D3DMULTISAMPLE_4_SAMPLES, 0, FALSE, &ds2, NULL));
#else
  sceneRt = backBuffer;
  sceneDs = autoDs;
  T_CHECK(IDirect3DDevice9_CreateDepthStencilSurface(t_dev, W, H, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, FALSE, &ds2, NULL));
#endif
  T_CHECK(IDirect3DDevice9_CreateTexture(t_dev, 4, 4, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &dynTex, NULL));
  T_CHECK(IDirect3DDevice9_CreateTexture(t_dev, 64, 64, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &rttTex, NULL));
  rttS = t_level0(rttTex);
  T_CHECK(IDirect3DDevice9_CreateVertexBuffer(t_dev, 4 * sizeof(struct TVertex), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &dynVb, NULL));
  T_CHECK(IDirect3DDevice9_CreateQuery(t_dev, D3DQUERYTYPE_EVENT, &event));
  T_CHECK(IDirect3DDevice9_CreateOffscreenPlainSurface(t_dev, W, H, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, NULL));
  return t_loop(frame);
}
