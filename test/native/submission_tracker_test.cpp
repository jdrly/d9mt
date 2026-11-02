// Unit test for SubmissionTracker (driver/backend/mtl_submission.cpp, compiled as is) against a
// fake shared event: a "GPU" completes serials when the test says so.
//  - isComplete answers from the cached value or the submission counter without reading the
//    event: a serial at or below the cache, and the recording (unsubmitted) serial;
//  - a retire batch never runs before its serial completes, runs once after, in serial order,
//    and comes back empty with its capacity;
//  - fence signals in a batch are run by the pump with no CS activity (an app thread waiting on
//    the fence would otherwise deadlock), also when the pump was asleep;
//  - processRetirements(completed) runs only what is due, also more than 8 batches at once;
//  - drainAndStopPump runs what is left, also a batch of a command buffer never submitted;
//  - random submit/complete/process from three threads: nothing runs early, everything runs,
//    and a fence every batch signals never goes backwards (the pump and the CS thread both run
//    retirements; sync::Fence stores whatever value it is given).
//  - a GPU hang (a submitted serial not signalled for 5 slices it spent submitted): one
//    termination (an observer stands in for TerminateProcess), and from then on no wait on an
//    incomplete serial returns to its caller (they park; the test's park blocks like production's
//    until the test releases it), also concurrently: the reporter, another thread's wait and the
//    device's drain; a serial completed exactly at the end of the wait, or submitted late in it,
//    is no hang; a serial nobody submits and D9MT_GPU_HUNG=continue give up the wait as before;
//    D9MT_GPU_HUNG is the tracker's from its construction (changed later, it doesn't apply);
//  - with DXVK_LOG_LEVEL=none the D9MT-GPU-HUNG line still reaches stderr (--log-none, its own
//    process).
// The tracker uses dxvk::thread and the driver's util, so this is a mingw exe run under Wine
// (no window). RetireBatch::run is the driver's minus the buffer recycling, which needs the device.
//   x86_64-w64-mingw32-g++ -std=c++17 -O1 -DWINEMETAL_API='extern "C"' -I driver/backend \
//     test/native/submission_tracker_test.cpp driver/backend/mtl_submission.cpp \
//     build/driver-x64/obj/util_{log_log,thread,util_env,util_string}.cpp.o ... -o stt.exe
#include "mtl_submission.h"
#include "mtl_profiler.h"
#include "../util/util_env.h"

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <string>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

using namespace dxvk;
using namespace dxvk::backend;

static std::atomic<int> g_fail = { 0 };
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

// ---- the fake GPU: one shared event --------------------------------------------------------

static std::mutex              g_gpuMutex;
static std::condition_variable g_gpuCond;
static uint64_t                g_signaled  = 0u;   // value the "GPU" has reached
static uint64_t                g_submitted = 0u;   // highest serial signalEvent was called for
static std::atomic<uint64_t>   g_reads     = { 0u };  // MTLSharedEvent_signaledValue calls
// The boundary case: after the g_boundaryAt-th timed-out slice, the "GPU" completes
// g_boundaryValue on the 4th event read that follows (hung()'s final check), not before.
static std::atomic<uint64_t>   g_timeouts  = { 0u };
static std::atomic<uint64_t>   g_waitCalls = { 0u };   // MTLSharedEvent_waitUntilSignaledValue calls
static uint64_t                g_boundaryAt = 0u, g_boundaryValue = 0u;
static std::atomic<uint64_t>   g_completeOnRead = { 0u };

static void gpuComplete(uint64_t serial) {
  { std::lock_guard<std::mutex> l(g_gpuMutex);
    if (serial > g_signaled)
      g_signaled = serial;
  }
  g_gpuCond.notify_all();
}

static std::mutex                g_releaseMutex;
static std::vector<obj_handle_t> g_released;

