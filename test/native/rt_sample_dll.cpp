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

// Calls ntdll's memmove on 256 KiB until the TSC has moved by `ticks`: the samples land in
// ntdll.dll, and the caller search should find this module
extern "C" __declspec(dllexport) __attribute__((noinline)) void rt_dll_memmove(uint64_t ticks) {
  using PFN_memmove = void* (__cdecl *)(void*, const void*, size_t);
  static const auto move = reinterpret_cast<PFN_memmove>(reinterpret_cast<void*>(
    GetProcAddress(GetModuleHandleA("ntdll.dll"), "memmove")));
  static char a[256 << 10], b[256 << 10];
  const uint64_t t0 = __rdtsc();
  while (__rdtsc() - t0 < ticks)
    move(a, b, sizeof(a));
}
