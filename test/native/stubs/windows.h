#pragma once

// Minimal <windows.h> for native (macOS) unit tests of driver code that only
// needs VirtualAlloc. Memory is released by the test's stubs with free().

#include <cstddef>
#include <cstdlib>

typedef size_t SIZE_T;

#define MEM_COMMIT     0x00001000u
#define MEM_RESERVE    0x00002000u
#define PAGE_READWRITE 0x04u

inline void* VirtualAlloc(void*, SIZE_T size, unsigned, unsigned) {
  return std::malloc(size);
}
