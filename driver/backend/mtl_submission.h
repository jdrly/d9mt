#pragma once

// A dedicated pump thread observes GPU completion independently of the CS
// thread — an app thread blocked on a fence that only a retirement callback
// can signal would otherwise deadlock waiting for CS activity that never comes.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "wmt_handle.h"

#include "../mtl/mtl_resource.h"
#include "../util/sync/sync_signal.h"
#include "../util/thread.h"

namespace dxvk::backend {

// What one command buffer releases once the GPU is past it, handed to the tracker once per
// submit: no std::function, no allocation per entry, one lock and at most one pump wake per
// command buffer. The tracker recycles batches with their capacity.
struct RetireBatch {
  std::vector<std::pair<Rc<MtlBuffer>, Rc<MtlAllocation>>> recycles;  // renamed-away storage
  std::vector<obj_handle_t>                                 objects;   // NSObject_release
  std::vector<std::pair<Rc<sync::Signal>, uint64_t>>        signals;
  std::vector<Rc<MtlResource>>                              keepAlive;
  WmtCommandBuffer initCmdbuf;
  WmtCommandBuffer cmdbuf;

  bool empty() const {
    return recycles.empty() && objects.empty() && signals.empty() && keepAlive.empty()
        && !initCmdbuf && !cmdbuf;
  }

  // In recording order: recycles and object releases, the fence signals, then the command
  // buffers and their keep-alives. Keeps the vectors' capacity. Runs with the tracker's
  // m_runMutex held: nothing it releases may call processRetirements() (see there).
  void run();
};

class SubmissionTracker {

public:

  explicit SubmissionTracker(obj_handle_t device);

  ~SubmissionTracker();

  uint64_t nextSerial() const { return m_nextSerial.load(std::memory_order_acquire); }

  uint64_t lastSubmittedSerial() const { return nextSerial() - 1u; }

  size_t retirementCount() { std::lock_guard<std::mutex> l(m_retireMutex); return m_retirements.size() + m_batches.size(); }

  // Invariant: one queue and one submitter (the one context, on the CS thread). The serial is
  // taken and signalled here in submission order; isComplete's "serial >= nextSerial() is not
  // complete" fast path and m_batches' FIFO order (batches are handed over in serial order)
  // both rely on it.
  uint64_t onSubmit(obj_handle_t queue);

  // Reads the shared event (a unix call) and refreshes the cache.
  uint64_t completedSerial();

  // The last value any completedSerial() saw: never ahead of the GPU, so a serial at or below
  // it is complete. Callers that can do with a stale answer try this first.
  uint64_t completedSerialCached() const { return m_completedCache.load(std::memory_order_acquire); }

  bool isComplete(uint64_t serial) {
    if (serial <= completedSerialCached())
      return true;
    // Only onSubmit signals the event, with a serial it has already taken: one that isn't
    // submitted yet (the recording command buffer's) can't be complete.
    if (serial >= nextSerial())
      return false;
    return serial <= completedSerial();
  }

  void retireAfter(uint64_t serial, std::function<void()> release);

  // Queues the batch of the command buffer just submitted as `serial` and returns an empty
  // one (recycled when possible) for the next command buffer.
  std::unique_ptr<RetireBatch> retireBatch(uint64_t serial, std::unique_ptr<RetireBatch>&& batch);

  // Runs what is due. Blocks while another thread runs retirements (pump, host-alloc retry).
  // Invariant: a retirement (RetireBatch::run, a retireAfter callback) must never call
  // processRetirements(), directly or through e.g. a host-allocation failure path: it would
  // wait for m_runMutex, which the thread running it holds.
  void processRetirements();

  // CS thread, `completed` from a completedSerial() it just made. Skipped while the pump runs
  // retirements: it runs them in serial order, so fence values only go up.
  void processRetirements(uint64_t completed);

  // Blocks until the GPU is past `serial`, in slices of s_sliceMs, at most 5 of them. A
  // submitted serial still not signalled then means the GPU stopped (a command buffer that
  // never finishes, cut off by the GPU's timeout: the queue stays dead). Unless
  // D9MT_GPU_HUNG=continue, that ends the process (onHung). A serial nobody submitted yet
  // isn't the GPU's fault: that wait is given up with an error line, as before.
  void waitFor(uint64_t serial);

  // The GPU stopped (see waitFor). Set once; from then on waits return at once and the
  // context commits nothing more.
  bool gpuHung() const { return m_gpuHung.load(std::memory_order_acquire); }

  void drainAndStopPump();

  // The process's exit code after D9MT-GPU-HUNG.
  static constexpr unsigned kGpuHungExitCode = 0xD9D9u;

  // Tests: a shorter slice, and a handler that runs instead of ending the process.
  static inline uint32_t s_sliceMs = 2000u;
  static inline void (*s_onHung)(uint64_t serial, uint64_t completed) = nullptr;

private:

  struct Retirement {
    uint64_t serial;
    std::function<void()> release;
  };

  WmtSharedEvent m_event;
  std::atomic<uint64_t> m_nextSerial = { 1u };
  std::atomic<uint64_t> m_completedCache = { 0u };

  std::mutex m_retireMutex;
  // Held while retirements run, so they run in serial order on one thread at a time: a
  // later fence value signalled before an earlier one would go backwards (sync::Fence stores
  // what it is given), and a waiter for a value in between would sleep until the next flush.
  std::mutex m_runMutex;
  std::condition_variable m_retireCond;
  std::vector<Retirement> m_retirements;
  std::deque<std::pair<uint64_t, std::unique_ptr<RetireBatch>>> m_batches;  // serial order (one submitter, see onSubmit)
  std::vector<std::unique_ptr<RetireBatch>> m_freeBatches;
  bool m_stop = false;
  bool m_drained = false;  // drainAndStopPump ran: the GPU is idle, retireAfter releases at once

  const bool m_failFast;   // D9MT_GPU_HUNG unset (not "continue")
  std::atomic<bool> m_gpuHung = { false };

  dxvk::thread m_pump;

  void pumpThread();

  void onHung(uint64_t serial, uint64_t completed);

  void runDue(uint64_t completed);  // m_runMutex held

};

}
