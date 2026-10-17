/* exc_guard: run a command with a timeout, and log its crashes instead of
 * leaving crash reports (and, for Wine's GUI processes, "quit unexpectedly"
 * dialogs on the desktop).
 *
 *   exc_guard <timeout s> <log> <command> [args...]
 *
 * The command is spawned with a task exception port for EXC_CORPSE_NOTIFY,
 * which its children (wineserver, the Windows processes) inherit. The kernel
 * offers a crashing process's corpse to the task's port before the host's,
 * where ReportCrash listens; exc_guard takes it, so no report is made. It
 * logs to <log> every thread's backtrace (image+offset, frame pointers) and,
 * for libplatform's `brk #0xb001` crashes (os_unfair_lock corruption), the
 * lock value and whether its owner thread still exists. Only crashes that
 * end the process are affected: exceptions and signals a process handles
 * itself (Wine's SIGSEGV/SIGTRAP handlers) never reach this port. Nothing
 * system-wide is changed.
 *
 * Exit status: the command's, 128+signal if it was killed, 124 on timeout,
 * 125 if a crash was caught in the command or one of its children.
 *
 * Build: mig mach_exc.defs (server side) and clang exc_guard.c mach_excServer.c
 * (scripts/run-hash-tests.sh does this). */
#include <errno.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach-o/loader.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "mach_excServer.h"

extern char **environ;
extern int pid_for_task(mach_port_t task, int *pid);

/* libdyld exports these for tools; the header is private. */
typedef struct dyld_process_info_base *dyld_process_info;
extern dyld_process_info _dyld_process_info_create(task_t task, uint64_t timestamp, kern_return_t *err);
extern void _dyld_process_info_for_each_image(dyld_process_info info,
    void (^cb)(uint64_t mh, const uuid_t uuid, const char *path));
extern void _dyld_process_info_release(dyld_process_info info);

#define BRK_B001 0xd4360020u  /* brk #0xb001, libplatform's crash trap */
#define MAX_IMAGES 2048

static FILE *g_log;
static volatile int g_caught, g_busy;
static struct { uint64_t base, size; char path[256]; } g_img[MAX_IMAGES];
static int g_nimg;

static int rread(task_t task, uint64_t addr, void *buf, size_t n) {
  mach_vm_size_t got = 0;
  return mach_vm_read_overwrite(task, addr, n, (mach_vm_address_t)buf, &got) == KERN_SUCCESS && got == n;
}

static void load_images(task_t task) {
  kern_return_t err = 0;
  dyld_process_info info = _dyld_process_info_create(task, 0, &err);
  g_nimg = 0;
  if (!info) return;
  _dyld_process_info_for_each_image(info, ^(uint64_t mh, const uuid_t uuid, const char *path) {
    struct mach_header_64 h;
    uint8_t cmds[16384];
    uint64_t size = 0;
    (void)uuid;
    if (g_nimg >= MAX_IMAGES || !rread(task, mh, &h, sizeof(h))) return;
    if (h.sizeofcmds <= sizeof(cmds) && rread(task, mh + sizeof(h), cmds, h.sizeofcmds)) {
      uint32_t off = 0;
      for (uint32_t i = 0; i < h.ncmds && off + sizeof(struct load_command) <= h.sizeofcmds; i++) {
        struct segment_command_64 *s = (struct segment_command_64 *)(cmds + off);
        if (s->cmd == LC_SEGMENT_64 && !strcmp(s->segname, "__TEXT")) size = s->vmsize;
        off += s->cmdsize ? s->cmdsize : h.sizeofcmds;
      }
    }
    g_img[g_nimg].base = mh;
    g_img[g_nimg].size = size;
    snprintf(g_img[g_nimg].path, sizeof(g_img[g_nimg].path), "%s", path ? path : "?");
    g_nimg++;
  });
  _dyld_process_info_release(info);
}

