#include "util_fatal_log.h"

#include <windows.h>

#include <cstdint>
#include <cstring>

// The handler runs while the program may be crashing: the heap may be corrupt, another thread may
// hold the loader lock, an image may be going away. So it allocates nothing, takes no lock, reads
// no memory it didn't write itself at install (no PE headers, no thread names, no Logger, no
// snprintf), formats into a fixed buffer on its stack and writes the line with one call.
// Everything it needs is looked up by install(), outside any exception.
//
// Its only system calls: GetLastError/SetLastError and GetCurrentThreadId (TEB fields),
// VirtualQuery (one NtQueryVirtualMemory) and the write.
//
// The recursion guard is a fixed table of the thread ids inside the handler, not a TLS slot: in
// Wine 11 / CrossOver 26.3, TlsSetValue on an index past the first 64 allocates the thread's
// expansion slots from the process heap on first use (dlls/kernelbase/thread.c), and a game
// that uses many slots would give d3d9.dll such an index.
//
// The write: Wine's __wine_dbg_write, one Unix write(2) of the whole line to the process's stderr,
// which is the game's log. Not __wine_dbg_output: in Wine 11 / CrossOver 26.3 that appends to a
// per-thread line buffer in the TEB and raises STATUS_BUFFER_OVERFLOW when the buffer can't take
// the text (dlls/ntdll/thread.c append_output), so a fault in the middle of a long debug line
// would get a second exception from the logger. __wine_dbg_write has no buffer and no state
// (dlls/ntdll/unix/debug.c unixcall_wine_dbg_write: `write( 2, str, len )`). Outside Wine,
// WriteFile to the stderr handle read at install.

namespace dxvk::fatal {

  namespace {

    using PFN_wineDbgWrite = int (WINAPI *)(const char*, unsigned int);

    /** Lines per process; then one line that says more were left out. */
    constexpr uint32_t MaxLines = 8u;

    constexpr DWORD StatusStackOverflow = 0xc00000fdu;

    /** A module named in the lines: its base and file name, taken at install. */
    struct Module {
      uintptr_t base;
      char      name[40];
    };

    /** The modules a fault in the game usually comes from; any other image is `base 0x…`. */
    constexpr const char* SnapshotNames[] = {
      "d3d9.dll", "winemetal.dll", "ntdll.dll", "kernelbase.dll", "kernel32.dll", "user32.dll",
      "win32u.dll", "gdi32.dll", "ucrtbase.dll", "msvcrt.dll", "msvcp140.dll", "vcruntime140.dll",
      "vcruntime140_1.dll", "dxgi.dll", "dinput8.dll", "xinput1_3.dll",
    };

    constexpr size_t MaxModules = 2u + sizeof(SnapshotNames) / sizeof(SnapshotNames[0]);

    PFN_wineDbgWrite                   s_wineWrite = nullptr;
    HANDLE                             s_stderr    = nullptr;
    std::atomic<void (*)(const char*)> s_sink      = { nullptr };
    std::atomic<DWORD>                 s_active[64] = { };
    Module                             s_modules[MaxModules] = { };
    size_t                             s_moduleCount = 0u;
    std::atomic<uint32_t>              s_seen      = { 0u };


    struct Line {
      char   buf[384] = { };
      size_t len      = 0u;

      void str(const char* s) {
        while (s != nullptr && *s != '\0' && len + 1u < sizeof(buf))
          buf[len++] = *s++;
      }

      void hex(uint64_t v, uint32_t minDigits) {
        char tmp[16];
        uint32_t n = 0u;
        do {
          tmp[n++] = "0123456789abcdef"[v & 0xfu];
          v >>= 4;
        } while (v != 0u && n < 16u);
        while (n < minDigits && n < 16u)
          tmp[n++] = '0';
        while (n != 0u && len + 1u < sizeof(buf))
          buf[len++] = tmp[--n];
      }

      void ptr(uint64_t v) {
        str("0x");
        hex(v, uint32_t(sizeof(void*) * 2u));
      }

      void dec(uint32_t v) {
        char tmp[10];
        uint32_t n = 0u;
        do {
          tmp[n++] = char('0' + v % 10u);
          v /= 10u;
        } while (v != 0u && n < 10u);
        while (n != 0u && len + 1u < sizeof(buf))
          buf[len++] = tmp[--n];
      }
    };


