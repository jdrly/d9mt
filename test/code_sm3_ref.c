/* SM3 shader harness for the reference tests (scripts/run-sm3-ref.sh).
 *
 *   code_sm3_ref.exe <manifest> [first case]
 *
 * The manifest has one item per line (# starts a comment):
 *
 *   create <file.bin>
 *       Create the shader (VS or PS by its version token) and nothing else:
 *       with D9MT_COMPILE_THREADS=0 nothing gets compiled, so this exercises
 *       creation-time checks only. Prints "CREATED <n> <file> <hr>".
 *
 *   case <name> <vs.bin> <ps.bin> <stride> <elements> {<usage> <index> <type> <offset>}...
 *        <floats> <bits>...
 *       Draw one point (size 4, so it covers pixel (1,1) of the 4x4 targets)
 *       with the vertex declaration given as D3DDECLUSAGE, usage index,
 *       D3DDECLTYPE and byte offset per element, and one vertex of <floats>
 *       32-bit words (hex bit patterns). Four A32B32G32R32F render targets
 *       are bound and cleared to 0x01020304 first; prints "RESULT <n> <name>",
 *       the 16 words of pixel (1,1) of oC0..oC3 and the red word of oC0's
 *       pixel (3,3), which only points larger than 5 cover, as hex.
 *
 * Ends with "DONE <items> <ms>", the time the items took.
 *
 * Points have one vertex, so pixel shader inputs are the vertex shader's
 * outputs bit for bit (no interpolation). Every line is flushed: when a
 * shader crashes the driver, the runner restarts at the next case.
 */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SHADERS 32768
#define RT_COUNT 4

struct shader_entry {
  char path[260];
  IUnknown *obj;
  HRESULT hr;
};

static struct shader_entry g_shaders[MAX_SHADERS];
static unsigned g_shader_count;

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_DESTROY) {
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcA(hwnd, msg, wp, lp);
}

static DWORD *read_file(const char *path, size_t *size) {
  FILE *f = fopen(path, "rb");
  DWORD *data;
  long n;
  if (!f)
    return NULL;
  fseek(f, 0, SEEK_END);
  n = ftell(f);
  fseek(f, 0, SEEK_SET);
  data = malloc(n + 4);
  if (data && fread(data, 1, n, f) != (size_t)n) {
    free(data);
    data = NULL;
  }
  fclose(f);
  *size = n;
  return data;
}

/* Creates (once) and returns the shader object for a bytecode file. */
static struct shader_entry *get_shader(IDirect3DDevice9 *dev, const char *path) {
  unsigned i;
  size_t size = 0;
  DWORD *code;
  struct shader_entry *e;

  for (i = 0; i < g_shader_count; i++) {
    if (!strcmp(g_shaders[i].path, path))
      return &g_shaders[i];
  }
  if (g_shader_count == MAX_SHADERS)
    return NULL;

  e = &g_shaders[g_shader_count++];
  strncpy(e->path, path, sizeof(e->path) - 1);
  e->obj = NULL;
  e->hr = E_FAIL;

  code = read_file(path, &size);
  if (!code || size < 4) {
    free(code);
    return e;
  }
  if ((code[0] & 0xffff0000u) == 0xfffe0000u)
    e->hr = IDirect3DDevice9_CreateVertexShader(dev, code, (IDirect3DVertexShader9 **)&e->obj);
  else
    e->hr = IDirect3DDevice9_CreatePixelShader(dev, code, (IDirect3DPixelShader9 **)&e->obj);
  free(code);
  return e;
}

static char *next_token(char **p) {
  char *s = *p, *t;
  while (*s == ' ' || *s == '\t')
    s++;
  if (!*s || *s == '\n' || *s == '\r')
    return NULL;
  t = s;
  while (*s && *s != ' ' && *s != '\t' && *s != '\n' && *s != '\r')
    s++;
  if (*s)
    *s++ = '\0';
  *p = s;
  return t;
}

static int next_uint(char **p, unsigned long *v) {
  char *t = next_token(p);
  if (!t)
    return 0;
  *v = strtoul(t, NULL, 0);
  return 1;
}

