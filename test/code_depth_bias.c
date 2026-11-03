/* code_depth_bias: the constant depth bias in the vertex shader (#163, D9MT_DEPTH_BIAS_VS).
 *
 * D3D9's D3DRS_DEPTHBIAS is absolute (depth-buffer units). D24 is Depth32Float on Apple GPUs,
 * where Metal's constant bias scales with the depth's exponent: the hardware path gives
 * B * 2^(e+1), right only for z in [0.5,1). With D9MT_DEPTH_BIAS_VS=1 a hooked vertex shader adds
 * k * w to clip-space z (k = B / the viewport's depth range), writes the original near/far planes
 * as clip distances 6/7, and d9mt clamps depth to the viewport range (D3D11.3 15.6, d9mt's policy).
 *
 * One frame renders every cell into a 2048x1536 offscreen target (16 x 16 cells of 128x96, each
 * its own viewport), first 1x, then 4x MSAA (resolved by StretchRect) when the device has it, and
 * prints one line per checked sample:
 *
 *   D9MT-DEPTHBIAS x<1|4> <cell> <sample> want=<w> got=<g> px=<rrggbb> legacy=<l> ok|FAIL|cross|info
 *
 * then "D9MT-DEPTHBIAS-SUMMARY x<n> gating ok=<n> fail=<n> cross=<n> fixture=<n>". Classes:
 * R red (the probe passed), G green (the reference held), B blue (nothing drawn), M a mix; px is
 * the raw resolved pixel. Wants: R/G/B exact; r: red present (4x line/wireframe partial
 * coverage); C: the resolved bytes equal the same draw with B = 0 (the cell's control, coverage);
 * c: the same comparison, informational (a 1x pixel centre exactly on a clipping edge); S:
 * cross-run, the whole line (px included) must equal the D9MT_DEPTH_BIAS_VS=0 run's (paths that
 * keep the hardware bias); I: informational. "legacy" is the B*2^(e+1) hardware model
 * (informational). D3D9 pixel centres are at integer coordinates (d9mt offsets the viewport by
 * half a pixel), so pixel (px, py) is sampled at (px, py). A depth sample's want comes from the oracle
 *
 *   final = clamp(d_win(p) + B * sum(mu_i a_i) + S * m_e, zn, zf)
 *
 * (mu: screen-space barycentrics, a_i: the vertex is in the shader's domain |z| <= 2^126,
 * 2^-100 <= |w| <= 2^126, m_e: the emitted plane's max(|dz/dx|, |dz/dy|) per pixel, [zn, zf]:
 * d9mt's Metal viewport depth range) compared with the reference by the cell's ZFUNC. Every 4x
 * depth sample (a reference and a want of R, G, r or the oracle, explicit wants included) must
 * also satisfy |final - Ref| > (|dz/dx| + |dz/dy|) / 2 + 2^-22 for the depth its want predicts,
 * checked at start-up (D9MT-DEPTHBIAS-FIXTURE lines) before any draw: a violation is reported as
 * fixture-FAIL in the 4x pass, not as a driver result. MASK_AUTO cells must find their two
 * covered and two uncovered control samples, or the cell is a fixture failure.
 *
 * Runs (GATES.md): D9MT_DEPTH_BIAS_VS=1 (every gating line ok), D9MT_DEPTH_BIAS_VS unset and the
 * base build (S lines compared across runs; the switch-off run equals base line for line),
 * D9MT_DEPTH_BIAS_VS=1 D9MT_DEPTH_BIAS_TEST_UNHOOK=1 (T2's middle draw takes the unhooked path),
 * and DBT_NO_DEPTHBIAS=1 with D9MT_DEBUG_FILE naming a file that says no_depthbias (rows N).
 * Shader bytes are hand-encoded SM3 (the source is in each array's comment). */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* vs_3_0 | dcl_position v0 | dcl_texcoord0 v1 | dcl_position o0 | dcl_texcoord0 o1 | mov o0, v0 | mov o1, v1 */
static const DWORD k_vs[] = {
  0xfffe0300,
  0x0200001f, 0x80000000, 0x900f0000,
  0x0200001f, 0x80000005, 0x900f0001,
  0x0200001f, 0x80000000, 0xe00f0000,
  0x0200001f, 0x80000005, 0xe00f0001,
  0x02000001, 0xe00f0000, 0x90e40000,
  0x02000001, 0xe00f0001, 0x90e40001,
  0x0000ffff,
};
/* k_vs + dcl_texcoord15 o2 | mov o2, v1: the D9MT_DEPTH_BIAS_TEST_UNHOOK marker */
static const DWORD k_vs_unhook[] = {
  0xfffe0300,
  0x0200001f, 0x80000000, 0x900f0000,
  0x0200001f, 0x80000005, 0x900f0001,
  0x0200001f, 0x80000000, 0xe00f0000,
  0x0200001f, 0x80000005, 0xe00f0001,
  0x0200001f, 0x800f0005, 0xe00f0002,
  0x02000001, 0xe00f0000, 0x90e40000,
  0x02000001, 0xe00f0001, 0x90e40001,
  0x02000001, 0xe00f0002, 0x90e40001,
  0x0000ffff,
};
/* ps_3_0 | mov oC0, c0 */
static const DWORD k_ps_color[] = {
  0xffff0300, 0x02000001, 0x800f0800, 0xa0e40000, 0x0000ffff,
};
/* ps_3_0 | mov oC0, c0 | mov oDepth, c1.x */
static const DWORD k_ps_depth_const[] = {
  0xffff0300, 0x02000001, 0x800f0800, 0xa0e40000,
  0x02000001, 0x90010800, 0xa0000001, 0x0000ffff,
};
/* ps_3_0 | dcl_texcoord0 v0 | mov oC0, c0 | mov oDepth, v0.x */
static const DWORD k_ps_depth_interp[] = {
  0xffff0300, 0x0200001f, 0x80000005, 0x900f0000,
  0x02000001, 0x800f0800, 0xa0e40000,
  0x02000001, 0x90010800, 0x90000000, 0x0000ffff,
};

#define CELL_W 128
#define CELL_H 96
#define GRID 16
#define RT_W (CELL_W * GRID)
#define RT_H (CELL_H * GRID)
#define MAX_CELLS (GRID * GRID)

static const double B = 1.0 / 1024.0;   /* 2^-10 */

typedef struct { float x, y, z, w; float t[4]; } V;
typedef struct { float x, y, z, rhw; DWORD c; } VT;

enum { PS_COLOR = 0, PS_DEPTH_CONST = 1, PS_DEPTH_INTERP = 2 };
enum { VS_MAIN = 0, VS_UNHOOK = 1, VS_FF = 2 };
enum { MASK_NONE = 0, MASK_FULL = 1, MASK_AUTO = 2 };

typedef struct {
  int px, py;
  int hasRef;
  double ref;          /* window depth of the reference quad */
  char want1, want4;   /* 0: the oracle */
  int any3;            /* classify the reddest of rows py-1..py+1 (line rasterization) */
  int mset;            /* the 4x margin uses mfinal/mgx/mgy, not the cell's oracle */
  double mfinal, mgx, mgy;
  int fixtureBad;      /* the 4x margin assertion failed (validate_fixture) */
} Sample;

typedef struct {
  char name[32];
  int prim;            /* D3DPT_* */
  int nv;
  V v[12];
  VT vt[6];            /* VS_FF: screen-space vertices (cell-local; offset when drawn) */
  double bias, slope;
  float minz, maxz;
  DWORD zfunc;
  float dc;
  DWORD fill;
  DWORD clipmask;
  int ps, vs;
  float psDepth;       /* PS_DEPTH_CONST: c1.x */
  int ns;
  Sample s[16];
  int ctrl;            /* the B = 0 control cell, or -1 */
  int mask;            /* MASK_*: compare the cell with its control */
  int exclX0, exclX1;  /* MASK_FULL: excluded columns (the crossing) */
  int legacy;          /* print the hardware model */
  int seq;             /* 1..4: a T sequence (drawn by run_sequences) */
  int isControl;
  int x0, y0;
} Cell;

static IDirect3DDevice9 *dev;
static IDirect3DVertexShader9 *vsMain, *vsUnhook;
static IDirect3DPixelShader9 *psColor, *psDepthConst, *psDepthInterp;
static IDirect3DVertexDeclaration9 *decl;
static IDirect3DSurface9 *rt[2], *ds[2], *ds16[2], *resolved, *sysmem, *backbuffer;
static Cell cells[MAX_CELLS];
static int ncells;
static int unhookActive, noDepthBiasRun;

