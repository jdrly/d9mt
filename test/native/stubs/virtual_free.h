#pragma once

// VirtualFree for native (macOS) unit tests built against DXVK's native <windows.h>, which
// doesn't declare it (test/native/residency_pso_test.cpp; force-included with -include).
// The test defines it.

#include <cstddef>

#define MEM_RELEASE 0x00008000u

extern "C" int VirtualFree(void* address, size_t size, unsigned type);