static void print_addr(uint64_t a) {
  for (int i = 0; i < g_nimg; i++) {
    if (a >= g_img[i].base && a < g_img[i].base + g_img[i].size) {
      const char *slash = strrchr(g_img[i].path, '/');
      fprintf(g_log, " %s+0x%llx", slash ? slash + 1 : g_img[i].path, (unsigned long long)(a - g_img[i].base));
      return;
    }
  }
  fprintf(g_log, " 0x%llx", (unsigned long long)a);
}

static void dump_thread(task_t task, thread_act_t th, int faulting) {
  arm_thread_state64_t st;
  mach_msg_type_number_t cnt = ARM_THREAD_STATE64_COUNT;
  thread_extended_info_data_t ext;
  mach_msg_type_number_t ecnt = THREAD_EXTENDED_INFO_COUNT;
  thread_identifier_info_data_t id;
  mach_msg_type_number_t icnt = THREAD_IDENTIFIER_INFO_COUNT;
  char name[64] = "";

  if (thread_info(th, THREAD_EXTENDED_INFO, (thread_info_t)&ext, &ecnt) == KERN_SUCCESS)
    snprintf(name, sizeof(name), "%s", ext.pth_name);
  if (thread_info(th, THREAD_IDENTIFIER_INFO, (thread_info_t)&id, &icnt) != KERN_SUCCESS)
    id.thread_id = 0;
  fprintf(g_log, "  thread 0x%llx%s \"%s\"", (unsigned long long)id.thread_id, faulting ? " (trapped)" : "", name);
  if (thread_get_state(th, ARM_THREAD_STATE64, (thread_state_t)&st, &cnt) != KERN_SUCCESS) {
    fprintf(g_log, " (no state)\n");
    return;
  }
  uint64_t pc = arm_thread_state64_get_pc(st), lr = arm_thread_state64_get_lr(st);
  uint64_t fp = arm_thread_state64_get_fp(st);
  fprintf(g_log, "\n    pc");
  print_addr(pc);
  fprintf(g_log, "\n    lr");
  print_addr(lr);
  for (int i = 0; i < 32 && fp && !(fp & 7); i++) {
    uint64_t fr[2];
    if (!rread(task, fp, fr, sizeof(fr)) || !fr[1]) break;
    fprintf(g_log, "\n    #%d", i);
    print_addr(fr[1] & 0x0000ffffffffffffull);
    if (fr[0] <= fp) break;
    fp = fr[0];
  }
  fprintf(g_log, "\n");
}

kern_return_t catch_mach_exception_raise(mach_port_t port, mach_port_t thread, mach_port_t task,
    exception_type_t exc, mach_exception_data_t code, mach_msg_type_number_t ncode) {
  thread_act_array_t threads;
  mach_msg_type_number_t n = 0;
  int pid = -1;
  time_t now = time(NULL);
  char when[32];
  (void)port;
  (void)thread;

  g_busy = 1;
  pid_for_task(task, &pid);
  strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", localtime(&now));
  fprintf(g_log, "EXC_GUARD %s pid %d: exception %d, code 0x%llx 0x%llx\n", when, pid, exc,
          ncode > 0 ? (unsigned long long)code[0] : 0ull, ncode > 1 ? (unsigned long long)code[1] : 0ull);
  load_images(task);
  if (task_threads(task, &threads, &n) == KERN_SUCCESS) {
    for (mach_msg_type_number_t i = 0; i < n; i++) {
      arm_thread_state64_t st;
      mach_msg_type_number_t cnt = ARM_THREAD_STATE64_COUNT;
      uint32_t insn = 0;
      int trapped = thread_get_state(threads[i], ARM_THREAD_STATE64, (thread_state_t)&st, &cnt) == KERN_SUCCESS
                 && rread(task, arm_thread_state64_get_pc(st), &insn, sizeof(insn)) && insn == BRK_B001;
      if (trapped) {
        uint32_t value = (uint32_t)st.__x[0];
        mach_port_type_t type = 0;
        kern_return_t kr = mach_port_type(task, value | 1u, &type);
        fprintf(g_log, "  brk #0xb001: lock value 0x%x, owner port 0x%x: %s\n", value, value | 1u,
                kr != KERN_SUCCESS ? "no such name (owner thread gone)"
                : (type & MACH_PORT_TYPE_DEAD_NAME) ? "dead name (owner thread gone)"
                : (type & MACH_PORT_TYPE_SEND) ? "send right (owner thread alive)" : "other");
      }
      dump_thread(task, threads[i], trapped);
      mach_port_deallocate(mach_task_self(), threads[i]);
    }
    vm_deallocate(mach_task_self(), (vm_address_t)threads, n * sizeof(*threads));
  }
  fflush(g_log);
  g_caught = 1;
  g_busy = 0;
  return KERN_SUCCESS;  /* handled: the host port (ReportCrash) isn't asked */
}

