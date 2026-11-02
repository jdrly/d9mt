// Unit test for the fatal-exception log (driver/util/util_fatal_log.cpp). A Windows program (x64):
// scripts/test-fatal-log.sh builds it with mingw and runs it under Wine. Modes:
//
//   (none)          lines go to a capture sink:
//     - a real integer divide by zero (`div` by a zero register) is one D9MT-FATAL line with the
//       code, its name, the address as fatal_log_test.exe+0x<rva>, the thread's name and the
//       last breadcrumb; the thread's last error is kept;
//     - a real access violation also says what was read and where;
//     - a thread without a name is "(unnamed)";
//     - C++ throws (mingw's own, and MSVC's code 0xE06D7363), thread naming (0x406D1388),
//       OutputDebugString and a breakpoint-class code are no line;
//     - the handler never resolves an exception: the test's own vectored handler, registered
//       after it, still sees every one;
//     - after 8 lines, one line says more were left out, then nothing.
//   --wine-output   no sink: one divide by zero, resolved by the test; the script finds the line
//                   in Wine's debug output (the path a game's log takes).
//   --unhandled     as a game that catches its own crash: a top-level filter that returns
//                   EXCEPTION_EXECUTE_HANDLER, then a divide by zero nobody resolves. Wine ends
//                   the process with the exception code, so the Unix exit status is 148 (0x94);
//                   the script checks it and the line.
//   --expect-off    D9MT_FATAL_LOG=0 (set by the script): install() returns false.
//
// Exit 0 if every check passed (except --unhandled, which never returns).
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

static LONG g_seen[4] = { };   // c0000094, c0000005, c0000001, everything else

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
  return EXCEPTION_CONTINUE_SEARCH;
}

static DWORD WINAPI unnamedThread(void*) {
  divideByZero();
  return 0u;
}

static void setName(const wchar_t* name) {
  using Proc = HRESULT (WINAPI *)(HANDLE, PCWSTR);
  auto proc = reinterpret_cast<Proc>(reinterpret_cast<void*>(
    GetProcAddress(GetModuleHandleA("kernelbase.dll"), "SetThreadDescription")));
  if (proc != nullptr)
    proc(GetCurrentThread(), name);
}

static LONG WINAPI gameLikeFilter(EXCEPTION_POINTERS*) {
  return EXCEPTION_EXECUTE_HANDLER;
}

int main(int argc, char** argv) {
  const std::string mode = argc > 1 ? argv[1] : "";

  if (mode == "--expect-off") {
    CHECK(!fatal::install());
    std::printf("fatal_log_test --expect-off: %s\n", g_fail ? "FAIL" : "ok");
    return g_fail ? 1 : 0;
  }

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

  AddVectoredExceptionHandler(0u, testHandler);

  if (mode == "--wine-output") {
    fatal::breadcrumb("test step: wine output");
    divideByZero();
    std::printf("fatal_log_test --wine-output: %s\n", g_seen[0] == 1 ? "ok" : "FAIL");
    return g_seen[0] == 1 ? 0 : 1;
  }

  fatal::setSinkForTest(sink);

  // Not logged: C++ throws, thread naming, debug output, breakpoint-class codes
  try {
    throw std::runtime_error("a C++ exception");
  } catch (const std::exception&) { }
  RaiseException(0xe06d7363u, 0u, 0u, nullptr);
  struct { DWORD type; LPCSTR name; DWORD threadId; DWORD flags; } nameInfo = { 0x1000u, "named", DWORD(-1), 0u };
  RaiseException(0x406d1388u, 0u, sizeof(nameInfo) / sizeof(ULONG_PTR), reinterpret_cast<const ULONG_PTR*>(&nameInfo));
  OutputDebugStringA("fatal_log_test: debug output\n");
  RaiseException(0x80000003u, 0u, 0u, nullptr);
  CHECK(g_lines.empty());
  CHECK(g_seen[3] == 3);
  for (const auto& l : g_lines)
    std::printf("unexpected line: %s", l.c_str());

  // A real divide by zero on a named thread
  setName(L"fatal-test-main");
  fatal::breadcrumb("test step: divide");
  SetLastError(1234u);
  divideByZero();
  CHECK(GetLastError() == 1234u);
  CHECK(g_seen[0] == 1);
  CHECK(g_lines.size() == 1u);
  CHECK(has(0, "err:   D9MT-FATAL: c0000094 integer divide by zero at 0x"));
  CHECK(has(0, "(fatal_log_test.exe+0x"));
  CHECK(has(0, "\"fatal-test-main\""));
  CHECK(has(0, "last d9mt step: test step: divide;"));
  CHECK(!g_lines.empty() && g_lines[0].back() == '\n');

  // A real access violation
  fatal::breadcrumb("test step: read");
  readAt0x10();
  CHECK(g_seen[1] == 1);
  CHECK(g_lines.size() == 2u);
  CHECK(has(1, "c0000005 access violation (read of 0x0000000000000010) at 0x"));
  CHECK(has(1, "last d9mt step: test step: read;"));

  // A thread without a name
  HANDLE t = CreateThread(nullptr, 0u, unnamedThread, nullptr, 0u, nullptr);
  CHECK(t != nullptr);
  if (t != nullptr) {
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
  }
  CHECK(g_seen[0] == 2);
  CHECK(g_lines.size() == 3u);
  CHECK(has(2, "(unnamed)"));

  // The cap: 8 lines, then one that says more were left out, then nothing
  for (int i = 0; i < 7; i++)
    RaiseException(0xc0000001u, 0u, 0u, nullptr);
  CHECK(g_seen[2] == 7);
  CHECK(g_lines.size() == 9u);
  CHECK(has(7, "c0000001 fatal exception at 0x"));
  CHECK(has(8, "D9MT-FATAL: more fatal exceptions, not logged (the first 8 are above)"));

  for (size_t i = 0; i < g_lines.size(); i++)
    std::printf("line %zu: %s", i, g_lines[i].c_str());
  std::printf("fatal_log_test: %s (%d failures)\n", g_fail ? "FAIL" : "ok", g_fail);
  return g_fail ? 1 : 0;
}
