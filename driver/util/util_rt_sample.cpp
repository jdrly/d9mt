#include "util_rt_sample.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>

// The sampler (util_rt_sample.h says what and why). Standalone: Win32 and the header only, so
// test/native/rt_sample_wine_test.cpp links this file alone (scripts/test-rt-sample.sh).
//
// While the render thread is suspended it may hold any lock of the process: the heap's, the
// loader lock, Wine's virtual-memory lock, the Logger's. So between SuspendThread and ResumeThread
// the sampler calls GetThreadContext into a CONTEXT on its own stack and nothing else, and every
// path after a successful SuspendThread resumes. The module lookup (GetModuleHandleEx takes the
// loader lock), the clock and the lines all come after the resume.

namespace dxvk::rtsample {

  namespace {

    /** A thread the fault watch looks after: its id, and the first CPU fault it raised (0: none). */
    struct FaultSlot {
      std::atomic<DWORD> tid  = { 0u };
      std::atomic<DWORD> code = { 0u };
    };

    constexpr uint32_t MaxSamplers = 8u;

    FaultSlot s_slots[MaxSamplers];


    /** An exception the CPU raises, as opposed to one RaiseException raises (a C++ throw, ...). */
    bool isCpuFault(DWORD code) {
      switch (code) {
        case 0x80000001u:  // guard page
        case 0x80000002u:  // datatype misalignment
        case 0x80000003u:  // breakpoint
        case 0x80000004u:  // single step
        case 0xc0000005u:  // access violation
        case 0xc0000006u:  // in-page error
        case 0xc000001du:  // illegal instruction
        case 0xc00000fdu:  // stack overflow
          return true;
        default:
          // array bounds, the float faults, integer divide by zero and overflow, privileged
          // instruction
          return code >= 0xc000008cu && code <= 0xc0000096u;
      }
    }


    /**
     * The fault watch: first in the vectored list, so a handler that resolves a fault can't hide
     * it. Records the code for a watched thread and changes nothing: no allocation, no lock, the
     * thread's last error untouched (GetCurrentThreadId reads the TEB).
     */
    LONG CALLBACK faultWatch(EXCEPTION_POINTERS* info) {
      const EXCEPTION_RECORD* rec = info != nullptr ? info->ExceptionRecord : nullptr;
      if (rec == nullptr || !isCpuFault(rec->ExceptionCode))
        return EXCEPTION_CONTINUE_SEARCH;

      const DWORD tid = GetCurrentThreadId();
      for (auto& slot : s_slots) {
        if (slot.tid.load(std::memory_order_acquire) == tid) {
          DWORD none = 0u;
          slot.code.compare_exchange_strong(none, rec->ExceptionCode, std::memory_order_release);
        }
      }
      return EXCEPTION_CONTINUE_SEARCH;
    }


