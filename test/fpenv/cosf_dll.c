/* cosf from the CRT of this DLL's architecture (ucrtbase: x64 or ARM64EC),
 * for test/fpenv/cosf_main.c. d3d9.dll takes cosf (light cone angles,
 * d3d9_state.h) from the same import. */
#include <math.h>
#include <stdint.h>

__declspec(dllexport) void cos_many(const float *in, float *out, uint32_t n) {
  for (uint32_t i = 0; i < n; i++)
    out[i] = cosf(in[i]);
}
