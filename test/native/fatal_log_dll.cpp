// A DLL for fatal_log_test (scripts/test-fatal-log.sh builds it): the fatal-exception log linked
// into a DLL the test loads and unloads, as d3d9.dll is, and a fault inside an image the test's
// own logger didn't know at install.
#include "../../driver/util/util_fatal_log.h"

#include <windows.h>

extern "C" __declspec(dllexport) int fatal_dll_install() {
  return dxvk::fatal::install() ? 1 : 0;
}

extern "C" __declspec(dllexport) void fatal_dll_set_sink(void (*sink)(const char* line)) {
  dxvk::fatal::setSinkForTest(sink);
}

extern "C" __declspec(dllexport) void fatal_dll_divide() {
  // div %ecx (F7 F1) with ecx = 0, as the test's own divideByZero()
  asm volatile("xor %%edx, %%edx\n\tmov $1, %%eax\n\tdiv %%ecx" : : "c"(0) : "eax", "edx", "memory");
}

// A breadcrumb in the DLL's own copy of the log, a literal in the DLL's image: after FreeLibrary
// the handler still reads it, so the image must still be mapped (pinned at install)
extern "C" __declspec(dllexport) void fatal_dll_breadcrumb() {
  dxvk::fatal::breadcrumb("test step: in the unloaded dll");
}