extern "C" {
obj_handle_t MTLDevice_newSharedEvent(obj_handle_t) { return 0x5eu; }
void MTL4CommandQueue_signalEvent(obj_handle_t, obj_handle_t, uint64_t value) {
  std::lock_guard<std::mutex> l(g_gpuMutex);
  g_submitted = value;
}
uint64_t MTLSharedEvent_signaledValue(obj_handle_t) {
  const uint64_t n = ++g_reads;
  std::lock_guard<std::mutex> l(g_gpuMutex);
  if (g_completeOnRead.load() && n == g_completeOnRead.load() && g_boundaryValue > g_signaled)
    g_signaled = g_boundaryValue;
  return g_signaled;
}
bool MTLSharedEvent_waitUntilSignaledValue(obj_handle_t, uint64_t value, uint64_t timeoutMs) {
  g_waitCalls++;
  std::unique_lock<std::mutex> l(g_gpuMutex);
  const bool done = g_gpuCond.wait_for(l, std::chrono::milliseconds(timeoutMs), [&] { return g_signaled >= value; });
  if (!done && g_boundaryAt && ++g_timeouts == g_boundaryAt)
    g_completeOnRead = g_reads.load() + 4u;
  return done;
}
void NSObject_release(obj_handle_t obj);
}

// Stress batches (handle kStress + serial) must never run before the GPU reached their serial.
static constexpr obj_handle_t kStress = 0x400000u;
static std::atomic<int> g_early = { 0 };

extern "C" void NSObject_release(obj_handle_t obj) {
  if (obj > kStress) {
    std::lock_guard<std::mutex> g(g_gpuMutex);
    if (g_signaled < obj - kStress)
      g_early++;
  }
  std::lock_guard<std::mutex> l(g_releaseMutex);
  g_released.push_back(obj);
}

extern "C" {
}

namespace dxvk::backend {
  void RetireBatch::run() {
    recycles.clear();
    for (obj_handle_t o : objects)
      NSObject_release(o);
    objects.clear();
    for (auto& s : signals)
      s.first->signal(s.second);
    signals.clear();
    initCmdbuf.reset();
    cmdbuf.reset();
    keepAlive.clear();
  }
}

// Counts signals with a value below the previous one.
class OrderCheck final : public sync::Signal {
public:
  uint64_t value() const { return m_last.load(); }
  void signal(uint64_t v) {
    if (v < m_last.load())
      m_backwards++;
    m_last.store(v);
  }
  void wait(uint64_t) { }
  std::atomic<uint64_t> m_last = { 0u };
  std::atomic<int>      m_backwards = { 0 };
};

// A batch whose object handle encodes its serial: a release before that serial completes is a bug.
static constexpr obj_handle_t kTag = 0x100000u;

static std::unique_ptr<RetireBatch> fill(std::unique_ptr<RetireBatch> b, uint64_t serial) {
  b->objects.push_back(kTag + serial);
  return b;
}

static bool released(obj_handle_t h) {
  std::lock_guard<std::mutex> l(g_releaseMutex);
  for (obj_handle_t r : g_released)
    if (r == h)
      return true;
  return false;
}

