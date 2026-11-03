// Unit test for the fatal-exception log (driver/util/util_fatal_log.cpp). A Windows program (x64):
// scripts/test-fatal-log.sh builds it and test/native/fatal_log_dll.cpp with mingw and runs it
// under Wine. Modes:
//
//   (none)           lines go to a capture sink:
//     - a real integer divide by zero (`div` by a zero register) is one D9MT-FATAL line with the
//       code, its name, the address as fatal_log_test.exe+0x<rva>, the thread's id (no name) and
//       the last breadcrumb; the thread's last error is kept;
//     - a real access violation also says what was read and where;
//     - a RaiseException is named by the snapshot's module (kernelbase.dll or ntdll.dll+0x…);
//     - C++ throws (mingw's own, and MSVC's code 0xE06D7363), thread naming (0x406D1388),
//       OutputDebugString, a breakpoint-class code and a stack overflow (0xC00000FD) are no line;
//     - a nested fault (in the sink, i.e. inside the handler) is not logged and still reaches
//       the program's handler, and the outer line still goes out;
//     - a fault in a DLL loaded after install is `base 0x<its base>+0x<rva>`, also with the
//       DLL's headers made unreadable (the handler reads nothing of an image);
//     - a fault in executable memory that isn't an image is "(not in a PE module)";
//     - the handler allocates nothing: a fault on a thread while every heap of the process is
//       locked (HeapLock) is logged and resolved, no deadlock;
//     - the handler never resolves an exception: the test's own vectored handler, registered
//       after it, still sees every one;
//     - after 8 lines, one line says more were left out, then nothing, however many follow.
//   --wine-output    no sink: one divide by zero under the heap lock, resolved by the test; the
//                    script finds the line in Wine's debug output (the game's log).
//   --unhandled      as a game that catches its own crash: a top-level filter that returns
//                    EXCEPTION_EXECUTE_HANDLER, then a divide by zero nobody resolves. Wine ends
//                    the process with the exception code, so the Unix exit status is 148 (0x94);
//                    the script checks it and the line.
//   --stack-overflow a real stack overflow nobody handles: no line (the script checks the log),
//                    the process ends as without the log.
//   --tls-exhausted  every TLS slot taken first: the log needs none, so it installs and logs.
//   --dll-unload     the log installed from fatal_log_dll.dll, which is then freed: the DLL stays
//                    mapped (pinned at install) and a later fault is logged with the DLL's
//                    breadcrumb, no crash.
//   --expect-off     D9MT_FATAL_LOG=0 (set by the script): install() returns false.
//
// Exit 0 if every check passed (except --unhandled and --stack-overflow, which never return).
#include "../../driver/util/util_fatal_log.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#if !defined(__x86_64__)
#error "fatal_log_test is an x64 program (the faults are x86-64 instructions)"
#endif

using namespace dxvk;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static std::vector<std::string> g_lines;
static void sink(const char* line) { g_lines.push_back(line); }

static bool has(size_t i, const char* text) {
  return i < g_lines.size() && g_lines[i].find(text) != std::string::npos;
}

// A sink that allocates nothing: for the heap-lock case
static char  g_fixedLine[512];
static LONG  g_fixedCount = 0;
static void fixedSink(const char* line) {
  std::strncpy(g_fixedLine, line, sizeof(g_fixedLine) - 1u);
  InterlockedIncrement(&g_fixedCount);
}

// ---- the faults: fixed instructions, so the test's handler can step over them -------------

static void divideByZero() {
  // div %ecx (F7 F1) with ecx = 0
  asm volatile("xor %%edx, %%edx\n\tmov $1, %%eax\n\tdiv %%ecx" : : "c"(0) : "eax", "edx", "memory");
}

static void readAt0x10() {
  // mov (%rax), %eax (8B 00) with rax = 0x10
  uint64_t p = 0x10u;
  asm volatile("mov (%%rax), %%eax" : "+a"(p) : : "memory");
}

