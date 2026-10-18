#include "mtl_submission.h"

#include <chrono>
#include <thread>

#include "../util/log/log.h"
#include "../util/util_env.h"
#include "../util/util_string.h"

namespace dxvk::backend {

SubmissionTracker::SubmissionTracker(obj_handle_t device) {
  m_event = WmtSharedEvent::adopt(MTLDevice_newSharedEvent(device));
  m_pump = dxvk::thread([this] { pumpThread(); });
}

SubmissionTracker::~SubmissionTracker() {
  drainAndStopPump();
}

void SubmissionTracker::drainAndStopPump() {
  { std::lock_guard<std::mutex> lock(m_retireMutex);
    if (m_stop) {
      return;
    }
    m_stop = true;
  }
  m_retireCond.notify_all();
  m_pump.join();

  // Drain the GPU, then run every remaining retirement here: dropping one would leak captured Rcs after the residency tracker is gone.
  waitFor(lastSubmittedSerial());

  std::vector<Retirement> remaining;
  { std::lock_guard<std::mutex> lock(m_retireMutex);
    remaining = std::move(m_retirements);
  }
  for (Retirement& r : remaining)
    r.release();
}

namespace {
  uint64_t steadyNs() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
  }
}

uint64_t SubmissionTracker::onSubmit(obj_handle_t queue) {
  uint64_t serial = m_nextSerial.fetch_add(1u, std::memory_order_acq_rel);
  MTL4CommandQueue_signalEvent(queue, m_event.handle(), serial);
  SubmitRecord& r = m_submits[serial % kSubmitRing];
  r.drawable.store(0u, std::memory_order_relaxed);
  r.timeNs.store(steadyNs(), std::memory_order_relaxed);
  r.serial.store(serial, std::memory_order_release);
  return serial;
}

void SubmissionTracker::notePresent(uint64_t serial, obj_handle_t drawable) {
  SubmitRecord& r = m_submits[serial % kSubmitRing];
  if (r.serial.load(std::memory_order_acquire) == serial)
    r.drawable.store(uint64_t(drawable), std::memory_order_relaxed);
}

void SubmissionTracker::describeStall(uint64_t waitingFor) {
  // Once per stuck serial: what the GPU queue is blocked on. The first serial
  // not completed is the one holding the queue up; if it is a present, the
  // queue is waiting for its drawable (waitForDrawable, d9mt#23), otherwise
  // it is executing (or faulted in) ordinary work.
  const uint64_t completed = completedSerial();
  const uint64_t stuck = completed + 1u;
  { std::lock_guard<std::mutex> lock(m_stallMutex);
    if (m_stallReported == stuck)
      return;
    m_stallReported = stuck;
  }
  const uint64_t now = steadyNs();
  std::string what = "unknown (older than the submission ring)";
  const SubmitRecord& r = m_submits[stuck % kSubmitRing];
  if (r.serial.load(std::memory_order_acquire) == stuck) {
    const uint64_t drawable = r.drawable.load(std::memory_order_relaxed);
    const double agoMs = double(now - r.timeNs.load(std::memory_order_relaxed)) * 1e-6;
    what = drawable
      ? str::format("a present: the queue waits for drawable 0x", std::hex, drawable, std::dec,
                    " (waitForDrawable), committed ", agoMs, " ms ago")
      : str::format("render work (no drawable wait), committed ", agoMs, " ms ago");
  }
  uint32_t presentsAfter = 0u;
  for (uint64_t s = stuck + 1u; s <= lastSubmittedSerial() && s < stuck + kSubmitRing; s++) {
    const SubmitRecord& q = m_submits[s % kSubmitRing];
    if (q.serial.load(std::memory_order_acquire) == s && q.drawable.load(std::memory_order_relaxed))
      presentsAfter++;
  }
  Logger::warn(str::format("SubmissionTracker: stall at serial ", stuck, " (waiting for ", waitingFor,
    ", last submitted ", lastSubmittedSerial(), "): ", what, "; ", presentsAfter, " presents queued behind it"));
}

uint64_t SubmissionTracker::completedSerial() {
  uint64_t value = MTLSharedEvent_signaledValue(m_event.handle());

  uint64_t cached = m_completedCache.load(std::memory_order_acquire);
  while (value > cached
      && !m_completedCache.compare_exchange_weak(cached, value,
            std::memory_order_acq_rel, std::memory_order_acquire)) { }

  return value > cached ? value : cached;
}

void SubmissionTracker::retireAfter(uint64_t serial, std::function<void()> release) {
  if (isComplete(serial)) {
    release();
    return;
  }

  { std::lock_guard<std::mutex> lock(m_retireMutex);
    m_retirements.push_back({ serial, std::move(release) });
  }
  m_retireCond.notify_one();
}

void SubmissionTracker::processRetirements() {
  std::vector<std::function<void()>> ready;
  {
    std::lock_guard<std::mutex> lock(m_retireMutex);
    if (m_retirements.empty())
      return;

    uint64_t completed = completedSerial();
    size_t write = 0u;

    for (size_t i = 0u; i < m_retirements.size(); i++) {
      if (m_retirements[i].serial <= completed) {
        ready.push_back(std::move(m_retirements[i].release));
      } else {
        if (write != i)
          m_retirements[write] = std::move(m_retirements[i]);
        write++;
      }
    }

    m_retirements.resize(write);
  }

  for (auto& release : ready)
    release();
}

void SubmissionTracker::pumpThread() {
  env::setThreadName("d9mt-retire");

  std::unique_lock<std::mutex> lock(m_retireMutex);

  while (true) {
    m_retireCond.wait(lock, [this] {
      return m_stop || !m_retirements.empty();
    });

    if (m_stop)
      return;

    uint64_t target = ~0ull;
    for (const Retirement& r : m_retirements)
      target = r.serial < target ? r.serial : target;

    lock.unlock();

    if (!isComplete(target))
      MTLSharedEvent_waitUntilSignaledValue(m_event.handle(), target, 500u);

    processRetirements();

    lock.lock();
  }
}

void SubmissionTracker::waitFor(uint64_t serial) {
  if (isComplete(serial))
    return;
  // Bounded blocking wait, not a spin, so a never-signalled command buffer degrades to a logged stall instead of a hang.
  for (int i = 0; i < 5; i++) {
    if (MTLSharedEvent_waitUntilSignaledValue(m_event.handle(), serial, 2000u)) {
      completedSerial();
      return;
    }
    completedSerial();
    if (isComplete(serial))
      return;
    Logger::warn(str::format("SubmissionTracker: GPU stall waiting serial ",
      serial, " (completed ", completedSerial(), ") — slice ", i + 1));
    if (i == 0)
      describeStall(serial);
  }
  Logger::err(str::format("SubmissionTracker: GPU never signalled serial ",
    serial, " (completed ", completedSerial(), ") — abandoning wait"));
}

}