    const char* codeName(DWORD code) {
      switch (code) {
        case 0xc0000005u: return "access violation";
        case 0xc0000006u: return "in-page error";
        case 0xc000001du: return "illegal instruction";
        case 0xc0000025u: return "noncontinuable exception";
        case 0xc0000026u: return "invalid disposition";
        case 0xc000008cu: return "array bounds exceeded";
        case 0xc000008du: return "float denormal operand";
        case 0xc000008eu: return "float divide by zero";
        case 0xc000008fu: return "float inexact result";
        case 0xc0000090u: return "float invalid operation";
        case 0xc0000091u: return "float overflow";
        case 0xc0000092u: return "float stack check";
        case 0xc0000093u: return "float underflow";
        case 0xc0000094u: return "integer divide by zero";
        case 0xc0000095u: return "integer overflow";
        case 0xc0000096u: return "privileged instruction";
        case 0xc0000374u: return "heap corruption";
        case 0xc0000409u: return "stack buffer overrun";
        default:          return "fatal exception";
      }
    }


    /** The snapshot's name for the image at \p base, or nullptr. */
    const char* moduleName(uintptr_t base) {
      for (size_t i = 0u; i < s_moduleCount; i++) {
        if (s_modules[i].base == base)
          return s_modules[i].name;
      }
      return nullptr;
    }


    void emit(const Line& line) {
      if (auto sink = s_sink.load(std::memory_order_acquire)) {
        sink(line.buf);
      } else if (s_wineWrite != nullptr) {
        s_wineWrite(line.buf, unsigned(line.len));
      } else if (s_stderr != nullptr && s_stderr != INVALID_HANDLE_VALUE) {
        DWORD written = 0u;
        WriteFile(s_stderr, line.buf, DWORD(line.len), &written, nullptr);
      }
    }


    void writeLine(const EXCEPTION_RECORD* rec) {
      const DWORD code = rec->ExceptionCode;
      Line line;
      line.str("err:   D9MT-FATAL: ");
      line.hex(code, 8u);
      line.str(" ");
      line.str(codeName(code));

      if ((code == 0xc0000005u || code == 0xc0000006u) && rec->NumberParameters >= 2u) {
        const ULONG_PTR kind = rec->ExceptionInformation[0];
        line.str(kind == 0u ? " (read of " : kind == 1u ? " (write to " : kind == 8u ? " (execute at " : " (access to ");
        line.ptr(uint64_t(rec->ExceptionInformation[1]));
        line.str(")");
      }

      const uintptr_t addr = uintptr_t(rec->ExceptionAddress);
      line.str(" at ");
      line.ptr(uint64_t(addr));

      // The image's base from the kernel's region record, not from the image: nothing of the
      // image is read, so an image unmapped or reprotected meanwhile can't fault here
      MEMORY_BASIC_INFORMATION mbi;
      if (VirtualQuery(rec->ExceptionAddress, &mbi, sizeof(mbi)) == sizeof(mbi)
       && mbi.Type == MEM_IMAGE && mbi.AllocationBase != nullptr
       && uintptr_t(mbi.AllocationBase) <= addr) {
        const uintptr_t base = uintptr_t(mbi.AllocationBase);
        line.str(" (");
        if (const char* name = moduleName(base)) {
          line.str(name);
          line.str("+0x");
        } else {
          line.str("base ");
          line.ptr(uint64_t(base));
          line.str("+0x");
        }
        line.hex(uint64_t(addr - base), 1u);
        line.str(")");
      } else {
        line.str(" (not in a PE module)");
      }

      line.str(", thread 0x");
      line.hex(GetCurrentThreadId(), 4u);

      line.str(", last d9mt step: ");
      const char* step = g_breadcrumb.load(std::memory_order_relaxed);
      line.str(step != nullptr ? step : "(none yet)");
      line.str("; the program's own handler may still take it\n");
      emit(line);
    }


    void writeCapLine() {
      Line line;
      line.str("err:   D9MT-FATAL: more fatal exceptions, not logged (the first ");
      line.dec(MaxLines);
      line.str(" are above)\n");
      emit(line);
    }


    /** Claims a line: its number (1-based), or 0 once the cap line went out. Never wraps. */
    uint32_t claimLine() {
      uint32_t n = s_seen.load(std::memory_order_relaxed);
      do {
        if (n > MaxLines)
          return 0u;
      } while (!s_seen.compare_exchange_weak(n, n + 1u, std::memory_order_relaxed));
      return n + 1u;
    }


