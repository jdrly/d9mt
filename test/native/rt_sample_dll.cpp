// A DLL for rt_sample_wine_test (scripts/test-rt-sample.sh builds it): a second module the
// sampled thread spends a known part of its time in, so the test can check the shares by module.
#include <windows.h>
#include <x86intrin.h>

#include <cstdint>

// Spins until the TSC has moved by `ticks`: no call out of this module while it spins
extern "C" __declspec(dllexport) __attribute__((noinline)) void rt_dll_spin(uint64_t ticks) {
  const uint64_t t0 = __rdtsc();
  while (__rdtsc() - t0 < ticks) { }
}
