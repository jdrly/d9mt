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
// The tracker uses dxvk::thread and the driver's util, so this is a mingw exe run under Wine
// (no window). RetireBatch::run is the driver's minus the buffer recycling, which needs the device.
//   x86_64-w64-mingw32-g++ -std=c++17 -O1 -DWINEMETAL_API='extern "C"' -I driver/backend \
//     test/native/submission_tracker_test.cpp driver/backend/mtl_submission.cpp \
//     build/driver-x64/obj/util_{log_log,thread,util_env,util_string}.cpp.o ... -o stt.exe
#include "mtl_submission.h"
#include "mtl_profiler.h"

#include <atomic>
#include <condition_variable>
#include <cstdio>
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
  g_reads++;
  std::lock_guard<std::mutex> l(g_gpuMutex);
  return g_signaled;
}
bool MTLSharedEvent_waitUntilSignaledValue(obj_handle_t, uint64_t value, uint64_t timeoutMs) {
  std::unique_lock<std::mutex> l(g_gpuMutex);
  return g_gpuCond.wait_for(l, std::chrono::milliseconds(timeoutMs), [&] { return g_signaled >= value; });
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

int main() {
  basics();
  unsubmittedWait();
  signalsWithoutCs();
  manyDue();
  drain();
  stress();
  std::printf("%s submission_tracker_test (%d failures)\n", g_fail ? "FAIL" : "PASS", g_fail.load());
  return g_fail ? 1 : 0;
}