    /**
     * Marks thread \p tid as inside the handler: its slot, or nullptr if the thread is in it
     * already (a nested fault) or 64 threads are (then this one isn't logged). Only the thread
     * itself writes its id, so the check for it can't race.
     */
    std::atomic<DWORD>* enter(DWORD tid) {
      for (auto& a : s_active) {
        if (a.load(std::memory_order_acquire) == tid)
          return nullptr;
      }
      for (auto& a : s_active) {
        DWORD expected = 0u;
        if (a.compare_exchange_strong(expected, tid, std::memory_order_acq_rel))
          return &a;
      }
      return nullptr;
    }


    LONG CALLBACK handler(EXCEPTION_POINTERS* info) {
      const EXCEPTION_RECORD* rec = info != nullptr ? info->ExceptionRecord : nullptr;

      // C++ throws (0xE06D7363, mingw's 0x20474343), thread naming (0x406D1388), debug output,
      // breakpoints and guard pages are not of the 0xC0000000 class: return before anything else
      if (rec == nullptr || (rec->ExceptionCode & 0xf0000000u) != 0xc0000000u)
        return EXCEPTION_CONTINUE_SEARCH;

      // A stack overflow is never logged: the thread is running on its last guard page, and
      // nothing here is proven to fit (the line alone is a few hundred bytes of stack)
      if (rec->ExceptionCode == StatusStackOverflow)
        return EXCEPTION_CONTINUE_SEARCH;

      // The program's handler may read the thread's last error: keep it
      const DWORD lastError = GetLastError();

      // Not again on this thread while a line is being written (a fault in the handler itself:
      // that one goes on to the program's handlers unlogged, and this line still goes out)
      std::atomic<DWORD>* slot = enter(GetCurrentThreadId());
      if (slot != nullptr) {
        const uint32_t n = claimLine();
        if (n != 0u && n <= MaxLines)
          writeLine(rec);
        else if (n == MaxLines + 1u)
          writeCapLine();
        slot->store(0u, std::memory_order_release);
      }

      SetLastError(lastError);
      return EXCEPTION_CONTINUE_SEARCH;
    }


    template<typename T>
    T procAddress(HMODULE module, const char* name) {
      return module != nullptr
        ? reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(module, name)))
        : nullptr;
    }


    /** Adds \p module to the snapshot under its file name, once. */
    void snapshotModule(HMODULE module) {
      if (module == nullptr || s_moduleCount >= MaxModules || moduleName(uintptr_t(module)) != nullptr)
        return;

      char path[MAX_PATH] = { };
      const DWORD len = GetModuleFileNameA(module, path, sizeof(path));
      if (len == 0u || len >= sizeof(path))
        return;

      const char* leaf = path;
      for (const char* p = path; *p != '\0'; p++) {
        if (*p == '\\' || *p == '/')
          leaf = p + 1;
      }

      Module& m = s_modules[s_moduleCount];
      m.base = uintptr_t(module);
      std::strncpy(m.name, leaf, sizeof(m.name) - 1u);
      m.name[sizeof(m.name) - 1u] = '\0';
      s_moduleCount++;
    }


    bool doInstall() {
      char value[8] = { };
      const DWORD n = GetEnvironmentVariableA("D9MT_FATAL_LOG", value, sizeof(value));
      if (n > 0u && n < sizeof(value) && std::strcmp(value, "0") == 0)
        return false;

      // The module that holds the handler stays mapped for the life of the process: the
      // handler can't be removed safely while another thread may be inside it, and the
      // breadcrumbs it reads are literals in this module. So never unloaded, never removed.
      HMODULE self = nullptr;
      if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                              reinterpret_cast<LPCWSTR>(&handler), &self))
        return false;

      s_wineWrite = procAddress<PFN_wineDbgWrite>(GetModuleHandleA("ntdll.dll"), "__wine_dbg_write");
      s_stderr    = GetStdHandle(STD_ERROR_HANDLE);

      snapshotModule(GetModuleHandleW(nullptr));
      snapshotModule(self);
      for (const char* name : SnapshotNames)
        snapshotModule(GetModuleHandleA(name));

      // Last in the vectored list at install: another vectored handler already registered that
      // resolves an exception (EXCEPTION_CONTINUE_EXECUTION) runs first, so what it resolves isn't
      // logged. Vectored handlers run before the frame-based ones and the top-level filter.
      return AddVectoredExceptionHandler(0u, handler) != nullptr;
    }

  }


  bool install() {
    static const bool s_installed = doInstall();
    return s_installed;
  }


  void setSinkForTest(void (*sink)(const char* line)) {
    s_sink.store(sink, std::memory_order_release);
  }

}
