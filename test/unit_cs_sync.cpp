// Two threads waiting for the CS thread at once: DxvkCsThread::synchronize from two threads
// for the same sequence number (a game's streaming threads on a device without
// D3DCREATE_MULTITHREADED, where the device lock is a no-op). The CS thread holds the
// chunk until both waiters sleep, then completes it. With notify_one only one of them
// woke and the other slept until some later chunk completed, forever if none came. Every
// waiter must return. Built against libdriver_core.a (scripts/test-cs-sync.sh); runs
// without a window or a GPU.
#include <windows.h>
#include <cstdio>

#include "cs/dxvk_cs.h"

using namespace dxvk;

namespace {

  constexpr int Waiters = 3;
  constexpr int Rounds  = 20;

  struct Waiter {
    DxvkCsThread*      cs;
    uint64_t           seq;
    volatile LONG*     done;
  };

  DWORD WINAPI waitThread(void* arg) {
    auto* w = static_cast<Waiter*>(arg);
    w->cs->synchronize(w->seq);
    InterlockedIncrement(w->done);
    return 0;
  }

  DxvkCsChunkRef chunk(DxvkCsChunkPool& pool, HANDLE gate) {
    DxvkCsChunkRef c(pool.allocChunk(DxvkCsChunkFlag::SingleUse), &pool);
    c->push([gate] (MtlContext*) {
      if (gate)
        WaitForSingleObject(gate, INFINITE);
    });
    return c;
  }

}

int main() {
  DxvkCsChunkPool pool;
  DxvkCsThread cs { Rc<MtlDevice>(), Rc<MtlContext>() };
  int failed = 0;

  for (int r = 0; r < Rounds; r++) {
    HANDLE gate = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    const uint64_t seq = cs.dispatchChunk(chunk(pool, gate));

    volatile LONG done = 0;
    Waiter w[Waiters];
    HANDLE threads[Waiters];
    for (int i = 0; i < Waiters; i++) {
      w[i] = { &cs, seq, &done };
      threads[i] = CreateThread(nullptr, 0, waitThread, &w[i], 0, nullptr);
    }

    // Every waiter is inside synchronize() and asleep before the chunk completes.
    Sleep(100);
    SetEvent(gate);

    const DWORD t0 = GetTickCount();
    while (done < Waiters && GetTickCount() - t0 < 2000)
      Sleep(5);
    const LONG woke = done;
    if (woke != Waiters) {
      failed++;
      std::printf("FAIL round %d: %ld of %d waiters returned within 2 s of their chunk\n", r, woke, Waiters);
    }

    // Release any waiter still asleep (each further sequenced chunk wakes at least one).
    while (done < Waiters) {
      cs.dispatchChunk(chunk(pool, nullptr));
      Sleep(5);
    }
    WaitForMultipleObjects(Waiters, threads, TRUE, INFINITE);
    for (HANDLE t : threads)
      CloseHandle(t);
    CloseHandle(gate);
  }

  std::printf("unit_cs_sync: %d rounds x %d waiters, %d rounds failed: %s\n",
    Rounds, Waiters, failed, failed ? "FAIL" : "PASS");
  std::fflush(stdout);
  return failed ? 1 : 0;
}
