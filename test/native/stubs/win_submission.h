#pragma once

// The Win32 calls driver/backend/mtl_submission.cpp makes, for native (macOS) unit tests built
// against DXVK's native <windows.h>, which doesn't declare them
// (test/native/submission_abandon_test.cpp; force-included with -include). The test defines them.

#include "virtual_free.h"

#ifndef STD_ERROR_HANDLE
#define STD_ERROR_HANDLE ((unsigned)-12)
#endif
#ifndef INFINITE
#define INFINITE 0xFFFFFFFFu
#endif

extern "C" void* GetStdHandle(unsigned handle);
extern "C" int   WriteFile(void* file, const void* data, unsigned size, unsigned* written, void* overlapped);
extern "C" int   FlushFileBuffers(void* file);
extern "C" int   TerminateProcess(void* process, unsigned exitCode);
extern "C" void* GetCurrentProcess();
extern "C" void  Sleep(unsigned ms);
