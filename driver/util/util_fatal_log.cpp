#include "util_fatal_log.h"

#include <windows.h>

#include <cstdint>
#include <cstring>

// The handler runs while the program may be crashing: the heap may be corrupt, another thread may
// hold the loader lock, the stack may be nearly gone. So it allocates nothing, takes no lock (no
// Logger, no GetModuleHandleEx, no snprintf), formats into a fixed buffer on its stack and writes
// the line in one call. Everything it looks up is looked up by install().

namespace dxvk::fatal {

  namespace {

    using PFN_wineDbgOutput        = int (__cdecl *)(const char*);
    using PFN_getThreadDescription = HRESULT (WINAPI *)(HANDLE, PWSTR*);

    /** Lines per process; then one line that says more were left out. */
    constexpr uint32_t MaxLines = 8u;

    PFN_wineDbgOutput        s_wineOutput           = nullptr;
    PFN_getThreadDescription s_getThreadDescription = nullptr;
    std::atomic<void (*)(const char*)> s_sink       = { nullptr };
    DWORD                    s_tls                  = TLS_OUT_OF_INDEXES;
    uintptr_t                s_exeBase              = 0u;
    char                     s_exeName[64]          = { };
    std::atomic<uint32_t>    s_seen                 = { 0u };


    struct Line {
      char   buf[512] = { };
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
        case 0xc00000fdu: return "stack overflow";
        case 0xc0000374u: return "heap corruption";
        case 0xc0000409u: return "stack buffer overrun";
        default:          return "fatal exception";
      }
    }


