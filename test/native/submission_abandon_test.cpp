// Native unit test of SubmissionTracker::abandon (#63; driver/backend/mtl_submission.cpp, compiled
// as is against stubbed bridge and Win32 calls). At process detach, after a residency commit that
// couldn't be made, the tracker must neither wait nor release:
//  - waitFor on a submitted serial the GPU never signals returns at once: no GPU-stall slices, no
//    D9MT-GPU-HUNG termination;
//  - no retirement runs or is destroyed, queued before or after abandon(), due or not (one could
//    release a prefix command buffer the GPU still uses); a retirement batch isn't run either;
//  - drainAndStopPump waits for nothing, runs nothing and leaves the shared event alive.
// Before abandon(), the same tracker runs a due retirement (the control).
//   scripts/test-residency.sh (DXVK's native headers; plain and ASan/UBSan)
#include "mtl_submission.h"

#include <atomic>
#include <chrono>
#include <cstdio>
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
extern "C" bool MTLSharedEvent_waitUntilSignaledValue(obj_handle_t, uint64_t value, uint64_t timeoutMs) {
  std::this_thread::sleep_for(std::chrono::milliseconds(timeoutMs < 10u ? timeoutMs : 10u));
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

int main() {
  SubmissionTracker::s_sliceMs = 50u;
  SubmissionTracker::s_terminate = [] (unsigned) { g_terminations++; };

  // The tracker's memory is never freed: its pump may still run (at a real detach it's dead).
  alignas(SubmissionTracker) static unsigned char storage[sizeof(SubmissionTracker)];
  SubmissionTracker* t = new (storage) SubmissionTracker(0xde5u);

  // Control: before abandon(), a due retirement runs.
  std::atomic<uint32_t> controlRuns = { 0u };
  t->retireAfter(0u, [&] { controlRuns++; });
  CHECK(controlRuns.load() == 1u);

  // Serial 1 is submitted and the GPU never signals it.
  const uint64_t serial = t->onSubmit(0x9u);
  CHECK(serial == 1u);

  std::atomic<uint32_t> runs = { 0u }, destroyed = { 0u };
  { auto p = std::make_shared<Probe>(Probe { &destroyed });
    t->retireAfter(serial, [p, &runs] { runs++; }); }
  auto batch = std::make_unique<RetireBatch>();
  batch->objects.push_back(0xb0u);
  batch = t->retireBatch(serial, std::move(batch));

  t->abandon();
  CHECK(t->abandoned());

  // No wait: a GPU-hang wait would take 5 slices (250 ms) and terminate.
  const auto t0 = std::chrono::steady_clock::now();
  t->waitFor(serial);
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
  if (ms >= 40) { g_fail++; std::printf("FAIL waitFor after abandon waited %lld ms\n", (long long)ms); }
  CHECK(g_terminations.load() == 0u);

  // Due now, but nothing runs: not by processRetirements, not by the pump.
  g_signaled = serial;
  t->processRetirements();
  t->processRetirements(serial);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  CHECK(runs.load() == 0u && destroyed.load() == 0u);
  CHECK(g_batchRuns.load() == 0u);

  // Queued after abandon(), complete already: leaked, not run.
  { auto p = std::make_shared<Probe>(Probe { &destroyed });
    t->retireAfter(0u, [p, &runs] { runs++; }); }
  batch->objects.push_back(0xb1u);
  batch = t->retireBatch(serial, std::move(batch));
  CHECK(runs.load() == 0u && destroyed.load() == 0u);
  CHECK(g_batchRuns.load() == 0u);

  // The drain: no wait, no retirement, the event left alive.
  g_signaled = 0u;
  const auto t1 = std::chrono::steady_clock::now();
  t->drainAndStopPump();
  t->drainAndStopPump();
  const auto ms1 = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t1).count();
  if (ms1 >= 40) { g_fail++; std::printf("FAIL drainAndStopPump after abandon waited %lld ms\n", (long long)ms1); }
  CHECK(runs.load() == 0u && destroyed.load() == 0u);
  CHECK(g_batchRuns.load() == 0u);
  CHECK(g_terminations.load() == 0u);
  CHECK(g_eventReleases.load() == 0u);

  std::printf("%s\n", g_fail ? "FAIL" : "ok");
  std::fflush(stdout);
  return g_fail ? 1 : 0;
}
