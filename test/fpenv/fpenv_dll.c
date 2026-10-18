/* The callee of test/fpenv/fpenv_main.c, built as x64 and as ARM64EC: what
 * floating-point environment does code in a DLL see when an x64 caller has
 * set MXCSR, and does its own change reach the caller? On ARM64EC the
 * environment is FPCR (rounding mode in bits 22-23, FZ in bit 24), which the
 * emulator has to derive from the caller's MXCSR at the call. */
#include <stdint.h>
#include <math.h>
#ifdef __arm64ec__
#define FP_IS_NATIVE 1
#else
#include <xmmintrin.h>
#define FP_IS_NATIVE 0
#endif

struct fp_probe {
  uint32_t env;        /* FPCR on ARM64EC, MXCSR on x64 */
  uint32_t quot_bits;  /* 1/3: the last bit depends on the rounding mode */
  uint32_t neg_quot_bits;
  int32_t  rint_pos, rint_neg;  /* lrintf(2.5f), lrintf(-2.5f) */
  uint32_t denorm_in;  /* denormal * 1: 0 if denormal inputs are flushed */
  uint32_t denorm_out; /* tiny * tiny with a denormal result: 0 if flushed */
  int      native;
};

static uint32_t read_env(void) {
#ifdef __arm64ec__
  uint64_t v;
  __asm__ volatile("mrs %0, fpcr" : "=r"(v));
  return (uint32_t)v;
#else
  return _mm_getcsr();
#endif
}

static uint32_t bits(float f) { union { float f; uint32_t u; } x = { f }; return x.u; }

__declspec(dllexport) void fp_probe(volatile const float *in, struct fp_probe *out) {
  /* in: 1, 3, 2.5, denormal 1e-40, tiny 1e-20 (volatile: no constant folding) */
  out->env = read_env();
  out->quot_bits = bits(in[0] / in[1]);
  out->neg_quot_bits = bits(-in[0] / in[1]);
  out->rint_pos = (int32_t)lrintf(in[2]);
  out->rint_neg = (int32_t)lrintf(-in[2]);
  out->denorm_in = bits(in[3] * in[0]);
  out->denorm_out = bits(in[4] * in[4] * in[4] * in[3] * 1e30f);
  out->native = FP_IS_NATIVE;
}

/* Sets round-toward-zero in the callee's own environment and returns. */
__declspec(dllexport) void fp_set_rz(void) {
#ifdef __arm64ec__
  uint64_t v;
  __asm__ volatile("mrs %0, fpcr" : "=r"(v));
  v = (v & ~(3ull << 22)) | (3ull << 22);
  __asm__ volatile("msr fpcr, %0" : : "r"(v));
#else
  _mm_setcsr((_mm_getcsr() & ~0x6000u) | 0x6000u);
#endif
}