#define CHECK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { \
    printf("FAIL %s: hr=%08lx (line %d)\n", #x, (unsigned long)hr_, __LINE__); fflush(stdout); ExitProcess(2); } } while (0)

static DWORD f2dw(float f) { DWORD d; memcpy(&d, &f, 4); return d; }
static double ndcx(double px) { return 2.0 * px / CELL_W - 1.0; }
static double ndcy(double py) { return 1.0 - 2.0 * py / CELL_H; }

/* d9mt's Metal viewport depth range for a D3D9 MinZ/MaxZ (SetViewport + BindViewportAndScissor) */
static void eff_range(float minz, float maxz, float *zn, float *zf) {
  float mx = minz < maxz ? maxz : minz + 0.001f;
  float zb = minz >= 0.5f ? 0.0f : 0.001f;
  float a = minz, b = mx > minz + zb ? mx : minz + zb;
  *zn = a < 0.0f ? 0.0f : a > 1.0f ? 1.0f : a;
  *zf = b < 0.0f ? 0.0f : b > 1.0f ? 1.0f : b;
}

/* a clip-space vertex at a cell pixel, NDC depth d, w */
static V vp(double px, double py, double d, double w) {
  V v;
  v.x = (float)(ndcx(px) * w);
  v.y = (float)(ndcy(py) * w);
  v.z = (float)(d * w);
  v.w = (float)w;
  v.t[0] = (float)d; v.t[1] = 0.0f; v.t[2] = 0.0f; v.t[3] = 1.0f;
  return v;
}

static V vraw(float x, float y, float z, float w) {
  V v = { x, y, z, w, { 0.0f, 0.0f, 0.0f, 1.0f } };
  return v;
}

static Cell *add_cell(const char *name) {
  if (ncells >= MAX_CELLS) { printf("FAIL too many cells\n"); ExitProcess(2); }
  Cell *c = &cells[ncells];
  memset(c, 0, sizeof(*c));
  snprintf(c->name, sizeof(c->name), "%s", name);
  c->prim = D3DPT_TRIANGLELIST;
  c->minz = 0.0f; c->maxz = 1.0f;
  c->zfunc = D3DCMP_LESS;
  c->dc = 1.0f;
  c->fill = D3DFILL_SOLID;
  c->ctrl = -1;
  c->x0 = (ncells % GRID) * CELL_W;
  c->y0 = (ncells / GRID) * CELL_H;
  ncells++;
  return c;
}

static void tri(Cell *c, V a, V b, V d) { c->v[c->nv++] = a; c->v[c->nv++] = b; c->v[c->nv++] = d; }

/* a full-cell quad whose NDC depth is f0 at px 0 and f1 at px 128 (linear in x), w = 1 */
static void quad_x(Cell *c, double f0, double f1) {
  V a = vp(0, 0, f0, 1), b = vp(CELL_W, 0, f1, 1), d = vp(0, CELL_H, f0, 1), e = vp(CELL_W, CELL_H, f1, 1);
  tri(c, a, b, d);
  tri(c, d, b, e);
}

static void quad_flat(Cell *c, double ndc) { quad_x(c, ndc, ndc); }

static void sample(Cell *c, int px, int py, int hasRef, double ref, char want1, char want4) {
  if (c->ns >= 16) { printf("FAIL too many samples in %s\n", c->name); ExitProcess(2); }
  Sample *s = &c->s[c->ns++];
  memset(s, 0, sizeof(*s));
  s->px = px; s->py = py; s->hasRef = hasRef; s->ref = ref; s->want1 = want1; s->want4 = want4;
}

/* the depth the last sample's explicit want predicts, for the 4x margin (cells whose geometry the
 * oracle doesn't see: T sequences, F's wireframe edge, N's no_depthbias run) */
static void sample_margin(Cell *c, double final, double gx, double gy) {
  Sample *s = &c->s[c->ns - 1];
  s->mset = 1; s->mfinal = final; s->mgx = gx; s->mgy = gy;
}

/* a B = 0 copy of cell i (same geometry, no samples): the coverage control */
static void add_control(int i) {
  char name[32];
  snprintf(name, sizeof(name), "%s.ctl", cells[i].name);
  Cell *c = add_cell(name);
  int x0 = c->x0, y0 = c->y0;
  *c = cells[i];
  snprintf(c->name, sizeof(c->name), "%s", name);
  c->x0 = x0; c->y0 = y0;
  c->bias = 0.0; c->slope = 0.0;
  c->ns = 0; c->ctrl = -1; c->mask = MASK_NONE; c->isControl = 1;
  cells[i].ctrl = (int)(c - cells);
}

/* ---- the oracle ---- */

typedef struct { double sx, sy, d; int a; } SV;

static int in_domain(float z, float w) {
  float az = fabsf(z), aw = fabsf(w);
  return az <= 0x1p126f && aw <= 0x1p126f && aw >= 0x1p-100f;
}

static SV to_sv(V v) {
  SV s;
  s.sx = ((double)v.x / v.w + 1.0) * CELL_W / 2.0;
  s.sy = (1.0 - (double)v.y / v.w) * CELL_H / 2.0;
  s.d = (double)v.z / v.w;
  s.a = in_domain(v.z, v.w);
  return s;
}

/* screen-space barycentrics of (px, py) in triangle t of cell c; 0 if outside */
static int bary(const Cell *c, int t, double px, double py, double mu[3], SV sv[3]) {
  for (int i = 0; i < 3; i++) sv[i] = to_sv(c->v[3 * t + i]);
  double den = (sv[1].sy - sv[2].sy) * (sv[0].sx - sv[2].sx) + (sv[2].sx - sv[1].sx) * (sv[0].sy - sv[2].sy);
  if (fabs(den) < 1e-12) return 0;
  mu[0] = ((sv[1].sy - sv[2].sy) * (px - sv[2].sx) + (sv[2].sx - sv[1].sx) * (py - sv[2].sy)) / den;
  mu[1] = ((sv[2].sy - sv[0].sy) * (px - sv[2].sx) + (sv[0].sx - sv[2].sx) * (py - sv[2].sy)) / den;
  mu[2] = 1.0 - mu[0] - mu[1];
  return mu[0] >= -1e-9 && mu[1] >= -1e-9 && mu[2] >= -1e-9;
}

/* plane gradient (per pixel) of the values val[i] at the screen vertices */
static void plane_grad(const SV sv[3], const double val[3], double *gx, double *gy) {
  double x1 = sv[1].sx - sv[0].sx, y1 = sv[1].sy - sv[0].sy;
  double x2 = sv[2].sx - sv[0].sx, y2 = sv[2].sy - sv[0].sy;
  double v1 = val[1] - val[0], v2 = val[2] - val[0];
  double det = x1 * y2 - x2 * y1;
  *gx = (v1 * y2 - v2 * y1) / det;
  *gy = (x1 * v2 - x2 * v1) / det;
}

typedef struct {
  int covered;         /* inside a triangle and inside the original frustum */
  double final, legacy, gx, gy;
  int legacyOk;
} Oracle;

static Oracle oracle(const Cell *c, double px, double py) {
  Oracle o;
  memset(&o, 0, sizeof(o));
  float zn, zf;
  eff_range(c->minz, c->maxz, &zn, &zf);
  double range = (double)zf - zn;
  for (int t = 0; t < c->nv / 3; t++) {
    double mu[3];
    SV sv[3];
    if (!bary(c, t, px, py, mu, sv)) continue;
    double dn = mu[0] * sv[0].d + mu[1] * sv[1].d + mu[2] * sv[2].d;
    if (dn < 0.0 || dn > 1.0) return o;
    double emitted[3], orig[3], maxw = 0.0;
    for (int i = 0; i < 3; i++) {
      orig[i] = zn + range * sv[i].d;
      emitted[i] = orig[i] + c->bias * (sv[i].a ? 1.0 : 0.0);
      if (orig[i] > maxw) maxw = orig[i];
    }
    double gx, gy;
    plane_grad(sv, emitted, &gx, &gy);
    double me = fmax(fabs(gx), fabs(gy));
    double e = mu[0] * emitted[0] + mu[1] * emitted[1] + mu[2] * emitted[2];
    double f = e + c->slope * me;
    o.final = f < zn ? zn : f > zf ? zf : f;
    o.gx = gx; o.gy = gy;
    o.covered = 1;
    /* the hardware model: B * 2^(e+1) with e the exponent of the primitive's max depth */
    if (maxw > 0.0 && maxw < 1.0) {
      int ex;
      frexp(maxw, &ex);            /* maxw = m * 2^ex, m in [0.5,1): exponent e = ex - 1 */
      double og[3], ogx, ogy;
      for (int i = 0; i < 3; i++) og[i] = orig[i];
      plane_grad(sv, og, &ogx, &ogy);
      double d = mu[0] * orig[0] + mu[1] * orig[1] + mu[2] * orig[2];
      o.legacy = d + c->bias * ldexp(1.0, ex) + c->slope * fmax(fabs(ogx), fabs(ogy));
      o.legacyOk = 1;
    }
    return o;
  }
  return o;
}

static int zpass(DWORD f, double a, double ref) {
  switch (f) {
    case D3DCMP_LESS: return a < ref;
    case D3DCMP_GREATER: return a > ref;
    case D3DCMP_ALWAYS: return 1;
    default: return 0;
  }
}

/* ---- drawing ---- */

static void set_viewport(const Cell *c, float minz, float maxz) {
  D3DVIEWPORT9 v = { (DWORD)c->x0, (DWORD)c->y0, CELL_W, CELL_H, minz, maxz };
  CHECK(IDirect3DDevice9_SetViewport(dev, &v));
}

static void color(float r, float g, float b) {
  const float c[4] = { r, g, b, 1.0f };
  IDirect3DDevice9_SetPixelShaderConstantF(dev, 0, c, 1);
}

static void bias_states(float bias, float slope) {
  IDirect3DDevice9_SetRenderState(dev, D3DRS_DEPTHBIAS, f2dw(bias));
  IDirect3DDevice9_SetRenderState(dev, D3DRS_SLOPESCALEDEPTHBIAS, f2dw(slope));
}

static void base_state(const Cell *c, int ms) {
  IDirect3DDevice9_SetVertexDeclaration(dev, decl);
  IDirect3DDevice9_SetVertexShader(dev, vsMain);
  IDirect3DDevice9_SetPixelShader(dev, psColor);
  IDirect3DDevice9_SetDepthStencilSurface(dev, ds[ms]);
  set_viewport(c, 0.0f, 1.0f);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, D3DZB_TRUE);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_ZWRITEENABLE, TRUE);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_ZFUNC, D3DCMP_ALWAYS);
  bias_states(0.0f, 0.0f);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_FILLMODE, D3DFILL_SOLID);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_CLIPPLANEENABLE, 0);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_SCISSORTESTENABLE, FALSE);
}

