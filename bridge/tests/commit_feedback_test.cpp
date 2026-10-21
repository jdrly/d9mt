/* Bridge bundle part 7 (docs/bridge-bundle.md): MTL4 commits through winemetal.so's commit
 * thunk, so that Metal's commit feedback handler runs for real. Empty command buffers never
 * fail; D9MT_COMMIT_FEEDBACK_TEST=<code> makes every feedback read as that error, which tests
 * the handler's line and its limit without hanging or faulting the GPU.
 *   commit_feedback_test <winemetal.so> <commits> [late]
 * late: after the commits, 10.5 s later, one more (the limit's next window). Prints
 * "commits: N completed" once the last one's event is signalled; the D9MT-GPU-ERROR lines go
 * to stderr (scripts/test-commit-feedback.sh counts them). */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <unistd.h>

#include "../winemetal.h"
#include "../winemetal_thunks.h"

extern "C" __attribute__((visibility("default"))) int NtSetEvent(void*, void*) { return 0; }

typedef int (*unix_fn)(void*);

static unix_fn* funcs;
static obj_handle_t device, queue4, allocator, event;
static uint64_t signalled;

static obj_handle_t make(int call, obj_handle_t on, const char* what) {
  struct unixcall_generic_obj_obj_ret mk = {};
  mk.handle = on;
  funcs[call](&mk);
  if (!mk.ret) { fprintf(stderr, "FAIL: %s\n", what); exit(1); }
  return mk.ret;
}

/* n empty command buffers, one commit each, then an event signal after them, waited for */
static void commit(unsigned n) {
  for (unsigned i = 0; i < n; i++) {
    obj_handle_t cb = make(159, device, "newMTL4CommandBuffer");
    struct unixcall_generic_obj_obj_noret begin = {};
    begin.handle = cb; begin.arg = allocator; funcs[164](&begin);
    struct unixcall_generic_obj_noret end = {};
    end.handle = cb; funcs[165](&end);
    obj_handle_t cbs[1] = { cb };
    struct unixcall_mtl4_queue_commit c = {};
    c.queue = queue4; c.cmdbufs.set(cbs); c.count = 1;
    funcs[167](&c);
  }
  struct unixcall_generic_obj_obj_uint64_noret sig = {};
  sig.handle = queue4; sig.arg0 = event; sig.arg1 = ++signalled; funcs[170](&sig);
  struct unixcall_mtlsharedevent_waituntilsignaledvalue w = {};
  w.event = event; w.value = signalled; w.timeout_ms = 5000;
  funcs[130](&w);
  /* ret_timeout holds waitUntilSignaledValue's YES/NO return, despite the field name */
  if (!w.ret_timeout) { fprintf(stderr, "FAIL: the commits did not complete\n"); exit(1); }
  usleep(300000);   /* feedback comes after completion, on Metal's thread */
}

int main(int argc, char** argv) {
  if (argc < 3) { fprintf(stderr, "usage: commit_feedback_test <winemetal.so> <commits> [late]\n"); return 2; }
  void* bridge = dlopen(argv[1], RTLD_NOW);
  if (!bridge) { fprintf(stderr, "FAIL: dlopen bridge: %s\n", dlerror()); return 1; }
  funcs = reinterpret_cast<unix_fn*>(dlsym(bridge, "__wine_unix_call_funcs"));
  if (!funcs) { fprintf(stderr, "FAIL: no call table\n"); return 1; }

  struct unixcall_generic_obj_ret devs = {};
  funcs[4](&devs);
  struct unixcall_generic_obj_uint64_obj_ret dev0 = {};
  dev0.handle = devs.ret; dev0.arg = 0;
  funcs[2](&dev0);
  if (!dev0.ret) { fprintf(stderr, "FAIL: no Metal device\n"); return 1; }
  device = dev0.ret;
  queue4 = make(157, device, "newMTL4CommandQueue");
  allocator = make(158, device, "newCommandAllocator");
  event = make(15, device, "newSharedEvent");

  const unsigned n = unsigned(atoi(argv[2]));
  commit(n);
  if (argc > 3 && !strcmp(argv[3], "late")) {
    sleep(10);
    usleep(500000);
    commit(1);
  }
  printf("commits: %u completed\n", n + (argc > 3 ? 1u : 0u));
  return 0;
}
