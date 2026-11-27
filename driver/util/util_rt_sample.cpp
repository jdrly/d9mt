#include "util_rt_sample.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>

// The sampler (util_rt_sample.h says what and why). Standalone: Win32 and the header only, so
// test/native/rt_sample_wine_test.cpp links this file alone (scripts/test-rt-sample.sh).
//
// While the render thread is suspended it may hold any lock of the process: the heap's, the
// loader lock, Wine's virtual-memory lock, the Logger's. So between SuspendThread and ResumeThread
// the sampler only
//   - lets its capture thread call GetThreadContext, and waits for it at most kCaptureMs: Wine's
//     GetThreadContext waits without a limit until the thread has stopped, and a thread that never
//     stops must not stay suspended. Past the limit the sampler resumes the thread, stops for
//     good and leaves the capture thread and its own state behind (never freed: the capture
//     thread may still write into them), and no sampler starts again in the process. A capture
//     thread that was delayed past the limit skips the request; one that had already checked
//     makes it on the running thread, which Wine stops once more for it: one suspend outside the
//     sampler's rhythm, after a 250 ms stall;
//   - reads the thread's TEB and copies stack slots with plain loads, only between the stack
//     pointer and the base of the stack the thread had when the sampler started, and only when
//     the stack pointer is inside that stack and at or above its committed limit: memory the
//     thread has used, which can't fault. A thread on another stack (a fiber) isn't read. No
//     fault may happen there: Wine's fault handler takes its virtual-memory lock.
// Every path after a successful SuspendThread resumes. The module lookups (GetModuleHandleEx and
// K32EnumProcessModules take the loader lock), the caller search, the clock and the lines all come
// after the resume.

namespace dxvk::rtsample {

  namespace {

    constexpr uint32_t MaxSamplers = 8u;
    constexpr DWORD    kCaptureMs  = 250u;   // a GetThreadContext normally takes well under 2 ms

    /** Set once a capture got stuck: its thread and state stay behind, so no sampler starts again. */
    std::atomic<bool> s_captureStuck = { false };

    /**
     * A thread the fault watch looks after, in one word: its id in the high half, the first CPU
     * fault it raised in the low half (0: none). Claimed, faulted and freed by compare-exchange or
     * a store of the whole word, so a fault can't land in a slot another thread has claimed since.
     */
    std::atomic<uint64_t> s_slots[MaxSamplers] = { };

