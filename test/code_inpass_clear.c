/* #129: a part-surface depth/stencil clear between draws of one pass: a viewport around a
 * region, Clear(STENCIL) with no rectangle (D3D9 clips it to the viewport), a mask drawn with
 * INCR, then draws tested against the mask. With the app profile's inpassClear (the `swtor` app
 * profile, or D9MT_INPASS_CLEAR=1) d9mt draws such a clear inside the pass; otherwise, and for
 * the cases it excludes, it ends the pass and clears in a pass of its own. Both must give the
 * same pixels. IPC_MSAA (code_inpass_clear_msaa.c) draws the scene into a 4x render target and a
 * 4x D24S8 surface; its pass is still open when StretchRect resolves it into the back buffer.
 *
 * Every frame:
 *  D. two render targets at once, sRGB writes on, a D24S8 surface: RED, a stencil clear in a
 *     viewport (40,20 80x40), GREEN where the stencil is the cleared value;
 *  E. an INTZ texture as the depth-stencil and on sampler 0 at once, depth writes off: a draw
 *     that samples it, a depth clear in the same viewport (0.25), the same draw again with the
 *     binding held (a depth texture shaders read: d9mt keeps the clear out of the pass, the draw
 *     after it must see 0.25);
 *  then one pass on the scene's targets:
 *  A. stencil 3 everywhere (no colour); viewport A (64,48 192x144), Clear(STENCIL, 1); a mask
 *     over A's left half with INCR (2); full viewport; RED where the stencil is 2, GREEN where 1,
 *     BLUE where 3;
 *  C. occlusion: a 64x64 quad counted alone, then counted with a stencil clear in a viewport
 *     between its draw and the query's end. The counts must be equal: the clear counts nothing;
 *  B. the draw state a clear mustn't leave behind: scissor test on over the right half, stencil
 *     reference 4, culling CW, ZFUNC LESS; a primer with that state that draws nothing; viewport
 *     B (352,240 192x144), Clear(ZBUFFER | STENCIL, 0.25, 5); the full viewport again, and with the
 *     primer's state unchanged: a YELLOW quad at z 0.4 (the right half outside B, where the depth
 *     is 1), a RED clockwise quad (culled), and a MAGENTA quad with stencil EQUAL to the reference
 *     4 (no pixel holds 4: nothing). A clear that left its own viewport, scissor, culling or
 *     stencil reference to the next draw changes these pixels;
 *  then D's two targets and E's target, a quarter size each, along the bottom.
 * Frame 1 reads the back buffer back and prints a FAIL line for each pixel or count that
 * differs from these. */
#include "t_common.h"

#ifndef IPC_NAME
#define IPC_NAME "d9mt_inpass_clear"
#endif

#define W 640
#define H 480
#define RED     0xff0000u
#define GREEN   0x00ff00u
#define BLUE    0x0000ffu
#define YELLOW  0xffff00u
#define MAGENTA 0xff00ffu
#define BG      0x181828u

static IDirect3DSurface9 *backBuffer, *autoDs, *sceneRt, *sceneDs, *sys;
static IDirect3DTexture9 *rtM0, *rtM1, *rtE, *intz;
static IDirect3DSurface9 *rtM0S, *rtM1S, *rtES, *intzS, *dsM;
static IDirect3DQuery9 *occA, *occB;
static int failed;

static void rs(D3DRENDERSTATETYPE s, DWORD v) { IDirect3DDevice9_SetRenderState(t_dev, s, v); }

static void viewport(DWORD x, DWORD y, DWORD w, DWORD h) {
  D3DVIEWPORT9 vp = { x, y, w, h, 0.0f, 1.0f };
  IDirect3DDevice9_SetViewport(t_dev, &vp);
}

static void colour(unsigned rgb) {
  t_color((float)((rgb >> 16) & 255) / 255.0f, (float)((rgb >> 8) & 255) / 255.0f, (float)(rgb & 255) / 255.0f, 1.0f);
}