/* a 5x5 green quad at window depth ref around cell pixel (px, py), viewport [0,1], ALWAYS */
static void draw_ref(int px, int py, double ref) {
  V q[6];
  double x0 = px - 2.5, x1 = px + 2.5, y0 = py - 2.5, y1 = py + 2.5;
  q[0] = vp(x0, y0, ref, 1); q[1] = vp(x1, y0, ref, 1); q[2] = vp(x0, y1, ref, 1);
  q[3] = vp(x0, y1, ref, 1); q[4] = vp(x1, y0, ref, 1); q[5] = vp(x1, y1, ref, 1);
  color(0.0f, 1.0f, 0.0f);
  CHECK(IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, q, sizeof(V)));
}

static void clear_cell(const Cell *c, float dc) {
  set_viewport(c, 0.0f, 1.0f);
  CHECK(IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                               D3DCOLOR_XRGB(0, 0, 255), dc, 0));
}

static UINT prim_count(int prim, int nv) {
  switch (prim) {
    case D3DPT_TRIANGLELIST: return nv / 3;
    case D3DPT_LINELIST: return nv / 2;
    case D3DPT_POINTLIST: return nv;
    default: return 0;
  }
}

static void draw_probe(const Cell *c) {
  set_viewport(c, c->minz, c->maxz);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_ZFUNC, c->zfunc);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_ZWRITEENABLE, FALSE);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_FILLMODE, c->fill);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_CLIPPLANEENABLE, c->clipmask);
  bias_states((float)c->bias, (float)c->slope);
  color(1.0f, 0.0f, 0.0f);

  if (c->vs == VS_FF) {
    VT q[6];
    for (int i = 0; i < 6; i++) {
      q[i] = c->vt[i];
      q[i].x += (float)c->x0;
      q[i].y += (float)c->y0;
    }
    IDirect3DDevice9_SetVertexShader(dev, NULL);
    IDirect3DDevice9_SetPixelShader(dev, NULL);
    IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    CHECK(IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, q, sizeof(VT)));
    return;
  }

  IDirect3DDevice9_SetVertexShader(dev, c->vs == VS_UNHOOK ? vsUnhook : vsMain);
  if (c->ps == PS_DEPTH_CONST) {
    const float d[4] = { c->psDepth, 0.0f, 0.0f, 0.0f };
    IDirect3DDevice9_SetPixelShaderConstantF(dev, 1, d, 1);
    IDirect3DDevice9_SetPixelShader(dev, psDepthConst);
  } else if (c->ps == PS_DEPTH_INTERP) {
    IDirect3DDevice9_SetPixelShader(dev, psDepthInterp);
  }
  CHECK(IDirect3DDevice9_DrawPrimitiveUP(dev, c->prim, prim_count(c->prim, c->nv), c->v, sizeof(V)));
}

static void draw_cell(const Cell *c, int ms) {
  base_state(c, ms);
  clear_cell(c, c->dc);
  for (int i = 0; i < c->ns; i++)
    if (c->s[i].hasRef) draw_ref(c->s[i].px, c->s[i].py, c->s[i].ref);
  draw_probe(c);
}

/* ---- the T sequences: references first, then each sequence with only the tested change
 * between its probes (geometry selects the 32x96 sub-rect; no scissor, viewport or state call) ---- */

static void sub_quad(V q[6], int j, double d) {
  double x0 = 32 * j + 2, x1 = 32 * j + 30;
  q[0] = vp(x0, 2, d, 1); q[1] = vp(x1, 2, d, 1); q[2] = vp(x0, 94, d, 1);
  q[3] = vp(x0, 94, d, 1); q[4] = vp(x1, 2, d, 1); q[5] = vp(x1, 94, d, 1);
}

static void run_sequences(int ms) {
  const double z = 0.2, pz = 0.2 + 0.75 * B;
  /* references */
  for (int i = 0; i < ncells; i++) {
    Cell *c = &cells[i];
    if (!c->seq) continue;
    base_state(c, ms);
    clear_cell(c, 1.0f);
    for (int j = 0; j < 4; j++) draw_ref(16 + 32 * j, 48, z);
    if (c->seq == 3) {
      IDirect3DDevice9_SetDepthStencilSurface(dev, ds16[ms]);
      CHECK(IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_ZBUFFER, 0, 1.0f, 0));
      draw_ref(16 + 32 * 1, 48, z);
      IDirect3DDevice9_SetDepthStencilSurface(dev, ds[ms]);
    }
  }
  for (int i = 0; i < ncells; i++) {
    Cell *c = &cells[i];
    if (!c->seq) continue;
    V q[6], ln[2], pt[1];
    base_state(c, ms);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZFUNC, D3DCMP_LESS);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZWRITEENABLE, FALSE);
    bias_states((float)-B, 0.0f);
    color(1.0f, 0.0f, 0.0f);
    switch (c->seq) {
      case 1: /* triangle -> line -> point -> triangle */
        sub_quad(q, 0, pz);
        CHECK(IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, q, sizeof(V)));
        ln[0] = vp(32 + 2, 48, pz, 1); ln[1] = vp(32 + 30, 48, pz, 1);
        CHECK(IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_LINELIST, 1, ln, sizeof(V)));
        pt[0] = vp(80, 48, pz, 1);
        CHECK(IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_POINTLIST, 1, pt, sizeof(V)));
        sub_quad(q, 3, pz);
        CHECK(IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, q, sizeof(V)));
        break;
      case 2: /* hooked VS -> unhooked VS -> hooked */
        sub_quad(q, 0, pz);
        CHECK(IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, q, sizeof(V)));
        IDirect3DDevice9_SetVertexShader(dev, vsUnhook);
        sub_quad(q, 1, pz);
        CHECK(IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, q, sizeof(V)));
        IDirect3DDevice9_SetVertexShader(dev, vsMain);
        sub_quad(q, 2, pz);
        CHECK(IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, q, sizeof(V)));
        break;
      case 3: /* float DS -> D16 -> null -> float */
        sub_quad(q, 0, pz);
        CHECK(IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, q, sizeof(V)));
        IDirect3DDevice9_SetDepthStencilSurface(dev, ds16[ms]);
        sub_quad(q, 1, pz);
        CHECK(IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, q, sizeof(V)));
        IDirect3DDevice9_SetDepthStencilSurface(dev, NULL);
        sub_quad(q, 2, pz);
        CHECK(IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, q, sizeof(V)));
        IDirect3DDevice9_SetDepthStencilSurface(dev, ds[ms]);
        sub_quad(q, 3, pz);
        CHECK(IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, q, sizeof(V)));
        break;
      case 4: { /* plain PS -> oDepth PS -> plain */
        const float d[4] = { (float)pz, 0.0f, 0.0f, 0.0f };
        sub_quad(q, 0, pz);
        CHECK(IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, q, sizeof(V)));
        IDirect3DDevice9_SetPixelShaderConstantF(dev, 1, d, 1);
        IDirect3DDevice9_SetPixelShader(dev, psDepthConst);
        sub_quad(q, 1, pz);
        CHECK(IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, q, sizeof(V)));
        IDirect3DDevice9_SetPixelShader(dev, psColor);
        sub_quad(q, 2, pz);
        CHECK(IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, q, sizeof(V)));
      } break;
    }
  }
}

