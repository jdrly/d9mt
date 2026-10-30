#include "mtl_submission.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <thread>

#include "mtl_profiler.h"

#include "../util/log/log.h"
#include "../util/util_env.h"
#include "../util/util_string.h"

namespace dxvk::backend {

// The GPU-hang paths' strings and the test seams go last in .rdata and .data: "$zz" sorts after
// every other section of its kind, so they move no other data (#53 moved 1621 .rdata symbols).
#define D9MT_HANG_RDATA __attribute__((section(".rdata$zz_d9mt_gpu_hang")))
#define D9MT_HANG_DATA  __attribute__((section(".data$zz_d9mt_gpu_hang")))

uint32_t SubmissionTracker::s_sliceMs D9MT_HANG_DATA = 2000u;
void (*SubmissionTracker::s_terminate)(unsigned exitCode) D9MT_HANG_DATA = nullptr;
void (*SubmissionTracker::s_park)() D9MT_HANG_DATA = nullptr;

namespace {
  const char kEnvGpuHung[] D9MT_HANG_RDATA = "D9MT_GPU_HUNG";
  const char kContinue[] D9MT_HANG_RDATA = "continue";
  const char kNotSubmitted0[] D9MT_HANG_RDATA = "SubmissionTracker: serial ";
  const char kNotSubmitted1[] D9MT_HANG_RDATA = " still not submitted after ";
  const char kNotSubmitted2[] D9MT_HANG_RDATA = " s (next ";
  const char kNotSubmitted3[] D9MT_HANG_RDATA = ") — abandoning wait";
  const char kHung0[] D9MT_HANG_RDATA = "D9MT-GPU-HUNG: serial ";
  const char kHung1[] D9MT_HANG_RDATA = " not signalled after ";
  const char kHung2[] D9MT_HANG_RDATA = " s on the GPU (completed ";
  const char kHung3[] D9MT_HANG_RDATA = ", submitted ";
  const char kHung4[] D9MT_HANG_RDATA = "): the GPU stopped and its queue won't recover; ending the game (exit code ";
  const char kHung5[] D9MT_HANG_RDATA = "; D9MT_GPU_HUNG=continue keeps it running)";
  const char kErrPrefix[] D9MT_HANG_RDATA = "err:   ";
  const char kNewline[] D9MT_HANG_RDATA = "\n";
  const char kKernel32[] D9MT_HANG_RDATA = "kernel32.dll";
  const char kGetStdHandle[] D9MT_HANG_RDATA = "GetStdHandle";
  const char kTerminateProcess[] D9MT_HANG_RDATA = "TerminateProcess";

  // D9MT_GPU_HUNG unset (not "continue"). Read when a stall reaches its budget, not when the
  // tracker is made: the constructor stays as it was before #53.
  [[gnu::cold, gnu::noinline]] bool gpuHungEndsGame() {
    return env::getEnvVar(kEnvGpuHung) != kContinue;
  }
}

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
  std::deque<std::pair<uint64_t, std::unique_ptr<RetireBatch>>> batches;
  { std::lock_guard<std::mutex> lock(m_retireMutex);
    remaining = std::move(m_retirements);
    batches = std::move(m_batches);
    m_drained = true;
  }
  std::lock_guard<std::mutex> run(m_runMutex);
  for (Retirement& r : remaining)
    r.release();
  for (auto& b : batches)
    b.second->run();
}

uint64_t SubmissionTracker::onSubmit(obj_handle_t queue) {
  uint64_t serial = m_nextSerial.fetch_add(1u, std::memory_order_acq_rel);
  MTL4CommandQueue_signalEvent(queue, m_event.handle(), serial);
  return serial;
}

uint64_t SubmissionTracker::completedSerial() {
  prof::countEventRead();
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

  bool wake;
  { std::unique_lock<std::mutex> lock(m_retireMutex);
    // After drainAndStopPump nothing runs the list any more (no pump, no CS thread), and
    // the GPU is idle: release now, or e.g. a texture view destroyed in device teardown leaks.
    if (m_drained) {
      lock.unlock();
      release();
      return;
    }
    wake = m_retirements.empty() && m_batches.empty();
    m_retirements.push_back({ serial, std::move(release) });
  }
  // The pump only sleeps on the condition while both lists are empty; otherwise it is
  // waiting on the event for an earlier serial and rescans the lists after it.
  if (wake)
    m_retireCond.notify_one();
}