    /**
     * Installs the fault watch once per process. The module that holds it is pinned: the handler
     * is never removed (another thread may be inside it), so the module can't be unloaded under
     * it, nor under a sampler thread still running.
     */
    bool installWatch() {
      static const bool s_installed = [] {
        HMODULE self = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                                reinterpret_cast<LPCWSTR>(&faultWatch), &self))
          return false;
        return AddVectoredExceptionHandler(1u, faultWatch) != nullptr;
      }();
      return s_installed;
    }


    /** The image's SizeOfImage from its headers, 0 if they look wrong. The module is referenced. */
    uintptr_t imageSize(HMODULE module) {
      const auto* base = reinterpret_cast<const uint8_t*>(module);
      const auto* dos  = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
      if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 4096)
        return 0u;
      const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
      if (nt->Signature != IMAGE_NT_SIGNATURE
       || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR_MAGIC)
        return 0u;
      return uintptr_t(nt->OptionalHeader.SizeOfImage);
    }


    /** A sampler's state. Its thread owns it and frees it; the starter keeps only handles. */
    struct State {
      HANDLE      target = nullptr;   // the render thread: suspend, resume, get context, query
      HANDLE      stop   = nullptr;   // set: write the last window and end
      DWORD       tid    = 0u;
      uint32_t    hz     = 0u;
      bool        guard  = true;
      FaultSlot*  slot   = nullptr;
      void      (*sink)(const char*) = nullptr;
      ModuleTable table;
      Window      window;
      LARGE_INTEGER freq = { };
    };


    void emit(const State& s, const std::vector<std::string>& lines) {
      for (const auto& l : lines)
        s.sink(l.c_str());
    }


    void emitLine(void (*sink)(const char*), const char* fmt, DWORD a, uint32_t b = 0u) {
      char buf[320];
      std::snprintf(buf, sizeof(buf), fmt, unsigned(a), unsigned(b));
      sink(buf);
    }


    /** The table's index of the module that holds \p ip, added on first sight; -1: none. */
    int moduleOf(State& s, uintptr_t ip) {
      int index = s.table.find(ip);
      if (index >= 0 || ip == 0u)
        return index;

      // Without UNCHANGED_REFCOUNT: referenced while its headers and name are read, so a
      // FreeLibrary on another thread can't unmap it in between
      HMODULE module = nullptr;
      if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(ip), &module)
       || module == nullptr)
        return -1;

      char path[MAX_PATH] = { };
      const DWORD len  = GetModuleFileNameA(module, path, sizeof(path));
      const uintptr_t size = imageSize(module);
      if (len != 0u && len < sizeof(path) && size != 0u)
        index = s.table.add(uintptr_t(module), size, leafName(path));
      FreeLibrary(module);
      return index;
    }


    enum class Sampled { Ok, Missed, ThreadGone };

    /** One sample: suspend, read the instruction pointer, resume; then the lookup. */
    Sampled sampleOnce(State& s) {
      CONTEXT ctx;
      std::memset(&ctx, 0, sizeof(ctx));
      ctx.ContextFlags = CONTEXT_CONTROL;

      LARGE_INTEGER t0, t1;
      QueryPerformanceCounter(&t0);
      if (SuspendThread(s.target) == DWORD(-1)) {
        DWORD exitCode = 0u;
        if (GetExitCodeThread(s.target, &exitCode) && exitCode != STILL_ACTIVE)
          return Sampled::ThreadGone;
        s.window.missed++;
        return Sampled::Missed;
      }
      // Suspended: GetThreadContext only, then resume, whatever it returned
      const BOOL got = GetThreadContext(s.target, &ctx);
      ResumeThread(s.target);
      QueryPerformanceCounter(&t1);

      if (!got) {
        s.window.missed++;
        return Sampled::Missed;
      }
#ifdef _WIN64
      const uintptr_t ip = uintptr_t(ctx.Rip);
#else
      const uintptr_t ip = uintptr_t(ctx.Eip);
#endif
      const uint64_t heldNs = uint64_t(double(t1.QuadPart - t0.QuadPart) * 1.0e9 / double(s.freq.QuadPart));
      s.window.add(moduleOf(s, ip), heldNs);
      return Sampled::Ok;
    }


    double secondsSince(const State& s, const LARGE_INTEGER& t0) {
      LARGE_INTEGER t;
      QueryPerformanceCounter(&t);
      return double(t.QuadPart - t0.QuadPart) / double(s.freq.QuadPart);
    }


    DWORD WINAPI samplerMain(void* arg) {
      State& s = *static_cast<State*>(arg);
      QueryPerformanceFrequency(&s.freq);
      const DWORD periodMs = 1000u / s.hz;

      // The watch second: no suspend at all. A thread that faults by then isn't sampled
      const bool released = WaitForSingleObject(s.stop, s.guard ? kWatchMs : 0u) == WAIT_OBJECT_0;
      DWORD fault = s.guard ? s.slot->code.load(std::memory_order_acquire) : 0u;

      if (!released && fault != 0u) {
        emitLine(s.sink, "D9MT-RT-SAMPLE: thread 0x%04x raised a CPU fault (%08x) in its first second: "
          "not sampled (suspending a thread while Wine handles its fault can end it under Rosetta)", s.tid, fault);
      } else if (!released) {
        LARGE_INTEGER windowStart;
        QueryPerformanceCounter(&windowStart);
        uint32_t number = 1u;
        const char* why = "last, device released";

        for (;;) {
          if (WaitForSingleObject(s.stop, periodMs) == WAIT_OBJECT_0)
            break;
          if (s.guard && (fault = s.slot->code.load(std::memory_order_acquire)) != 0u) {
            why = "last, stopped at a CPU fault";
            break;
          }
          if (sampleOnce(s) == Sampled::ThreadGone) {
            why = "last, the thread ended";
            break;
          }
          const double seconds = secondsSince(s, windowStart);
          if (seconds * 1000.0 >= double(kReportMs)) {
            emit(s, windowLines(s.table, s.window, s.tid, number, seconds, s.hz, "every 10 s"));
            s.window = Window();
            QueryPerformanceCounter(&windowStart);
            number++;
          }
        }

        emit(s, windowLines(s.table, s.window, s.tid, number, secondsSince(s, windowStart), s.hz, why));
        if (fault != 0u) {
          emitLine(s.sink, "D9MT-RT-SAMPLE: thread 0x%04x raised a CPU fault (%08x): sampling stopped for good "
            "(suspending a thread while Wine handles its fault can end it under Rosetta)", s.tid, fault);
        }
      }

      s.slot->tid.store(0u, std::memory_order_release);
      s.slot->code.store(0u, std::memory_order_relaxed);
      CloseHandle(s.target);
      CloseHandle(s.stop);
      delete &s;
      return 0u;
    }


    Handle start(uint32_t hz, void (*sink)(const char*), bool guard) {
      const DWORD tid = GetCurrentThreadId();
      if (!installWatch()) {
        emitLine(sink, "D9MT-RT-SAMPLE: thread 0x%04x not sampled: the fault watch couldn't be installed", tid);
        return Handle();
      }

      FaultSlot* slot = nullptr;
      for (auto& candidate : s_slots) {
        DWORD none = 0u;
        if (candidate.tid.compare_exchange_strong(none, tid, std::memory_order_acq_rel)) {
          slot = &candidate;
          break;
        }
      }
      if (slot == nullptr) {
        emitLine(sink, "D9MT-RT-SAMPLE: thread 0x%04x not sampled: %u samplers run already", tid, MaxSamplers);
        return Handle();
      }
      slot->code.store(0u, std::memory_order_release);

      auto* s = new State();
      s->tid   = tid;
      s->hz    = hz;
      s->guard = guard;
      s->slot  = slot;
      s->sink  = sink;

      const HANDLE self = GetCurrentProcess();
      Handle h;
      bool ok = DuplicateHandle(self, GetCurrentThread(), self, &s->target,
          THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, 0u)
        && (s->stop = CreateEventW(nullptr, TRUE, FALSE, nullptr)) != nullptr
        && DuplicateHandle(self, s->stop, self, &h.stop, 0u, FALSE, DUPLICATE_SAME_ACCESS)
        && (h.thread = CreateThread(nullptr, 0u, samplerMain, s, 0u, nullptr)) != nullptr;

      if (!ok) {
        const DWORD error = GetLastError();
        if (h.stop != nullptr)
          CloseHandle(h.stop);
        if (s->stop != nullptr)
          CloseHandle(s->stop);
        if (s->target != nullptr)
          CloseHandle(s->target);
        delete s;
        slot->tid.store(0u, std::memory_order_release);
        emitLine(sink, "D9MT-RT-SAMPLE: thread 0x%04x not sampled: no sampler thread (error %u)", tid, error);
        return Handle();
      }

      // s belongs to the sampler thread from here on
      emitLine(sink, guard
        ? "D9MT-RT-SAMPLE: sampling thread 0x%04x at %u Hz after a second's watch, a window every 10 s; "
          "a developer switch, it suspends the thread, and stops at the thread's first CPU fault"
        : "D9MT-RT-SAMPLE: sampling thread 0x%04x at %u Hz, no fault guard (tests only)", tid, hz);
      return h;
    }

  }


  Handle startFromEnv(void (*sink)(const char* line)) {
    char value[16] = { };
    const DWORD n = GetEnvironmentVariableA("D9MT_RT_SAMPLE", value, sizeof(value));
    if (n == 0u)
      return Handle();

    bool invalid = n >= sizeof(value);
    const uint32_t hz = invalid ? 0u : hzFromEnv(value, &invalid);
    if (invalid) {
      char buf[160];
      std::snprintf(buf, sizeof(buf), "D9MT-RT-SAMPLE: D9MT_RT_SAMPLE=\"%s%s\" ignored (a whole number of Hz, 1 to %u)",
        value, n >= sizeof(value) ? "..." : "", unsigned(kMaxHz));
      sink(buf);
      return Handle();
    }
    return hz != 0u ? start(hz, sink, true) : Handle();
  }


  Handle startForTest(uint32_t hz, void (*sink)(const char* line), bool guard) {
    return start(hz, sink, guard);
  }


  void stop(Handle& h, bool wait) {
    if (h.stop != nullptr) {
      SetEvent(h.stop);
      CloseHandle(h.stop);
    }
    if (h.thread != nullptr) {
      if (wait)
        WaitForSingleObject(h.thread, 500u);
      CloseHandle(h.thread);
    }
    h = Handle();
  }

}