// ---- the test's vectored handler: after the one under test; resolves what the test raised --

static LONG g_seen[5] = { };   // c0000094, c0000005, c0000001, not-logged codes, c00000fd

static LONG CALLBACK testHandler(EXCEPTION_POINTERS* info) {
  const DWORD code = info->ExceptionRecord->ExceptionCode;
  const auto* rip = reinterpret_cast<const uint8_t*>(info->ContextRecord->Rip);

  if (code == 0xc0000094u && rip[0] == 0xf7 && rip[1] == 0xf1) {
    InterlockedIncrement(&g_seen[0]);
    info->ContextRecord->Rip += 2u;
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  if (code == 0xc0000005u && rip[0] == 0x8b && rip[1] == 0x00) {
    InterlockedIncrement(&g_seen[1]);
    info->ContextRecord->Rip += 2u;
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  // Raised by RaiseException below (continuable): continuing returns from RaiseException
  if (code == 0xc0000001u) {
    InterlockedIncrement(&g_seen[2]);
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  if (code == 0xe06d7363u || code == 0x406d1388u || code == 0x80000003u) {
    InterlockedIncrement(&g_seen[3]);
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  if (code == 0xc00000fdu && info->ExceptionRecord->ExceptionFlags == 0u) {
    InterlockedIncrement(&g_seen[4]);
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

// ---- a nested fault: the sink (inside the handler) faults once ----------------------------

static bool g_faultInSink = false;
static void faultingSink(const char* line) {
  if (g_faultInSink) {
    g_faultInSink = false;
    readAt0x10();
  }
  g_lines.push_back(line);
}

// ---- the heap-lock witness ----------------------------------------------------------------

struct HeapLockRun {
  HANDLE ready = nullptr, go = nullptr, done = nullptr;
};

static DWORD WINAPI heapLockThread(void* p) {
  auto* run = static_cast<HeapLockRun*>(p);
  SetEvent(run->ready);
  WaitForSingleObject(run->go, INFINITE);
  divideByZero();
  SetEvent(run->done);
  return 0u;
}

/** A divide by zero on a thread while every heap of the process is locked: true if it ended. */
static bool divideUnderHeapLock() {
  HeapLockRun run;
  run.ready = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  run.go    = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  run.done  = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  HANDLE t = CreateThread(nullptr, 0u, heapLockThread, &run, 0u, nullptr);
  if (t == nullptr)
    return false;
  WaitForSingleObject(run.ready, INFINITE);

  static HANDLE heaps[256];
  DWORD count = GetProcessHeaps(256u, heaps);
  if (count > 256u)
    count = 256u;
  DWORD locked = 0u;
  for (DWORD i = 0u; i < count; i++)
    locked += HeapLock(heaps[i]) ? 1u : 0u;

  SetEvent(run.go);
  const bool ended = WaitForSingleObject(run.done, 5000u) == WAIT_OBJECT_0;

  for (DWORD i = 0u; i < count; i++)
    HeapUnlock(heaps[i]);
  std::printf("heap lock: %lu of %lu heaps locked, the fault %s\n", (unsigned long)locked,
              (unsigned long)count, ended ? "ended" : "did NOT end within 5 s");

  WaitForSingleObject(t, ended ? INFINITE : 5000u);
  CloseHandle(t);
  return ended && locked != 0u && locked == count;
}

// ---- the other modes ----------------------------------------------------------------------

static LONG WINAPI gameLikeFilter(EXCEPTION_POINTERS*) {
  return EXCEPTION_EXECUTE_HANDLER;
}

__attribute__((noinline)) static uint32_t recurse(uint32_t n) {
  volatile char pad[4096];
  pad[0] = char(n);
  pad[sizeof(pad) - 1u] = char(n);
  if (n > 1000000u)   // never: 4 GiB of stack
    return n;
  return recurse(n + 1u) + uint32_t(pad[0]) + uint32_t(pad[sizeof(pad) - 1u]);
}

using PFN_dllInstall    = int (*)();
using PFN_dllSetSink    = void (*)(void (*)(const char*));
using PFN_dllVoid       = void (*)();

template<typename T>
static T dllProc(HMODULE dll, const char* name) {
  return reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(dll, name)));
}

static int dllUnload() {
  HMODULE dll = LoadLibraryA("fatal_log_dll.dll");
  CHECK(dll != nullptr);
  if (dll == nullptr)
    return 1;
  auto install    = dllProc<PFN_dllInstall>(dll, "fatal_dll_install");
  auto setSink    = dllProc<PFN_dllSetSink>(dll, "fatal_dll_set_sink");
  auto breadcrumb = dllProc<PFN_dllVoid>(dll, "fatal_dll_breadcrumb");
  CHECK(install != nullptr && setSink != nullptr && breadcrumb != nullptr);
  if (install == nullptr || setSink == nullptr || breadcrumb == nullptr)
    return 1;

  CHECK(install() == 1);
  setSink(sink);
  breadcrumb();
  AddVectoredExceptionHandler(0u, testHandler);

  CHECK(FreeLibrary(dll));
  // Pinned at install: still mapped, as is its handler
  CHECK(GetModuleHandleA("fatal_log_dll.dll") == dll);

  divideByZero();
  try {
    throw std::runtime_error("a C++ exception after the unload");
  } catch (const std::exception&) { }

  CHECK(g_seen[0] == 1);
  CHECK(g_lines.size() == 1u);
  CHECK(has(0, "c0000094 integer divide by zero at 0x"));
  CHECK(has(0, "(fatal_log_test.exe+0x"));
  CHECK(has(0, "last d9mt step: test step: in the unloaded dll;"));
  for (size_t i = 0; i < g_lines.size(); i++)
    std::printf("line %zu: %s", i, g_lines[i].c_str());
  std::printf("fatal_log_test --dll-unload: %s (%d failures)\n", g_fail ? "FAIL" : "ok", g_fail);
  return g_fail ? 1 : 0;
}

static std::string warmUp(const std::string& s) {
  static const std::string first = s;   // a guarded static: the runtime's own TLS use happens here
  return first;
}

static int tlsExhausted() {
  // The test program's runtime (mingw's static-init guard, stdio) takes TLS slots on first use and
  // aborts without one; that's the test's runtime, not the log. So it's warmed up first, and every
  // slot is free again before anything else runs.
  std::printf("fatal_log_test --tls-exhausted: %s\n", warmUp("start").c_str());
  std::fflush(stdout);
  static DWORD taken[4096];
  DWORD slots = 0u;
  for (DWORD i; slots < 4096u && (i = TlsAlloc()) != TLS_OUT_OF_INDEXES; )
    taken[slots++] = i;
  const bool exhausted = TlsAlloc() == TLS_OUT_OF_INDEXES;
  const bool installed = fatal::install();
  for (DWORD i = 0u; i < slots; i++)
    TlsFree(taken[i]);
  CHECK(exhausted);
  CHECK(installed);

  fatal::setSinkForTest(sink);
  AddVectoredExceptionHandler(0u, testHandler);
  fatal::breadcrumb("test step: no tls");
  divideByZero();
  CHECK(g_seen[0] == 1);
  CHECK(g_lines.size() == 1u);
  CHECK(has(0, "c0000094 integer divide by zero at 0x"));
  CHECK(has(0, "last d9mt step: test step: no tls;"));
  for (size_t i = 0; i < g_lines.size(); i++)
    std::printf("line %zu: %s", i, g_lines[i].c_str());
  std::printf("fatal_log_test --tls-exhausted: %s (%lu slots taken first, %d failures)\n",
              g_fail ? "FAIL" : "ok", (unsigned long)slots, g_fail);
  return g_fail ? 1 : 0;
}

int main(int argc, char** argv) {
  const std::string mode = argc > 1 ? argv[1] : "";

  if (mode == "--expect-off") {
    CHECK(!fatal::install());
    std::printf("fatal_log_test --expect-off: %s\n", g_fail ? "FAIL" : "ok");
    return g_fail ? 1 : 0;
  }
  if (mode == "--tls-exhausted")
    return tlsExhausted();
  if (mode == "--dll-unload")
    return dllUnload();

  CHECK(fatal::install());
  CHECK(fatal::install());   // once per process; the second call only answers

  if (mode == "--unhandled") {
    SetUnhandledExceptionFilter(gameLikeFilter);
    fatal::breadcrumb("test step: unhandled divide");
    std::fflush(stdout);
    divideByZero();
    std::printf("fatal_log_test --unhandled: FAIL (still running)\n");
    return 1;
  }

  if (mode == "--stack-overflow") {
    SetUnhandledExceptionFilter(gameLikeFilter);
    fatal::breadcrumb("test step: stack overflow");
    std::printf("fatal_log_test --stack-overflow: recursing\n");
    std::fflush(stdout);
    const uint32_t r = recurse(0u);
    std::printf("fatal_log_test --stack-overflow: FAIL (still running, %u)\n", r);
    return 1;
  }

  AddVectoredExceptionHandler(0u, testHandler);

  if (mode == "--wine-output") {
    fatal::breadcrumb("test step: wine output");
    const bool ended = divideUnderHeapLock();
    const bool ok = ended && g_seen[0] == 1;
    std::printf("fatal_log_test --wine-output: %s\n", ok ? "ok" : "FAIL");
    return ok ? 0 : 1;
  }

  fatal::setSinkForTest(sink);

  // Not logged: C++ throws, thread naming, debug output, breakpoint-class codes, stack overflow
  try {
    throw std::runtime_error("a C++ exception");
  } catch (const std::exception&) { }
  RaiseException(0xe06d7363u, 0u, 0u, nullptr);
  struct { DWORD type; LPCSTR name; DWORD threadId; DWORD flags; } nameInfo = { 0x1000u, "named", DWORD(-1), 0u };
  RaiseException(0x406d1388u, 0u, sizeof(nameInfo) / sizeof(ULONG_PTR), reinterpret_cast<const ULONG_PTR*>(&nameInfo));
  OutputDebugStringA("fatal_log_test: debug output\n");
  RaiseException(0x80000003u, 0u, 0u, nullptr);
  RaiseException(0xc00000fdu, 0u, 0u, nullptr);
  CHECK(g_lines.empty());
  CHECK(g_seen[3] == 3);
  CHECK(g_seen[4] == 1);
  for (const auto& l : g_lines)
    std::printf("unexpected line: %s", l.c_str());

  // A real divide by zero
  fatal::breadcrumb("test step: divide");
  SetLastError(1234u);
  divideByZero();
  CHECK(GetLastError() == 1234u);
  CHECK(g_seen[0] == 1);
  CHECK(g_lines.size() == 1u);
  CHECK(has(0, "err:   D9MT-FATAL: c0000094 integer divide by zero at 0x"));
  CHECK(has(0, "(fatal_log_test.exe+0x"));
  CHECK(has(0, ", thread 0x"));
  CHECK(!has(0, "\""));
  CHECK(has(0, "last d9mt step: test step: divide;"));
  CHECK(!g_lines.empty() && g_lines[0].back() == '\n');

  // A real access violation
  fatal::breadcrumb("test step: read");
  readAt0x10();
  CHECK(g_seen[1] == 1);
  CHECK(g_lines.size() == 2u);
  CHECK(has(1, "c0000005 access violation (read of 0x0000000000000010) at 0x"));
  CHECK(has(1, "last d9mt step: test step: read;"));

  // RaiseException: the address is in a module of the install snapshot
  RaiseException(0xc0000001u, 0u, 0u, nullptr);
  CHECK(g_seen[2] == 1);
  CHECK(g_lines.size() == 3u);
  CHECK(has(2, "c0000001 fatal exception at 0x"));
  CHECK(has(2, ".dll+0x"));
  CHECK(!has(2, "base 0x"));

  // A nested fault: the sink faults while the divide's line is being written
  fatal::setSinkForTest(faultingSink);
  g_faultInSink = true;
  fatal::breadcrumb("test step: nested");
  divideByZero();
  fatal::setSinkForTest(sink);
  CHECK(g_seen[0] == 2);
  CHECK(g_seen[1] == 2);             // the nested access violation reached the program's handler
  CHECK(g_lines.size() == 4u);       // and only the outer line went out
  CHECK(has(3, "c0000094 integer divide by zero at 0x"));
  CHECK(has(3, "last d9mt step: test step: nested;"));

  // A DLL loaded after install, then the same with its headers unreadable
  HMODULE dll = LoadLibraryA("fatal_log_dll.dll");
  CHECK(dll != nullptr);
  auto dllDivide = dll != nullptr ? dllProc<PFN_dllVoid>(dll, "fatal_dll_divide") : nullptr;
  CHECK(dllDivide != nullptr);
  char dllBase[40];
  std::snprintf(dllBase, sizeof(dllBase), "(base 0x%016llx+0x", (unsigned long long)uintptr_t(dll));
  if (dllDivide != nullptr) {
    dllDivide();
    CHECK(g_seen[0] == 3);
    CHECK(g_lines.size() == 5u);
    CHECK(has(4, dllBase));

    DWORD old = 0u;
    const BOOL hidden = VirtualProtect(dll, 4096u, PAGE_NOACCESS, &old);
    CHECK(hidden);
    dllDivide();
    if (hidden)
      VirtualProtect(dll, 4096u, old, &old);
    CHECK(g_seen[0] == 4);
    CHECK(g_lines.size() == 6u);
    CHECK(has(5, dllBase));
  }

  // Executable memory that isn't an image: xor edx,edx; mov eax,1; div ecx; ret (ecx = 0, the
  // first argument)
  static const uint8_t code[] = { 0x31, 0xd2, 0xb8, 0x01, 0x00, 0x00, 0x00, 0xf7, 0xf1, 0xc3 };
  void* mem = VirtualAlloc(nullptr, 4096u, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
  CHECK(mem != nullptr);
  if (mem != nullptr) {
    std::memcpy(mem, code, sizeof(code));
    FlushInstructionCache(GetCurrentProcess(), mem, sizeof(code));
    reinterpret_cast<void (*)(uint32_t)>(mem)(0u);
    VirtualFree(mem, 0u, MEM_RELEASE);
  }
  CHECK(g_seen[0] == 5);
  CHECK(g_lines.size() == 7u);
  CHECK(has(6, "(not in a PE module)"));

  // No allocation: every heap locked while a thread faults; the line goes to a fixed buffer
  fatal::setSinkForTest(fixedSink);
  fatal::breadcrumb("test step: heap lock");
  CHECK(divideUnderHeapLock());
  fatal::setSinkForTest(sink);
  CHECK(g_seen[0] == 6);
  CHECK(g_fixedCount == 1);
  CHECK(std::strstr(g_fixedLine, "c0000094 integer divide by zero at 0x") != nullptr);
  CHECK(std::strstr(g_fixedLine, "last d9mt step: test step: heap lock;") != nullptr);
  g_lines.push_back(g_fixedLine);

  // The cap: 8 lines are above, so one line says more were left out, then nothing however
  // many follow
  for (int i = 0; i < 100; i++)
    RaiseException(0xc0000001u, 0u, 0u, nullptr);
  CHECK(g_seen[2] == 101);
  CHECK(g_lines.size() == 9u);
  CHECK(has(8, "D9MT-FATAL: more fatal exceptions, not logged (the first 8 are above)"));

  for (size_t i = 0; i < g_lines.size(); i++)
    std::printf("line %zu: %s", i, g_lines[i].c_str());
  std::printf("fatal_log_test: %s (%d failures)\n", g_fail ? "FAIL" : "ok", g_fail);
  return g_fail ? 1 : 0;
}
