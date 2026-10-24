/* Vertex step functions (#60). Metal 4's vertex fetch has no bounds, so a stream must never
 * step further than D3D9 does.
 *  1. Stride 0: every vertex reads the stream's first element. A quad (fixed function, D3DCOLOR
 *     from stream 1 at stride 0) whose colour buffer holds green, then red: the whole quad is
 *     green. d9mt walked the stream with the declaration's size, so vertices 1-3 read red.
 *  2. The instance divisor: four instances of a quad (vs_3_0), the per-instance stream at
 *     D3DSTREAMSOURCE_INSTANCEDATA | 2, so instances 0-1 read element 0 (red, offset 0) and 2-3
 *     element 1 (green, offset 0.3). Elements 2 and 3 (blue at offset 0, yellow at 0.6) must not
 *     be read. d9mt stepped every instance, so instance 2 drew blue over the red quad and
 *     instance 3 a yellow one. */
#include "t_xfer.h"

/* vs_3_0: dcl_position v0, dcl_color v1, dcl_texcoord v2, dcl_position o0, dcl_color o1;
 * add o0, v0, v2; mov o1, v1 */
static const DWORD step_vs[] = {
  0xfffe0300u,
  0x0200001fu, 0x80000000u, 0x900f0000u,
  0x0200001fu, 0x8000000au, 0x900f0001u,
  0x0200001fu, 0x80000005u, 0x900f0002u,
  0x0200001fu, 0x80000000u, 0xe00f0000u,
  0x0200001fu, 0x8000000au, 0xe00f0001u,
  0x03000002u, 0xe00f0000u, 0x90e40000u, 0x90e40002u,
  0x02000001u, 0xe00f0001u, 0x90e40001u,
  0x0000ffffu };
/* ps_3_0: dcl_color v0; mov oC0, v0 */
static const DWORD step_ps[] = {
  0xffff0300u,
  0x0200001fu, 0x8000000au, 0x900f0000u,
  0x02000001u, 0x800f0800u, 0x90e40000u,
  0x0000ffffu };

static IDirect3DVertexDeclaration9 *ffDecl, *instDecl;
static IDirect3DVertexShader9 *stepVs;
static IDirect3DPixelShader9 *stepPs;
static IDirect3DVertexBuffer9 *ffPos, *ffColor, *quad, *inst;
static IDirect3DIndexBuffer9 *quadIdx;

static void *fill(IDirect3DVertexBuffer9 *vb) {
  void *p = NULL;
  X_CHECK(IDirect3DVertexBuffer9_Lock(vb, 0, 0, &p, 0));
  return p;
}