static int run_case(IDirect3DDevice9 *dev, IDirect3DSurface9 **rts, IDirect3DSurface9 **sys,
                    unsigned n, char *rest) {
  D3DVERTEXELEMENT9 elems[17];
  IDirect3DVertexDeclaration9 *decl = NULL;
  struct shader_entry *vs, *ps;
  DWORD vertex[64];
  unsigned long stride, count, nfloats, v;
  DWORD corner = 0;
  char *name, *vsPath, *psPath;
  unsigned i;
  HRESULT hr;

  name = next_token(&rest);
  vsPath = next_token(&rest);
  psPath = next_token(&rest);
  if (!name || !vsPath || !psPath || !next_uint(&rest, &stride) || !next_uint(&rest, &count) || count > 16)
    return 0;

  for (i = 0; i < count; i++) {
    unsigned long usage, index, type, offset;
    if (!next_uint(&rest, &usage) || !next_uint(&rest, &index) || !next_uint(&rest, &type) || !next_uint(&rest, &offset))
      return 0;
    elems[i].Stream = 0;
    elems[i].Offset = (WORD)offset;
    elems[i].Type = (BYTE)type;
    elems[i].Method = D3DDECLMETHOD_DEFAULT;
    elems[i].Usage = (BYTE)usage;
    elems[i].UsageIndex = (BYTE)index;
  }
  {
    D3DVERTEXELEMENT9 end = D3DDECL_END();
    elems[count] = end;
  }
  if (!next_uint(&rest, &nfloats) || nfloats > 64)
    return 0;
  for (i = 0; i < nfloats; i++) {
    if (!next_uint(&rest, &v))
      return 0;
    vertex[i] = (DWORD)v;
  }

  vs = get_shader(dev, vsPath);
  ps = get_shader(dev, psPath);
  if (!vs || !ps || FAILED(vs->hr) || FAILED(ps->hr)) {
    printf("RESULT %u %s create-failed vs=%08lx ps=%08lx\n", n, name,
           vs ? (unsigned long)vs->hr : 0ul, ps ? (unsigned long)ps->hr : 0ul);
    fflush(stdout);
    return 1;
  }

  hr = IDirect3DDevice9_CreateVertexDeclaration(dev, elems, &decl);
  if (FAILED(hr)) {
    printf("RESULT %u %s decl-failed %08lx\n", n, name, (unsigned long)hr);
    fflush(stdout);
    return 1;
  }

  IDirect3DDevice9_SetVertexDeclaration(dev, decl);
  IDirect3DDevice9_SetVertexShader(dev, (IDirect3DVertexShader9 *)vs->obj);
  IDirect3DDevice9_SetPixelShader(dev, (IDirect3DPixelShader9 *)ps->obj);
  IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0x01020304, 1.0f, 0);
  IDirect3DDevice9_BeginScene(dev);
  hr = IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_POINTLIST, 1, vertex, stride);
  IDirect3DDevice9_EndScene(dev);

  printf("RESULT %u %s", n, name);
  for (i = 0; i < RT_COUNT; i++) {
    D3DLOCKED_RECT lr;
    const DWORD *px;
    HRESULT rhr = IDirect3DDevice9_GetRenderTargetData(dev, rts[i], sys[i]);
    if (FAILED(rhr) || FAILED(IDirect3DSurface9_LockRect(sys[i], &lr, NULL, D3DLOCK_READONLY))) {
      printf(" readback-failed");
      break;
    }
    px = (const DWORD *)((const BYTE *)lr.pBits + lr.Pitch) + 4;
    printf(" %08lx %08lx %08lx %08lx", (unsigned long)px[0], (unsigned long)px[1],
           (unsigned long)px[2], (unsigned long)px[3]);
    if (i == 0) {
      /* pixel (3,3): covered only by points larger than 5 */
      px = (const DWORD *)((const BYTE *)lr.pBits + 3 * lr.Pitch) + 12;
      corner = px[0];
    }
    IDirect3DSurface9_UnlockRect(sys[i]);
  }
  printf(" %08lx", (unsigned long)corner);
  if (FAILED(hr))
    printf(" draw-failed %08lx", (unsigned long)hr);
  printf("\n");
  fflush(stdout);

  IDirect3DDevice9_SetVertexDeclaration(dev, NULL);
  IDirect3DVertexDeclaration9_Release(decl);
  return 1;
}

