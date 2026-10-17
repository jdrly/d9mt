#include <windows.h>
typedef LONG NTSTATUS;

typedef UINT64 unixlib_handle_t;

#define MemoryWineUnixFuncs 1000

extern NTSTATUS WINAPI __wine_unix_call(unixlib_handle_t handle,
                                        unsigned int code, void *args);
extern NTSTATUS WINAPI NtQueryVirtualMemory(HANDLE process, LPCVOID addr,
                                            int info_class, PVOID info,
                                            SIZE_T size, SIZE_T *ret_size);

extern IMAGE_DOS_HEADER __ImageBase;

unixlib_handle_t __wine_unixlib_handle = 0;

#ifndef __arm64ec__

static NTSTATUS WINAPI
compat_dispatcher(unixlib_handle_t handle, unsigned int code, void *args) {
  return __wine_unix_call(handle, code, args);
}

NTSTATUS (WINAPI *__wine_unix_call_dispatcher)(unixlib_handle_t,
                                               unsigned int, void *) =
    compat_dispatcher;

#else

/* ARM64EC (arm64 Wine). ntdll exports no __wine_unix_call function there, only
 * dispatcher pointers. As Wine's winecrt0 does since Wine 9.10
 * (dlls/winecrt0/unix_lib.c, libs/winecrt0/unix_lib.c in master): read ntdll's
 * __wine_unix_call_dispatcher, which in an ARM64EC process holds ntdll's own
 * ARM64EC stub (the unix loader redirects it, dlls/ntdll/unix/loader.c), and
 * branch to it. A plain C call through the pointer would go through the
 * ARM64EC indirect-call check. The pointer starts at an init-and-forward
 * function, as upstream's; __wine_init_unix_call (DllMain) resolves it before
 * the first unix call and fails the DLL load if ntdll lacks the export. */
typedef struct _UNICODE_STRING {
  USHORT Length;
  USHORT MaximumLength;
  PWSTR Buffer;
} UNICODE_STRING;
extern NTSTATUS WINAPI LdrGetDllHandle(LPCWSTR load_path, ULONG flags,
                                       const UNICODE_STRING *name,
                                       HMODULE *base);
extern void *WINAPI RtlFindExportedRoutineByName(HMODULE module,
                                                 const char *name);

static NTSTATUS WINAPI
unix_call_init_arm64ec(unixlib_handle_t handle, unsigned int code, void *args);

static __attribute__((used)) NTSTATUS (WINAPI *__wine_unix_call_dispatcher_arm64ec)(
    unixlib_handle_t, unsigned int, void *) = unix_call_init_arm64ec;

NTSTATUS __attribute__((naked))
__wine_unix_call_arm64ec(unixlib_handle_t handle, unsigned int code, void *args) {
  __asm__(".seh_proc \"#__wine_unix_call_arm64ec\"\n\t"
          ".seh_endprologue\n\t"
          "adrp x16, __wine_unix_call_dispatcher_arm64ec\n\t"
          "ldr x16, [x16, #:lo12:__wine_unix_call_dispatcher_arm64ec]\n\t"
          "br x16\n\t"
          ".seh_endproc");
}

static NTSTATUS WINAPI
unix_call_fallback(unixlib_handle_t handle, unsigned int code, void *args) {
  return 0xc0000135; /* STATUS_DLL_NOT_FOUND */
}

static void *
find_dispatcher_arm64ec(void) {
  static WCHAR ntdll_name[] = L"ntdll.dll";
  UNICODE_STRING name = {sizeof(ntdll_name) - sizeof(WCHAR), sizeof(ntdll_name),
                         ntdll_name};
  HMODULE module = NULL;
  void **dispatcher;

  if (LdrGetDllHandle(NULL, 0, &name, &module) || !module)
    return NULL;
  dispatcher = RtlFindExportedRoutineByName(module, "__wine_unix_call_dispatcher");
  return dispatcher ? *dispatcher : NULL;
}

static NTSTATUS WINAPI
unix_call_init_arm64ec(unixlib_handle_t handle, unsigned int code, void *args) {
  void *dispatcher = find_dispatcher_arm64ec();
  InterlockedExchangePointer((void **)&__wine_unix_call_dispatcher_arm64ec,
                             dispatcher ? dispatcher : (void *)unix_call_fallback);
  return __wine_unix_call_arm64ec(handle, code, args);
}

static NTSTATUS
init_dispatcher_arm64ec(void) {
  void *dispatcher = find_dispatcher_arm64ec();
  if (!dispatcher)
    return 0xc0000139; /* STATUS_ENTRYPOINT_NOT_FOUND */
  InterlockedExchangePointer((void **)&__wine_unix_call_dispatcher_arm64ec, dispatcher);
  return 0;
}

#endif

NTSTATUS WINAPI
__wine_init_unix_call(void) {
  SIZE_T ret_size = 0;
#ifdef __arm64ec__
  NTSTATUS status = init_dispatcher_arm64ec();
  if (status)
    return status;
#endif
  return NtQueryVirtualMemory(GetCurrentProcess(), &__ImageBase,
                              MemoryWineUnixFuncs, &__wine_unixlib_handle,
                              sizeof(__wine_unixlib_handle), &ret_size);
}