static void setup(void) {
  static const D3DVERTEXELEMENT9 ff[] = {
      {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITIONT, 0},
      {1, 0, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
      D3DDECL_END()};
  static const D3DVERTEXELEMENT9 in[] = {
      {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
      {1, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
      {1, 16, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
      D3DDECL_END()};
  X_CHECK(IDirect3DDevice9_CreateVertexDeclaration(x_dev, ff, &ffDecl));
  X_CHECK(IDirect3DDevice9_CreateVertexDeclaration(x_dev, in, &instDecl));
  X_CHECK(IDirect3DDevice9_CreateVertexShader(x_dev, step_vs, &stepVs));
  X_CHECK(IDirect3DDevice9_CreatePixelShader(x_dev, step_ps, &stepPs));

  /* 1: a quad at 40..200 px; colour element 0 green, elements 1-15 red */
  X_CHECK(IDirect3DDevice9_CreateVertexBuffer(x_dev, 4 * 16, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &ffPos, NULL));
  X_CHECK(IDirect3DDevice9_CreateVertexBuffer(x_dev, 16 * 4, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &ffColor, NULL));
  float *p = (float *)fill(ffPos);
  const float pos[16] = {40, 40, 0.5f, 1, 200, 40, 0.5f, 1, 40, 200, 0.5f, 1, 200, 200, 0.5f, 1};
  memcpy(p, pos, sizeof(pos));
  IDirect3DVertexBuffer9_Unlock(ffPos);
  DWORD *c = (DWORD *)fill(ffColor);
  for (int i = 0; i < 16; i++)
    c[i] = i ? 0xffff0000u : 0xff00ff00u;
  IDirect3DVertexBuffer9_Unlock(ffColor);

  /* 2: a quad at clip x 0..0.2, y -0.9..-0.6, and four instance elements of 20 bytes */
  X_CHECK(IDirect3DDevice9_CreateVertexBuffer(x_dev, 4 * 16, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &quad, NULL));
  X_CHECK(IDirect3DDevice9_CreateVertexBuffer(x_dev, 4 * 20, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &inst, NULL));
  X_CHECK(IDirect3DDevice9_CreateIndexBuffer(x_dev, 6 * sizeof(WORD), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &quadIdx, NULL));
  p = (float *)fill(quad);
  const float q[16] = {0, -0.6f, 0.5f, 1, 0.2f, -0.6f, 0.5f, 1, 0, -0.9f, 0.5f, 1, 0.2f, -0.9f, 0.5f, 1};
  memcpy(p, q, sizeof(q));
  IDirect3DVertexBuffer9_Unlock(quad);
  struct I { float ox, oy, oz, ow; DWORD c; } *e = (struct I *)fill(inst);
  const struct I el[4] = {{0, 0, 0, 0, 0xffff0000u}, {0.3f, 0, 0, 0, 0xff00ff00u},
                          {0, 0, 0, 0, 0xff0000ffu}, {0.6f, 0, 0, 0, 0xffffff00u}};
  memcpy(e, el, sizeof(el));
  IDirect3DVertexBuffer9_Unlock(inst);
  WORD *x = NULL;
  X_CHECK(IDirect3DIndexBuffer9_Lock(quadIdx, 0, 0, (void **)&x, 0));
  const WORD idx[6] = {0, 1, 2, 2, 1, 3};
  memcpy(x, idx, sizeof(idx));
  IDirect3DIndexBuffer9_Unlock(quadIdx);
}

static void frame(int n) {
  IDirect3DDevice9_SetRenderTarget(x_dev, 0, x_backBuffer);
  IDirect3DDevice9_Clear(x_dev, 0, NULL, D3DCLEAR_TARGET, 0xff000080u, 1.0f, 0);
  IDirect3DDevice9_BeginScene(x_dev);

  IDirect3DDevice9_SetVertexShader(x_dev, NULL);
  IDirect3DDevice9_SetPixelShader(x_dev, NULL);
  IDirect3DDevice9_SetVertexDeclaration(x_dev, ffDecl);
  IDirect3DDevice9_SetStreamSource(x_dev, 0, ffPos, 0, 16);
  IDirect3DDevice9_SetStreamSource(x_dev, 1, ffColor, 0, 0);
  IDirect3DDevice9_DrawPrimitive(x_dev, D3DPT_TRIANGLESTRIP, 0, 2);

  IDirect3DDevice9_SetVertexShader(x_dev, stepVs);
  IDirect3DDevice9_SetPixelShader(x_dev, stepPs);
  IDirect3DDevice9_SetVertexDeclaration(x_dev, instDecl);
  IDirect3DDevice9_SetStreamSource(x_dev, 0, quad, 0, 16);
  IDirect3DDevice9_SetStreamSource(x_dev, 1, inst, 0, 20);
  IDirect3DDevice9_SetStreamSourceFreq(x_dev, 0, D3DSTREAMSOURCE_INDEXEDDATA | 4u);
  IDirect3DDevice9_SetStreamSourceFreq(x_dev, 1, D3DSTREAMSOURCE_INSTANCEDATA | 2u);
  IDirect3DDevice9_SetIndices(x_dev, quadIdx);
  IDirect3DDevice9_DrawIndexedPrimitive(x_dev, D3DPT_TRIANGLELIST, 0, 0, 4, 0, 2);
  IDirect3DDevice9_SetStreamSourceFreq(x_dev, 0, 1);
  IDirect3DDevice9_SetStreamSourceFreq(x_dev, 1, 1);
  IDirect3DDevice9_EndScene(x_dev);

  if (n == 1) {
    XReadback r = x_read(x_backBuffer);
    x_expect("stride 0: the quad's first corner", &r, 50, 50, 0xff00ff00u, 0x00ffffffu);
    x_expect("stride 0: the quad's centre", &r, 120, 120, 0xff00ff00u, 0x00ffffffu);
    x_expect("stride 0: the quad's last corner", &r, 190, 190, 0xff00ff00u, 0x00ffffffu);
    x_expect("divisor 2: instances 0-1 (red at 0, not instance 2's blue)", &r, 352, 420, 0xffff0000u, 0x00ffffffu);
    x_expect("divisor 2: instances 2-3 (green at 0.3)", &r, 448, 420, 0xff00ff00u, 0x00ffffffu);
    x_expect("divisor 2: nothing at 0.6 (element 3 unread)", &r, 544, 420, 0xff000080u, 0x00ffffffu);
    x_expect("outside", &r, 600, 40, 0xff000080u, 0x00ffffffu);
    x_done(&r);
    x_report("code_vb_step");
  }
}

int main(void) {
  x_init("d9mt_vb_step");
  IDirect3DDevice9_SetRenderState(x_dev, D3DRS_ALPHABLENDENABLE, FALSE);
  IDirect3DDevice9_SetRenderState(x_dev, D3DRS_LIGHTING, FALSE);
  setup();
  return x_loop(frame);
}
