// Native unit test of SubmissionTracker::abandon (#63; driver/backend/mtl_submission.cpp, compiled
// as is against stubbed bridge and Win32 calls). A submit dropped at process detach took no serial,
// but a prefix of it may be on the GPU, uncovered by any signal. After abandon():
//  - waits are unchanged: waitFor still blocks until a submitted serial is signalled (a caller
//    takes its return as "the GPU is done"), and the drain still waits for every submitted serial;
//  - what is retired after a serial past lastSubmittedSerial() is leaked, never run or destroyed:
//    by retireAfter and retireBatch, before and after the drain, and by the drain itself;
//  - what is retired at or below it still runs, in the drain and after it.
// Before abandon(), the same tracker runs a due retirement (the control). No D9MT-GPU-HUNG.
//   scripts/test-residency.sh (DXVK's native headers; plain and ASan/UBSan)
#include "mtl_submission.h"

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <new>
#include <string>
#include <thread>

#include "../util/log/log.h"

using namespace dxvk;
using namespace dxvk::backend;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

// ---- the bridge, Win32 and the logger ------------------------------------------------------

static constexpr obj_handle_t kEvent = 0x5eu;
static std::atomic<uint64_t> g_signaled = { 0u };
static std::atomic<uint32_t> g_eventReleases = { 0u }, g_terminations = { 0u }, g_batchRuns = { 0u };

extern "C" obj_handle_t MTLDevice_newSharedEvent(obj_handle_t) { return kEvent; }
extern "C" uint64_t MTLSharedEvent_signaledValue(obj_handle_t) { return g_signaled.load(); }
// As Metal's: returns once the value is signalled, or after the whole timeout.
extern "C" bool MTLSharedEvent_waitUntilSignaledValue(obj_handle_t, uint64_t value, uint64_t timeoutMs) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (g_signaled.load() < value && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  return g_signaled.load() >= value;
}
extern "C" void MTL4CommandQueue_signalEvent(obj_handle_t, obj_handle_t, uint64_t) { }
extern "C" void NSObject_release(obj_handle_t h) { if (h == kEvent) g_eventReleases++; }

extern "C" int VirtualFree(void*, size_t, unsigned) { return 1; }
extern "C" void* GetStdHandle(unsigned) { return nullptr; }
extern "C" int WriteFile(void*, const void*, unsigned, unsigned*, void*) { return 1; }
extern "C" int FlushFileBuffers(void*) { return 1; }
extern "C" int TerminateProcess(void*, unsigned) { g_terminations++; return 1; }
extern "C" void* GetCurrentProcess() { return nullptr; }
extern "C" void Sleep(unsigned) { }

namespace dxvk {
  Logger::Logger(const std::string&) : m_minLevel(LogLevel::Info) { }
  Logger::~Logger() { }
  Logger Logger::s_instance("");
  void Logger::trace(const std::string&) { }
  void Logger::debug(const std::string&) { }
  void Logger::info (const std::string& m) { std::printf("info: %s\n", m.c_str()); }
  void Logger::warn (const std::string& m) { std::printf("warn: %s\n", m.c_str()); }
  void Logger::err  (const std::string& m) { std::printf("err: %s\n", m.c_str()); }
  void Logger::log  (LogLevel, const std::string& m) { std::printf("log: %s\n", m.c_str()); }
  namespace env {
    std::string getEnvVar(const char*) { return std::string(); }
    void setThreadName(const std::string&) { }
  }
}

namespace dxvk::backend {
  void RetireBatch::run() { g_batchRuns++; objects.clear(); }
}

// A retirement's captured state: counts runs, and destructions (a destroyed closure releases
// what it captured, as WmtCommandBuffer's does).
struct Probe {
  std::atomic<uint32_t>* destroyed;
  ~Probe() { (*destroyed)++; }
};

struct Retired {
  std::atomic<uint32_t> runs = { 0u }, destroyed = { 0u };
  std::function<void()> closure() {
    auto p = std::make_shared<Probe>(Probe { &destroyed });
    return [p, this] { runs++; };
  }
  bool ran() const { return runs.load() == 1u; }
  bool leaked() const { return runs.load() == 0u && destroyed.load() == 0u; }
};

