// Threads waiting for the CS thread at once (a game's streaming threads on a device without
// D3DCREATE_MULTITHREADED, where the device lock is a no-op). Two cases, 20 rounds each:
//  - same:  three threads in DxvkCsThread::synchronize for the same sequence number. The CS
//           thread holds the chunk until all of them sleep, then completes it.
//  - mixed: upstream DXVK 7e03c549's case. One thread in injectChunk(HighPriority, ..., true)
//           and one in synchronize() for an ordered chunk: the same condition variable, two
//           different predicates. The CS thread holds the ordered chunk until both sleep, then
//           completes it and runs the injected one.
// With notify_one a completed chunk woke only one waiter: in `same` the others slept until some
// later chunk completed, forever if none came; in `mixed` the wake-up could go to the waiter
// whose predicate was still false, and the other one slept on. Every waiter must return.
// Built against libdriver_core.a (scripts/test-cs-sync.sh); runs without a window or a GPU.
#include <windows.h>
#include <cstdio>

#include "cs/dxvk_cs.h"

using namespace dxvk;

namespace {

  constexpr int Waiters = 3;
  constexpr int Rounds  = 20;

  struct Waiter {
    DxvkCsThread*      cs;
    DxvkCsChunkPool*   pool;
    uint64_t           seq;     // 0: inject a high-priority chunk and wait for it
    volatile LONG*     done;
  };

  DxvkCsChunkRef chunk(DxvkCsChunkPool& pool, HANDLE gate) {
    DxvkCsChunkRef c(pool.allocChunk(DxvkCsChunkFlag::SingleUse), &pool);
    c->push([gate] (MtlContext*) {
      if (gate)
        WaitForSingleObject(gate, INFINITE);
    });
    return c;
  }

  DWORD WINAPI waitThread(void* arg) {
    auto* w = static_cast<Waiter*>(arg);
    if (w->seq)
      w->cs->synchronize(w->seq);
    else
      w->cs->injectChunk(DxvkCsQueue::HighPriority, chunk(*w->pool, nullptr), true);
    InterlockedIncrement(w->done);
    return 0;
  }

  // Runs one round: starts the waiters (in order, each given time to fall asleep), opens the
  // gate and counts the waiters that return within 2 s. Then releases any still asleep.
  bool round(DxvkCsThread& cs, DxvkCsChunkPool& pool, const char* name, int r, int n, bool mixed) {
    HANDLE gate = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    const uint64_t seq = cs.dispatchChunk(chunk(pool, gate));
    Sleep(20);  // the CS thread is inside the gated chunk

    volatile LONG done = 0;
    Waiter w[Waiters];
    HANDLE threads[Waiters];
    for (int i = 0; i < n; i++) {
      w[i] = { &cs, &pool, (mixed && i == 0) ? 0u : seq, &done };
      threads[i] = CreateThread(nullptr, 0, waitThread, &w[i], 0, nullptr);
      Sleep(50);
    }

    // Every waiter is asleep before the chunk completes.
    Sleep(100);
    SetEvent(gate);

    const DWORD t0 = GetTickCount();
    while (done < n && GetTickCount() - t0 < 2000)
      Sleep(5);
    const LONG woke = done;
    if (woke != n)
      std::printf("FAIL %s round %d: %ld of %d waiters returned within 2 s of their chunk\n", name, r, woke, n);

    // Release any waiter still asleep (each further sequenced chunk wakes at least one).
    while (done < n) {
      cs.dispatchChunk(chunk(pool, nullptr));
      Sleep(5);
    }
    WaitForMultipleObjects(n, threads, TRUE, INFINITE);
    for (int i = 0; i < n; i++)
      CloseHandle(threads[i]);
    CloseHandle(gate);
    return woke == n;
  }

}

int main() {
  DxvkCsChunkPool pool;
  DxvkCsThread cs { Rc<MtlDevice>(), Rc<MtlContext>() };
  int failedSame = 0, failedMixed = 0;

  for (int r = 0; r < Rounds; r++)
    failedSame += round(cs, pool, "same", r, Waiters, false) ? 0 : 1;
  for (int r = 0; r < Rounds; r++)
    failedMixed += round(cs, pool, "mixed", r, 2, true) ? 0 : 1;

  std::printf("unit_cs_sync: same %d rounds x %d waiters, %d failed; mixed %d rounds x 2 waiters, %d failed: %s\n",
    Rounds, Waiters, failedSame, Rounds, failedMixed, (failedSame || failedMixed) ? "FAIL" : "PASS");
  std::fflush(stdout);
  return (failedSame || failedMixed) ? 1 : 0;
}