int main(int argc, char **argv) {
  IDirect3DSurface9 *rts[RT_COUNT], *sys[RT_COUNT];
  unsigned first = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 0) : 0;
  unsigned n = 0, i;
  LARGE_INTEGER freq, t0, t1;
  char line[8192];
  FILE *manifest;

  if (argc < 2) {
    printf("usage: code_sm3_ref.exe <manifest> [first case]\n");
    return 2;
  }
  manifest = fopen(argv[1], "r");
  if (!manifest) {
    printf("FAIL cannot open %s\n", argv[1]);
    return 1;
  }

  WNDCLASSA wc = {0};
  wc.lpfnWndProc = wnd_proc;
  wc.hInstance = GetModuleHandleA(NULL);
  wc.lpszClassName = "d9mt_sm3_ref";
  RegisterClassA(&wc);

  HWND hwnd = CreateWindowA("d9mt_sm3_ref", "d9mt sm3 ref", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                            CW_USEDEFAULT, CW_USEDEFAULT, 320, 240, NULL, NULL, wc.hInstance, NULL);
  if (!hwnd) {
    printf("FAIL window\n");
    return 1;
  }

  IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
  if (!d3d) {
    printf("FAIL d3d9\n");
    return 1;
  }

  D3DPRESENT_PARAMETERS pp = {0};
  pp.BackBufferWidth = 64;
  pp.BackBufferHeight = 64;
  pp.BackBufferFormat = D3DFMT_A8R8G8B8;
  pp.BackBufferCount = 1;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.hDeviceWindow = hwnd;
  pp.Windowed = TRUE;
  pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

  IDirect3DDevice9 *dev = NULL;
  HRESULT hr = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                       D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
  if (FAILED(hr) || !dev) {
    printf("FAIL device %08lx\n", (unsigned long)hr);
    return 1;
  }

  for (i = 0; i < RT_COUNT; i++) {
    hr = IDirect3DDevice9_CreateRenderTarget(dev, 4, 4, D3DFMT_A32B32G32R32F, D3DMULTISAMPLE_NONE,
                                             0, FALSE, &rts[i], NULL);
    if (SUCCEEDED(hr))
      hr = IDirect3DDevice9_CreateOffscreenPlainSurface(dev, 4, 4, D3DFMT_A32B32G32R32F,
                                                        D3DPOOL_SYSTEMMEM, &sys[i], NULL);
    if (FAILED(hr)) {
      printf("FAIL render target %u %08lx\n", i, (unsigned long)hr);
      return 1;
    }
    IDirect3DDevice9_SetRenderTarget(dev, i, rts[i]);
  }

  {
    float pointSize = 4.0f;
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, D3DZB_FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_POINTSIZE, *(DWORD *)&pointSize);
  }

  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&t0);

  while (fgets(line, sizeof(line), manifest)) {
    char *rest = line, *kind;
    if (line[0] == '#')
      continue;
    kind = next_token(&rest);
    if (!kind)
      continue;

    if (!strcmp(kind, "create")) {
      char *path = next_token(&rest);
      struct shader_entry *e;
      if (!path)
        continue;
      if (n++ < first)
        continue;
      e = get_shader(dev, path);
      printf("CREATED %u %s %08lx\n", n - 1, path, e ? (unsigned long)e->hr : 0xffffffffu);
      fflush(stdout);
    } else if (!strcmp(kind, "case")) {
      if (n++ < first)
        continue;
      if (!run_case(dev, rts, sys, n - 1, rest)) {
        printf("RESULT %u bad-manifest-line\n", n - 1);
        fflush(stdout);
      }
    }
  }
  fclose(manifest);

  QueryPerformanceCounter(&t1);
  printf("DONE %u %.3f ms\n", n, (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart);
  fflush(stdout);

  for (i = 0; i < g_shader_count; i++) {
    if (g_shaders[i].obj)
      IUnknown_Release(g_shaders[i].obj);
  }
  for (i = 0; i < RT_COUNT; i++) {
    IDirect3DSurface9_Release(sys[i]);
    IDirect3DSurface9_Release(rts[i]);
  }
  IDirect3DDevice9_Release(dev);
  IDirect3D9_Release(d3d);
  return 0;
}
