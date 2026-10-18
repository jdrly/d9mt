/* Vertex buffers of sizes around 4K, 16K and 64K page boundaries, through each
 * of d9mt's buffer paths: the CPU-visible ones are host memory handed to Metal
 * with newBufferWithBytesNoCopy at the buffer's exact size, on 4K pages
 * (Rosetta) or 16K pages (arm64). The only triangle is the last one in the
 * buffer: if the GPU doesn't see the buffer's last bytes, the center isn't
 * green. Prints one CASE line per size and path. */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <string.h>

struct Vertex { float x, y, z, rhw; DWORD color; };

static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
  return DefWindowProcA(h, m, w, l);
}

static DWORD read_center(IDirect3DDevice9 *dev) {
  IDirect3DSurface9 *rt = NULL, *sys = NULL;
  D3DSURFACE_DESC desc;
  D3DLOCKED_RECT lr;
  DWORD c = 0xdeadbeef;
  if (FAILED(IDirect3DDevice9_GetRenderTarget(dev, 0, &rt))) return c;
  IDirect3DSurface9_GetDesc(rt, &desc);
  if (SUCCEEDED(IDirect3DDevice9_CreateOffscreenPlainSurface(
          dev, desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &sys, NULL))) {
    if (SUCCEEDED(IDirect3DDevice9_GetRenderTargetData(dev, rt, sys)) &&
        SUCCEEDED(IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY))) {
      c = ((DWORD *)lr.pBits)[(desc.Height / 2) * lr.Pitch / 4 + desc.Width / 2] & 0xffffff;
      IDirect3DSurface9_UnlockRect(sys);
    }
    IDirect3DSurface9_Release(sys);
  }
  IDirect3DSurface9_Release(rt);
  return c;
}

int main(void) {
  static const UINT sizes[] = { 60, 4096, 4100, 8192, 16380, 16384, 16400, 20480, 32768 + 20,
                                65536, 65540, 69632, 1048576 + 4096 + 20 };
  static const struct { const char *name; DWORD usage; D3DPOOL pool; } paths[] = {
    { "default-wo", D3DUSAGE_WRITEONLY, D3DPOOL_DEFAULT },
    { "default-rw", 0, D3DPOOL_DEFAULT },
    { "dynamic-wo", D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, D3DPOOL_DEFAULT },
    { "managed", 0, D3DPOOL_MANAGED },
  };
  WNDCLASSA wc = {0};
  int fails = 0;
  wc.lpfnWndProc = wnd_proc;
  wc.hInstance = GetModuleHandleA(NULL);
  wc.lpszClassName = "d9mt_vbsizes";
  RegisterClassA(&wc);
  HWND hwnd = CreateWindowA(wc.lpszClassName, "d9mt vbsizes", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                            CW_USEDEFAULT, CW_USEDEFAULT, 320, 240, NULL, NULL, wc.hInstance, NULL);
  IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
  D3DPRESENT_PARAMETERS pp = {0};
  IDirect3DDevice9 *dev = NULL;
  if (!hwnd || !d3d) { printf("FAIL init\n"); return 1; }
  pp.BackBufferWidth = 64;
  pp.BackBufferHeight = 64;
  pp.BackBufferFormat = D3DFMT_X8R8G8B8;
  pp.BackBufferCount = 1;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.hDeviceWindow = hwnd;
  pp.Windowed = TRUE;
  pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
  if (FAILED(IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                     D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev)) || !dev) {
    printf("FAIL device\n");
    return 1;
  }
  IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);

  for (unsigned p = 0; p < sizeof(paths) / sizeof(paths[0]); p++) {
    for (unsigned s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
      UINT size = sizes[s], nverts = size / sizeof(struct Vertex), first = nverts - 3;
      IDirect3DVertexBuffer9 *vb = NULL;
      struct Vertex *v = NULL;
      DWORD c = 0xdeadbeef;
      if (FAILED(IDirect3DDevice9_CreateVertexBuffer(dev, size, paths[p].usage, 0, paths[p].pool, &vb, NULL)) || !vb) {
        printf("CASE %s size=%u FAIL create\n", paths[p].name, size);
        fails++;
        continue;
      }
      if (SUCCEEDED(IDirect3DVertexBuffer9_Lock(vb, 0, 0, (void **)&v, 0))) {
        /* everything red, then the last full triangle green and covering the center */
        for (UINT i = 0; i < nverts; i++) {
          v[i].x = 0.0f; v[i].y = 0.0f; v[i].z = 0.5f; v[i].rhw = 1.0f; v[i].color = 0xffff0000;
        }
        memset((char *)v + nverts * sizeof(struct Vertex), 0xcc, size - nverts * sizeof(struct Vertex));
        v[first + 0].x = -10.0f; v[first + 0].y = -10.0f;
        v[first + 1].x = 74.0f;  v[first + 1].y = -10.0f;
        v[first + 2].x = 32.0f;  v[first + 2].y = 74.0f;
        v[first + 0].color = v[first + 1].color = v[first + 2].color = 0xff00ff00;
        IDirect3DVertexBuffer9_Unlock(vb);
        IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 255), 1.0f, 0);
        IDirect3DDevice9_BeginScene(dev);
        IDirect3DDevice9_SetStreamSource(dev, 0, vb, 0, sizeof(struct Vertex));
        IDirect3DDevice9_DrawPrimitive(dev, D3DPT_TRIANGLELIST, first, 1);
        IDirect3DDevice9_EndScene(dev);
        c = read_center(dev);
      }
      IDirect3DDevice9_SetStreamSource(dev, 0, NULL, 0, 0);
      IDirect3DVertexBuffer9_Release(vb);
      printf("CASE %s size=%u center=%06lx %s\n", paths[p].name, size, c, c == 0x00ff00 ? "PASS" : "FAIL");
      if (c != 0x00ff00) fails++;
    }
  }
  printf("vbsizes fails=%d\n", fails);
  fflush(stdout);
  IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
  IDirect3DDevice9_Release(dev);
  IDirect3D9_Release(d3d);
  return 0;
}