/* ---- readback ---- */

static D3DLOCKED_RECT lr;

static void read_back(int ms) {
  if (ms) CHECK(IDirect3DDevice9_StretchRect(dev, rt[1], NULL, resolved, NULL, D3DTEXF_NONE));
  CHECK(IDirect3DDevice9_GetRenderTargetData(dev, ms ? resolved : rt[0], sysmem));
  CHECK(IDirect3DSurface9_LockRect(sysmem, &lr, NULL, D3DLOCK_READONLY));
}

static DWORD pixel(int x, int y) {
  return ((const DWORD *)((const BYTE *)lr.pBits + (size_t)y * lr.Pitch))[x] & 0xffffffu;
}

static char classify(DWORD p) {
  int r = (p >> 16) & 0xff, g = (p >> 8) & 0xff, b = p & 0xff;
  if (r >= 200 && g <= 60 && b <= 60) return 'R';
  if (g >= 200 && r <= 60 && b <= 60) return 'G';
  if (b >= 200 && r <= 60 && g <= 60) return 'B';
  return 'M';
}

static int red_present(DWORD p) { return ((p >> 16) & 0xff) >= 60; }

/* ---- checking ---- */

static int nOk, nFail, nCross, nFixture;

static void report(int ms, const Cell *c, const char *sname, char want, char got, DWORD p, char legacy,
                   const char *verdict) {
  printf("D9MT-DEPTHBIAS x%d %s %s want=%c got=%c px=%06lx legacy=%c %s\n", ms ? 4 : 1, c->name, sname,
         want, got, (unsigned long)p, legacy, verdict);
}

/* a sample whose 4x verdict compares a depth with its reference (DESIGN 5.1's 4x-gated depth
 * samples): the oracle's or an explicit R/G/r. S, I, C, c and reference-less samples aren't. */
static int depth_gated4(const Sample *s) {
  return s->hasRef && (s->want4 == 0 || s->want4 == 'R' || s->want4 == 'G' || s->want4 == 'r');
}

/* DESIGN 5.1: every 4x-gated depth sample satisfies |final - Ref| > (|g_x| + |g_y|) / 2 + 2^-22
 * for the depth its want predicts, so all 4 samples of the pixel agree with the centre oracle.
 * Runs once after build(), before any draw: a violation marks the sample (reported as
 * fixture-FAIL in the 4x pass), never a driver result. */
static int validate_fixture(void) {
  int checked = 0, bad = 0;
  for (int i = 0; i < ncells; i++) {
    Cell *c = &cells[i];
    if (c->isControl) continue;
    for (int j = 0; j < c->ns; j++) {
      Sample *s = &c->s[j];
      if (!depth_gated4(s)) continue;
      double fin, gx, gy;
      if (s->mset) { fin = s->mfinal; gx = s->mgx; gy = s->mgy; }
      else {
        Oracle o = oracle(c, s->px, s->py);
        if (!o.covered) continue;   /* the probe doesn't reach the sample: no depth compare */
        fin = o.final; gx = o.gx; gy = o.gy;
      }
      double need = 0.5 * (fabs(gx) + fabs(gy)) + 0x1p-22;
      checked++;
      if (!(fabs(fin - s->ref) > need)) {
        s->fixtureBad = 1;
        bad++;
        printf("D9MT-DEPTHBIAS-FIXTURE %s s%d(%d,%d) final=%.9g ref=%.9g need>%.3g FAIL\n", c->name, j,
               s->px, s->py, fin, s->ref, need);
      }
    }
  }
  printf("D9MT-DEPTHBIAS-FIXTURE 4x margin checked=%d bad=%d\n", checked, bad);
  return bad;
}

static void check_cell(int ms, const Cell *c) {
  char sname[16];
  for (int i = 0; i < c->ns; i++) {
    const Sample *s = &c->s[i];
    int ax = c->x0 + s->px, ay = c->y0 + s->py;
    DWORD p = pixel(ax, ay);
    if (s->any3) {
      for (int dy = -1; dy <= 1; dy++) {
        DWORD q = pixel(ax, ay + dy);
        if (((q >> 16) & 0xff) > ((p >> 16) & 0xff)) p = q;
      }
    }
    char got = classify(p);
    char want = ms ? s->want4 : s->want1;
    char legacy = '?';
    snprintf(sname, sizeof(sname), "s%d(%d,%d)", i, s->px, s->py);

    Oracle o = oracle(c, s->px, s->py);
    if (c->legacy && o.covered && o.legacyOk && s->hasRef)
      legacy = zpass(c->zfunc, o.legacy, s->ref) ? 'R' : 'G';

    if (ms && s->fixtureBad) {
      report(ms, c, sname, want ? want : '?', got, p, legacy, "fixture-FAIL");
      nFixture++;
      continue;
    }

    if (!want) {
      if (!o.covered) want = s->hasRef ? 'G' : 'B';
      else if (!s->hasRef) want = 'R';
      else want = zpass(c->zfunc, o.final, s->ref) ? 'R' : 'G';
    }

    switch (want) {
      case 'S': report(ms, c, sname, want, got, p, legacy, "cross"); nCross++; break;
      case 'I': report(ms, c, sname, want, got, p, legacy, "info"); break;
      case 'r': {
        int ok = red_present(p);
        report(ms, c, sname, want, ok ? 'r' : got, p, legacy, ok ? "ok" : "FAIL");
        if (ok) nOk++; else nFail++;
      } break;
      case 'C':
      case 'c': {
        const Cell *k = &cells[c->ctrl];
        DWORD q = pixel(k->x0 + s->px, k->y0 + s->py);
        int ok = p == q;
        printf("D9MT-DEPTHBIAS x%d %s %s want=%c got=%06lx ctl=%06lx %s\n", ms ? 4 : 1, c->name, sname, want,
               (unsigned long)p, (unsigned long)q, want == 'c' ? "info" : ok ? "ok" : "FAIL");
        if (want == 'C') { if (ok) nOk++; else nFail++; }
      } break;
      default: {
        int ok = got == want;
        report(ms, c, sname, want, got, p, legacy, ok ? "ok" : "FAIL");
        if (ok) nOk++; else nFail++;
      }
    }
  }

  if (c->mask != MASK_NONE && c->ctrl >= 0) {
    const Cell *k = &cells[c->ctrl];
    int mism = 0;
    for (int y = 0; y < CELL_H; y++)
      for (int x = 0; x < CELL_W; x++) {
        if (c->mask == MASK_FULL && x >= c->exclX0 && x <= c->exclX1) continue;
        int a = classify(pixel(c->x0 + x, c->y0 + y)) == 'R';
        int b = classify(pixel(k->x0 + x, k->y0 + y)) == 'R';
        mism += a != b;
      }
    if (c->mask == MASK_FULL) {
      printf("D9MT-DEPTHBIAS x%d %s mask mismatches=%d (columns %d-%d excluded) %s\n", ms ? 4 : 1,
             c->name, mism, c->exclX0, c->exclX1, mism == 0 ? "ok" : "FAIL");
      if (mism == 0) nOk++; else nFail++;
    } else {
      /* MASK_AUTO: 2 covered and 2 uncovered samples whose 9x9 neighbourhood is uniform in the
       * control; the resolved bytes must equal the control's. A control without both regions
       * (a blank or a fully covered cell) proves nothing: fixture failure. */
      int picked = 0, wantR = 2, wantN = 2;
      for (int y = 6; y < CELL_H - 6 && picked < 4; y += 5)
        for (int x = 6; x < CELL_W - 6 && picked < 4; x += 7) {
          int r0 = classify(pixel(k->x0 + x, k->y0 + y)) == 'R', uniform = 1;
          for (int dy = -4; dy <= 4 && uniform; dy++)
            for (int dx = -4; dx <= 4 && uniform; dx++)
              uniform = (classify(pixel(k->x0 + x + dx, k->y0 + y + dy)) == 'R') == r0;
          if (!uniform || (r0 ? wantR : wantN) == 0) continue;
          if (r0) wantR--; else wantN--;
          picked++;
          DWORD p = pixel(c->x0 + x, c->y0 + y), q = pixel(k->x0 + x, k->y0 + y);
          int ok = p == q;
          printf("D9MT-DEPTHBIAS x%d %s auto(%d,%d) want=C got=%06lx ctl=%06lx %s\n", ms ? 4 : 1, c->name,
                 x, y, (unsigned long)p, (unsigned long)q, ok ? "ok" : "FAIL");
          if (ok) nOk++; else nFail++;
        }
      if (picked != 4 || wantR != 0 || wantN != 0) {
        printf("D9MT-DEPTHBIAS x%d %s auto picked=%d (covered missing %d, uncovered missing %d) fixture-FAIL\n",
               ms ? 4 : 1, c->name, picked, wantR, wantN);
        nFixture++;
      }
      printf("D9MT-DEPTHBIAS x%d %s mask mismatches=%d (whole cell) info\n", ms ? 4 : 1, c->name, mism);
    }
  }
}

