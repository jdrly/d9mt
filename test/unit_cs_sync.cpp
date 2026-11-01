// Threads waiting for the CS thread at once (a game's streaming threads on a device without
// D3DCREATE_MULTITHREADED, where the device lock is a no-op). Two cases, 20 rounds each:
//  - same:  three threads in DxvkCsThread::synchronize for the same sequence number;
//  - mixed: upstream DXVK 7e03c549's case. One thread in injectChunk(HighPriority, ..., true)
//           and one in synchronize() for an ordered chunk: the same condition variable, two
//           different predicates.
// Each round the CS thread runs a gated chunk: the test waits until the CS thread is inside it,
// starts the waiters, waits until each has reached its call, gives them 100 ms to fall asleep in
// it, then opens the gate. The last step (a thread entering the condition-variable wait) can't be
// observed from outside, so this is a timing-based stress repro: a round whose waiter found its
// chunk already complete is counted as not exercised, never as a pass of the bug's scenario.
// With notify_one a completed chunk woke only one waiter: in `same` the others slept until some
// later chunk completed, forever if none came; in `mixed` the wake-up could go to the waiter
// whose predicate was still false, and the other one slept on. Every waiter must return within
// 2 s of its chunk. Recovery after a failed round is bounded (10 s); past it the test exits 3.
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
    uint64_t           seq;       // 0: inject a high-priority chunk and wait for it
    HANDLE             entered;   // set right before the waiting call
    volatile LONG*     done;
    volatile LONG*     early;     // the gated chunk was already complete at the call
  };

  DxvkCsChunkRef chunk(DxvkCsChunkPool& pool, HANDLE inside, HANDLE gate) {
    DxvkCsChunkRef c(pool.allocChunk(DxvkCsChunkFlag::SingleUse), &pool);
    c->push([inside, gate] (MtlContext*) {
      if (inside)
        SetEvent(inside);
      if (gate)
        WaitForSingleObject(gate, INFINITE);
    });
    return c;
  }

  DWORD WINAPI waitThread(void* arg) {
    auto* w = static_cast<Waiter*>(arg);
    SetEvent(w->entered);
    if (w->seq) {
      if (w->cs->lastSequenceNumber() >= w->seq)
        InterlockedIncrement(w->early);
      w->cs->synchronize(w->seq);
    } else {
      w->cs->injectChunk(DxvkCsQueue::HighPriority, chunk(*w->pool, nullptr, nullptr), true);
    }
    InterlockedIncrement(w->done);
    return 0;
  }

  // One round. Returns 1 if every waiter returned in time, 0 if one didn't, -1 if not exercised.
  int round(DxvkCsThread& cs, DxvkCsChunkPool& pool, const char* name, int r, int n, bool mixed) {
    HANDLE inside = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    HANDLE gate = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    const uint64_t seq = cs.dispatchChunk(chunk(pool, inside, gate));
    WaitForSingleObject(inside, INFINITE);  // the CS thread is inside the gated chunk

    volatile LONG done = 0, early = 0;
    Waiter w[Waiters];
    HANDLE threads[Waiters], entered[Waiters];
    for (int i = 0; i < n; i++) {
      entered[i] = CreateEventA(nullptr, TRUE, FALSE, nullptr);
      w[i] = { &cs, &pool, (mixed && i == 0) ? 0u : seq, entered[i], &done, &early };
      threads[i] = CreateThread(nullptr, 0, waitThread, &w[i], 0, nullptr);
      WaitForSingleObject(entered[i], INFINITE);
      Sleep(50);
    }

    // Every waiter has reached its call; give the last one time to fall asleep in it.
    Sleep(100);
    const LONG doneBeforeGate = done;
    SetEvent(gate);

    const DWORD t0 = GetTickCount();
    while (done < n && GetTickCount() - t0 < 2000)
      Sleep(5);
    const LONG woke = done;
    int result = 1;
    if (doneBeforeGate || early) {
      result = -1;
      std::printf("SKIP %s round %d: not exercised (%ld returned before the gate, %ld found the chunk complete)\n",
        name, r, doneBeforeGate, early);
    } else if (woke != n) {
      result = 0;
      std::printf("FAIL %s round %d: %ld of %d waiters returned within 2 s of their chunk\n", name, r, woke, n);
    }

    // Release any waiter still asleep (each further sequenced chunk wakes at least one), for 10 s at most.
    const DWORD t1 = GetTickCount();
    while (done < n) {
      if (GetTickCount() - t1 > 10000) {
        std::printf("FAIL %s round %d: %ld of %d waiters still asleep 10 s after recovery began; giving up\n",
          name, r, n - done, n);
        std::fflush(stdout);
        ExitProcess(3);  // the CS thread can't be joined safely with waiters stuck
      }
      cs.dispatchChunk(chunk(pool, nullptr, nullptr));
      Sleep(5);
    }
    WaitForMultipleObjects(n, threads, TRUE, INFINITE);
    for (int i = 0; i < n; i++) {
      CloseHandle(threads[i]);
      CloseHandle(entered[i]);
    }
    CloseHandle(gate);
    CloseHandle(inside);
    return result;
  }

  struct Tally { int failed = 0, skipped = 0; };

}

int main() {
  DxvkCsChunkPool pool;
  DxvkCsThread cs { Rc<MtlDevice>(), Rc<MtlContext>() };
  Tally same, mixed;

  for (int r = 0; r < Rounds; r++) {
    int res = round(cs, pool, "same", r, Waiters, false);
    same.failed += res == 0; same.skipped += res < 0;
  }
  for (int r = 0; r < Rounds; r++) {
    int res = round(cs, pool, "mixed", r, 2, true);
    mixed.failed += res == 0; mixed.skipped += res < 0;
  }

  // PASS needs no failed round and most rounds exercised (a run that exercised little proves little).
  const bool fail = same.failed || mixed.failed;
  const bool thin = same.skipped > Rounds / 4 || mixed.skipped > Rounds / 4;
  std::printf("unit_cs_sync: same %d rounds x %d waiters, %d failed, %d not exercised; "
    "mixed %d rounds x 2 waiters, %d failed, %d not exercised: %s\n",
    Rounds, Waiters, same.failed, same.skipped, Rounds, mixed.failed, mixed.skipped,
    fail ? "FAIL" : thin ? "INCONCLUSIVE" : "PASS");
  std::fflush(stdout);
  return fail ? 1 : thin ? 2 : 0;
}
