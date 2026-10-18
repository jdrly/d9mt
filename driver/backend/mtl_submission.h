#pragma once

// A dedicated pump thread observes GPU completion independently of the CS
// thread — an app thread blocked on a fence that only a retirement callback
// can signal would otherwise deadlock waiting for CS activity that never comes.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

#include "wmt_handle.h"

#include "../util/thread.h"

namespace dxvk::backend {

class SubmissionTracker {

public:

  explicit SubmissionTracker(obj_handle_t device);

  ~SubmissionTracker();

  uint64_t nextSerial() const { return m_nextSerial.load(std::memory_order_acquire); }

  uint64_t lastSubmittedSerial() const { return nextSerial() - 1u; }

  size_t retirementCount() { std::lock_guard<std::mutex> l(m_retireMutex); return m_retirements.size(); }

  uint64_t onSubmit(obj_handle_t queue);

  uint64_t completedSerial();

  bool isComplete(uint64_t serial) {
    return serial <= completedSerial();
  }

  void retireAfter(uint64_t serial, std::function<void()> release);

  void processRetirements();

  void waitFor(uint64_t serial);

  void drainAndStopPump();

  /// CS thread: `serial` is the submission right after a queue-side
  /// waitForDrawable (a present). Only read when a wait stalls (d9mt#23).
  void notePresent(uint64_t serial, obj_handle_t drawable);

private:

  // The last submissions, indexed by serial % size: when they were committed
  // and whether the queue waited for a drawable just before (d9mt#23: a stall
  // names what the stuck serial is). Written by the CS thread, read on stalls.
  struct SubmitRecord {
    std::atomic<uint64_t> serial   = { 0u };
    std::atomic<uint64_t> timeNs   = { 0u };
    std::atomic<uint64_t> drawable = { 0u };   // 0: not a present
  };
  static constexpr size_t kSubmitRing = 64u;
  SubmitRecord m_submits[kSubmitRing];
  uint64_t     m_stallReported = 0u;   // serial whose stall was described (waitFor callers)
  std::mutex   m_stallMutex;

  void describeStall(uint64_t waitingFor);

  struct Retirement {
    uint64_t serial;
    std::function<void()> release;
  };

  WmtSharedEvent m_event;
  std::atomic<uint64_t> m_nextSerial = { 1u };
  std::atomic<uint64_t> m_completedCache = { 0u };

  std::mutex m_retireMutex;
  std::condition_variable m_retireCond;
  std::vector<Retirement> m_retirements;
  bool m_stop = false;

  dxvk::thread m_pump;

  void pumpThread();

};

}