std::unique_ptr<RetireBatch> SubmissionTracker::retireBatch(uint64_t serial, std::unique_ptr<RetireBatch>&& batch) {
  if (batch == nullptr || batch->empty())
    return batch != nullptr ? std::move(batch) : std::make_unique<RetireBatch>();

  std::unique_ptr<RetireBatch> next;
  bool wake = false;
  bool stopped;
  { std::lock_guard<std::mutex> lock(m_retireMutex);
    if (!m_freeBatches.empty()) {
      next = std::move(m_freeBatches.back());
      m_freeBatches.pop_back();
    }
    // Stopped: drainAndStopPump won't look at the list again; run it below, once the
    // GPU is past everything submitted (an unsubmitted command buffer never runs).
    stopped = m_stop;
    if (!stopped) {
      wake = m_retirements.empty() && m_batches.empty();
      m_batches.emplace_back(serial, std::move(batch));
    }
  }
  if (stopped) {
    waitFor(std::min(serial, lastSubmittedSerial()));
    std::lock_guard<std::mutex> run(m_runMutex);
    batch->run();
  }
  if (wake)
    m_retireCond.notify_one();
  return next != nullptr ? std::move(next) : std::make_unique<RetireBatch>();
}

void SubmissionTracker::processRetirements() {
  std::lock_guard<std::mutex> run(m_runMutex);
  runDue(completedSerial());
}

void SubmissionTracker::processRetirements(uint64_t completed) {
  std::unique_lock<std::mutex> run(m_runMutex, std::try_to_lock);
  if (run.owns_lock())
    runDue(completed);
}

void SubmissionTracker::runDue(uint64_t completed) {
  std::vector<std::function<void()>> ready;
  RetireBatch* due[8];
  uint32_t     dueCount = 0u;
  {
    std::lock_guard<std::mutex> lock(m_retireMutex);
    if (m_retirements.empty() && m_batches.empty())
      return;

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

    while (dueCount < 8u && !m_batches.empty() && m_batches.front().first <= completed) {
      due[dueCount++] = m_batches.front().second.release();
      m_batches.pop_front();
    }
  }

  for (auto& release : ready)
    release();

  if (!dueCount)
    return;
  for (uint32_t i = 0u; i < dueCount; i++)
    due[i]->run();
  bool more;
  { std::lock_guard<std::mutex> lock(m_retireMutex);
    for (uint32_t i = 0u; i < dueCount; i++) {
      if (m_freeBatches.size() < 8u)
        m_freeBatches.emplace_back(due[i]);
      else
        delete due[i];
    }
    more = !m_batches.empty() && m_batches.front().first <= completed;
  }
  if (more)
    runDue(completed);
}

void SubmissionTracker::pumpThread() {
  env::setThreadName("d9mt-retire");

  std::unique_lock<std::mutex> lock(m_retireMutex);

  while (true) {
    m_retireCond.wait(lock, [this] {
      return m_stop || !m_retirements.empty() || !m_batches.empty();
    });

    if (m_stop)
      return;

    uint64_t target = m_batches.empty() ? ~0ull : m_batches.front().first;
    for (const Retirement& r : m_retirements)
      target = r.serial < target ? r.serial : target;

    lock.unlock();

    if (!isComplete(target))
      MTLSharedEvent_waitUntilSignaledValue(m_event.handle(), target, 500u);

    processRetirements();

    lock.lock();
  }
}

// D9MT_PROFILE's note of a wait on a serial nobody had submitted yet; out of line, so the wait
// doesn't inline it at each of its returns.
[[gnu::cold, gnu::noinline]] static void noteUnsubmitted(std::chrono::steady_clock::time_point t0, uint64_t serial) {
  prof::noteUnsubmittedWait(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::steady_clock::now() - t0).count()), serial);
}

// waitFor's code is as large as before #53, to the byte: its hang paths are in stallEnds (cold), so
// no function after it moves (a layout cost, PERFORMANCE.md §1). Check a change here with the sizes
// in the PR (#53's cost-free candidate) before it merges.
void SubmissionTracker::waitFor(uint64_t serial) {
  if (isComplete(serial))
    return;
  // D9MT_PROFILE: a wait that starts on a serial nobody submitted yet, timed until it returns.
  struct UnsubmittedWait {
    std::chrono::steady_clock::time_point t0;
    uint64_t serial;
    bool on;
    ~UnsubmittedWait() {
      if (on)
        noteUnsubmitted(t0, serial);
    }
  } unsubmitted = { std::chrono::steady_clock::now(), serial, prof::g_profEnabled && serial >= nextSerial() };
  // Bounded blocking wait, not a spin. Only slices the serial spent submitted count towards a GPU
  // hang (5 of them: 10 s on the GPU); a serial still not submitted after 5 slices is a missing
  // flush, not the GPU's fault, and the wait is given up as before. After a GPU hang, no wait on
  // an incomplete serial returns: stallEnds parks it at the end of its first slice.
  uint32_t slices = 0u, submittedSlices = 0u;
  for (;;) {
    const bool submitted = serial < nextSerial();
    if (MTLSharedEvent_waitUntilSignaledValue(m_event.handle(), serial, s_sliceMs)) {
      completedSerial();
      return;
    }
    completedSerial();
    if (isComplete(serial))
      return;
    slices++;
    if (submitted)
      submittedSlices++;
    // Waiting on a serial that is still not submitted after a whole slice: whoever waits
    // should have flushed first (G1: a pass kept open by a deferrable flush). Waiting on the
    // serial the CS thread is about to submit is normal (WaitForResource flushes, then syncs
    // the CS thread only up to the resource's last use) and ends within the first slice.
    if (serial >= nextSerial())
      Logger::warn(str::format("SubmissionTracker: waiting on serial ", serial,
        ", still not submitted yet after ", slices * s_sliceMs / 1000u, " s (next ", nextSerial(), ")"));
    Logger::warn(str::format("SubmissionTracker: GPU stall waiting serial ",
      serial, " (completed ", completedSerial(), ") — slice ", slices));
    if (const int end = stallEnds(serial, slices, submittedSlices)) {
      if (end == 1)
        return;
      break;
    }
  }
  Logger::err(str::format("SubmissionTracker: GPU never signalled serial ",
    serial, " (completed ", completedSerial(), ") — abandoning wait"));
}