kern_return_t catch_mach_exception_raise_state(mach_port_t p, exception_type_t e, const mach_exception_data_t c,
    mach_msg_type_number_t nc, int *f, const thread_state_t is, mach_msg_type_number_t isc,
    thread_state_t os, mach_msg_type_number_t *osc) {
  (void)p; (void)e; (void)c; (void)nc; (void)f; (void)is; (void)isc; (void)os; (void)osc;
  return KERN_FAILURE;
}

kern_return_t catch_mach_exception_raise_state_identity(mach_port_t p, mach_port_t t, mach_port_t k,
    exception_type_t e, mach_exception_data_t c, mach_msg_type_number_t nc, int *f, thread_state_t is,
    mach_msg_type_number_t isc, thread_state_t os, mach_msg_type_number_t *osc) {
  (void)p; (void)t; (void)k; (void)e; (void)c; (void)nc; (void)f; (void)is; (void)isc; (void)os; (void)osc;
  return KERN_FAILURE;
}

static void *server(void *arg) {
  mach_msg_server(mach_exc_server, 8192, (mach_port_t)(uintptr_t)arg, 0);
  return NULL;
}

int main(int argc, char **argv) {
  mach_port_t port;
  posix_spawnattr_t attr;
  pthread_t thr;
  pid_t pid;
  int status = 0;

  if (argc < 4) {
    fprintf(stderr, "usage: exc_guard <timeout s> <log> <command> [args...]\n");
    return 2;
  }
  unsigned timeout = (unsigned)atoi(argv[1]);
  if (!(g_log = fopen(argv[2], "a"))) { perror(argv[2]); return 2; }
  if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port)
   || mach_port_insert_right(mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND)) {
    fprintf(stderr, "exc_guard: no port\n");
    return 2;
  }
  pthread_create(&thr, NULL, server, (void *)(uintptr_t)port);
  posix_spawnattr_init(&attr);
  posix_spawnattr_setexceptionports_np(&attr, EXC_MASK_CORPSE_NOTIFY, port,
                                       EXCEPTION_DEFAULT | MACH_EXCEPTION_CODES, THREAD_STATE_NONE);
  if ((errno = posix_spawnp(&pid, argv[3], NULL, &attr, argv + 3, environ))) {
    perror(argv[3]);
    return 127;
  }
  for (unsigned waited = 0;; waited++) {
    pid_t r = waitpid(pid, &status, WNOHANG);
    if (r == pid) break;
    if (r < 0) { perror("waitpid"); return 2; }
    if (timeout && waited >= timeout * 10u) {
      kill(pid, SIGKILL);
      waitpid(pid, &status, 0);
      return 124;
    }
    usleep(100000);
  }
  /* The corpse is offered while the process exits: let the handler finish. */
  usleep(300000);
  while (g_busy) usleep(10000);
  if (g_caught) return 125;
  if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
  return WEXITSTATUS(status);
}
