/* C2: a pass's targets switched for other targets that are only cleared, then switched back
 * ("a visit"). With the app profile's keepPassOverClearVisit (the `swtor` app profile, or
 * D9MT_KEEP_PASS_OVER_CLEAR_VISIT=1) d9mt keeps the first pass open across such a visit;
 * otherwise it ends the pass, runs the visit, and opens the pass again. Both must give the same
 * pixels. CV_MSAA (code_clear_visit_msaa.c) draws the scene into a 4x render target and a 4x
 * D24S8 surface and resolves it into the back buffer with StretchRect.
 *
 * Every frame: six 128x128 targets S1..S6 (each with a D24S8 surface) are drawn YELLOW, each in a
 * pass of its own. Then the scene pass (GREY), and in it:
 *  V1. a visit that clears S1 (BLUE, depth, stencil); back: a RED cell;
 *  V2. a cell that samples S2 (YELLOW); S2 unbound; a visit that clears S2 (BLUE); back: S2 bound
 *      again, a cell that samples it (BLUE). The first cell must stay YELLOW: its draw comes
 *      before the clear;
 *  V3. as V2 with S3 still bound for sampling when the visit ends;
 *  V4. a visit that clears S4 (BLUE) and draws MAGENTA in its middle; back: a RED cell;
 *  V5. a visit that clears S5 (BLUE), then another target (S6's surface pair is not used: a
 *      separate 128x128 target C, nothing drawn), then back;
 *  V6. a visit to S6 with no clear and no draw;
 *  then S1, S4, S5 and S6 sampled into cells: BLUE, MAGENTA in a BLUE frame, BLUE, YELLOW.
 * Frame 1 reads the back buffer back and prints a FAIL line for each pixel that differs. */
#include "t_common.h"

#ifndef CV_NAME
#define CV_NAME "d9mt_clear_visit"
#endif

#define W 640
#define H 480
#define SZ 128
#define RED     0xff0000u
#define BLUE    0x0000ffu
#define YELLOW  0xffff00u
#define MAGENTA 0xff00ffu
#define GREY    0x404040u
#define BG      0x181828u

static IDirect3DSurface9 *backBuffer, *autoDs, *sceneRt, *sceneDs, *sys, *dsS, *cS;
static IDirect3DTexture9 *tex[7], *cTex;
static IDirect3DSurface9 *surf[7];
static int failed;

static void rs(D3DRENDERSTATETYPE s, DWORD v) { IDirect3DDevice9_SetRenderState(t_dev, s, v); }

static void colour(unsigned rgb) {
  t_color((float)((rgb >> 16) & 255) / 255.0f, (float)((rgb >> 8) & 255) / 255.0f, (float)(rgb & 255) / 255.0f, 1.0f);
}

/* a cell of the back buffer, in pixels */
static void cell(int x, int y, int w, int h) {
  t_quad((float)x / (W / 2) - 1.0f, 1.0f - (float)(y + h) / (H / 2),
         (float)(x + w) / (W / 2) - 1.0f, 1.0f - (float)y / (H / 2), 0.5f);
}

static void scene(void) {
  IDirect3DDevice9_SetRenderTarget(t_dev, 0, sceneRt);
  IDirect3DDevice9_SetDepthStencilSurface(t_dev, sceneDs);
}

static void visit(int k) {
  IDirect3DDevice9_SetRenderTarget(t_dev, 0, surf[k]);
  IDirect3DDevice9_SetDepthStencilSurface(t_dev, dsS);
}

static void colourCell(unsigned rgb, int x, int y) {
  IDirect3DDevice9_SetPixelShader(t_dev, t_ps_color);
  colour(rgb);
  cell(x, y, 80, 80);
}

static void sampleCell(int k, int x, int y) {
  IDirect3DDevice9_SetPixelShader(t_dev, t_ps_sample);
  IDirect3DDevice9_SetTexture(t_dev, 0, (IDirect3DBaseTexture9 *)tex[k]);
  cell(x, y, 80, 80);
}

static void unbind(void) {
  IDirect3DDevice9_SetTexture(t_dev, 0, NULL);
  IDirect3DDevice9_SetPixelShader(t_dev, t_ps_color);
}

