/* code_flat_select: D3DSHADE_FLAT's function selection on the PE side (the 0.1.8 recompile
 * bundle, "flat shading on the PE side"). The AIR compiler doesn't honour flat yet, so every
 * frame must hash the same as base; the selection itself is checked through D9MT_FLAT_CHECK=1:
 *
 * One frame, one VS/PS pair reading COLOR0, SHADEMODE FLAT held, the draws alternating
 *   1 non-indexed triangle list   effective flat 1, flat PS
 *   2 indexed triangle fan        0 (fans stay Gouraud: Metal's provoking vertex would be the hub,
 *   3 non-indexed triangle fan    0  D3D9's is vertex t+1; until the fan index rotation, #170)
 *   4 indexed triangle list       1
 *   5 a PS that reads no COLOR    effective 1, Gouraud PS (flat changes only colour inputs)
 *   6 the reorder pair, FLAT      1: a pair that must go through the semantic and mismatched-I/O
 *                                    reconciliation: the VS writes only TEXCOORD0.xy (and an extra
 *                                    TEXCOORD2, in another order), the PS reads COLOR0, COLOR1,
 *                                    all of TEXCOORD0, TEXCOORD1_centroid and an unwritten TEXCOORD3
 *   then SHADEMODE GOURAUD:
 *   7 non-indexed triangle list   0
 *   8 the reorder pair            0
 *
 * Draw 2 changed the frames with #187. Before it, d9mt drew that indexed fan's four indices as a
 * triangle list, so only 1 of its 2 triangles, and the checks below never looked at the missing
 * one.
 *
 * D9MT_FLAT_CHECK=1 logs "D9MT-FLAT-DRAW topo=<D3DPT> indexed=<0|1> effective=<0|1> ps=sm
 * psflat=<0|1>" per draw (expected, frame 1: 1/1, 0/0, 0/0, 1/1, 1/0, 1/1, 0/0, 0/0) and one
 * "D9MT-FLAT-CHECK <ps> <flat|gouraud> <prev reconciled=0|1|standalone> ok (...)" per PS
 * function compiled. Its expectations come from the shader's original IR, not the flat mask:
 * inputs named by the semantic the original IR declares at their location; COLOR0/COLOR1
 * exactly flat in the flat IR and not in the Gouraud one; every other semantic (the centroid
 * TEXCOORD1 included) with its original interpolation in both. The reorder PS's two "prev"
 * lines must say reconciled=1 (the ordering evidence); its "ok" names the inputs the
 * reconciliation removed (TEXCOORD3).
 * Shader bytes: scripts/sm3_corpus.py assemble (the source is in each array's comment). */
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <string.h>

/* vs_3_0 | dcl_position v0 | dcl_color0 v1 | dcl_texcoord0 v2 | dcl_position o0 | dcl_color0 o1 | dcl_texcoord0 o2 | mov o0, v0 | mov o1, v1 | mov o2, v2 */
static const DWORD k_vs_color[] = {
  0xfffe0300, 0x0200001f, 0x80000000, 0x900f0000, 0x0200001f, 0x8000000a,
  0x900f0001, 0x0200001f, 0x80000005, 0x900f0002, 0x0200001f, 0x80000000,
  0xe00f0000, 0x0200001f, 0x8000000a, 0xe00f0001, 0x0200001f, 0x80000005,
  0xe00f0002, 0x02000001, 0xe00f0000, 0x90e40000, 0x02000001, 0xe00f0001,
  0x90e40001, 0x02000001, 0xe00f0002, 0x90e40002, 0x0000ffff,
};
/* ps_3_0 | dcl_color0 v0 | mov oC0, v0 */
static const DWORD k_ps_color[] = {
  0xffff0300, 0x0200001f, 0x8000000a, 0x900f0000, 0x02000001, 0x800f0800,
  0x90e40000, 0x0000ffff,
};
/* ps_3_0 | dcl_texcoord0 v0 | mov oC0, v0 */
static const DWORD k_ps_nocolor[] = {
  0xffff0300, 0x0200001f, 0x80000005, 0x900f0000, 0x02000001, 0x800f0800,
  0x90e40000, 0x0000ffff,
};
/* vs_3_0 | dcl_position v0 | dcl_color0 v1 | dcl_texcoord0 v2 | dcl_position o0 | dcl_texcoord2 o1 | dcl_texcoord1 o2 | dcl_color1 o3 | dcl_texcoord0 o4.xy | dcl_color0 o5 | mov o0, v0 | mov o1, v2 | mov o2, v2.yxwz | mov o3, v1.zyxw | mov o4.xy, v2 | mov o5, v1 */
static const DWORD k_vs_reorder[] = {
  0xfffe0300, 0x0200001f, 0x80000000, 0x900f0000, 0x0200001f, 0x8000000a,
  0x900f0001, 0x0200001f, 0x80000005, 0x900f0002, 0x0200001f, 0x80000000,
  0xe00f0000, 0x0200001f, 0x80020005, 0xe00f0001, 0x0200001f, 0x80010005,
  0xe00f0002, 0x0200001f, 0x8001000a, 0xe00f0003, 0x0200001f, 0x80000005,
  0xe0030004, 0x0200001f, 0x8000000a, 0xe00f0005, 0x02000001, 0xe00f0000,
  0x90e40000, 0x02000001, 0xe00f0001, 0x90e40002, 0x02000001, 0xe00f0002,
  0x90b10002, 0x02000001, 0xe00f0003, 0x90c60001, 0x02000001, 0xe0030004,
  0x90e40002, 0x02000001, 0xe00f0005, 0x90e40001, 0x0000ffff,
};
/* ps_3_0 | def c0, 0.25, 0.25, 0.25, 0 | dcl_color0 v0 | dcl_color1 v1 | dcl_texcoord0 v2 | dcl_texcoord1_centroid v3 | dcl_texcoord3 v4 | mul r0, v0, v1 | add r0, r0, v2 | add r0, r0, v4 | mad oC0, v3, c0, r0 */
static const DWORD k_ps_reorder[] = {
  0xffff0300, 0x05000051, 0xa00f0000, 0x3e800000, 0x3e800000, 0x3e800000,
  0x00000000, 0x0200001f, 0x8000000a, 0x900f0000, 0x0200001f, 0x8001000a,
  0x900f0001, 0x0200001f, 0x80000005, 0x900f0002, 0x0200001f, 0x80010005,
  0x904f0003, 0x0200001f, 0x80030005, 0x900f0004, 0x03000005, 0x800f0000,
  0x90e40000, 0x90e40001, 0x03000002, 0x800f0000, 0x80e40000, 0x90e40002,
  0x03000002, 0x800f0000, 0x80e40000, 0x90e40004, 0x04000004, 0x800f0800,
  0x90e40003, 0xa0e40000, 0x80e40000, 0x0000ffff,
};