// Signals `serial` after `ms`, as the GPU would.
static std::thread signalLater(uint64_t serial, uint32_t ms) {
  return std::thread([serial, ms] {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    g_signaled = serial;
  });
}

static long long msSince(std::chrono::steady_clock::time_point t0) {
  return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
}

static RetireBatch* batchWith(std::unique_ptr<RetireBatch>& b, obj_handle_t object) {
  b = std::make_unique<RetireBatch>();
  b->objects.push_back(object);
  return b.get();
}

int main() {
  alarm(60u);                            // never outlive a runner deadline, whatever blocks
  SubmissionTracker::s_sliceMs = 200u;   // a GPU-hang verdict would take 1 s
  SubmissionTracker::s_terminate = [] (unsigned) { g_terminations++; };
  SubmissionTracker::s_park = [] { };    // a hang verdict is counted above and the wait returns

  auto* t = new SubmissionTracker(0xde5u);

  // Control: before abandon(), a due retirement runs.
  Retired control;
  t->retireAfter(0u, control.closure());
  CHECK(control.ran());

  // Serial 1 is submitted; a retirement and a batch wait for it.
  const uint64_t s1 = t->onSubmit(0x9u);
  Retired a;
  t->retireAfter(s1, a.closure());
  std::unique_ptr<RetireBatch> b;
  batchWith(b, 0xb1u);
  b = t->retireBatch(s1, std::move(b));

  // The open serial's submit is dropped: abandon. What is retired after it is leaked at once.
  t->abandon();
  CHECK(t->abandoned());
  const uint64_t open = t->nextSerial();
  Retired prefix;
  t->retireAfter(open, prefix.closure());
  batchWith(b, 0xb2u);
  b = t->retireBatch(open, std::move(b));
  CHECK(prefix.leaked());

  // waitFor still waits for a submitted serial.
  { std::thread gpu = signalLater(s1, 150u);
    const auto t0 = std::chrono::steady_clock::now();
    t->waitFor(s1);
    const long long ms = msSince(t0);
    gpu.join();
    if (ms < 100) { g_fail++; std::printf("FAIL waitFor after abandon returned after %lld ms, before the GPU signalled\n", ms); }
    CHECK(t->isComplete(s1));
  }

  // Serial 2 is submitted too (an earlier, complete submit of the same frame, say).
  const uint64_t s2 = t->onSubmit(0x9u);
  Retired c;
  t->retireAfter(s2, c.closure());

  // The drain waits for serial 2, runs what is at or below it, leaks what is past it.
  { std::thread gpu = signalLater(s2, 150u);
    const auto t0 = std::chrono::steady_clock::now();
    t->drainAndStopPump();
    const long long ms = msSince(t0);
    gpu.join();
    if (ms < 100) { g_fail++; std::printf("FAIL the drain after abandon returned after %lld ms, before the GPU signalled\n", ms); }
  }
  CHECK(a.ran() && c.ran());
  if (!prefix.leaked()) { g_fail++; std::printf("FAIL the drain ran or destroyed a retirement past the last submitted serial\n"); }
  CHECK(g_batchRuns.load() == 1u);          // serial 1's batch ran, the open serial's didn't

  // After the drain: past the last submitted serial, leaked; at or below it, released at once.
  Retired late, done;
  t->retireAfter(open, late.closure());
  t->retireAfter(s2, done.closure());
  if (!late.leaked()) { g_fail++; std::printf("FAIL retireAfter after the drain ran a retirement past the last submitted serial\n"); }
  CHECK(done.ran());
  batchWith(b, 0xb3u);
  b = t->retireBatch(open, std::move(b));
  CHECK(g_batchRuns.load() == 1u);

  CHECK(g_terminations.load() == 0u);
  delete t;

  std::printf("%s\n", g_fail ? "FAIL" : "ok");
  std::fflush(stdout);
  return g_fail ? 1 : 0;
}
