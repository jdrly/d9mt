// Does macOS keep x18 for this binary? Windows ARM64 (and ARM64EC) keeps the
// TEB in x18; macOS clears it on returns to user space for current binaries,
// but not for ones linked for an old macOS (docs/arm64-runtime.md).
//
//   for v in 11.0 12.0 26.0; do
//     clang -O1 -arch arm64 test/native/x18_platform_test.c -o /tmp/x18-$v \
//       -Wl,-platform_version,macos,$v,$v && /tmp/x18-$v
//   done
//
// macOS 27.0.1, M5 Max: 11.0 and 12.0 lose x18 in 0 of 20000 checks,
// 26.0 in 13333 (every syscall). What counts is the SDK version in
// LC_BUILD_VERSION, not the minimum OS: `vtool -set-build-version macos 26.0
// 12.0` (minos 26, SDK 12) plus an ad-hoc re-sign keeps x18; SDK 13.0 and up
// lose it.
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
static uint64_t get(void) { uint64_t v; __asm__ volatile("mov %0, x18" : "=r"(v)); return v; }
static void set(uint64_t v) { __asm__ volatile("mov x18, %0" :: "r"(v) : "x18"); }
static void* spin(void* a) { volatile uint64_t x = 0; for (;;) x++; return a; }
int main(void) {
  pthread_t t[16]; for (int i = 0; i < 16; i++) pthread_create(&t[i], 0, spin, 0); // force preemption
  set(0x1234567812345678ull);
  unsigned lost = 0, n = 0;
  for (int i = 0; i < 20000; i++) {
    if (i % 3 == 0) getpid(); else if (i % 3 == 1) sched_yield(); else usleep(10);
    n++; if (get() != 0x1234567812345678ull) { lost++; set(0x1234567812345678ull); }
  }
  printf("x18 lost %u of %u checks\n", lost, n);
  return 0;
}