static void stencil(DWORD func, DWORD ref, DWORD pass) {
  rs(D3DRS_STENCILENABLE, TRUE);
  rs(D3DRS_STENCILFUNC, func);
  rs(D3DRS_STENCILREF, ref);
  rs(D3DRS_STENCILPASS, pass);
  rs(D3DRS_STENCILFAIL, D3DSTENCILOP_KEEP);
  rs(D3DRS_STENCILZFAIL, D3DSTENCILOP_KEEP);
  rs(D3DRS_STENCILMASK, 0xff);
  rs(D3DRS_STENCILWRITEMASK, 0xff);
}

/* t_quad with the other winding: counter-clockwise on screen */
static void quadCcw(float x0, float y0, float x1, float y1, float z) {
  struct TVertex v[4] = {
      {x0, y0, z, 0.0f, 1.0f}, {x1, y0, z, 1.0f, 1.0f},
      {x0, y1, z, 0.0f, 0.0f}, {x1, y1, z, 1.0f, 0.0f},
  };
  T_CHECK(IDirect3DDevice9_DrawPrimitiveUP(t_dev, D3DPT_TRIANGLESTRIP, 2, v, sizeof(v[0])));
}

static DWORD occlusion(IDirect3DQuery9 *q) {
  DWORD n = 0;
  for (int i = 0; i < 5000; i++) {   /* at most about 5 s */
    HRESULT hr = IDirect3DQuery9_GetData(q, &n, sizeof(n), D3DGETDATA_FLUSH);
    if (hr == S_OK) return n;
    if (FAILED(hr)) break;
    Sleep(1);
  }
  printf("FAIL occlusion query: no result\n");
  failed++;
  return ~0u;
}