struct V { float x, y, z, w; DWORD c; float t[4]; };

static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
  return DefWindowProcA(h, m, w, l);
}

/* the 4 corners of region r (4 x 2 grid), one colour each */
static void corners(int r, struct V out[4]) {
  static const DWORD col[4] = { 0xffff0000, 0xff00ff00, 0xff0000ff, 0xffffffff };
  const float x0 = -1.0f + 0.5f * (r % 4) + 0.05f, x1 = x0 + 0.4f;
  const float y0 = 1.0f - 1.0f * (r / 4) - 0.1f, y1 = y0 - 0.8f;
  const float xs[4] = { x0, x1, x0, x1 }, ys[4] = { y0, y0, y1, y1 };
  for (int i = 0; i < 4; i++) {
    struct V v = { xs[i], ys[i], 0.5f, 1.0f, col[i], { 0.1f * i, 0.2f, 0.3f + 0.1f * r, 1.0f } };
    out[i] = v;
  }
}

int main(void) {
  WNDCLASSA wc = {0};
  wc.lpfnWndProc = wnd_proc;
  wc.hInstance = GetModuleHandleA(NULL);
  wc.lpszClassName = "d9mt_flat_select";
  RegisterClassA(&wc);
  RECT rc = {0, 0, 640, 480};
  AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
  HWND hwnd = CreateWindowA("d9mt_flat_select", "d9mt flat selection", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                            CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, NULL, NULL, wc.hInstance, NULL);
  if (!hwnd) return 1;
  IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
  if (!d3d) return 1;
  D3DPRESENT_PARAMETERS pp = {0};
  pp.BackBufferWidth = 640; pp.BackBufferHeight = 480;
  pp.BackBufferFormat = D3DFMT_X8R8G8B8; pp.BackBufferCount = 1;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = hwnd;
  pp.Windowed = TRUE; pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
  IDirect3DDevice9 *dev;
  if (FAILED(IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                     D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev)))
    return 1;

  static const D3DVERTEXELEMENT9 elems[] = {
    { 0, 0,  D3DDECLTYPE_FLOAT4,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
    { 0, 16, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR,    0 },
    { 0, 20, D3DDECLTYPE_FLOAT4,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
    D3DDECL_END()
  };
  IDirect3DVertexDeclaration9 *decl;
  IDirect3DVertexShader9 *vsColor, *vsReorder;
  IDirect3DPixelShader9 *psColor, *psNoColor, *psReorder;
  if (FAILED(IDirect3DDevice9_CreateVertexDeclaration(dev, elems, &decl))
   || FAILED(IDirect3DDevice9_CreateVertexShader(dev, k_vs_color, &vsColor))
   || FAILED(IDirect3DDevice9_CreateVertexShader(dev, k_vs_reorder, &vsReorder))
   || FAILED(IDirect3DDevice9_CreatePixelShader(dev, k_ps_color, &psColor))
   || FAILED(IDirect3DDevice9_CreatePixelShader(dev, k_ps_nocolor, &psNoColor))
   || FAILED(IDirect3DDevice9_CreatePixelShader(dev, k_ps_reorder, &psReorder))) {
    printf("FAIL: shader or declaration creation\n");
    return 1;
  }

  /* VB: region 0 list (6), 1 indexed fan (4), 2 fan (4), 3 indexed list (4), 4..7 lists (6 each) */
  struct V verts[6 + 4 + 4 + 4 + 4 * 6];
  int n = 0;
  int first[8];
  for (int r = 0; r < 8; r++) {
    struct V c[4];
    corners(r, c);
    first[r] = n;
    if (r == 1 || r == 2) {        /* fans: around the quad */
      verts[n++] = c[0]; verts[n++] = c[1]; verts[n++] = c[3]; verts[n++] = c[2];
    } else if (r == 3) {           /* indexed list: the corners */
      for (int i = 0; i < 4; i++) verts[n++] = c[i];
    } else {                       /* lists */
      verts[n++] = c[0]; verts[n++] = c[1]; verts[n++] = c[2];
      verts[n++] = c[2]; verts[n++] = c[1]; verts[n++] = c[3];
    }
  }
  static const WORD idx[10] = { 0, 1, 2, 3,  0, 1, 2, 2, 1, 3 };
  IDirect3DVertexBuffer9 *vb;
  IDirect3DIndexBuffer9 *ib;
  void *p;
  if (FAILED(IDirect3DDevice9_CreateVertexBuffer(dev, sizeof(verts), D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &vb, NULL))
   || FAILED(IDirect3DDevice9_CreateIndexBuffer(dev, sizeof(idx), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &ib, NULL)))
    return 1;
  IDirect3DVertexBuffer9_Lock(vb, 0, 0, &p, 0); memcpy(p, verts, sizeof(verts)); IDirect3DVertexBuffer9_Unlock(vb);
  IDirect3DIndexBuffer9_Lock(ib, 0, 0, &p, 0); memcpy(p, idx, sizeof(idx)); IDirect3DIndexBuffer9_Unlock(ib);

  IDirect3DDevice9_SetVertexDeclaration(dev, decl);
  IDirect3DDevice9_SetStreamSource(dev, 0, vb, 0, sizeof(struct V));
  IDirect3DDevice9_SetIndices(dev, ib);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, FALSE);

  int frames = 0;
  MSG msg;
  for (;;) {
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) goto done;
      TranslateMessage(&msg);
      DispatchMessageA(&msg);
    }
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(16, 16, 24), 1.0f, 0);
    IDirect3DDevice9_BeginScene(dev);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_SHADEMODE, D3DSHADE_FLAT);
    IDirect3DDevice9_SetVertexShader(dev, vsColor);
    IDirect3DDevice9_SetPixelShader(dev, psColor);
    IDirect3DDevice9_DrawPrimitive(dev, D3DPT_TRIANGLELIST, first[0], 2);                    /* 1 */
    IDirect3DDevice9_DrawIndexedPrimitive(dev, D3DPT_TRIANGLEFAN, first[1], 0, 4, 0, 2);     /* 2 */
    IDirect3DDevice9_DrawPrimitive(dev, D3DPT_TRIANGLEFAN, first[2], 2);                     /* 3 */
    IDirect3DDevice9_DrawIndexedPrimitive(dev, D3DPT_TRIANGLELIST, first[3], 0, 4, 4, 2);    /* 4 */
    IDirect3DDevice9_SetPixelShader(dev, psNoColor);
    IDirect3DDevice9_DrawPrimitive(dev, D3DPT_TRIANGLELIST, first[4], 2);                    /* 5 */
    IDirect3DDevice9_SetVertexShader(dev, vsReorder);
    IDirect3DDevice9_SetPixelShader(dev, psReorder);
    IDirect3DDevice9_DrawPrimitive(dev, D3DPT_TRIANGLELIST, first[5], 2);                    /* 6 */
    IDirect3DDevice9_SetRenderState(dev, D3DRS_SHADEMODE, D3DSHADE_GOURAUD);
    IDirect3DDevice9_SetVertexShader(dev, vsColor);
    IDirect3DDevice9_SetPixelShader(dev, psColor);
    IDirect3DDevice9_DrawPrimitive(dev, D3DPT_TRIANGLELIST, first[6], 2);                    /* 7 */
    IDirect3DDevice9_SetVertexShader(dev, vsReorder);
    IDirect3DDevice9_SetPixelShader(dev, psReorder);
    IDirect3DDevice9_DrawPrimitive(dev, D3DPT_TRIANGLELIST, first[7], 2);                    /* 8 */
    IDirect3DDevice9_EndScene(dev);
    IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
    if (++frames == 3) printf("3 frames\n");
  }
done:
  IDirect3DDevice9_Release(dev);
  IDirect3D9_Release(d3d);
  return 0;
}
