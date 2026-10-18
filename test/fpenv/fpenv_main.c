/* x64 caller: for several MXCSR settings, what fp_probe (fpenv_dll.c, an x64
 * or ARM64EC DLL next to it) computes and sees, and whether the callee's own
 * rounding-mode change comes back in MXCSR. With "d3d9": the x87 control word
 * and MXCSR before and after IDirect3D9::CreateDevice (d3d9 normally sets up
 * the x87 FPU unless D3DCREATE_FPU_PRESERVE).
 *
 *   fpenv_main.exe [d3d9]
 *
 * Output lines are compared between engines/DLL builds by scripts. */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <xmmintrin.h>

struct fp_probe {
  uint32_t env, quot_bits, neg_quot_bits;
  int32_t rint_pos, rint_neg;
  uint32_t denorm_in, denorm_out;
  int native;
};
typedef void (*probe_fn)(volatile const float *, struct fp_probe *);
typedef void (*set_fn)(void);

static uint16_t x87_cw(void) { uint16_t cw; __asm__ volatile("fnstcw %0" : "=m"(cw)); return cw; }

static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }

static int d3d9_fpu(void) {
  WNDCLASSA wc = {0};
  wc.lpfnWndProc = wnd_proc;
  wc.hInstance = GetModuleHandleA(NULL);
  wc.lpszClassName = "fpenv";
  RegisterClassA(&wc);
  HWND hwnd = CreateWindowA("fpenv", "fpenv", WS_OVERLAPPEDWINDOW, 0, 0, 320, 240, NULL, NULL, wc.hInstance, NULL);
  IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
  D3DPRESENT_PARAMETERS pp = {0};
  IDirect3DDevice9 *dev = NULL;
  pp.BackBufferWidth = 64; pp.BackBufferHeight = 64; pp.BackBufferFormat = D3DFMT_X8R8G8B8;
  pp.BackBufferCount = 1; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = hwnd; pp.Windowed = TRUE;
  if (!d3d) { printf("FAIL d3d9\n"); return 1; }
  _mm_setcsr(0x1f80);
  printf("d3d9 before x87_cw=%04x mxcsr=%08x\n", x87_cw(), _mm_getcsr());
  if (FAILED(IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                     D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev)) || !dev) {
    printf("FAIL device\n"); return 1;
  }
  printf("d3d9 after  x87_cw=%04x mxcsr=%08x\n", x87_cw(), _mm_getcsr());
  fflush(stdout);
  ExitProcess(0);  /* no teardown: only the CreateDevice effect matters */
}

int main(int argc, char **argv) {
  static const struct { const char *name; uint32_t mxcsr; } modes[] = {
    { "nearest", 0x1f80 }, { "down", 0x3f80 }, { "up", 0x5f80 }, { "zero", 0x7f80 },
    { "ftz", 0x9f80 }, { "daz", 0x1fc0 }, { "ftz+daz", 0x9fc0 },
  };
  volatile float in[5] = { 1.0f, 3.0f, 2.5f, 1e-40f, 1e-20f };
  if (argc > 1 && !strcmp(argv[1], "d3d9")) return d3d9_fpu();

  HMODULE dll = LoadLibraryA("fpenv_dll.dll");
  probe_fn probe = dll ? (probe_fn)GetProcAddress(dll, "fp_probe") : NULL;
  set_fn set_rz = dll ? (set_fn)GetProcAddress(dll, "fp_set_rz") : NULL;
  if (!probe || !set_rz) { printf("FAIL load fpenv_dll.dll\n"); return 1; }
  for (unsigned i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
    struct fp_probe r;
    _mm_setcsr(modes[i].mxcsr);
    probe(in, &r);
    uint32_t after = _mm_getcsr();
    _mm_setcsr(0x1f80);
    printf("mode=%-8s quot=%08x neg_quot=%08x rint=%d,%d denorm_in=%08x denorm_out=%08x mxcsr_after=%08x callee_env=%08x native=%d\n",
           modes[i].name, r.quot_bits, r.neg_quot_bits, r.rint_pos, r.rint_neg, r.denorm_in, r.denorm_out,
           after & 0xffc0u, r.env, r.native);
  }
  _mm_setcsr(0x1f80);
  set_rz();
  printf("callee_set_rz mxcsr_after=%08x\n", _mm_getcsr() & 0xffc0u);
  _mm_setcsr(0x1f80);
  return 0;
}