static void frame(int n) {
  IDirect3DDevice9_BeginScene(t_dev);
  IDirect3DDevice9_SetVertexShader(t_dev, t_vs_pos);
  IDirect3DDevice9_SetPixelShader(t_dev, t_ps_color);
  rs(D3DRS_SCISSORTESTENABLE, FALSE);
  rs(D3DRS_CULLMODE, D3DCULL_NONE);
  rs(D3DRS_STENCILENABLE, FALSE);

  /* D: two targets, sRGB writes */
  IDirect3DDevice9_SetRenderTarget(t_dev, 0, rtM0S);
  IDirect3DDevice9_SetRenderTarget(t_dev, 1, rtM1S);
  IDirect3DDevice9_SetDepthStencilSurface(t_dev, dsM);
  viewport(0, 0, W, H);
  rs(D3DRS_SRGBWRITEENABLE, TRUE);
  rs(D3DRS_ZENABLE, D3DZB_TRUE);
  rs(D3DRS_ZWRITEENABLE, FALSE);
  rs(D3DRS_ZFUNC, D3DCMP_ALWAYS);
  IDirect3DDevice9_Clear(t_dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0xff000000u, 1.0f, 3);
  colour(RED);
  t_quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.5f);
  viewport(40, 20, 80, 40);
  IDirect3DDevice9_Clear(t_dev, 0, NULL, D3DCLEAR_STENCIL, 0, 1.0f, 1);
  viewport(0, 0, W, H);
  stencil(D3DCMP_EQUAL, 1, D3DSTENCILOP_KEEP);
  colour(GREEN);
  t_quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.5f);
  rs(D3DRS_STENCILENABLE, FALSE);
  rs(D3DRS_SRGBWRITEENABLE, FALSE);
  IDirect3DDevice9_SetRenderTarget(t_dev, 1, NULL);

  /* E: an INTZ depth-stencil sampled while it is bound, before and after a part clear of its
   * depth, the binding held across the clear */
  if (intz) {
    IDirect3DDevice9_SetRenderTarget(t_dev, 0, rtES);
    IDirect3DDevice9_SetDepthStencilSurface(t_dev, intzS);
    IDirect3DDevice9_Clear(t_dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000u, 1.0f, 0);
    IDirect3DDevice9_SetPixelShader(t_dev, t_ps_sample);
    IDirect3DDevice9_SetTexture(t_dev, 0, (IDirect3DBaseTexture9 *)intz);
    t_quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.5f);
    viewport(40, 20, 80, 40);
    IDirect3DDevice9_Clear(t_dev, 0, NULL, D3DCLEAR_ZBUFFER, 0, 0.25f, 0);
    viewport(0, 0, W, H);
    t_quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.5f);
    IDirect3DDevice9_SetTexture(t_dev, 0, NULL);
    IDirect3DDevice9_SetPixelShader(t_dev, t_ps_color);
  }

  IDirect3DDevice9_SetRenderTarget(t_dev, 0, sceneRt);
  IDirect3DDevice9_SetDepthStencilSurface(t_dev, sceneDs);
  viewport(0, 0, W, H);
  rs(D3DRS_SCISSORTESTENABLE, FALSE);
  rs(D3DRS_CULLMODE, D3DCULL_NONE);
  IDirect3DDevice9_Clear(t_dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0xff000000u | BG, 1.0f, 0);
  IDirect3DDevice9_SetVertexShader(t_dev, t_vs_pos);
  IDirect3DDevice9_SetPixelShader(t_dev, t_ps_color);
  rs(D3DRS_ZENABLE, D3DZB_TRUE);
  rs(D3DRS_ZWRITEENABLE, FALSE);
  rs(D3DRS_ZFUNC, D3DCMP_ALWAYS);

  /* A: a stencil mask cleared in a viewport */
  rs(D3DRS_COLORWRITEENABLE, 0);
  stencil(D3DCMP_ALWAYS, 3, D3DSTENCILOP_REPLACE);
  t_quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.5f);
  viewport(64, 48, 192, 144);
  IDirect3DDevice9_Clear(t_dev, 0, NULL, D3DCLEAR_STENCIL, 0, 1.0f, 1);
  stencil(D3DCMP_ALWAYS, 0, D3DSTENCILOP_INCR);
  t_quad(-1.0f, -1.0f, 0.0f, 1.0f, 0.5f);
  viewport(0, 0, W, H);
  rs(D3DRS_COLORWRITEENABLE, 0xf);
  stencil(D3DCMP_EQUAL, 2, D3DSTENCILOP_KEEP); colour(RED);   t_quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.5f);
  stencil(D3DCMP_EQUAL, 1, D3DSTENCILOP_KEEP); colour(GREEN); t_quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.5f);
  stencil(D3DCMP_EQUAL, 3, D3DSTENCILOP_KEEP); colour(BLUE);  t_quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.5f);

  /* C: a clear inside an occlusion query counts nothing */
  rs(D3DRS_COLORWRITEENABLE, 0);
  IDirect3DQuery9_Issue(occA, D3DISSUE_BEGIN);
  t_quad(-0.1f, -2.0f / 15.0f, 0.1f, 2.0f / 15.0f, 0.5f);   /* 64x64 pixels */
  IDirect3DQuery9_Issue(occA, D3DISSUE_END);
  IDirect3DQuery9_Issue(occB, D3DISSUE_BEGIN);
  t_quad(-0.1f, -2.0f / 15.0f, 0.1f, 2.0f / 15.0f, 0.5f);
  viewport(400, 20, 100, 100);
  IDirect3DDevice9_Clear(t_dev, 0, NULL, D3DCLEAR_STENCIL, 0, 1.0f, 9);
  viewport(0, 0, W, H);
  IDirect3DQuery9_Issue(occB, D3DISSUE_END);
  rs(D3DRS_COLORWRITEENABLE, 0xf);

  /* B: a depth and stencil clear between draws of one state. A primer draws nothing with the
   * state the draws after the clear use; the clear changes only the viewport, back before them. */
  RECT right = { W / 2, 0, W, H };
  IDirect3DDevice9_SetScissorRect(t_dev, &right);
  rs(D3DRS_SCISSORTESTENABLE, TRUE);
  rs(D3DRS_STENCILENABLE, FALSE);
  rs(D3DRS_STENCILREF, 4);
  rs(D3DRS_CULLMODE, D3DCULL_CW);
  rs(D3DRS_ZFUNC, D3DCMP_LESS);
  colour(YELLOW);
  quadCcw(-1.0f, -1.0f, 1.0f, 1.0f, 1.0f);          /* the primer: z 1 is not LESS than 1 anywhere */
  viewport(352, 240, 192, 144);
  IDirect3DDevice9_Clear(t_dev, 0, NULL, D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0, 0.25f, 5);
  viewport(0, 0, W, H);
  quadCcw(-1.0f, -1.0f, 1.0f, 1.0f, 0.4f);          /* YELLOW: the right half outside B */
  colour(RED);
  t_quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.1f);           /* t_quad is clockwise on screen: culled */
  rs(D3DRS_ZFUNC, D3DCMP_ALWAYS);
  rs(D3DRS_STENCILENABLE, TRUE);
  rs(D3DRS_STENCILFUNC, D3DCMP_EQUAL);
  rs(D3DRS_STENCILPASS, D3DSTENCILOP_KEEP);
  colour(MAGENTA);
  quadCcw(-1.0f, -1.0f, 1.0f, 1.0f, 0.4f);          /* stencil EQUAL to the reference 4: nowhere */
  rs(D3DRS_STENCILENABLE, FALSE);
  rs(D3DRS_SCISSORTESTENABLE, FALSE);
  rs(D3DRS_CULLMODE, D3DCULL_NONE);

  /* D's targets and E's target, a quarter size each, along the bottom (point sampling) */
  IDirect3DDevice9_SetPixelShader(t_dev, t_ps_sample);
  IDirect3DDevice9_SetTexture(t_dev, 0, (IDirect3DBaseTexture9 *)rtM0);
  t_quad(-1.0f, -1.0f, -0.5f, -2.0f / 3.0f, 0.5f);
  IDirect3DDevice9_SetTexture(t_dev, 0, (IDirect3DBaseTexture9 *)rtM1);
  t_quad(0.0f, -1.0f, 0.5f, -2.0f / 3.0f, 0.5f);
  if (intz) {
    IDirect3DDevice9_SetTexture(t_dev, 0, (IDirect3DBaseTexture9 *)rtE);
    t_quad(-0.5f, -1.0f, 0.0f, -2.0f / 3.0f, 0.5f);
  }
  IDirect3DDevice9_SetTexture(t_dev, 0, NULL);
  IDirect3DDevice9_SetPixelShader(t_dev, t_ps_color);
  IDirect3DDevice9_EndScene(t_dev);

  if (sceneRt != backBuffer)
    T_CHECK(IDirect3DDevice9_StretchRect(t_dev, sceneRt, NULL, backBuffer, NULL, D3DTEXF_NONE));

  if (n == 1) {
    const DWORD a = occlusion(occA), b = occlusion(occB);
    printf("IPC occlusion alone=%lu with-clear=%lu\n", (unsigned long)a, (unsigned long)b);
    if (a != b || a == 0u) { printf("FAIL occlusion: %lu alone, %lu with a clear inside the query\n", (unsigned long)a, (unsigned long)b); failed++; }

    static const struct { int x, y; unsigned rgb; const char *what; } px[] = {
      { 100, 100, RED,    "A: mask (stencil 2)" },
      { 200, 100, GREEN,  "A: cleared in the viewport (stencil 1)" },
      {  30, 300, BLUE,   "A: outside the viewport (stencil 3)" },
      { 250, 100, GREEN,  "A: the viewport's right edge" },
      { 420, 100, YELLOW, "B: right half outside B (scissor kept, depth 1)" },
      { 448, 300, BLUE,   "B: inside B (depth cleared to 0.25, no magenta: reference kept)" },
      { 200, 350, BLUE,   "B: left half (scissored out)" },
      {  20, 406, GREEN,  "D: two targets, sRGB: inside the cleared viewport" },
      { 100, 440, RED,    "D: outside it" },
      { 340, 440, 0x000000u, "D: the second target, cleared and never written" },
    };
    D3DLOCKED_RECT lr;
    T_CHECK(IDirect3DDevice9_GetRenderTargetData(t_dev, backBuffer, sys));
    T_CHECK(IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY));
    for (size_t i = 0; i < sizeof(px) / sizeof(px[0]); i++) {
      const unsigned got = *(const DWORD *)((const char *)lr.pBits + (size_t)px[i].y * lr.Pitch + px[i].x * 4) & 0xffffffu;
      printf("IPC pixel %d,%d %06x (want %06x) %s\n", px[i].x, px[i].y, got, px[i].rgb, px[i].what);
      if (got != px[i].rgb) { printf("FAIL pixel %d,%d: %06x, want %06x (%s)\n", px[i].x, px[i].y, got, px[i].rgb, px[i].what); failed++; }
    }
    if (intz) {
      static const struct { int x, y, lo, hi; const char *what; } gr[] = {
        { 180, 406, 61, 67, "E: INTZ sampled inside the cleared viewport (0.25)" },
        { 260, 440, 252, 255, "E: outside it (1.0)" },
      };
      for (size_t i = 0; i < sizeof(gr) / sizeof(gr[0]); i++) {
        const unsigned got = *(const DWORD *)((const char *)lr.pBits + (size_t)gr[i].y * lr.Pitch + gr[i].x * 4) & 0xffffffu;
        const int r = (int)((got >> 16) & 255);
        printf("IPC pixel %d,%d %06x (want red %d..%d) %s\n", gr[i].x, gr[i].y, got, gr[i].lo, gr[i].hi, gr[i].what);
        if (r < gr[i].lo || r > gr[i].hi) { printf("FAIL pixel %d,%d: %06x (%s)\n", gr[i].x, gr[i].y, got, gr[i].what); failed++; }
      }
    } else {
      printf("IPC no INTZ: case E skipped\n");
    }
    IDirect3DSurface9_UnlockRect(sys);
    printf("IPC %s: %d failed\n", IPC_NAME, failed);
    fflush(stdout);
  }
}