    uint64_t slotOf(DWORD tid) { return uint64_t(tid) << 32; }


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
        case 0xc00002b4u:  // float multiple faults (SSE)
        case 0xc00002b5u:  // float multiple traps (SSE)
          return true;
        default:
          // array bounds, the float faults, integer divide by zero and overflow, privileged
          // instruction
          return code >= 0xc000008cu && code <= 0xc0000096u;
      }
    }


    /**
     * The fault watch: first in the vectored list when installed, so a handler that resolves a
     * fault can't hide it (one added later as first still can). Records the code for a watched
     * thread and changes nothing: no allocation, no lock, the thread's last error untouched
     * (GetCurrentThreadId reads the TEB).
     */
    LONG CALLBACK faultWatch(EXCEPTION_POINTERS* info) {
      const EXCEPTION_RECORD* rec = info != nullptr ? info->ExceptionRecord : nullptr;
      if (rec == nullptr || !isCpuFault(rec->ExceptionCode))
        return EXCEPTION_CONTINUE_SEARCH;

      const uint64_t mine = slotOf(GetCurrentThreadId());
      for (auto& slot : s_slots) {
        uint64_t clean = mine;   // this thread's, no fault yet
        slot.compare_exchange_strong(clean, mine | rec->ExceptionCode, std::memory_order_acq_rel);
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


    /** An image's size and executable sections, from its headers. The module is referenced. */
    struct ImageInfo {
      uintptr_t size = 0u;   // 0: the headers look wrong
      Range     code[kMaxCodeRanges] = { };
      uint32_t  codeRanges = 0u;
    };

    ImageInfo imageInfo(HMODULE module) {
      ImageInfo info;
      const auto* base = reinterpret_cast<const uint8_t*>(module);
      const auto* dos  = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
      if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 4096)
        return info;
      const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
      if (nt->Signature != IMAGE_NT_SIGNATURE
       || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR_MAGIC)
        return info;

      // the section table must lie inside the headers the loader mapped
      const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
      const uint32_t n = nt->FileHeader.NumberOfSections;
      const uintptr_t tableEnd = uintptr_t(reinterpret_cast<const uint8_t*>(sec + n) - base);
      if (n > 96u || tableEnd > nt->OptionalHeader.SizeOfHeaders)
        return info;

      info.size = uintptr_t(nt->OptionalHeader.SizeOfImage);
      for (uint32_t i = 0u; i < n && info.codeRanges < kMaxCodeRanges; i++) {
        if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE))
          continue;
        const uintptr_t vsize = sec[i].Misc.VirtualSize != 0u ? sec[i].Misc.VirtualSize : sec[i].SizeOfRawData;
        info.code[info.codeRanges++] = { uintptr_t(module) + sec[i].VirtualAddress,
                                         uintptr_t(module) + sec[i].VirtualAddress + vsize };
      }
      return info;
    }


    /** A sampler's state. Its thread owns it and frees it; the starter keeps only handles. */
    struct State {
      HANDLE      target  = nullptr;   // the render thread: suspend, resume, get context, wait
      HANDLE      stop    = nullptr;   // set: write the last window and end
      DWORD       tid     = 0u;
      uint32_t    hz      = 0u;
      bool        guard   = true;
      std::atomic<uint64_t>* slot = nullptr;
      void      (*sink)(const char*) = nullptr;

      // the render thread's TEB, and its own stack as it was at the start: [stackLow, stackHigh)
      const NT_TIB* tib   = nullptr;
      uintptr_t   stackLow  = 0u;
      uintptr_t   stackHigh = 0u;

      // the capture thread: `go` asks it for one GetThreadContext into `ctx`, `done` answers
      HANDLE      captureThread = nullptr;
      HANDLE      go      = nullptr;
      HANDLE      done    = nullptr;
      std::atomic<bool> quit = { false };    // the capture thread ends, or skips a late request
      CONTEXT     ctx     = { };
      BOOL        got     = FALSE;

      ModuleTable table;
      Window      window;
      LARGE_INTEGER freq  = { };

      char         rvaName[40] = { };  // the module written by RVA
      int          rvaIndex = -1;      // its index in the table, once seen
      RvaHistogram rvaSamples;         // samples inside it
      RvaHistogram rvaCallers;         // return addresses into it, of samples in a system DLL
    };


    DWORD WINAPI captureMain(void* arg) {
      State& s = *static_cast<State*>(arg);
      while (WaitForSingleObject(s.go, INFINITE) == WAIT_OBJECT_0) {
        std::memset(&s.ctx, 0, sizeof(s.ctx));
        s.ctx.ContextFlags = CONTEXT_CONTROL;
        // A request the sampler gave up on (it resumed the thread and set quit) isn't made: on a
        // running thread Wine would stop it once more to read its context
        if (s.quit.load(std::memory_order_acquire))
          break;
        s.got = GetThreadContext(s.target, &s.ctx);
        SetEvent(s.done);
      }
      return 0u;
    }


    void emit(const State& s, const std::vector<std::string>& lines) {
      for (const auto& l : lines)
        s.sink(l.c_str());
    }


    void emitLine(void (*sink)(const char*), const char* fmt, DWORD a, uint32_t b = 0u) {
      char buf[320];
      std::snprintf(buf, sizeof(buf), fmt, unsigned(a), unsigned(b));
      sink(buf);
    }


    /** Adds \p module (referenced by the caller) to the table: its index, or -1. */
    int addModule(State& s, HMODULE module) {
      char path[MAX_PATH] = { };
      const DWORD len = GetModuleFileNameA(module, path, sizeof(path));
      const ImageInfo info = imageInfo(module);
      if (len == 0u || len >= sizeof(path) || info.size == 0u)
        return -1;
      const int index = s.table.add(uintptr_t(module), info.size, leafName(path), info.code, info.codeRanges);
      if (index >= 0 && s.rvaIndex < 0 && sameName(s.table.at(uint32_t(index)).name, s.rvaName)
       && s.rvaSamples.init(info.size) && s.rvaCallers.init(info.size))
        s.rvaIndex = index;
      return index;
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
      index = addModule(s, module);
      FreeLibrary(module);
      return index;
    }


    /**
     * Makes the table again from the modules loaded now, so the caller search knows them (it looks
     * only in the table: a lookup per stack slot would take the loader lock 64 times a sample),
     * and an unloaded DLL's range doesn't keep its name. At the start and at each window's end.
     */
    void loadTable(State& s) {
      s.table = ModuleTable();
      s.rvaIndex = -1;

      using PFN_Enum = BOOL (WINAPI *)(HANDLE, HMODULE*, DWORD, DWORD*);
      static const auto enumModules = reinterpret_cast<PFN_Enum>(reinterpret_cast<void*>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "K32EnumProcessModules")));
      if (enumModules == nullptr)
        return;
      HMODULE modules[512];
      DWORD bytes = 0u;
      if (!enumModules(GetCurrentProcess(), modules, sizeof(modules), &bytes))
        return;
      const DWORD n = std::min<DWORD>(bytes / sizeof(HMODULE), DWORD(sizeof(modules) / sizeof(HMODULE)));
      for (DWORD i = 0u; i < n; i++) {
        // referenced while it is read: it may be unloading
        HMODULE module = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(modules[i]), &module)
         && module != nullptr) {
          if (s.table.find(uintptr_t(module)) < 0)
            addModule(s, module);
          FreeLibrary(module);
        }
      }
    }


    enum class Sampled { Ok, Missed, ThreadGone, CaptureStuck };

    /**
     * One sample: suspend, read the instruction pointer and copy the top of the stack, resume;
     * then the lookups.
     */
    Sampled sampleOnce(State& s) {
      uintptr_t slots[kStackSlots];
      uint32_t  nSlots = 0u;
      bool      stackRead = false;

      LARGE_INTEGER t0, t1;
      QueryPerformanceCounter(&t0);
      if (SuspendThread(s.target) == DWORD(-1)) {
        if (WaitForSingleObject(s.target, 0u) == WAIT_OBJECT_0)
          return Sampled::ThreadGone;
        s.window.missed++;
        return Sampled::Missed;
      }

      // Suspended: from here to ResumeThread, see the top of this file
      SetEvent(s.go);
      if (WaitForSingleObject(s.done, kCaptureMs) != WAIT_OBJECT_0) {
        s.quit.store(true, std::memory_order_release);
        s_captureStuck.store(true, std::memory_order_release);
        ResumeThread(s.target);
        return Sampled::CaptureStuck;
      }
      const BOOL got = s.got;
      if (got) {
#ifdef _WIN64
        const uintptr_t sp = uintptr_t(s.ctx.Rsp);
#else
        const uintptr_t sp = uintptr_t(s.ctx.Esp);
#endif
        const uintptr_t limit = uintptr_t(s.tib->StackLimit);
        if (sp >= s.stackLow && sp < s.stackHigh && sp >= limit && sp % sizeof(uintptr_t) == 0u) {
          nSlots = uint32_t(std::min<uintptr_t>(kStackSlots, (s.stackHigh - sp) / sizeof(uintptr_t)));
          const auto* src = reinterpret_cast<const volatile uintptr_t*>(sp);
          for (uint32_t i = 0u; i < nSlots; i++)
            slots[i] = src[i];
          stackRead = true;
        }
      }
      ResumeThread(s.target);
      QueryPerformanceCounter(&t1);

      if (!got) {
        s.window.missed++;
        return Sampled::Missed;
      }
#ifdef _WIN64
      const uintptr_t ip = uintptr_t(s.ctx.Rip);
#else
      const uintptr_t ip = uintptr_t(s.ctx.Eip);
#endif
      const uint64_t heldNs = uint64_t(double(t1.QuadPart - t0.QuadPart) * 1.0e9 / double(s.freq.QuadPart));
      const int index = moduleOf(s, ip);
      s.window.add(index, heldNs);
      if (index < 0)
        return Sampled::Ok;

      const Module& m = s.table.at(uint32_t(index));
      if (index == s.rvaIndex)
        s.rvaSamples.add(ip - m.base);
      if (m.leaf >= 0) {
        uint32_t slot = 0u;
        const int caller = stackRead ? s.table.findCaller(slots, nSlots, &slot) : -2;
        s.window.addCaller(m.leaf, caller);
        if (caller >= 0 && caller == s.rvaIndex)
          s.rvaCallers.add(slots[slot] - s.table.at(uint32_t(caller)).base);
      }
      return Sampled::Ok;
    }


    /** The window's lines, then the RVA lines; empties both. */
    void emitWindow(State& s, uint32_t number, double seconds, const char* why) {
      emit(s, windowLines(s.table, s.window, s.tid, number, seconds, s.hz, why));
      if (s.rvaIndex >= 0) {
        emit(s, s.rvaSamples.lines(s.rvaName, "rva", number));
        emit(s, s.rvaCallers.lines(s.rvaName, "caller rva", number));
      }
      s.window = Window();
      s.rvaSamples.clear();
      s.rvaCallers.clear();
    }


    double secondsSince(const State& s, const LARGE_INTEGER& t0) {
      LARGE_INTEGER t;
      QueryPerformanceCounter(&t);
      return double(t.QuadPart - t0.QuadPart) / double(s.freq.QuadPart);
    }


    /** The fault the watch recorded for this sampler's thread, 0 for none. */
    DWORD faultSeen(const State& s) {
      return s.guard ? DWORD(s.slot->load(std::memory_order_acquire) & 0xffffffffu) : 0u;
    }


    DWORD WINAPI samplerMain(void* arg) {
      State& s = *static_cast<State*>(arg);
      QueryPerformanceFrequency(&s.freq);
      bool stuck = false;

      // The watch second: no suspend at all. A thread that faults by then isn't sampled
      const bool released = WaitForSingleObject(s.stop, s.guard ? kWatchMs : 0u) == WAIT_OBJECT_0;
      DWORD fault = faultSeen(s);

      if (released) {
        emit(s, windowLines(s.table, s.window, s.tid, 1u, 0.0, s.hz, "last, device released in the watch second"));
      } else if (fault != 0u) {
        emitLine(s.sink, "D9MT-RT-SAMPLE: thread 0x%04x raised a CPU fault (%08x) in its first second: "
          "not sampled (suspending a thread while Wine handles its fault can end it under Rosetta)", s.tid, fault);
      } else {
        loadTable(s);
        LARGE_INTEGER windowStart, next;
        QueryPerformanceCounter(&windowStart);
        next = windowStart;
        const LONGLONG period = std::max<LONGLONG>(1, s.freq.QuadPart / LONGLONG(s.hz));
        uint32_t number = 1u;
        const char* why = "last, device released";

        for (;;) {
          // the next sample at its deadline (whole ms waits, so it may come early; never two for one)
          next.QuadPart += period;
          LARGE_INTEGER now;
          QueryPerformanceCounter(&now);
          if (next.QuadPart < now.QuadPart - period)
            next = now;
          const LONGLONG ahead = next.QuadPart - now.QuadPart;
          const DWORD waitMs = ahead > 0 ? DWORD(ahead * 1000 / s.freq.QuadPart) : 0u;
          if (WaitForSingleObject(s.stop, waitMs) == WAIT_OBJECT_0)
            break;
          if ((fault = faultSeen(s)) != 0u) {
            why = "last, stopped at a CPU fault";
            break;
          }
          const Sampled r = sampleOnce(s);
          if (r == Sampled::ThreadGone) {
            why = "last, the thread ended";
            break;
          }
          if (r == Sampled::CaptureStuck) {
            why = "last, a context capture took too long";
            stuck = true;
            break;
          }
          const double seconds = secondsSince(s, windowStart);
          if (seconds * 1000.0 >= double(kReportMs)) {
            emitWindow(s, number, seconds, "every 10 s");
            loadTable(s);
            QueryPerformanceCounter(&windowStart);
            number++;
          }
        }

        emitWindow(s, number, secondsSince(s, windowStart), why);
        if (fault != 0u) {
          emitLine(s.sink, "D9MT-RT-SAMPLE: thread 0x%04x raised a CPU fault (%08x): sampling stopped for good "
            "(suspending a thread while Wine handles its fault can end it under Rosetta)", s.tid, fault);
        }
        if (stuck) {
          emitLine(s.sink, "D9MT-RT-SAMPLE: thread 0x%04x: GetThreadContext didn't return within %u ms; the thread "
            "is resumed and sampling stopped for good", s.tid, kCaptureMs);
        }
      }

      s.slot->store(0u, std::memory_order_release);

      // The capture thread: idle unless a capture got stuck. Then it may still write into the
      // state, so the state, its handles and the thread are left as they are
      if (!stuck) {
        s.quit.store(true, std::memory_order_release);
        SetEvent(s.go);
        if (WaitForSingleObject(s.captureThread, 1000u) != WAIT_OBJECT_0)
          stuck = true;
      }
      if (stuck)
        return 0u;

      CloseHandle(s.captureThread);
      CloseHandle(s.go);
      CloseHandle(s.done);
      CloseHandle(s.target);
      CloseHandle(s.stop);
      delete &s;
      return 0u;
    }


    void closeAll(State* s) {
      for (HANDLE h : { s->go, s->done, s->stop, s->target }) {
        if (h != nullptr)
          CloseHandle(h);
      }
    }


    Handle start(uint32_t hz, void (*sink)(const char*), bool guard, const char* rvaModule) {
      const DWORD tid = GetCurrentThreadId();
      if (s_captureStuck.load(std::memory_order_acquire)) {
        emitLine(sink, "D9MT-RT-SAMPLE: thread 0x%04x not sampled: an earlier sampler's context capture got stuck", tid);
        return Handle();
      }
      if (!installWatch()) {
        emitLine(sink, "D9MT-RT-SAMPLE: thread 0x%04x not sampled: the fault watch couldn't be installed", tid);
        return Handle();
      }

      std::atomic<uint64_t>* slot = nullptr;
      for (auto& candidate : s_slots) {
        uint64_t unclaimed = 0u;
        if (candidate.compare_exchange_strong(unclaimed, slotOf(tid), std::memory_order_acq_rel)) {
          slot = &candidate;
          break;
        }
      }
      if (slot == nullptr) {
        emitLine(sink, "D9MT-RT-SAMPLE: thread 0x%04x not sampled: %u samplers run already", tid, MaxSamplers);
        return Handle();
      }

      auto* s = new State();
      s->tid   = tid;
      s->hz    = hz;
      s->guard = guard;
      s->slot  = slot;
      s->sink  = sink;
      std::strncpy(s->rvaName, rvaModule, sizeof(s->rvaName) - 1u);

      // This thread's stack, now, on this thread: the range the stack copy may read
      s->tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
      MEMORY_BASIC_INFORMATION mbi = { };
      if (VirtualQuery(&mbi, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        s->stackLow  = uintptr_t(mbi.AllocationBase);
        s->stackHigh = uintptr_t(s->tib->StackBase);
        if (!(s->stackLow < uintptr_t(&mbi) && uintptr_t(&mbi) < s->stackHigh))
          s->stackLow = s->stackHigh = 0u;   // not on its own stack: the stack is never read
      }

      const HANDLE self = GetCurrentProcess();
      Handle h;
      bool ok = DuplicateHandle(self, GetCurrentThread(), self, &s->target,
          THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | SYNCHRONIZE, FALSE, 0u)
        && (s->stop = CreateEventW(nullptr, TRUE, FALSE, nullptr)) != nullptr
        && (s->go   = CreateEventW(nullptr, FALSE, FALSE, nullptr)) != nullptr
        && (s->done = CreateEventW(nullptr, FALSE, FALSE, nullptr)) != nullptr
        && DuplicateHandle(self, s->stop, self, &h.stop, 0u, FALSE, DUPLICATE_SAME_ACCESS);
      DWORD error = ok ? 0u : GetLastError();
      if (ok && (s->captureThread = CreateThread(nullptr, 0u, captureMain, s, 0u, nullptr)) == nullptr) {
        error = GetLastError();
        ok = false;
      }
      if (ok && (h.thread = CreateThread(nullptr, 0u, samplerMain, s, 0u, nullptr)) == nullptr) {
        error = GetLastError();
        // the capture thread waits on `go`: end it before the state goes, or leave the state
        s->quit.store(true, std::memory_order_release);
        SetEvent(s->go);
        if (WaitForSingleObject(s->captureThread, 1000u) != WAIT_OBJECT_0) {
          if (h.stop != nullptr)
            CloseHandle(h.stop);
          slot->store(0u, std::memory_order_release);
          emitLine(sink, "D9MT-RT-SAMPLE: thread 0x%04x not sampled: no sampler thread (error %u)", tid, error);
          return Handle();
        }
        ok = false;
      }

      if (!ok) {
        if (s->captureThread != nullptr)
          CloseHandle(s->captureThread);
        if (h.stop != nullptr)
          CloseHandle(h.stop);
        closeAll(s);
        delete s;
        slot->store(0u, std::memory_order_release);
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

    char buf[160];
    if (n >= sizeof(value)) {
      // the buffer's content is undefined then: not printed
      std::snprintf(buf, sizeof(buf), "D9MT-RT-SAMPLE: D9MT_RT_SAMPLE ignored: %u characters (a whole number of Hz, 1 to %u)",
        unsigned(n - 1u), unsigned(kMaxHz));
      sink(buf);
      return Handle();
    }
    bool invalid = false;
    const uint32_t hz = hzFromEnv(value, &invalid);
    if (invalid) {
      std::snprintf(buf, sizeof(buf), "D9MT-RT-SAMPLE: D9MT_RT_SAMPLE=\"%s\" ignored (a whole number of Hz, 1 to %u)",
        value, unsigned(kMaxHz));
      sink(buf);
      return Handle();
    }
    return hz != 0u ? start(hz, sink, true, kRvaModule) : Handle();
  }


  Handle startForTest(uint32_t hz, void (*sink)(const char* line), bool guard, const char* rvaModule) {
    return start(hz, sink, guard, rvaModule);
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
