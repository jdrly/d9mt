#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#define NOINSTR __attribute__((no_instrument_function))

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace {

  constexpr uint32_t kMaxDepth   = 512u;
  constexpr uint32_t kTableSize  = 1u << 15;
  constexpr uint32_t kMaxThreads = 256u;
  constexpr double   kDumpPeriodSec = 10.0;

  struct StackFrame {
    void*    fn;
    uint64_t t0;
    uint64_t childTicks;
  };

  struct Slot {
    void*    fn;
    uint64_t calls;
    uint64_t selfTicks;
    uint64_t totalTicks;
  };

  struct ThreadState {
    StackFrame stack[kMaxDepth];
    Slot       table[kTableSize];
    uint32_t   depth;
    uint32_t   overflow;
    uint64_t   droppedSlots;
    uint32_t   zeroEvents;
    uint32_t   exits;
    LARGE_INTEGER lastDumpQpc;
    DWORD      tid;
    char       name[128];  /* its description, read while it runs: Wine drops it when the thread ends */
  };

  /* -1 = not probed, 0 = off, 1 = on; racy init is benign since it's idempotent. */
  volatile int g_enabledState = -1;

  volatile LONG g_calibInit = 0;
  uint64_t      g_tsc0  = 0;
  LARGE_INTEGER g_qpc0  = {};
  LARGE_INTEGER g_qpcHz = {};

  thread_local ThreadState* t_state = nullptr;

  /* Every thread's state, for the dump at exit. */
  ThreadState*  g_threads[kMaxThreads] = {};
  volatile LONG g_threadCount = 0;

  NOINSTR static inline uint64_t rdtscNow() {
    return __builtin_ia32_rdtsc();
  }

  NOINSTR static bool profEnabled() {
    int e = g_enabledState;
    if (e >= 0)
      return e != 0;
    const char* v = std::getenv("D9MT_PROFILE_INSTRUMENT");
    e = (v && v[0] && v[0] != '0') ? 1 : 0;
    g_enabledState = e;
    return e != 0;
  }

  NOINSTR static ThreadState* initThread() {
    ThreadState* ts = static_cast<ThreadState*>(std::calloc(1u, sizeof(ThreadState)));
    if (!ts)
      return nullptr;
    ts->tid = GetCurrentThreadId();
    if (InterlockedCompareExchange(&g_calibInit, 1, 0) == 0) {
      QueryPerformanceFrequency(&g_qpcHz);
      QueryPerformanceCounter(&g_qpc0);
      g_tsc0 = rdtscNow();
    }
    QueryPerformanceCounter(&ts->lastDumpQpc);
    LONG n = InterlockedIncrement(&g_threadCount) - 1;
    if (n < LONG(kMaxThreads))
      g_threads[n] = ts;
    t_state = ts;
    return ts;
  }

  NOINSTR static Slot* lookup(ThreadState* ts, void* fn) {
    uint32_t h = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(fn) >> 2)
               * 2654435761u;
    uint32_t i = h & (kTableSize - 1u);
    for (uint32_t probe = 0u; probe < kTableSize; probe++) {
      Slot& s = ts->table[i];
      if (s.fn == fn)
        return &s;
      if (!s.fn) {
        s.fn = fn;
        return &s;
      }
      i = (i + 1u) & (kTableSize - 1u);
    }
    return nullptr;
  }

  NOINSTR static double nsPerTick() {
    LARGE_INTEGER qpcNow;
    QueryPerformanceCounter(&qpcNow);
    uint64_t tscNow = rdtscNow();
    double elapsedNs = double(qpcNow.QuadPart - g_qpc0.QuadPart) * 1e9
                     / double(g_qpcHz.QuadPart ? g_qpcHz.QuadPart : 1);
    double elapsedTicks = double(tscNow - g_tsc0);
    return elapsedTicks > 0.0 ? elapsedNs / elapsedTicks : 0.0;
  }

  /* The calling thread's description (SetThreadDescription, e.g. dxvk-cs) in UTF-8, or "-". */
  NOINSTR static void threadName(char* out, int size) {
    using GetThreadDescriptionProc = HRESULT (WINAPI *) (HANDLE, PWSTR*);
    static auto getDescription = reinterpret_cast<GetThreadDescriptionProc>(
      GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription"));
    PWSTR wide = nullptr;
    out[0] = '\0';
    if (getDescription && SUCCEEDED(getDescription(GetCurrentThread(), &wide)) && wide) {
      if (!WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, size, nullptr, nullptr))
        out[0] = '\0';
      LocalFree(wide);
    }
    if (!out[0])
      std::snprintf(out, size, "-");
  }

  NOINSTR static void dump(ThreadState* ts, const char* when) {
    double npt = nsPerTick();
    if (npt <= 0.0)
      return;
    uintptr_t base = reinterpret_cast<uintptr_t>(&__ImageBase);
    std::fprintf(stderr,
      "D9MTINST BEGIN tid=%lu base=0x%08llx ns_per_tick=%.6f dropped=%llu overflow=%lu at=%s name=%s\n",
      static_cast<unsigned long>(ts->tid),
      static_cast<unsigned long long>(base),  /* unsigned long is 32 bits on Windows */
      npt,
      static_cast<unsigned long long>(ts->droppedSlots),
      static_cast<unsigned long>(ts->overflow),
      when,
      ts->name[0] ? ts->name : "-");
    for (uint32_t i = 0u; i < kTableSize; i++) {
      const Slot& s = ts->table[i];
      if (!s.fn)
        continue;
      std::fprintf(stderr, "D9MTINST F 0x%08lx %llu %llu %llu\n",
        static_cast<unsigned long>(reinterpret_cast<uintptr_t>(s.fn) - base),
        static_cast<unsigned long long>(s.calls),
        static_cast<unsigned long long>(uint64_t(double(s.selfTicks)  * npt)),
        static_cast<unsigned long long>(uint64_t(double(s.totalTicks) * npt)));
    }
    std::fprintf(stderr, "D9MTINST END tid=%lu\n",
      static_cast<unsigned long>(ts->tid));
    std::fflush(stderr);
    QueryPerformanceCounter(&ts->lastDumpQpc);
  }

  /* At process exit (the DLL's static destructors run at DLL_PROCESS_DETACH, after the process's
   * other threads ended): every thread's table, also a run shorter than kDumpPeriodSec and a
   * thread whose instrumented outermost frame never returns (the CS thread's loop). A process
   * that is killed gets only the periodic dumps. */
  struct ExitDump {
    NOINSTR ~ExitDump() {
      if (g_enabledState != 1)
        return;
      LONG n = g_threadCount;
      for (LONG i = 0; i < n && i < LONG(kMaxThreads); i++) {
        if (g_threads[i])
          dump(g_threads[i], "exit");
      }
    }
  } g_exitDump;

}

