/* Draw bounds (#61). A non-indexed draw that names more vertices than its vertex buffer holds
 * must fetch only the vertices inside it: Metal 4's vertex fetch has no bounds.
 * Two D3DPOOL_SYSTEMMEM + D3DUSAGE_DYNAMIC buffers of 48 vertices (960 bytes, a whole number of
 * cache lines), which d9mt copies per draw into consecutive upload slices:
 *  1. green triangles at the top, drawn with DrawPrimitive(TRIANGLELIST, 0, 32): 96 vertices
 *     named, 48 there;
 *  2. red triangles at the bottom, drawn with colour writes off: they must not show.
 * The first draw's vertices 48..95 lie past its slice, where the second draw's copy follows.
 * d9mt drew them, so red triangles showed at the bottom; bounded, the draw stops at vertex 47.
 * Five frames only: past the upload buffer's end (after ~540 frames) the unbounded read would
 * leave mapped memory. */
#include "t_xfer.h"

struct V { float x, y, z, rhw; DWORD c; };   /* 20 bytes */
#define FVF (D3DFVF_XYZRHW | D3DFVF_DIFFUSE)
#define NV 48

static IDirect3DVertexBuffer9 *top, *bottom;

static void row(struct V *v, float y0, DWORD c) {   /* 16 triangles of 30 x 40 px in a row */
  for (int t = 0; t < NV / 3; t++) {
    const float x0 = 20.0f + 38.0f * (float)t;
    const struct V tri[3] = {{x0, y0, 0.5f, 1, c}, {x0 + 30, y0, 0.5f, 1, c}, {x0, y0 + 40, 0.5f, 1, c}};
    memcpy(&v[3 * t], tri, sizeof(tri));
  }
}

static IDirect3DVertexBuffer9 *make(float y0, DWORD c) {
  IDirect3DVertexBuffer9 *vb = NULL;
  struct V *v = NULL;
  X_CHECK(IDirect3DDevice9_CreateVertexBuffer(x_dev, NV * sizeof(struct V), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
                                              FVF, D3DPOOL_SYSTEMMEM, &vb, NULL));
  X_CHECK(IDirect3DVertexBuffer9_Lock(vb, 0, 0, (void **)&v, 0));
  row(v, y0, c);
  IDirect3DVertexBuffer9_Unlock(vb);
  return vb;
}

static void frame(int n) {
  IDirect3DDevice9_SetRenderTarget(x_dev, 0, x_backBuffer);
  IDirect3DDevice9_Clear(x_dev, 0, NULL, D3DCLEAR_TARGET, 0xff000080u, 1.0f, 0);
  IDirect3DDevice9_BeginScene(x_dev);
  IDirect3DDevice9_SetRenderState(x_dev, D3DRS_COLORWRITEENABLE, 0xf);
  IDirect3DDevice9_SetStreamSource(x_dev, 0, top, 0, sizeof(struct V));
  IDirect3DDevice9_DrawPrimitive(x_dev, D3DPT_TRIANGLELIST, 0, 32);
  IDirect3DDevice9_SetRenderState(x_dev, D3DRS_COLORWRITEENABLE, 0);
  IDirect3DDevice9_SetStreamSource(x_dev, 0, bottom, 0, sizeof(struct V));
  IDirect3DDevice9_DrawPrimitive(x_dev, D3DPT_TRIANGLELIST, 0, NV / 3);
  IDirect3DDevice9_SetRenderState(x_dev, D3DRS_COLORWRITEENABLE, 0xf);
  IDirect3DDevice9_EndScene(x_dev);
  if (n == 1) {
    XReadback r = x_read(x_backBuffer);
    x_expect("the in-range triangles: first", &r, 26, 60, 0xff00ff00u, 0x00ffffffu);
    x_expect("the in-range triangles: last", &r, 20 + 38 * 15 + 6, 60, 0xff00ff00u, 0x00ffffffu);
    x_expect("past the buffer: nothing (first)", &r, 26, 360, 0xff000080u, 0x00ffffffu);
    x_expect("past the buffer: nothing (last)", &r, 20 + 38 * 15 + 6, 360, 0xff000080u, 0x00ffffffu);
    x_done(&r);
    x_report("code_draw_bounds");
  }
}

int main(void) {
  x_init("d9mt_draw_bounds");
  IDirect3DDevice9_SetVertexShader(x_dev, NULL);
  IDirect3DDevice9_SetPixelShader(x_dev, NULL);
  IDirect3DDevice9_SetFVF(x_dev, FVF);
  IDirect3DDevice9_SetRenderState(x_dev, D3DRS_ALPHABLENDENABLE, FALSE);
  IDirect3DDevice9_SetRenderState(x_dev, D3DRS_LIGHTING, FALSE);
  top = make(40.0f, 0xff00ff00u);
  bottom = make(340.0f, 0xffff0000u);
  MSG msg;
  for (int n = 1; n <= 5; n++) {
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageA(&msg);
    }
    frame(n);
    IDirect3DDevice9_Present(x_dev, NULL, NULL, NULL, NULL);
  }
  return 0;
}
