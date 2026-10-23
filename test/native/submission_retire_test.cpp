// Unit test for SubmissionTracker retirement (driver/backend/mtl_submission.cpp, compiled as is
// against stubbed bridge calls; a Windows program because dxvk::thread is, run it under Wine):
//  - retireAfter() of a pending serial waits for it (the pump releases once it completes);
//  - after drainAndStopPump() nothing runs the list any more: retireAfter() must release at
//    once (a texture view destroyed in device teardown would leak otherwise);
//  - a release that itself calls retireAfter() (a view dying inside a retirement) doesn't
//    deadlock, also during the drain.
//   x86_64-w64-mingw32-g++ -std=c++17 -O1 '-DWINEMETAL_API=extern "C"' -I driver/backend \
//     test/native/submission_retire_test.cpp driver/backend/mtl_submission.cpp driver/util/thread.cpp \
//     -o submission_retire_test.exe -static && wine submission_retire_test.exe
#include "mtl_submission.h"
#include "../util/log/log.h"
#include "../util/util_env.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

using namespace dxvk::backend;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

// ---- the GPU timeline: the test decides what has completed ------------------------------

static std::atomic<uint64_t> g_completed = { 0u };
static std::atomic<int>      g_released  = { 0 };

extern "C" obj_handle_t MTLDevice_newSharedEvent(obj_handle_t) { return 0x5eu; }
extern "C" uint64_t MTLSharedEvent_signaledValue(obj_handle_t) { return g_completed.load(); }
extern "C" bool MTLSharedEvent_waitUntilSignaledValue(obj_handle_t, uint64_t value, uint64_t timeout) {
  // a GPU that completes on its own after a short while (the pump's 500 ms slice and waitFor)
  for (uint64_t t = 0u; t < timeout && g_completed.load() < value; t += 5u)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  return g_completed.load() >= value;
}
extern "C" void MTL4CommandQueue_signalEvent(obj_handle_t, obj_handle_t, uint64_t) { }
extern "C" void NSObject_release(obj_handle_t) { }
extern "C" void NSObject_retain(obj_handle_t) { }

// RetireBatch::run lives in mtl_device_impl.cpp (it recycles buffer storage); this test hands
// over no batches, so the tracker's calls to it never run.
namespace dxvk::backend {
  void RetireBatch::run() { }
}

// mtl_submission.cpp asks the logger's level (a GPU hang's line falls back to stderr at "none")
dxvk::Logger dxvk::Logger::s_instance = { "" };
dxvk::Logger::Logger(const std::string&) : m_minLevel(LogLevel::Info) { }
dxvk::Logger::~Logger() { }

namespace dxvk {
  void Logger::info(const std::string&) { }
  void Logger::warn(const std::string&) { }
  void Logger::err(const std::string&) { }
  namespace env {
    void setThreadName(const std::string&) { }
    std::string getEnvVar(const char*) { return std::string(); }
  }
}

static bool waitReleased(int want, int ms) {
  for (int t = 0; t < ms && g_released.load() < want; t += 5)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  return g_released.load() >= want;
}

int main() {
  SubmissionTracker st(0u);

  // 1. pending: not released before its serial completes, released by the pump after
  const uint64_t s1 = st.onSubmit(0u);
  st.retireAfter(s1, [] () { g_released++; });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  CHECK(g_released.load() == 0);
  g_completed = s1;
  CHECK(waitReleased(1, 2000));

  // 2. a release that retires something itself (a view dying inside a retirement)
  const uint64_t s2 = st.onSubmit(0u);
  st.retireAfter(s2, [&st] () {
    g_released++;
    st.retireAfter(st.nextSerial(), [] () { g_released++; });
  });
  g_completed = s2;
  CHECK(waitReleased(2, 2000));

  // 3. still pending at teardown: the drain waits for the GPU, then releases everything
  const uint64_t s3 = st.onSubmit(0u);
  st.retireAfter(s3, [&st] () {
    g_released++;
    st.retireAfter(st.nextSerial(), [] () { g_released++; });  // during the drain
  });
  std::thread gpu([s3] () { std::this_thread::sleep_for(std::chrono::milliseconds(100)); g_completed = s3 + 1u; });
  st.drainAndStopPump();
  gpu.join();
  CHECK(g_released.load() >= 4);  // 1, 2's outer, 3, 3's inner (2's inner too, once its serial completed)

  // 4. after the drain: released at once, never left in a list nobody runs
  const int before = g_released.load();
  st.retireAfter(st.nextSerial(), [] () { g_released++; });
  CHECK(g_released.load() == before + 1);

  std::printf("%s (%d failures)\n", g_fail ? "FAILED" : "OK", g_fail);
  return g_fail ? 1 : 0;
}