/* ---- the rows (DESIGN v4 section 5.2) ---- */

static void build(void) {
  static const double Z6[6] = { 0.9, 0.6, 0.4, 0.2, 0.05, 0.01 };
  char n[32];
  Cell *c;

  /* A, P: flat, the pull and push pairs */
  for (int i = 0; i < 6; i++)
    for (int j = 0; j < 2; j++) {
      double d = j ? 1.25 : 0.75;
      snprintf(n, sizeof(n), "A%d", 2 * i + j + 1);
      c = add_cell(n); quad_flat(c, Z6[i] + d * B); c->bias = -B; c->legacy = 1;
      sample(c, 64, 48, 1, Z6[i], 0, 0);
      snprintf(n, sizeof(n), "P%d", 2 * i + j + 1);
      c = add_cell(n); quad_flat(c, Z6[i] - d * B); c->bias = B; c->legacy = 1;
      sample(c, 64, 48, 1, Z6[i], 0, 0);
    }

  /* W: mixed w, slope 0; refs at final -/+ B/4 */
  for (int sign = 0; sign < 2; sign++)
    for (int side = 0; side < 2; side++) {
      snprintf(n, sizeof(n), "W%c%c", sign ? '+' : '-', side ? 'g' : 'r');
      c = add_cell(n);
      tri(c, vp(8, 8, 0.500, 1), vp(120, 8, 0.505, 2), vp(8, 88, 0.510, 4));
      c->bias = sign ? B : -B;
      c->zfunc = sign ? D3DCMP_GREATER : D3DCMP_LESS;
      c->dc = sign ? 0.0f : 1.0f;
      static const int sp[3][2] = { { 40, 30 }, { 24, 60 }, { 70, 20 } };
      for (int k = 0; k < 3; k++) {
        Oracle o = oracle(c, sp[k][0], sp[k][1]);
        double off = (sign ? -1 : 1) * (side ? -B / 4 : B / 4);   /* R side first, then G side */
        sample(c, sp[k][0], sp[k][1], 1, o.final + off, 0, 0);
      }
    }

  /* Y: slope + constant, x-only slope m = B/8 */
  for (int v = 0; v < 2; v++)
    for (int side = 0; side < 2; side++) {
      snprintf(n, sizeof(n), "Y%d%c", v + 1, side ? 'g' : 'r');
      c = add_cell(n);
      quad_x(c, 0.40 + (B / 8) * (0 - 8), 0.40 + (B / 8) * (CELL_W - 8));
      c->bias = v ? 2 * B : -B;
      c->slope = v ? -8.0 : 8.0;
      for (int k = 0; k < 2; k++) {
        int px = k ? 72 : 40;
        Oracle o = oracle(c, px, 48);
        sample(c, px, 48, 1, o.final + (side ? -B / 4 : B / 4), 0, 0);
      }
    }

  /* V: reduced viewport [0.25, 0.75], window depths 0.7, 0.5, 0.3 */
  {
    static const double wz[3] = { 0.7, 0.5, 0.3 };
    for (int i = 0; i < 3; i++)
      for (int j = 0; j < 2; j++) {
        snprintf(n, sizeof(n), "V%d", 2 * i + j + 1);
        c = add_cell(n);
        c->minz = 0.25f; c->maxz = 0.75f;
        quad_flat(c, (wz[i] + (j ? 1.25 : 0.75) * B - 0.25) / 0.5);
        c->bias = -B; c->legacy = 1;
        sample(c, 64, 48, 1, wz[i], 0, 0);
      }
  }

  /* K: the clamp contract */
  c = add_cell("K1"); c->minz = 0.25f; c->maxz = 0.75f; quad_flat(c, 1.0 - B); c->bias = B;
  sample(c, 64, 48, 1, 0.75 + B / 4, 'R', 'R');
  c = add_cell("K2"); c->minz = 0.25f; c->maxz = 0.75f; quad_flat(c, B); c->bias = -B;
  c->zfunc = D3DCMP_GREATER; c->dc = 0.0f;
  sample(c, 64, 48, 1, 0.25 - B / 4, 'R', 'R');
  {
    /* x-sloped K3/K4 probe: window 0.75 - B/2 + (B/16)(x - 64), NDC in [0.25, 0.75] */
    double w0 = 0.75 - B / 2 + (B / 16) * (0 - 64), w1 = 0.75 - B / 2 + (B / 16) * (CELL_W - 64);
    c = add_cell("K3"); c->minz = 0.25f; c->maxz = 0.75f; quad_x(c, (w0 - 0.25) / 0.5, (w1 - 0.25) / 0.5);
    c->slope = 16.0;
    sample(c, 64, 48, 1, 0.75 + B / 4, 'S', 'S');
    c = add_cell("K4"); c->minz = 0.25f; c->maxz = 0.75f; quad_x(c, (w0 - 0.25) / 0.5, (w1 - 0.25) / 0.5);
    c->bias = 2 * B; c->slope = -16.0; c->zfunc = D3DCMP_GREATER; c->dc = 0.0f;
    sample(c, 64, 48, 1, 0.75 - B / 2, 'R', 'R');
    /* K5: window 0.25 + B/2 - (B/16)(x - 64) */
    w0 = 0.25 + B / 2 - (B / 16) * (0 - 64); w1 = 0.25 + B / 2 - (B / 16) * (CELL_W - 64);
    c = add_cell("K5"); c->minz = 0.25f; c->maxz = 0.75f; quad_x(c, (w0 - 0.25) / 0.5, (w1 - 0.25) / 0.5);
    c->bias = -2 * B; c->slope = 16.0;
    sample(c, 64, 48, 1, 0.25 + B / 2, 'R', 'R');
  }
  c = add_cell("K6");
  tri(c, vp(8, 48, 1 - 4 * B, 1), vp(72, 8, 1 - B / 2, 1), vp(72, 88, 1 - B / 2, 1));
  c->bias = B; c->zfunc = D3DCMP_GREATER; c->dc = 0.0f;
  sample(c, 40, 48, 1, 1 - 1.375 * B, 'R', 'R');
  c = add_cell("K7"); c->minz = 0.25f; c->maxz = 0.75f;
  tri(c, vp(8, 48, (0.75 - 4 * B - 0.25) / 0.5, 1), vp(72, 8, (0.75 - B / 2 - 0.25) / 0.5, 1),
      vp(72, 88, (0.75 - B / 2 - 0.25) / 0.5, 1));
  c->bias = B; c->zfunc = D3DCMP_GREATER; c->dc = 0.0f;
  sample(c, 40, 48, 1, 0.75 - 1.375 * B, 'R', 'R');

  /* C: coverage at the planes, ALWAYS */
  {
    static const double nd[4] = { 1 - B / 2, -B / 2, B / 2, 1 + B / 2 };
    static const double bs[4] = { 1, 1, -1, -1 };
    static const char w[4] = { 'R', 'B', 'R', 'B' };
    for (int i = 0; i < 4; i++) {
      snprintf(n, sizeof(n), "C%d", i + 1);
      c = add_cell(n); quad_flat(c, nd[i]); c->bias = bs[i] * B; c->zfunc = D3DCMP_ALWAYS;
      sample(c, 64, 48, 0, 0, w[i], 'C');
      add_control((int)(c - cells));
    }
  }

  /* E: straddles; the whole mask equals the B = 0 draw except the crossing column +-1 */
  for (int i = 0; i < 4; i++) {
    int farp = i < 2;
    snprintf(n, sizeof(n), "E%d", i + 1);
    c = add_cell(n);
    if (farp) quad_x(c, 0.9, 1.1); else quad_x(c, 0.1, -0.1);
    c->bias = (i % 2 ? -1 : 1) * B; c->zfunc = D3DCMP_ALWAYS;
    sample(c, 32, 48, 0, 0, 'R', 'C');
    sample(c, 96, 48, 0, 0, 'B', 'C');
    sample(c, 64, 48, 0, 0, 'I', 'C');
    c->mask = MASK_FULL; c->exclX0 = 63; c->exclX1 = 65;
    add_control((int)(c - cells));
  }

  /* U: six user planes (keep y_ndc <= Y_i) + d6/d7. U1's clipping edges against its B = 0
   * control (same planes): the far crossing (NDC 1 at x = 64) and plane 5's edge (y_ndc = 0.5 at
   * y = 24) each run through a pixel centre, so at 4x those pixels are half covered and the
   * resolved bytes witness the edge's sub-pixel position; their neighbours are fully in or out.
   * At 1x the centre lies on the edge itself: informational (c). */
  for (int i = 0; i < 3; i++) {
    snprintf(n, sizeof(n), "U%d", i + 1);
    c = add_cell(n); quad_x(c, 0.9, 1.1); c->bias = B; c->zfunc = D3DCMP_ALWAYS;
    c->clipmask = i == 0 ? 0x3fu : i == 1 ? ((1u << 1) | (1u << 4)) : 0u;
    sample(c, 32, 60, 0, 0, 'R', 'R');
    sample(c, 100, 60, 0, 0, 'B', 'B');
    sample(c, 32, 10, 0, 0, i < 2 ? 'B' : 'R', i < 2 ? 'B' : 'R');
    if (i == 0) {
      for (int dx = -1; dx <= 1; dx++) sample(c, 64 + dx, 60, 0, 0, dx ? 'C' : 'c', 'C');
      for (int dy = -1; dy <= 1; dy++) sample(c, 32, 24 + dy, 0, 0, dy ? 'C' : 'c', 'C');
      add_control((int)(c - cells));
    }
  }

  /* R: wireframe crossing the far / near plane: rows 79..81 equal the B = 0 draw. The bottom
   * edge's depth is 0.9 -> 1.1 (0.1 -> -0.1) over x = 0..128, so it crosses at x = 64, a pixel
   * centre: R1's crossing pixel (64, rows 79..81) is partially covered at 4x (bytes gating), its
   * neighbours 63/65 fully in/out; at 1x the crossing centre is informational (c). */
  for (int i = 0; i < 2; i++) {
    snprintf(n, sizeof(n), "R%d", i + 1);
    c = add_cell(n);
    if (i == 0) tri(c, vp(0, 80, 0.9, 1), vp(CELL_W, 80, 1.1, 1), vp(64, 10, 1.0, 1));
    else tri(c, vp(0, 80, 0.1, 1), vp(CELL_W, 80, -0.1, 1), vp(64, 10, 0.0, 1));
    c->fill = D3DFILL_WIREFRAME; c->bias = B; c->zfunc = D3DCMP_ALWAYS;
    for (int dy = -1; dy <= 1; dy++) {
      sample(c, 32, 80 + dy, 0, 0, 'C', 'C');
      sample(c, 96, 80 + dy, 0, 0, 'C', 'C');
    }
    if (i == 0) {
      for (int dy = -1; dy <= 1; dy++) sample(c, 64, 80 + dy, 0, 0, 'c', 'C');
      sample(c, 63, 80, 0, 0, 'C', 'C');
      sample(c, 65, 80, 0, 0, 'C', 'C');
    }
    add_control((int)(c - cells));
  }

  /* G: guard band + unequal w (P0 at x_ndc -3) */
  c = add_cell("G1");
  tri(c, vraw(-3.0f, 0.0f, 0.5f, 1.0f), vp(120, 8, 0.8, 2), vp(120, 88, 1.2, 4));
  c->bias = B; c->zfunc = D3DCMP_ALWAYS;
  sample(c, 40, 48, 0, 0, 'R', 'C');
  sample(c, 100, 20, 0, 0, 'R', 'C');
  sample(c, 100, 70, 0, 0, 'B', 'C');
  sample(c, 110, 80, 0, 0, 'B', 'C');
  add_control((int)(c - cells));
  for (int side = 0; side < 2; side++) {
    snprintf(n, sizeof(n), "G2%c", side ? 'g' : 'r');
    c = add_cell(n);
    tri(c, vraw(-3.0f, 0.0f, 0.5f, 1.0f), vp(120, 8, 0.8, 2), vp(120, 88, 1.2, 4));
    c->bias = B;
    Oracle o = oracle(c, 40, 48);
    sample(c, 40, 48, 1, o.final + (side ? -B / 4 : B / 4), 0, 'I');
  }

  /* M: mixed-sign w and near-zero positive w */
  c = add_cell("M1");
  tri(c, vp(8, 80, 0.5, 1), vp(120, 80, 0.5, 1), vraw(0.0f, -0.5f, -0.5f, -1.0f));
  c->bias = B; c->zfunc = D3DCMP_ALWAYS; c->mask = MASK_AUTO;
  add_control((int)(c - cells));
  c = add_cell("M2");
  tri(c, vp(8, 80, 0.5, 1), vp(120, 80, 0.5, 1), vraw(0.0f, 0.5f * 0x1p-20f, 0.5f * 0x1p-20f, 0x1p-20f));
  c->bias = B; c->zfunc = D3DCMP_ALWAYS; c->mask = MASK_AUTO;
  add_control((int)(c - cells));

  /* Q: the domain boundary, w = 2^126 (in) and 2^127 (out: bias dropped) */
  for (int q = 0; q < 2; q++)
    for (int j = 0; j < 3; j++) {
      double w = q ? 0x1p127 : 0x1p126;
      double d = j == 2 ? 0.5 : 0.5 + (j ? 1.25 : 0.75) * B;
      snprintf(n, sizeof(n), "Q%d%c", q + 1, j == 2 ? 'a' : j ? 'g' : 'r');
      c = add_cell(n);
      V a = vp(0, 0, d, w), b = vp(CELL_W, 0, d, w), e = vp(0, CELL_H, d, w), f = vp(CELL_W, CELL_H, d, w);
      tri(c, a, b, e); tri(c, e, b, f);
      c->bias = -B;
      if (j == 2) { c->zfunc = D3DCMP_ALWAYS; sample(c, 64, 48, 0, 0, 'R', 'R'); }
      else sample(c, 64, 48, 1, 0.5, 0, 0);
    }

  /* J: mixed domain (P0 w = 2^127) */
  for (int s2 = 0; s2 < 2; s2++)
    for (int side = 0; side < 2; side++) {
      snprintf(n, sizeof(n), "J%d%c", s2 + 1, side ? 'g' : 'r');
      c = add_cell(n);
      tri(c, vp(8, 48, 0.5, 0x1p127), vp(120, 8, 0.5, 1), vp(120, 88, 0.5, 1));
      c->bias = -B; c->slope = s2 ? 16.0 : 0.0;
      for (int k = 0; k < 2; k++) {
        int px = k ? 100 : 40;
        Oracle o = oracle(c, px, 48);
        sample(c, px, 48, 1, o.final + (side ? -B / 8 : B / 8), 0, 0);
      }
    }
  c = add_cell("J3");
  tri(c, vraw((float)(ndcx(8) * 0x1p127), 0.0f, -0x1p127f, 0x1p127f), vp(120, 8, 0.5, 1), vp(120, 88, 0.5, 1));
  c->bias = B; c->zfunc = D3DCMP_ALWAYS;
  sample(c, 100, 40, 0, 0, 'C', 'C');
  sample(c, 100, 56, 0, 0, 'C', 'C');
  sample(c, 60, 48, 0, 0, 'C', 'C');
  sample(c, 40, 48, 0, 0, 'C', 'C');
  /* the near crossing: NDC depth -1 + 1.5 (x - 8) / 112 = 0 at x = 82.67 (a vertical line). Pixel 83
   * (centre 83, 1x in) is partially covered at 4x, 82 is out, 84 in. */
  sample(c, 82, 48, 0, 0, 'C', 'C');
  sample(c, 83, 48, 0, 0, 'C', 'C');
  sample(c, 84, 48, 0, 0, 'C', 'C');
  sample(c, 83, 40, 0, 0, 'C', 'C');
  sample(c, 83, 56, 0, 0, 'C', 'C');
  add_control((int)(c - cells));

  /* D: degenerate viewports */
  {
    const double BD = 1.0 / 131072.0;   /* 2^-17 */
    static const float mz[2] = { 0.2f, 0.6f };
    for (int i = 0; i < 2; i++)
      for (int j = 0; j < 2; j++) {
        float zn, zf;
        eff_range(mz[i], mz[i], &zn, &zf);
        double range = (double)zf - zn;
        snprintf(n, sizeof(n), "D%d%c", i + 1, j ? 'g' : 'r');
        c = add_cell(n); c->minz = c->maxz = mz[i];
        quad_flat(c, (0.0005 + (j ? 1.25 : 0.75) * BD) / range);
        c->bias = -BD;
        sample(c, 64, 48, 1, zn + 0.0005, 0, 0);
      }
    c = add_cell("D3r"); c->minz = c->maxz = 1.0f; quad_flat(c, 0.5); c->bias = -B;
    sample(c, 64, 48, 0, 0, 'S', 'S');
    c = add_cell("D3a"); c->minz = c->maxz = 1.0f; quad_flat(c, 0.5); c->bias = -B; c->zfunc = D3DCMP_ALWAYS;
    sample(c, 64, 48, 0, 0, 'R', 'R');
    for (int j = 0; j < 2; j++) {
      snprintf(n, sizeof(n), "D4%c", j ? 'g' : 'r');
      c = add_cell(n); c->minz = c->maxz = 0.2f;
      quad_flat(c, (0.0005 + (j ? 1.25 : 0.75) * B) / 0.001);
      c->bias = -B;
      sample(c, 64, 48, 1, 0.2005, 'S', 'S');
    }
  }

  /* H: the k guard at 2^-6 */
  for (int h = 0; h < 2; h++)
    for (int j = 0; j < 2; j++) {
      double bh = h ? 0x1p-6 * (1 + 0x1p-10) : 0x1p-6;
      snprintf(n, sizeof(n), "H%d%c", h + 1, j ? 'g' : 'r');
      c = add_cell(n); quad_flat(c, 0.2 + (j ? 1.25 : 0.75) * bh); c->bias = -bh; c->legacy = 1;
      sample(c, 64, 48, 1, 0.2, h ? 'S' : 0, h ? 'S' : 0);
    }

  /* L: lines and points coplanar with the reference */
  for (int i = 0; i < 4; i++) {
    double z = i % 2 ? 0.2 : 0.6;
    snprintf(n, sizeof(n), "L%d", i + 1);
    c = add_cell(n); c->bias = -B;
    if (i < 2) { c->prim = D3DPT_LINELIST; c->v[0] = vp(8, 48, z, 1); c->v[1] = vp(120, 48, z, 1); c->nv = 2; }
    else { c->prim = D3DPT_POINTLIST; c->v[0] = vp(64, 48, z, 1); c->nv = 1; }
    sample(c, 64, 48, 1, z, 'S', 'S');
  }

  /* F: wireframe triangle, flat, the pull probe */
  for (int i = 0; i < 2; i++) {
    double z = i ? 0.2 : 0.6;
    snprintf(n, sizeof(n), "F%d", i + 1);
    c = add_cell(n);
    tri(c, vp(8, 80, z + 0.75 * B, 1), vp(120, 80, z + 0.75 * B, 1), vp(64, 16, z + 0.75 * B, 1));
    c->fill = D3DFILL_WIREFRAME; c->bias = -B;
    sample(c, 64, 80, 1, z, 'R', 'r');
    c->s[0].any3 = 1;
    /* flat: every covered sample of the edge (4x partial coverage) has the depth z - B/4 */
    sample_margin(c, z + 0.75 * B - B, 0.0, 0.0);
  }

  /* T: the transition sequences (run_sequences). Their probes are flat sub-quads at
   * pz = 0.2 + 0.75B with -B (the cells have no oracle geometry): an R want predicts the D3D9
   * depth pz - B (VS path, D16's exact hardware bias; null DS has no test); the unhooked G the
   * hardware model pz - B/4 (B * 2^(e+1), e = -3). Gradient 0. */
  {
    const double pz = 0.2 + 0.75 * B, dR = pz - B, dLegacy = pz - B / 4;
    for (int t = 1; t <= 4; t++) {
      snprintf(n, sizeof(n), "T%d", t);
      c = add_cell(n); c->seq = t;
      switch (t) {
        case 1:
          sample(c, 16, 48, 1, 0.2, 'R', 'R'); sample_margin(c, dR, 0.0, 0.0);
          sample(c, 48, 48, 1, 0.2, 'S', 'S');
          sample(c, 80, 48, 1, 0.2, 'S', 'S');
          sample(c, 112, 48, 1, 0.2, 'R', 'R'); sample_margin(c, dR, 0.0, 0.0);
          break;
        case 2:
          sample(c, 16, 48, 1, 0.2, 'R', 'R'); sample_margin(c, dR, 0.0, 0.0);
          sample(c, 48, 48, 1, 0.2, unhookActive ? 'G' : 'R', unhookActive ? 'G' : 'R');
          sample_margin(c, unhookActive ? dLegacy : dR, 0.0, 0.0);
          sample(c, 80, 48, 1, 0.2, 'R', 'R'); sample_margin(c, dR, 0.0, 0.0);
          break;
        case 3:
          for (int j = 0; j < 4; j++) { sample(c, 16 + 32 * j, 48, 1, 0.2, 'R', 'R'); sample_margin(c, dR, 0.0, 0.0); }
          break;
        case 4:
          sample(c, 16, 48, 1, 0.2, 'R', 'R'); sample_margin(c, dR, 0.0, 0.0);
          sample(c, 48, 48, 1, 0.2, 'S', 'S');
          sample(c, 80, 48, 1, 0.2, 'R', 'R'); sample_margin(c, dR, 0.0, 0.0);
          break;
      }
    }
  }

  /* O: pixel shaders writing depth keep the hardware path */
  for (int i = 0; i < 8; i++) {
    snprintf(n, sizeof(n), "O%d", i + 1);
    c = add_cell(n);
    quad_flat(c, 0.2 + 0.75 * B);
    c->ps = i < 4 ? PS_DEPTH_CONST : PS_DEPTH_INTERP;
    c->psDepth = (float)(0.2 + 0.75 * B);
    c->bias = (i % 2 ? 1 : -1) * B;
    c->slope = (i / 2) % 2 ? 1.0 : 0.0;
    sample(c, 64, 48, 1, 0.2, 'S', 'S');
  }

  /* X: fixed function, POSITIONT */
  {
    static const double xz[4] = { 0.9, 0.6, 0.2, 0.05 };
    for (int i = 0; i < 4; i++)
      for (int j = 0; j < 2; j++) {
        double z = xz[i] + (j ? 1.25 : 0.75) * B;
        snprintf(n, sizeof(n), "X%d%c", i + 1, j ? 'g' : 'r');
        c = add_cell(n); c->vs = VS_FF; c->bias = -B; c->legacy = 1;
        VT a = { 0.0f, 0.0f, (float)z, 1.0f, 0xffff0000 }, b = { (float)CELL_W, 0.0f, (float)z, 1.0f, 0xffff0000 };
        VT d = { 0.0f, (float)CELL_H, (float)z, 1.0f, 0xffff0000 }, e = { (float)CELL_W, (float)CELL_H, (float)z, 1.0f, 0xffff0000 };
        c->vt[0] = a; c->vt[1] = b; c->vt[2] = d; c->vt[3] = d; c->vt[4] = b; c->vt[5] = e;
        quad_flat(c, z);   /* for the oracle only */
        char w = xz[i] >= 0.5 ? (j ? 'G' : 'R') : 'S';
        sample(c, 64, 48, 1, xz[i], w, w);
      }
  }

  /* N: the no_depthbias debug flag (the DBT_NO_DEPTHBIAS=1 run) */
  for (int j = 0; j < 2; j++) {
    snprintf(n, sizeof(n), "N%d", j + 1);
    c = add_cell(n); quad_flat(c, 0.6 + (j ? 1.25 : 0.75) * B); c->bias = -B;
    char w = noDepthBiasRun ? 'G' : 'I';
    sample(c, 64, 48, 1, 0.6, w, w);
    sample_margin(c, 0.6 + (j ? 1.25 : 0.75) * B, 0.0, 0.0);   /* no_depthbias: the unbiased depth */
  }

  /* Z: no bias */
  c = add_cell("Z1"); quad_flat(c, 0.6 + 0.75 * B); sample(c, 64, 48, 1, 0.6, 'G', 'G');
  c = add_cell("Z2"); quad_flat(c, 0.6 - 0.75 * B); sample(c, 64, 48, 1, 0.6, 'R', 'R');
}