static void frame(int n) {
  IDirect3DDevice9_BeginScene(t_dev);
  IDirect3DDevice9_SetVertexShader(t_dev, t_vs_pos);
  IDirect3DDevice9_SetPixelShader(t_dev, t_ps_color);
  rs(D3DRS_ZENABLE, D3DZB_TRUE);
  rs(D3DRS_ZWRITEENABLE, FALSE);
  rs(D3DRS_ZFUNC, D3DCMP_ALWAYS);
  rs(D3DRS_STENCILENABLE, FALSE);
  rs(D3DRS_CULLMODE, D3DCULL_NONE);
  IDirect3DDevice9_SetTexture(t_dev, 0, NULL);

  for (int k = 1; k <= 6; k++) {
    visit(k);
    colour(YELLOW);
    t_quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.5f);
  }

  scene();
  IDirect3DDevice9_Clear(t_dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0xff000000u | BG, 1.0f, 0);
  colour(GREY);
  t_quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.5f);

  /* V1 */
  visit(1);
  IDirect3DDevice9_Clear(t_dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0xff000000u | BLUE, 1.0f, 0);
  scene();
  colourCell(RED, 20, 20);

  /* V2 */
  sampleCell(2, 120, 20);
  unbind();
  visit(2);
  IDirect3DDevice9_Clear(t_dev, 0, NULL, D3DCLEAR_TARGET, 0xff000000u | BLUE, 1.0f, 0);
  scene();
  sampleCell(2, 220, 20);
  unbind();

  /* V3 */
  sampleCell(3, 320, 20);
  visit(3);
  IDirect3DDevice9_Clear(t_dev, 0, NULL, D3DCLEAR_TARGET, 0xff000000u | BLUE, 1.0f, 0);
  scene();
  sampleCell(3, 420, 20);
  unbind();

  /* V4 */
  visit(4);
  IDirect3DDevice9_Clear(t_dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000u | BLUE, 1.0f, 0);
  colour(MAGENTA);
  t_quad(-0.5f, -0.5f, 0.5f, 0.5f, 0.5f);
  scene();
  colourCell(RED, 520, 20);

  /* V5 */
  visit(5);
  IDirect3DDevice9_Clear(t_dev, 0, NULL, D3DCLEAR_TARGET, 0xff000000u | BLUE, 1.0f, 0);
  IDirect3DDevice9_SetRenderTarget(t_dev, 0, cS);
  IDirect3DDevice9_Clear(t_dev, 0, NULL, D3DCLEAR_TARGET, 0xff000000u | RED, 1.0f, 0);
  scene();
  colourCell(RED, 20, 120);

  /* V6 */
  visit(6);
  scene();
  colourCell(RED, 120, 120);

  /* the visited targets */
  sampleCell(1, 20, 240);
  sampleCell(4, 120, 240);
  sampleCell(5, 220, 240);
  sampleCell(6, 320, 240);
  IDirect3DDevice9_SetPixelShader(t_dev, t_ps_sample);
  IDirect3DDevice9_SetTexture(t_dev, 0, (IDirect3DBaseTexture9 *)cTex);
  cell(420, 240, 80, 80);
  unbind();
  IDirect3DDevice9_EndScene(t_dev);

  if (sceneRt != backBuffer)
    T_CHECK(IDirect3DDevice9_StretchRect(t_dev, sceneRt, NULL, backBuffer, NULL, D3DTEXF_NONE));

  if (n == 1) {
    static const struct { int x, y; unsigned rgb; const char *what; } px[] = {
      {  60,  60, RED,     "V1: the scene after a visit that cleared S1" },
      { 160,  60, YELLOW,  "V2: S2 sampled before its clear" },
      { 260,  60, BLUE,    "V2: S2 sampled after its clear" },
      { 360,  60, YELLOW,  "V3: S3 sampled before its clear" },
      { 460,  60, BLUE,    "V3: S3 sampled after its clear, bound throughout" },
      { 560,  60, RED,     "V4: the scene after a visit that drew" },
      {  60, 160, RED,     "V5: the scene after a visit and another target" },
      { 160, 160, RED,     "V6: the scene after a visit without a clear" },
      { 300, 300, GREY,    "the scene between cells" },
      {  60, 280, BLUE,    "S1 cleared by its visit" },
      { 160, 280, MAGENTA, "S4: drawn in its visit" },
      { 125, 245, BLUE,    "S4: cleared by its visit" },
      { 260, 280, BLUE,    "S5 cleared by its visit" },
      { 360, 280, YELLOW,  "S6 untouched by its visit" },
      { 460, 280, RED,     "C cleared after S5's visit" },
    };
    D3DLOCKED_RECT lr;
    T_CHECK(IDirect3DDevice9_GetRenderTargetData(t_dev, backBuffer, sys));
    T_CHECK(IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY));
    for (size_t i = 0; i < sizeof(px) / sizeof(px[0]); i++) {
      const unsigned got = *(const DWORD *)((const char *)lr.pBits + (size_t)px[i].y * lr.Pitch + px[i].x * 4) & 0xffffffu;
      printf("CV pixel %d,%d %06x (want %06x) %s\n", px[i].x, px[i].y, got, px[i].rgb, px[i].what);
      if (got != px[i].rgb) { printf("FAIL pixel %d,%d: %06x, want %06x (%s)\n", px[i].x, px[i].y, got, px[i].rgb, px[i].what); failed++; }
    }
    IDirect3DSurface9_UnlockRect(sys);
    printf("CV %s: %d failed\n", CV_NAME, failed);
    fflush(stdout);
  }
}

int main(void) {
  t_init(CV_NAME, W, H);
  T_CHECK(IDirect3DDevice9_GetBackBuffer(t_dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer));
  T_CHECK(IDirect3DDevice9_GetDepthStencilSurface(t_dev, &autoDs));
#ifdef CV_MSAA
  T_CHECK(IDirect3DDevice9_CreateRenderTarget(t_dev, W, H, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_4_SAMPLES, 0, FALSE, &sceneRt, NULL));
  T_CHECK(IDirect3DDevice9_CreateDepthStencilSurface(t_dev, W, H, D3DFMT_D24S8, D3DMULTISAMPLE_4_SAMPLES, 0, FALSE, &sceneDs, NULL));
#else
  sceneRt = backBuffer;
  sceneDs = autoDs;
#endif
  for (int k = 1; k <= 6; k++) {
    T_CHECK(IDirect3DDevice9_CreateTexture(t_dev, SZ, SZ, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &tex[k], NULL));
    surf[k] = t_level0(tex[k]);
  }
  T_CHECK(IDirect3DDevice9_CreateTexture(t_dev, SZ, SZ, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &cTex, NULL));
  cS = t_level0(cTex);
  T_CHECK(IDirect3DDevice9_CreateDepthStencilSurface(t_dev, SZ, SZ, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, FALSE, &dsS, NULL));
  T_CHECK(IDirect3DDevice9_CreateOffscreenPlainSurface(t_dev, W, H, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, NULL));
  return t_loop(frame);
}