extern "C" NOINSTR void __cyg_profile_func_enter(void* this_fn, void* call_site) {
  (void)call_site;
  if (!profEnabled())
    return;
  ThreadState* ts = t_state;
  if (!ts && !(ts = initThread()))
    return;
  if (ts->depth >= kMaxDepth) {
    ts->overflow++;
    return;
  }
  StackFrame& f = ts->stack[ts->depth++];
  f.fn         = this_fn;
  f.childTicks = 0u;
  f.t0         = rdtscNow();
}

extern "C" NOINSTR void __cyg_profile_func_exit(void* this_fn, void* call_site) {
  (void)call_site;
  if (!profEnabled())
    return;
  ThreadState* ts = t_state;
  if (!ts)
    return;
  if (ts->overflow) {
    ts->overflow--;
    return;
  }
  if (!ts->depth)
    return;

  /* The name at the thread's first and every 2^20th return, at any depth: a thread that ExitProcess
   * ends inside its outermost instrumented call (the CS thread's loop) still names itself. The
   * call (a few us under Wine) counts in the returning function's time. */
  if (uint32_t e = ++ts->exits; e == 1u || (e & 0xFFFFFu) == 0u)
    threadName(ts->name, int(sizeof(ts->name)));

  uint64_t now = rdtscNow();

  /* Exceptions/longjmp can skip exit hooks, desyncing the top frame from
   * this_fn; resync by searching down and discarding frames above it. */
  uint32_t top = ts->depth - 1u;
  if (ts->stack[top].fn != this_fn) {
    uint32_t j = top;
    while (j > 0u && ts->stack[j].fn != this_fn)
      j--;
    if (ts->stack[j].fn != this_fn)
      return;
    ts->depth = j + 1u;
    top = j;
  }

  StackFrame f = ts->stack[top];
  ts->depth = top;

  uint64_t incl = now - f.t0;
  uint64_t self = incl > f.childTicks ? incl - f.childTicks : 0u;

  if (Slot* s = lookup(ts, f.fn)) {
    s->calls++;
    s->totalTicks += incl;
    s->selfTicks  += self;
  } else {
    ts->droppedSlots++;
  }

  if (ts->depth) {
    ts->stack[ts->depth - 1u].childTicks += incl;
  } else if ((++ts->zeroEvents & 0x1FFFu) == 0u) {
    LARGE_INTEGER qpcNow;
    QueryPerformanceCounter(&qpcNow);
    if (double(qpcNow.QuadPart - ts->lastDumpQpc.QuadPart)
        > kDumpPeriodSec * double(g_qpcHz.QuadPart))
      dump(ts, "period");
  }
}
