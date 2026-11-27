// rt_sample_wine_test: the render-thread sampler (driver/util/util_rt_sample.cpp) under Wine. A
// mingw x64 console program; scripts/test-rt-sample.sh builds it with rt_sample_dll.cpp and runs
// each mode in a fresh process:
//
//   ratio      the sampled thread spins 30 ms in this exe, then 10 ms in rt_sample_dll.dll, for
//              4 s: the exe's samples must be 65-85 % of the two, and the two at least 90 % of all
//              (the control: the same loop with the two spins swapped must give the mirror image)
//   swapped    that control: 10 ms in the exe, 30 ms in the DLL
//   wait       the thread waits on an event for 3 s: samples come, the exe has under 5 % of them,
//              and the thread wakes and ends normally
//   syscall    the thread calls SwitchToThread and GetFileAttributes for 3 s: it survives
//   fault      2 s of spinning, then an access violation every ms for 2 s, which the program's own
//              handler steps over: the sampler stops at the first one, with its line, and the
//              thread handles every fault and ends normally
//   fault-early  the faults start at once: the sampler never samples
//   storm <hz> <faults per s> <s> <guard 0|1>
//              the measurement, not a check: sampling at <hz>, the thread spins and faults at the
//              rate for that long; prints whether the thread survived. Exit 0 survived, 3 died
//
// Prints the sampler's lines as they come, then "rt_sample_wine_test <mode>: ok" or the failure.

#include "../../driver/util/util_rt_sample.h"

#include <windows.h>
#include <x86intrin.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" __declspec(dllimport) void rt_dll_spin(uint64_t ticks);

using namespace dxvk;

namespace {

  CRITICAL_SECTION         g_linesLock;
  std::vector<std::string> g_lines;

  void sink(const char* line) {
    EnterCriticalSection(&g_linesLock);
    g_lines.emplace_back(line);
    std::printf("%s\n", line);
    std::fflush(stdout);
    LeaveCriticalSection(&g_linesLock);
  }

  uint64_t g_ticksPerMs = 0u;

  void calibrate() {
    LARGE_INTEGER f, q0, q1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&q0);
    const uint64_t t0 = __rdtsc();
    Sleep(200);
    QueryPerformanceCounter(&q1);
    const uint64_t t1 = __rdtsc();
    const double ms = double(q1.QuadPart - q0.QuadPart) * 1000.0 / double(f.QuadPart);
    g_ticksPerMs = uint64_t(double(t1 - t0) / ms);
  }

  __attribute__((noinline)) void exeSpin(uint64_t ticks) {
    const uint64_t t0 = __rdtsc();
    while (__rdtsc() - t0 < ticks) { }
  }

  double nowMs() {
    LARGE_INTEGER f, q;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&q);
    return double(q.QuadPart) * 1000.0 / double(f.QuadPart);
  }

  // ---- faults: mov (%rax), %eax (8B 00) at 0x10, stepped over by the handler below ---------

  volatile LONG g_faults = 0;

  __attribute__((noinline)) void readAt0x10() {
    uint64_t p = 0x10u;
    asm volatile("mov (%%rax), %%eax" : "+a"(p) : : "memory");
  }

  LONG CALLBACK stepOver(EXCEPTION_POINTERS* info) {
    const auto* rip = reinterpret_cast<const uint8_t*>(info->ContextRecord->Rip);
    if (info->ExceptionRecord->ExceptionCode == 0xc0000005u && rip[0] == 0x8b && rip[1] == 0x00) {
      InterlockedIncrement(&g_faults);
      info->ContextRecord->Rip += 2u;
      return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
  }

  // ---- the sampled thread -----------------------------------------------------------------

  struct Job {
    std::string mode;
    uint32_t    hz        = 200u;
    bool        guard     = true;
    double      faultRate = 0.0;   // faults per second, storm
    double      seconds   = 0.0;   // storm
    volatile LONG done    = 0;
  };

  void faultFor(double ms, double perSecond) {
    const uint64_t gap = uint64_t(double(g_ticksPerMs) * 1000.0 / perSecond);
    const double end = nowMs() + ms;
    while (nowMs() < end) {
      readAt0x10();
      exeSpin(gap);
    }
  }

  DWORD WINAPI worker(void* arg) {
    Job& job = *static_cast<Job*>(arg);
    rtsample::Handle h = rtsample::startForTest(job.hz, sink, job.guard);

    if (job.mode == "ratio" || job.mode == "swapped") {
      const uint64_t a = (job.mode == "ratio" ? 30u : 10u) * g_ticksPerMs;
      const uint64_t b = (job.mode == "ratio" ? 10u : 30u) * g_ticksPerMs;
      const double end = nowMs() + 5000.0;
      while (nowMs() < end) {
        exeSpin(a);
        rt_dll_spin(b);
      }
    } else if (job.mode == "wait") {
      HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
      WaitForSingleObject(ev, 4000u);
      CloseHandle(ev);
    } else if (job.mode == "syscall") {
      const double end = nowMs() + 4000.0;
      while (nowMs() < end) {
        SwitchToThread();
        GetFileAttributesW(L"C:\\windows");
      }
    } else if (job.mode == "fault") {
      exeSpin(2000u * g_ticksPerMs);
      faultFor(2000.0, 1000.0);
      exeSpin(500u * g_ticksPerMs);
    } else if (job.mode == "fault-early") {
      faultFor(3000.0, 1000.0);
    } else if (job.mode == "storm") {
      if (job.guard)
        exeSpin(2000u * g_ticksPerMs);
      faultFor(job.seconds * 1000.0, job.faultRate);
    }

    rtsample::stop(h, true);
    InterlockedExchange(&job.done, 1);
    return 0u;
  }

  // ---- reading the lines back -------------------------------------------------------------

  struct Tally {
    uint64_t total  = 0u;
    uint64_t exe    = 0u;
    uint64_t dll    = 0u;
    uint32_t windows = 0u;
    std::string top;
    uint64_t topCount = 0u;
  };

  Tally tally() {
    Tally t;
    EnterCriticalSection(&g_linesLock);
    std::vector<std::pair<std::string, uint64_t>> perName;
    for (const auto& l : g_lines) {
      if (l.find(" samples at ") != std::string::npos) {
        t.windows++;
        continue;
      }
      const size_t pct = l.find(" % ");
      const size_t open = l.rfind(" (");
      if (l.rfind("D9MT-RT-SAMPLE:   ", 0) != 0 || pct == std::string::npos || open == std::string::npos || open < pct)
        continue;
      const std::string name = l.substr(pct + 3u, open - pct - 3u);
      const uint64_t n = std::strtoull(l.c_str() + open + 2u, nullptr, 10);
      t.total += n;
      if (name == "rt_sample_wine_test.exe")
        t.exe += n;
      if (name == "rt_sample_dll.dll")
        t.dll += n;
      bool found = false;
      for (auto& p : perName) {
        if (p.first == name) {
          p.second += n;
          found = true;
        }
      }
      if (!found)
        perName.emplace_back(name, n);
    }
    for (const auto& p : perName) {
      if (p.second > t.topCount) {
        t.top = p.first;
        t.topCount = p.second;
      }
    }
    LeaveCriticalSection(&g_linesLock);
    return t;
  }

  bool hasLine(const char* text) {
    EnterCriticalSection(&g_linesLock);
    bool found = false;
    for (const auto& l : g_lines)
      found = found || l.find(text) != std::string::npos;
    LeaveCriticalSection(&g_linesLock);
    return found;
  }

  int fail(const std::string& mode, const char* why) {
    std::printf("rt_sample_wine_test %s: FAIL: %s\n", mode.c_str(), why);
    return 1;
  }

}


