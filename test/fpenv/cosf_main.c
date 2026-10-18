/* Writes cosf(x) for every 64th float in [0, 2*pi] (about 17M values), as
 * computed by cos_many in the given DLL (cosf_dll.c, x64 or ARM64EC), to a
 * file, for comparison between CRTs and engines (test/fpenv/ulpcmp.c).
 *
 *   cosf_main.exe <dll> <out file> */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void (*cos_fn)(const float *, float *, uint32_t);

int main(int argc, char **argv) {
  if (argc < 3) { printf("FAIL usage\n"); return 2; }
  HMODULE dll = LoadLibraryA(argv[1]);
  cos_fn fn = dll ? (cos_fn)GetProcAddress(dll, "cos_many") : NULL;
  if (!fn) { printf("FAIL load %s\n", argv[1]); return 1; }
  const uint32_t hi = 0x40c90fdbu;  /* 2*pi */
  uint32_t n = hi / 64u + 1u;
  float *in = malloc(n * sizeof(float)), *out = malloc(n * sizeof(float));
  for (uint32_t i = 0; i < n; i++) { uint32_t b = i * 64u; memcpy(&in[i], &b, 4); }
  LARGE_INTEGER f, t0, t1;
  QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&t0);
  fn(in, out, n);
  QueryPerformanceCounter(&t1);
  FILE *o = fopen(argv[2], "wb");
  if (!o || fwrite(out, sizeof(float), n, o) != n) { printf("FAIL write\n"); return 1; }
  fclose(o);
  printf("COSF n=%u ns_per_call=%.1f\n", n, (double)(t1.QuadPart - t0.QuadPart) * 1e9 / f.QuadPart / n);
  return 0;
}