    /** Bytes readable from \p p in its region, 0 if none (uncommitted, no access, guard). */
    size_t readableBytes(const void* p) {
      MEMORY_BASIC_INFORMATION mbi;
      if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT
       || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || mbi.Protect == 0u)
        return 0u;
      return uintptr_t(mbi.BaseAddress) + mbi.RegionSize - uintptr_t(p);
    }


    /**
     * The PE image that holds \p addr: its base (0 if none) and its name, from the export
     * directory, or the exe's file name. VirtualQuery and the image headers instead of
     * GetModuleHandleEx, which takes the loader lock.
     */
    uintptr_t imageOf(const void* addr, char* name, size_t nameSize) {
      std::strcpy(name, "?");

      MEMORY_BASIC_INFORMATION mbi;
      if (!VirtualQuery(addr, &mbi, sizeof(mbi)) || mbi.Type != MEM_IMAGE || mbi.AllocationBase == nullptr)
        return 0u;

      const auto* base = static_cast<const uint8_t*>(mbi.AllocationBase);

      if (uintptr_t(base) == s_exeBase && s_exeName[0] != '\0') {
        std::strncpy(name, s_exeName, nameSize - 1u);
        name[nameSize - 1u] = '\0';
        return uintptr_t(base);
      }

      const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
      if (readableBytes(dos) < sizeof(*dos) || dos->e_magic != IMAGE_DOS_SIGNATURE
       || dos->e_lfanew <= 0 || dos->e_lfanew > 4096)
        return uintptr_t(base);

      const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
      if (readableBytes(nt) < sizeof(*nt) || nt->Signature != IMAGE_NT_SIGNATURE)
        return uintptr_t(base);

      const uint32_t imageSize = nt->OptionalHeader.SizeOfImage;
      if (nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT)
        return uintptr_t(base);

      const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
      if (dir.VirtualAddress == 0u || dir.VirtualAddress >= imageSize || dir.Size < sizeof(IMAGE_EXPORT_DIRECTORY))
        return uintptr_t(base);

      const auto* exp = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + dir.VirtualAddress);
      if (readableBytes(exp) < sizeof(*exp) || exp->Name == 0u || exp->Name >= imageSize)
        return uintptr_t(base);

      const char* s = reinterpret_cast<const char*>(base + exp->Name);
      size_t avail = readableBytes(s);
      size_t i = 0u;
      while (i + 1u < nameSize && i < avail && s[i] != '\0') {
        name[i] = s[i];
        i++;
      }
      name[i] = '\0';
      if (i == 0u)
        std::strcpy(name, "?");
      return uintptr_t(base);
    }


    /** The thread's name (SetThreadDescription), ASCII only, or "(unnamed)". */
    void threadName(Line& line) {
      PWSTR w = nullptr;
      if (s_getThreadDescription == nullptr
       || FAILED(s_getThreadDescription(GetCurrentThread(), &w)) || w == nullptr) {
        line.str("(unnamed)");
        return;
      }
      char name[48];
      size_t n = 0u;
      for (; w[n] != L'\0' && n + 1u < sizeof(name); n++)
        name[n] = (w[n] >= 0x20 && w[n] < 0x7f && w[n] != L'"') ? char(w[n]) : '?';
      name[n] = '\0';
      LocalFree(w);
      if (n == 0u) {
        line.str("(unnamed)");
        return;
      }
      line.str("\"");
      line.str(name);
      line.str("\"");
    }


    void emit(const Line& line) {
      if (auto sink = s_sink.load(std::memory_order_acquire)) {
        sink(line.buf);
      } else if (s_wineOutput != nullptr) {
        s_wineOutput(line.buf);
      } else {
        DWORD written = 0u;
        WriteFile(GetStdHandle(STD_ERROR_HANDLE), line.buf, DWORD(line.len), &written, nullptr);
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

      const void* addr = rec->ExceptionAddress;
      line.str(" at ");
      line.ptr(uint64_t(uintptr_t(addr)));

      char module[64];
      const uintptr_t base = imageOf(addr, module, sizeof(module));
      if (base != 0u) {
        line.str(" (");
        line.str(module);
        line.str("+0x");
        line.hex(uint64_t(uintptr_t(addr) - base), 1u);
        line.str(")");
      } else {
        line.str(" (not in a PE module)");
      }

      line.str(", thread 0x");
      line.hex(GetCurrentThreadId(), 4u);
      line.str(" ");
      // Not for a stack overflow (little stack left) or a corrupt heap (the name is allocated)
      if (code == 0xc00000fdu || code == 0xc0000374u)
        line.str("(name not read)");
      else
        threadName(line);

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


    LONG CALLBACK handler(EXCEPTION_POINTERS* info) {
      const EXCEPTION_RECORD* rec = info != nullptr ? info->ExceptionRecord : nullptr;

      // C++ throws (0xE06D7363, mingw's 0x20474343), thread naming (0x406D1388), debug output,
      // breakpoints and guard pages are not of the 0xC0000000 class: return before anything else
      if (rec == nullptr || (rec->ExceptionCode & 0xf0000000u) != 0xc0000000u)
        return EXCEPTION_CONTINUE_SEARCH;

      // The program's handler may read the thread's last error: keep it
      const DWORD lastError = GetLastError();

      // Not again on this thread while a line is being written (a fault in the handler itself)
      if (s_tls != TLS_OUT_OF_INDEXES) {
        if (TlsGetValue(s_tls) != nullptr) {
          SetLastError(lastError);
          return EXCEPTION_CONTINUE_SEARCH;
        }
        TlsSetValue(s_tls, reinterpret_cast<void*>(1));
      }

      const uint32_t n = s_seen.fetch_add(1u, std::memory_order_relaxed) + 1u;
      if (n <= MaxLines)
        writeLine(rec);
      else if (n == MaxLines + 1u)
        writeCapLine();

      if (s_tls != TLS_OUT_OF_INDEXES)
        TlsSetValue(s_tls, nullptr);

      SetLastError(lastError);
      return EXCEPTION_CONTINUE_SEARCH;
    }


    template<typename T>
    T procAddress(HMODULE module, const char* name) {
      return module != nullptr
        ? reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(module, name)))
        : nullptr;
    }


    bool doInstall() {
      char value[8] = { };
      const DWORD n = GetEnvironmentVariableA("D9MT_FATAL_LOG", value, sizeof(value));
      if (n > 0u && n < sizeof(value) && std::strcmp(value, "0") == 0)
        return false;

      s_wineOutput = procAddress<PFN_wineDbgOutput>(GetModuleHandleA("ntdll.dll"), "__wine_dbg_output");
      s_getThreadDescription = procAddress<PFN_getThreadDescription>(GetModuleHandleA("kernelbase.dll"), "GetThreadDescription");
      if (s_getThreadDescription == nullptr)
        s_getThreadDescription = procAddress<PFN_getThreadDescription>(GetModuleHandleA("kernel32.dll"), "GetThreadDescription");

      s_exeBase = uintptr_t(GetModuleHandleW(nullptr));
      char path[MAX_PATH] = { };
      const DWORD len = GetModuleFileNameA(nullptr, path, sizeof(path));
      if (len > 0u && len < sizeof(path)) {
        const char* leaf = path;
        for (const char* p = path; *p != '\0'; p++) {
          if (*p == '\\' || *p == '/')
            leaf = p + 1;
        }
        std::strncpy(s_exeName, leaf, sizeof(s_exeName) - 1u);
      }

      s_tls = TlsAlloc();
      // Last in the vectored list: another vectored handler that resolves an exception
      // (EXCEPTION_CONTINUE_EXECUTION) runs first, so what it resolves isn't logged.
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
