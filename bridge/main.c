/*
 * Derived from DXMT src/winemetal/main.c (https://github.com/3Shain/dxmt)
 * Copyright (c) 2023-2026 Feifan He for CodeWeavers
 * SPDX-License-Identifier: LGPL-2.1-or-later (see bridge/LICENSE, bridge/COPYING.LIB)
 *
 * Changed for d9mt by:
 *   neo773 (d9mt-v2), 2026-07 to 2026-08
 *   Jan Drlý and contributors (d9mt), 2026-10
 * The changes are listed in bridge/UPSTREAM; the git history has each one with its date.
 */

#include "windef.h"
#include "winbase.h"
#include "wineunixlib.h"
#include <stdint.h>

#if (defined(__x86_64__) || defined(_M_X64)) && !defined(__aarch64__) &&       \
    !defined(__arm64ec__) && !defined(_M_ARM64) && !defined(_M_ARM64EC)
#define DXMT_FH4_BYPASS_HAS_X64_GS 1
#endif

static uintptr_t read_gs_qword(unsigned int offset) {
#if DXMT_FH4_BYPASS_HAS_X64_GS
  uintptr_t value = 0;
#if defined(__GNUC__) || defined(__clang__)
  if (offset == 0x20)
    __asm__ volatile("movq %%gs:0x20,%0" : "=r"(value));
#endif
  return value;
#else
  (void)offset;
  return 0;
#endif
}

static void write_gs_qword(unsigned int offset, uintptr_t value) {
#if DXMT_FH4_BYPASS_HAS_X64_GS
#if defined(__GNUC__) || defined(__clang__)
  if (offset == 0x20)
    __asm__ volatile("movq %0,%%gs:0x20" : : "r"(value) : "memory");
#else
  (void)offset;
  (void)value;
#endif
#else
  (void)offset;
  (void)value;
#endif
}

static void apply_fh4_bad_fiber_data_bypass(void) {
  WCHAR path[MAX_PATH + 1] = {0};
  DWORD len = GetModuleFileNameW(NULL, path, MAX_PATH);
  const WCHAR *base = path;
  uintptr_t fiber_data;

  if (!len)
    return;

  for (DWORD i = 0; i < len; i++) {
    if (path[i] == L'\\' || path[i] == L'/')
      base = path + i + 1;
  }

  if (lstrcmpiW(base, L"ForzaHorizon4.exe") != 0)
    return;

  /* this game can inherit a bogus low-address FiberData value under Wine and crash before the D3D runtime is initialized. */
  fiber_data = read_gs_qword(0x20);
  if (fiber_data && fiber_data < 0x10000)
    write_gs_qword(0x20, 0);
}


BOOL WINAPI
DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  if (reason != DLL_PROCESS_ATTACH)
    return TRUE;

  apply_fh4_bad_fiber_data_bypass();
  DisableThreadLibraryCalls(instance);
  return !__wine_init_unix_call();
}

extern BOOL WINAPI DllMainCRTStartup(HANDLE hDllHandle, DWORD dwReason, LPVOID lpreserved);