int main(void) {
  t_init(IPC_NAME, W, H);
  T_CHECK(IDirect3DDevice9_GetBackBuffer(t_dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer));
  T_CHECK(IDirect3DDevice9_GetDepthStencilSurface(t_dev, &autoDs));
#ifdef IPC_MSAA
  T_CHECK(IDirect3DDevice9_CreateRenderTarget(t_dev, W, H, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_4_SAMPLES, 0, FALSE, &sceneRt, NULL));
  T_CHECK(IDirect3DDevice9_CreateDepthStencilSurface(t_dev, W, H, D3DFMT_D24S8, D3DMULTISAMPLE_4_SAMPLES, 0, FALSE, &sceneDs, NULL));
#else
  sceneRt = backBuffer;
  sceneDs = autoDs;
#endif
  T_CHECK(IDirect3DDevice9_CreateOffscreenPlainSurface(t_dev, W, H, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, NULL));
  T_CHECK(IDirect3DDevice9_CreateTexture(t_dev, W, H, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &rtM0, NULL));
  T_CHECK(IDirect3DDevice9_CreateTexture(t_dev, W, H, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &rtM1, NULL));
  T_CHECK(IDirect3DDevice9_CreateTexture(t_dev, W, H, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &rtE, NULL));
  rtM0S = t_level0(rtM0); rtM1S = t_level0(rtM1); rtES = t_level0(rtE);
  T_CHECK(IDirect3DDevice9_CreateDepthStencilSurface(t_dev, W, H, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, FALSE, &dsM, NULL));
  if (FAILED(IDirect3DDevice9_CreateTexture(t_dev, W, H, 1, D3DUSAGE_DEPTHSTENCIL, (D3DFORMAT)MAKEFOURCC('I', 'N', 'T', 'Z'),
                                            D3DPOOL_DEFAULT, &intz, NULL)))
    intz = NULL;
  if (intz)
    intzS = t_level0(intz);
  T_CHECK(IDirect3DDevice9_CreateQuery(t_dev, D3DQUERYTYPE_OCCLUSION, &occA));
  T_CHECK(IDirect3DDevice9_CreateQuery(t_dev, D3DQUERYTYPE_OCCLUSION, &occB));
  return t_loop(frame);
}
