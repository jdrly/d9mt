// PSO archive partition harness (scripts/test-pso-partition.sh): one bridge start, without Wine.
// It opens winemetal.so, takes the first Metal device and creates an MTL4 compiler, which sets up
// the PSO archives once (pso_archive_init): the directory under D9MT_PSO_ARCHIVE_DIR, named
// os-<build>-<gpu>-c<identity>-t<T>, and the removal of every other os-* directory. The bridge
// logs the directory ("d9mt: PSO archives: ... loaded from <dir>"); the script checks it.
//
//   pso_partition_test <winemetal.so>
#include <cstdio>
#include <dlfcn.h>

#include "../winemetal.h"
#include "../winemetal_thunks.h"

extern "C" __attribute__((visibility("default"))) int NtSetEvent(void*, void*) { return 0; }

typedef int (*unix_fn)(void*);

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <winemetal.so>\n", argv[0]);
    return 2;
  }
  void* bridge = dlopen(argv[1], RTLD_NOW);
  if (!bridge) { fprintf(stderr, "FAIL: dlopen: %s\n", dlerror()); return 1; }
  unix_fn* funcs = reinterpret_cast<unix_fn*>(dlsym(bridge, "__wine_unix_call_funcs"));
  if (!funcs) { fprintf(stderr, "FAIL: no call table\n"); return 1; }

  // as compile_mt_test: the device list (4), its first device (2), newMTL4Compiler (161)
  struct unixcall_generic_obj_ret devs = {};
  funcs[4](&devs);
  struct unixcall_generic_obj_uint64_obj_ret dev0 = {};
  dev0.handle = devs.ret;
  funcs[2](&dev0);
  if (!dev0.ret) { fprintf(stderr, "FAIL: no Metal device\n"); return 1; }
  struct unixcall_generic_obj_obj_ret mk = {};
  mk.handle = dev0.ret;
  funcs[161](&mk);
  if (!mk.ret) { fprintf(stderr, "FAIL: newMTL4Compiler\n"); return 1; }
  printf("ok\n");
  return 0;
}