int main(int argc, char** argv) {
  InitializeCriticalSection(&g_linesLock);
  AddVectoredExceptionHandler(0u, stepOver);
  calibrate();

  Job job;
  job.mode = argc > 1 ? argv[1] : "";
  if (job.mode == "storm") {
    if (argc != 6)
      return fail(job.mode, "usage: storm <hz> <faults per s> <s> <guard 0|1>");
    job.hz        = uint32_t(std::atoi(argv[2]));
    job.faultRate = std::atof(argv[3]);
    job.seconds   = std::atof(argv[4]);
    job.guard     = std::atoi(argv[5]) != 0;
  }

  HANDLE t = CreateThread(nullptr, 0u, worker, &job, 0u, nullptr);
  const double limitMs = 20000.0 + job.seconds * 1000.0;
  const DWORD waited = WaitForSingleObject(t, DWORD(limitMs));
  CloseHandle(t);
  const bool finished = waited == WAIT_OBJECT_0 && job.done != 0;
  const Tally s = tally();
  std::printf("rt_sample_wine_test %s: %u windows, %llu samples, exe %llu, dll %llu, most in %s (%llu), "
    "faults handled %ld, thread %s\n", job.mode.c_str(), s.windows, (unsigned long long)s.total,
    (unsigned long long)s.exe, (unsigned long long)s.dll, s.top.empty() ? "-" : s.top.c_str(),
    (unsigned long long)s.topCount, long(g_faults),
    finished ? "ended normally" : waited == WAIT_OBJECT_0 ? "DIED (ended without finishing)" : "still running");

  if (job.mode == "storm")
    return finished ? 0 : 3;

  if (!finished)
    return fail(job.mode, "the sampled thread didn't end normally");

  if (job.mode == "ratio" || job.mode == "swapped") {
    if (s.total < 400u)
      return fail(job.mode, "under 400 samples in 4 s at 200 Hz");
    const double exeShare = double(s.exe) / double(s.exe + s.dll);
    const bool want = job.mode == "ratio" ? exeShare >= 0.65 && exeShare <= 0.85
                                          : exeShare >= 0.15 && exeShare <= 0.35;
    std::printf("rt_sample_wine_test %s: exe/(exe+dll) %.3f, (exe+dll)/all %.3f\n", job.mode.c_str(),
      exeShare, double(s.exe + s.dll) / double(s.total));
    if (!want)
      return fail(job.mode, "the exe's share of exe+dll is off the spins' ratio");
    if (double(s.exe + s.dll) < 0.9 * double(s.total))
      return fail(job.mode, "under 90 % of the samples in the two spinning modules");
  } else if (job.mode == "wait" || job.mode == "syscall") {
    if (s.total < 200u)
      return fail(job.mode, "under 200 samples in 3 s at 200 Hz");
    if (job.mode == "wait" && double(s.exe) >= 0.05 * double(s.total))
      return fail(job.mode, "the exe has 5 % or more of a waiting thread's samples");
  } else if (job.mode == "fault") {
    if (!hasLine("sampling stopped for good") || !hasLine("(c0000005)"))
      return fail(job.mode, "no stop line naming c0000005");
    if (s.total < 100u)
      return fail(job.mode, "under 100 samples in the clean second before the faults");
    if (g_faults < 500)
      return fail(job.mode, "the thread handled under 500 faults");
  } else if (job.mode == "fault-early") {
    if (!hasLine("in its first second: not sampled"))
      return fail(job.mode, "no line that the thread isn't sampled");
    if (s.windows != 0u)
      return fail(job.mode, "a window was written");
  } else {
    return fail(job.mode, "unknown mode");
  }

  std::printf("rt_sample_wine_test %s: ok\n", job.mode.c_str());
  return 0;
}