// A slice the GPU didn't signal: does the wait end? After a GPU hang it parks. Both modes wait 5
// slices at least. Unless D9MT_GPU_HUNG=continue, only slices the serial spent submitted count
// towards a GPU hang, and a serial still not submitted after 5 slices is abandoned.
// D9MT_GPU_HUNG=continue keeps the old budget: 5 slices in all, submitted or not.
int SubmissionTracker::stallEnds(uint64_t serial, uint32_t slices, uint32_t submittedSlices) {
  if (gpuHung()) {
    parkHung();
    return 1;
  }
  if (slices < 5u)
    return 0;
  if (!gpuHungEndsGame())
    return 2;
  if (submittedSlices < 5u) {
    if (serial < nextSerial())
      return 0;
    Logger::err(str::format(kNotSubmitted0, serial, kNotSubmitted1,
      slices * s_sliceMs / 1000u, kNotSubmitted2, nextSerial(), kNotSubmitted3));
    return 1;
  }
  hung(serial);
  return 1;
}

// The GPU stopped: rendering on (as before) leaves the game frozen, its dead GPU context
// alive, and the process running past a logout, one rejected commit every 10 s. The first
// thread to see it says so and ends the process. No wait on an incomplete serial may return to
// its caller from here on: it would release or reuse what the GPU may still use (ADR-0001), so
// every such wait parks until the process is gone.
void SubmissionTracker::hung(uint64_t serial) {
  // completed at the very end of the wait: no hang
  const uint64_t completed = completedSerial();
  if (completed >= serial)
    return;
  if (m_gpuHung.exchange(true, std::memory_order_acq_rel))
    return parkHung();
  // Looked up here, not imported: a new import adds its thunk among the code linked after the
  // driver's own and moves all of it.
  const HMODULE kernel32 = GetModuleHandleA(kKernel32);
  const auto getStdHandle = reinterpret_cast<HANDLE (WINAPI*)(DWORD)>(
    reinterpret_cast<void*>(GetProcAddress(kernel32, kGetStdHandle)));
  const auto terminateProcess = reinterpret_cast<BOOL (WINAPI*)(HANDLE, UINT)>(
    reinterpret_cast<void*>(GetProcAddress(kernel32, kTerminateProcess)));
  const std::string line = str::format(kHung0, serial, kHung1, 5u * s_sliceMs / 1000u, kHung2, completed,
    kHung3, lastSubmittedSerial(), kHung4, kGpuHungExitCode, kHung5);
  // The line must be out whatever the log level is: through the logger if it writes errors
  // (__wine_dbg_output writes synchronously; only "none" drops them; the level it fixed at start),
  // else straight to stderr, flushed.
  if (Logger::logLevel() != LogLevel::None) {
    Logger::err(line);
  } else {
    const std::string out = kErrPrefix + line + kNewline;
    HANDLE h = getStdHandle ? getStdHandle(STD_ERROR_HANDLE) : nullptr;
    DWORD written = 0u;
    if (h && h != INVALID_HANDLE_VALUE) {
      WriteFile(h, out.data(), DWORD(out.size()), &written, nullptr);
      FlushFileBuffers(h);
    }
  }
  if (s_terminate)
    s_terminate(kGpuHungExitCode);
  else if (terminateProcess)
    terminateProcess(GetCurrentProcess(), kGpuHungExitCode);
  parkHung();
}

void SubmissionTracker::parkHung() {
  if (s_park)
    return s_park();
  for (;;)
    Sleep(INFINITE);
}

}