static bool waitReleased(obj_handle_t h, int ms) {
  for (int i = 0; i < ms; i++) {
    if (released(h))
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return released(h);
}

static void basics() {
  SubmissionTracker t(0x1u);
  CHECK(t.nextSerial() == 1u);

  // Recording serial 1: not complete, and no event read to find that out.
  uint64_t r0 = g_reads;
  CHECK(!t.isComplete(1u));
  CHECK(g_reads == r0);
  // Serial 0 (never used): complete from the cache.
  CHECK(t.isComplete(0u));
  CHECK(g_reads == r0);

  // Submit 1; the GPU hasn't finished: a submitted serial above the cache reads the event.
  uint64_t s1 = t.onSubmit(0x2u);
  CHECK(s1 == 1u);
  CHECK(!t.isComplete(1u));
  CHECK(g_reads == r0 + 1u);

  // Batch for 1: queued, the pump waits for it.
  auto spare = t.retireBatch(s1, fill(std::make_unique<RetireBatch>(), s1));
  CHECK(spare != nullptr && spare->empty());
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  CHECK(!released(kTag + 1u));

  gpuComplete(1u);
  CHECK(waitReleased(kTag + 1u, 2000));   // the pump, no processRetirements from "CS"
  CHECK(t.completedSerialCached() == 1u);
  r0 = g_reads;
  CHECK(t.isComplete(1u));
  CHECK(g_reads == r0);

  // The run batch is recycled: the next retireBatch hands it back, empty, capacity kept.
  uint64_t s2 = t.onSubmit(0x2u);
  auto b2 = fill(std::move(spare), s2);
  auto back = t.retireBatch(s2, std::move(b2));
  CHECK(back->empty());
  CHECK(back->objects.capacity() >= 1u);
  gpuComplete(2u);
  CHECK(waitReleased(kTag + 2u, 2000));

  // An empty batch is handed straight back, nothing queued.
  RetireBatch* raw = back.get();
  auto same = t.retireBatch(t.nextSerial(), std::move(back));
  CHECK(same.get() == raw);
}

static void signalsWithoutCs() {
  // An app thread waits on a fence the tracker signals; the CS thread does nothing more after
  // the submit. Let the pump fall asleep first (empty lists), so the wake path is tested.
  SubmissionTracker t(0x1u);
  g_signaled = 0u; g_submitted = 0u;
  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  Rc<sync::Fence> fence = new sync::Fence(0u);
  uint64_t s = t.onSubmit(0x2u);
  auto b = std::make_unique<RetireBatch>();
  b->signals.emplace_back(fence, 7u);
  t.retireBatch(s, std::move(b));

  std::atomic<bool> woke = { false };
  std::thread app([&] { fence->wait(7u); woke = true; });
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  CHECK(!woke);
  gpuComplete(s);
  for (int i = 0; i < 2000 && !woke; i++)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  CHECK(woke);
  if (!woke)
    fence->signal(7u);
  app.join();
}

static void manyDue() {
  SubmissionTracker t(0x1u);
  g_signaled = 0u;
  std::vector<uint64_t> serials;
  for (int i = 0; i < 20; i++) {
    uint64_t s = t.onSubmit(0x2u);
    serials.push_back(s);
    t.retireBatch(s, fill(std::make_unique<RetireBatch>(), 1000u + s));
  }
  // Half complete: process from the "CS" with the value it read; only those run.
  {
    std::lock_guard<std::mutex> l(g_gpuMutex);
    g_signaled = serials[9];   // no notify: the pump keeps sleeping on its 500 ms slice
  }
  // The pump may be running some of them right now: wait for the due ones.
  t.processRetirements(t.completedSerial());
  for (int i = 0; i < 20; i++)
    CHECK(i < 10 ? waitReleased(kTag + 1000u + serials[i], 2000) : !released(kTag + 1000u + serials[i]));
  gpuComplete(serials.back());
  t.processRetirements(t.completedSerial());
  for (int i = 0; i < 20; i++)
    CHECK(waitReleased(kTag + 1000u + serials[i], 2000));
  CHECK(t.retirementCount() == 0u);
}

static void drain() {
  g_signaled = 0u;
  auto t = std::make_unique<SubmissionTracker>(0x1u);
  uint64_t s = t->onSubmit(0x2u);
  t->retireBatch(s, fill(std::make_unique<RetireBatch>(), 5000u + s));
  // A context destroyed with releases recorded for a command buffer it never submitted.
  uint64_t open = t->nextSerial();
  t->retireBatch(open, fill(std::make_unique<RetireBatch>(), 5000u + open));
  std::thread gpu([] { std::this_thread::sleep_for(std::chrono::milliseconds(50)); gpuComplete(1u); });
  t->drainAndStopPump();
  gpu.join();
  CHECK(released(kTag + 5000u + s));
  CHECK(released(kTag + 5000u + open));
  // After the stop, a batch runs at once.
  t->retireBatch(t->nextSerial(), fill(std::make_unique<RetireBatch>(), 6000u));
  CHECK(released(kTag + 6000u));
}

static void stress() {
  g_signaled = 0u;
  SubmissionTracker t(0x1u);
  std::atomic<uint64_t> lastSubmitted = { 0u };
  std::atomic<bool> stop = { false };
  std::atomic<int> early = { 0 };

  // The "GPU": completes up to what was submitted, in random steps.
  std::thread gpu([&] {
    std::mt19937 rng(7u);
    uint64_t done = 0u;
    while (!stop || done < lastSubmitted) {
      uint64_t sub = lastSubmitted;
      if (done < sub)
        done += 1u + rng() % (sub - done);
      gpuComplete(done);
      std::this_thread::sleep_for(std::chrono::microseconds(rng() % 300));
    }
  });
  // Another thread polling like the app (queries, isResourceInUse).
  std::thread app([&] {
    while (!stop) {
      uint64_t s = lastSubmitted;
      if (s && t.isComplete(s)) {
        std::lock_guard<std::mutex> l(g_gpuMutex);
        if (g_signaled < s)
          early++;
      }
      std::this_thread::yield();
    }
  });

  constexpr int N = 3000;
  Rc<OrderCheck> order = new OrderCheck();
  std::unique_ptr<RetireBatch> next = std::make_unique<RetireBatch>();
  for (int i = 0; i < N; i++) {
    uint64_t s = t.onSubmit(0x2u);
    lastSubmitted = s;
    next->objects.push_back(kStress + s);
    next->signals.emplace_back(order, s);
    next = t.retireBatch(s, std::move(next));
    CHECK(next != nullptr && next->empty());
    if (i % 3 == 0)
      t.processRetirements(t.completedSerial());
  }
  stop = true;
  gpu.join();
  app.join();
  t.drainAndStopPump();
  CHECK(early == 0);
  CHECK(g_early == 0);
  CHECK(order->m_backwards == 0);
  CHECK(order->m_last == uint64_t(N));

  // Every batch ran exactly once, and never before its serial completed: releases are
  // recorded in run order, which must be serial order per thread; check the set and the count.
  std::lock_guard<std::mutex> l(g_releaseMutex);
  size_t mine = 0u;
  std::vector<int> seen(N + 2, 0);
  for (obj_handle_t r : g_released) {
    if (r > kStress && r <= kStress + N) {
      mine++;
      seen[r - kStress]++;
    }
  }
  CHECK(mine == size_t(N));
  for (int s = 1; s <= N; s++)
    CHECK(seen[s] == 1);
}

// G1's diagnostic (PROF unsubWait=): a waitFor() that starts on a serial nobody submitted yet
// is counted and timed until it returns. Positive test: the serial is submitted and completed
// 20 ms later from another thread. Needs D9MT_PROFILE=1 (prof::g_profEnabled is read once).
static void unsubmittedWait() {
  if (!prof::g_profEnabled) {
    std::printf("FAIL unsubmittedWait: run with D9MT_PROFILE=1\n");
    g_fail++;
    return;
  }
  g_signaled = 0u;
  SubmissionTracker t(0x1u);
  const uint64_t before = prof::g_unsubWaits.load();
  prof::g_unsubWaitMaxNs.store(0u);
  const uint64_t open = t.nextSerial();
  std::thread cs([&t, open] {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    uint64_t s = t.onSubmit(0x2u);
    if (s == open)
      gpuComplete(s);
  });
  t.waitFor(open);
  cs.join();
  CHECK(prof::g_unsubWaits.load() == before + 1u);
  CHECK(prof::g_unsubWaitMaxNs.load() >= 15000000u);   // >= 15 ms of the 20 ms
  // A wait on a submitted serial isn't counted.
  uint64_t s2 = t.onSubmit(0x2u);
  std::thread gpu([s2] { std::this_thread::sleep_for(std::chrono::milliseconds(5)); gpuComplete(s2); });
  t.waitFor(s2);
  gpu.join();
  CHECK(prof::g_unsubWaits.load() == before + 1u);
}

// ---- GPU hang: the observer stands in for TerminateProcess; the park blocks like production's
// (Sleep(INFINITE)) until the test releases it, then returns so the test can end

static std::atomic<int>      g_terminated = { 0 };
static std::atomic<unsigned> g_exitCode = { 0u };
static std::atomic<int64_t>  g_terminatedAtMs = { 0 };
static std::atomic<int>      g_parked = { 0 };
static std::mutex              g_parkMutex;
static std::condition_variable g_parkCond;
static bool                    g_parkRelease = true;   // true: a park returns at once (counted)
static std::chrono::steady_clock::time_point g_t0;

static int64_t msSince(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
}

static void observeTerminate(unsigned code) {
  g_exitCode = code;
  g_terminatedAtMs = msSince(g_t0);
  g_terminated++;
}

static void testPark() {
  g_parked++;
  std::unique_lock<std::mutex> l(g_parkMutex);
  g_parkCond.wait(l, [] { return g_parkRelease; });
}

static void resetHang() {
  g_terminated = 0; g_parked = 0; g_exitCode = 0u; g_terminatedAtMs = 0;
  g_boundaryAt = 0u; g_boundaryValue = 0u; g_completeOnRead = 0u; g_timeouts = 0u;
  { std::lock_guard<std::mutex> l(g_parkMutex); g_parkRelease = true; }
  { std::lock_guard<std::mutex> l(g_gpuMutex); g_signaled = 0u; }
}

static void gpuHung() {
  SubmissionTracker::s_sliceMs = 20u;
  SubmissionTracker::s_terminate = observeTerminate;
  SubmissionTracker::s_park = testPark;

  { // 1. a submitted serial never signalled: one report, then termination; later waits park
    resetHang();
    SubmissionTracker t(0x1u);
    uint64_t s1 = t.onSubmit(0x2u);
    gpuComplete(s1);
    uint64_t s2 = t.onSubmit(0x2u);
    uint64_t s3 = t.onSubmit(0x2u);
    g_t0 = std::chrono::steady_clock::now();
    t.waitFor(s2);
    CHECK(msSince(g_t0) >= 90);   // 5 slices of 20 ms on the GPU
    CHECK(g_terminated.load() == 1);
    CHECK(g_exitCode.load() == SubmissionTracker::kGpuHungExitCode);
    CHECK(g_parked.load() == 1);  // termination doesn't return in production: the reporter parks
    CHECK(t.gpuHung());
    t.waitFor(s3);                // a later wait on an incomplete serial parks at once
    CHECK(g_parked.load() == 2);
    t.waitFor(s1);                // a complete one returns
    CHECK(g_parked.load() == 2);
    CHECK(g_terminated.load() == 1);
  }
  CHECK(g_parked.load() == 3);    // the destructor's drain parks too
  CHECK(g_terminated.load() == 1);

  { // 2. the GPU completes the serial exactly at the end of the wait (hung()'s final check):
    //    not a hang
    resetHang();
    SubmissionTracker t(0x1u);
    uint64_t s = t.onSubmit(0x2u);
    g_boundaryAt = 5u;
    g_boundaryValue = s;
    t.waitFor(s);
    CHECK(g_completeOnRead.load() != 0u);   // the boundary was reached
    CHECK(t.completedSerialCached() >= s || t.isComplete(s));
    CHECK(g_terminated.load() == 0);
    CHECK(g_parked.load() == 0);
    CHECK(!t.gpuHung());
  }

  { // 3. submitted late in the wait: the GPU gets its whole budget from the submission on
    resetHang();
    SubmissionTracker t(0x1u);
    const uint64_t s = t.nextSerial();
    g_t0 = std::chrono::steady_clock::now();
    std::thread cs([&t] { std::this_thread::sleep_for(std::chrono::milliseconds(90)); t.onSubmit(0x2u); });
    t.waitFor(s);
    cs.join();
    CHECK(g_terminated.load() == 1);
    CHECK(g_terminatedAtMs.load() >= 90 + 5 * 20 - 10);   // not right after the submission
    CHECK(g_parked.load() == 1);
  }

  { // 3b. submitted late and completed soon after: no hang
    resetHang();
    SubmissionTracker t(0x1u);
    const uint64_t s = t.nextSerial();
    std::thread cs([&t, s] {
      std::this_thread::sleep_for(std::chrono::milliseconds(90));
      t.onSubmit(0x2u);
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
      gpuComplete(s);
    });
    t.waitFor(s);
    cs.join();
    CHECK(t.isComplete(s));
    CHECK(g_terminated.load() == 0);
    CHECK(g_parked.load() == 0);
  }

  { // 4. a serial nobody submits: given up after 5 slices, as before; no hang, no park
    resetHang();
    SubmissionTracker t(0x1u);
    t.waitFor(t.nextSerial());
    CHECK(g_terminated.load() == 0);
    CHECK(g_parked.load() == 0);
    CHECK(!t.gpuHung());
  }

  { // 5. D9MT_GPU_HUNG=continue: the wait is given up, the game renders on (as before)
    resetHang();
    SetEnvironmentVariableA("D9MT_GPU_HUNG", "continue");
    SubmissionTracker t(0x1u);
    uint64_t s = t.onSubmit(0x2u);
    t.waitFor(s);
    CHECK(g_terminated.load() == 0);
    CHECK(g_parked.load() == 0);
    CHECK(!t.gpuHung());
    gpuComplete(s);
    SetEnvironmentVariableA("D9MT_GPU_HUNG", nullptr);
  }

  { // 5b. D9MT_GPU_HUNG=continue keeps the old budget: 5 slices in all, also when the serial is
    //     submitted late in the wait (no longer submitted-only slices)
    resetHang();
    SetEnvironmentVariableA("D9MT_GPU_HUNG", "continue");
    SubmissionTracker t(0x1u);
    const uint64_t s = t.nextSerial();
    std::thread cs([&t] { std::this_thread::sleep_for(std::chrono::milliseconds(90)); t.onSubmit(0x2u); });
    const uint64_t calls0 = g_waitCalls.load();
    t.waitFor(s);
    cs.join();
    CHECK(g_waitCalls.load() - calls0 == 5u);   // the fail-fast budget would wait more slices
    CHECK(!t.isComplete(s));
    CHECK(g_terminated.load() == 0);
    CHECK(g_parked.load() == 0);
    gpuComplete(s);
    SetEnvironmentVariableA("D9MT_GPU_HUNG", nullptr);
  }

  { // 5c. the policy is the tracker's, from when it was made: D9MT_GPU_HUNG=continue set later
    //     doesn't stop a hang from ending the game
    resetHang();
    SubmissionTracker t(0x1u);
    SetEnvironmentVariableA("D9MT_GPU_HUNG", "continue");
    uint64_t s = t.onSubmit(0x2u);
    t.waitFor(s);
    CHECK(g_terminated.load() == 1);
    CHECK(g_parked.load() == 1);
    CHECK(t.gpuHung());
    SetEnvironmentVariableA("D9MT_GPU_HUNG", nullptr);
    gpuComplete(s);
  }

  { // 5d. ... and a tracker made with D9MT_GPU_HUNG=continue keeps rendering on after it's unset
    resetHang();
    SetEnvironmentVariableA("D9MT_GPU_HUNG", "continue");
    SubmissionTracker t(0x1u);
    SetEnvironmentVariableA("D9MT_GPU_HUNG", nullptr);
    uint64_t s = t.onSubmit(0x2u);
    t.waitFor(s);
    CHECK(g_terminated.load() == 0);
    CHECK(g_parked.load() == 0);
    CHECK(!t.gpuHung());
    gpuComplete(s);
  }

  { // 6. concurrent: while the reporter is past termination (parked), another thread's wait and
    //    the device's drain must not return to their callers: they park, production's way
    resetHang();
    { std::lock_guard<std::mutex> l(g_parkMutex); g_parkRelease = false; }
    auto* t = new SubmissionTracker(0x1u);
    uint64_t s1 = t->onSubmit(0x2u);
    uint64_t s2 = t->onSubmit(0x2u);
    std::atomic<int> returned = { 0 };
    std::thread reporter([t, s1, &returned] { t->waitFor(s1); returned++; });
    for (int i = 0; i < 400 && g_terminated.load() == 0; i++)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(g_terminated.load() == 1);
    std::thread waiter([t, s2, &returned] { t->waitFor(s2); returned++; });
    std::thread drain([t, &returned] { t->drainAndStopPump(); returned++; });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(returned.load() == 0);          // nobody went back to a caller that would reuse memory
    CHECK(g_parked.load() == 3);          // reporter, waiter, drain
    CHECK(g_terminated.load() == 1);      // one report
    { std::lock_guard<std::mutex> l(g_parkMutex); g_parkRelease = true; }
    g_parkCond.notify_all();
    reporter.join(); waiter.join(); drain.join();
    CHECK(returned.load() == 3);
    delete t;
  }

  SubmissionTracker::s_sliceMs = 2000u;
  SubmissionTracker::s_terminate = nullptr;
  SubmissionTracker::s_park = nullptr;
}

// With DXVK_LOG_LEVEL=none the logger writes nothing; the D9MT-GPU-HUNG line must still reach
// stderr. Run as its own process: submission_tracker_test.exe --log-none (env DXVK_LOG_LEVEL=none).
// (Windows only: it redirects the stderr handle; a native ThreadSanitizer build leaves it out.)
#ifdef _WIN32
static int logNone() {
  if (dxvk::Logger::logLevel() != dxvk::LogLevel::None) {
    std::printf("FAIL --log-none needs DXVK_LOG_LEVEL=none\n");
    return 1;
  }
  char path[MAX_PATH];
  GetTempPathA(MAX_PATH, path);
  std::strcat(path, "stt-hung-stderr.txt");
  HANDLE h = CreateFileA(path, GENERIC_WRITE | GENERIC_READ, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0, nullptr);
  HANDLE old = GetStdHandle(STD_ERROR_HANDLE);
  SetStdHandle(STD_ERROR_HANDLE, h);
  resetHang();
  SubmissionTracker::s_sliceMs = 20u;
  SubmissionTracker::s_terminate = observeTerminate;
  SubmissionTracker::s_park = testPark;
  {
    SubmissionTracker t(0x1u);
    t.waitFor(t.onSubmit(0x2u));
  }
  SetStdHandle(STD_ERROR_HANDLE, old);
  CloseHandle(h);
  std::string text;
  if (FILE* f = std::fopen(path, "rb")) {
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
      text.append(buf, n);
    std::fclose(f);
  }
  DeleteFileA(path);
  CHECK(g_terminated.load() == 1);
  CHECK(text.find("D9MT-GPU-HUNG: serial") != std::string::npos);
  std::printf("%s submission_tracker_test --log-none (%d failures)\n", g_fail ? "FAIL" : "PASS", g_fail.load());
  return g_fail ? 1 : 0;
}
#endif

int main(int argc, char** argv) {
#ifdef _WIN32
  if (argc > 1 && std::strcmp(argv[1], "--log-none") == 0)
    return logNone();
#endif
  basics();
  unsubmittedWait();
  signalsWithoutCs();
  manyDue();
  drain();
  stress();
  gpuHung();
  std::printf("%s submission_tracker_test (%d failures)\n", g_fail ? "FAIL" : "PASS", g_fail.load());
  return g_fail ? 1 : 0;
}
