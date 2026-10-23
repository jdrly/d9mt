/* code_vb_upload_oob: SWTOR's vertex-range hint on a vertex buffer d9mt uploads per draw (#56).
 * The buffer is D3DPOOL_SYSTEMMEM + D3DUSAGE_DYNAMIC (768 vertices of 24 bytes), so every draw
 * copies its hinted vertex range from the buffer's CPU copy (UploadPerDrawData).
 * DrawIndexedPrimitive names MinVertexIndex 600, NumVertices 256: a hint past the buffer's end, as
 * SWTOR's are. Its indices, 600..700, stay inside. The clamp for a partly out-of-range copy used
 * to measure from the stream offset instead of the first vertex, so the copy read 600 × 24 bytes
 * past the CPU copy: a page fault where nothing follows. Fixed, it copies up to the buffer's end.
 * Five frames of green triangles; prints "RESULT vbupload frames=5", or "RESULT FAIL <call>" and
 * exits non-zero when a D3D9 call fails. */
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <string.h>

/* 24 bytes, as SWTOR's streams; FVF order: position, point size, then diffuse */
struct V { float x, y, z, rhw; float psize; DWORD c; };
#define FVF (D3DFVF_XYZRHW | D3DFVF_PSIZE | D3DFVF_DIFFUSE)
#define NV 768

#define TRY(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { \
    printf("RESULT FAIL %s: hr=%08lx\n", #x, (unsigned long)hr_); fflush(stdout); return 1; } } while (0)

int main(void) {
  WNDCLASSA wc = { 0 }; wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "vbup";
  RegisterClassA(&wc);
  HWND hwnd = CreateWindowA("vbup", "vbup", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 340, 280, NULL, NULL, wc.hInstance, NULL);
  IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
  if (!hwnd || !d3d) { printf("RESULT FAIL window/d3d\n"); return 1; }
  D3DPRESENT_PARAMETERS pp = { 0 };
  pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferFormat = D3DFMT_X8R8G8B8;
  pp.BackBufferWidth = 320; pp.BackBufferHeight = 240; pp.hDeviceWindow = hwnd;
  pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
  IDirect3DDevice9 *dev = NULL;
  TRY(IDirect3D9_CreateDevice(d3d, 0, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev));
  IDirect3DVertexBuffer9 *vb = NULL;
  IDirect3DIndexBuffer9 *ib = NULL;
  TRY(IDirect3DDevice9_CreateVertexBuffer(dev, NV * sizeof(struct V), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, FVF, D3DPOOL_SYSTEMMEM, &vb, NULL));
  TRY(IDirect3DDevice9_CreateIndexBuffer(dev, 300 * sizeof(WORD), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &ib, NULL));
  {
    struct V *v = NULL;
    TRY(IDirect3DVertexBuffer9_Lock(vb, 0, 0, (void **)&v, 0));
    for (int i = 0; i < NV; i++) {   /* the indices use vertices 600..700: a fan of triangles across the window */
      float a = (float)(i % 100) * 3.2f;
      v[i].x = 20.0f + a; v[i].y = (i % 2) ? 40.0f : 200.0f; v[i].z = 0.5f; v[i].rhw = 1.0f;
      v[i].psize = 1.0f; v[i].c = 0xff00c000u;
    }
    TRY(IDirect3DVertexBuffer9_Unlock(vb));
    WORD *x = NULL;
    TRY(IDirect3DIndexBuffer9_Lock(ib, 0, 0, (void **)&x, 0));
    for (int t = 0; t < 100; t++) { x[3 * t] = (WORD)(600 + t); x[3 * t + 1] = (WORD)(601 + t); x[3 * t + 2] = (WORD)(600 + (t + 2) % 100); }
    TRY(IDirect3DIndexBuffer9_Unlock(ib));
  }
  TRY(IDirect3DDevice9_SetFVF(dev, FVF));
  TRY(IDirect3DDevice9_SetStreamSource(dev, 0, vb, 0, sizeof(struct V)));
  TRY(IDirect3DDevice9_SetIndices(dev, ib));
  TRY(IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE));
  TRY(IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, FALSE));
  TRY(IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE));
  int frames = 0;
  for (; frames < 5; frames++) {
    TRY(IDirect3DDevice9_BeginScene(dev));
    TRY(IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xff202040, 1.0f, 0));
    /* hint: vertices 600..855 of a 768-vertex buffer; indices: 600..700 */
    TRY(IDirect3DDevice9_DrawIndexedPrimitive(dev, D3DPT_TRIANGLELIST, 0, 600, 256, 0, 100));
    TRY(IDirect3DDevice9_EndScene(dev));
    TRY(IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL));
  }
  printf("RESULT vbupload frames=%d\n", frames);
  fflush(stdout);
  IDirect3DIndexBuffer9_Release(ib);
  IDirect3DVertexBuffer9_Release(vb);
  IDirect3DDevice9_Release(dev);
  IDirect3D9_Release(d3d);
  return 0;
}