/* ---- main ---- */

static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
  return DefWindowProcA(h, m, w, l);
}

static void render_pass(int ms) {
  IDirect3DDevice9_SetRenderTarget(dev, 0, rt[ms]);
  for (int i = 0; i < ncells; i++)
    if (!cells[i].seq) draw_cell(&cells[i], ms);
  run_sequences(ms);
}

static void check_pass(int ms) {
  nOk = nFail = nCross = nFixture = 0;
  read_back(ms);
  for (int i = 0; i < ncells; i++)
    if (!cells[i].isControl) check_cell(ms, &cells[i]);
  IDirect3DSurface9_UnlockRect(sysmem);
  printf("D9MT-DEPTHBIAS-SUMMARY x%d gating ok=%d fail=%d cross=%d fixture=%d\n",
         ms ? 4 : 1, nOk, nFail, nCross, nFixture);
  fflush(stdout);
}

int main(void) {
  const char *e = getenv("D9MT_DEPTH_BIAS_TEST_UNHOOK");
  unhookActive = e && !strcmp(e, "1") && (e = getenv("D9MT_DEPTH_BIAS_VS")) && !strcmp(e, "1");
  e = getenv("DBT_NO_DEPTHBIAS");
  noDepthBiasRun = e && !strcmp(e, "1");

  WNDCLASSA wc = {0};
  wc.lpfnWndProc = wnd_proc;
  wc.hInstance = GetModuleHandleA(NULL);
  wc.lpszClassName = "d9mt_depth_bias";
  RegisterClassA(&wc);
  RECT rc = {0, 0, 1024, 768};
  AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
  HWND hwnd = CreateWindowA("d9mt_depth_bias", "d9mt depth bias", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                            CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
                            NULL, NULL, wc.hInstance, NULL);
  if (!hwnd) return 1;
  IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
  if (!d3d) return 1;
  D3DPRESENT_PARAMETERS pp = {0};
  pp.BackBufferWidth = 1024; pp.BackBufferHeight = 768;
  pp.BackBufferFormat = D3DFMT_X8R8G8B8; pp.BackBufferCount = 1;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = hwnd;
  pp.Windowed = TRUE; pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
  CHECK(IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev));

  static const D3DVERTEXELEMENT9 elems[] = {
    { 0, 0,  D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
    { 0, 16, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
    D3DDECL_END()
  };
  CHECK(IDirect3DDevice9_CreateVertexDeclaration(dev, elems, &decl));
  CHECK(IDirect3DDevice9_CreateVertexShader(dev, k_vs, &vsMain));
  CHECK(IDirect3DDevice9_CreateVertexShader(dev, k_vs_unhook, &vsUnhook));
  CHECK(IDirect3DDevice9_CreatePixelShader(dev, k_ps_color, &psColor));
  CHECK(IDirect3DDevice9_CreatePixelShader(dev, k_ps_depth_const, &psDepthConst));
  CHECK(IDirect3DDevice9_CreatePixelShader(dev, k_ps_depth_interp, &psDepthInterp));

  int have4x = SUCCEEDED(IDirect3D9_CheckDeviceMultiSampleType(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
                 D3DFMT_X8R8G8B8, TRUE, D3DMULTISAMPLE_4_SAMPLES, NULL))
            && SUCCEEDED(IDirect3D9_CheckDeviceMultiSampleType(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
                 D3DFMT_D24S8, TRUE, D3DMULTISAMPLE_4_SAMPLES, NULL))
            && SUCCEEDED(IDirect3D9_CheckDeviceMultiSampleType(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
                 D3DFMT_D16, TRUE, D3DMULTISAMPLE_4_SAMPLES, NULL));

  for (int ms = 0; ms < (have4x ? 2 : 1); ms++) {
    D3DMULTISAMPLE_TYPE t = ms ? D3DMULTISAMPLE_4_SAMPLES : D3DMULTISAMPLE_NONE;
    CHECK(IDirect3DDevice9_CreateRenderTarget(dev, RT_W, RT_H, D3DFMT_X8R8G8B8, t, 0, FALSE, &rt[ms], NULL));
    CHECK(IDirect3DDevice9_CreateDepthStencilSurface(dev, RT_W, RT_H, D3DFMT_D24S8, t, 0, FALSE, &ds[ms], NULL));
    CHECK(IDirect3DDevice9_CreateDepthStencilSurface(dev, RT_W, RT_H, D3DFMT_D16, t, 0, FALSE, &ds16[ms], NULL));
  }
  CHECK(IDirect3DDevice9_CreateRenderTarget(dev, RT_W, RT_H, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &resolved, NULL));
  CHECK(IDirect3DDevice9_CreateOffscreenPlainSurface(dev, RT_W, RT_H, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sysmem, NULL));
  CHECK(IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &backbuffer));

  /* user clip planes for rows U (clip space, keep y <= Y_i * w) */
  for (int i = 0; i < 6; i++) {
    const float pl[4] = { 0.0f, -1.0f, 0.0f, 0.5f + 0.1f * (5 - i) };
    IDirect3DDevice9_SetClipPlane(dev, i, pl);
  }

  build();
  printf("D9MT-DEPTHBIAS cells=%d 4x=%s unhook=%d no_depthbias=%d\n", ncells, have4x ? "yes" : "no",
         unhookActive, noDepthBiasRun);
  validate_fixture();

  /* D9MT_DEBUG_FILE is polled once a second: the no_depthbias run renders after ~2 s */
  const int renderFrame = noDepthBiasRun ? 120 : 0;
  int frames = 0;
  MSG msg;
  for (;;) {
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) goto done;
      TranslateMessage(&msg);
      DispatchMessageA(&msg);
    }
    IDirect3DDevice9_BeginScene(dev);
    if (frames == renderFrame) {
      for (int ms = 0; ms < (have4x ? 2 : 1); ms++)
        render_pass(ms);
    }
    IDirect3DDevice9_EndScene(dev);
    if (frames == renderFrame) {
      for (int ms = 0; ms < (have4x ? 2 : 1); ms++)
        check_pass(ms);
    }
    IDirect3DDevice9_SetRenderTarget(dev, 0, backbuffer);
    IDirect3DDevice9_StretchRect(dev, rt[0], NULL, backbuffer, NULL, D3DTEXF_POINT);
    IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
    if (++frames == 3) printf("3 frames\n");
  }
done:
  IDirect3DDevice9_Release(dev);
  IDirect3D9_Release(d3d);
  return 0;
}
