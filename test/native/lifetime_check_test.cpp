// Unit test for D9MT_LIFETIME_CHECK (driver/backend/mtl_lifetime.cpp, compiled as is with the
// real SubmissionTracker against stubbed bridge calls; a Windows program because
// dxvk::thread is, run it under Wine):
//  - a texture released while the command buffer that uses it is being recorded, or is
//    committed but not complete, is reported (the use, the serials);
//  - released once the GPU is past it, or never used: nothing;
//  - the stamp goes with the release: a later object with the same handle starts clean;
//  - a drawable's texture is reported as "drawable" and never aborts;
//  - with several of d9mt's references to one texture, only the last release is checked;
//  - a submit serial that differs from the recorded one is reported once.
// D9MT_LIFETIME_CHECK=log (set by the test itself: abort mode would end it at the first).
//   x86_64-w64-mingw32-g++ -std=c++17 -O1 '-DWINEMETAL_API=extern "C"' -I driver/backend \
//     test/native/lifetime_check_test.cpp driver/backend/mtl_lifetime.cpp \
//     driver/backend/mtl_submission.cpp driver/util/thread.cpp \
//     -o lifetime_check_test.exe -static && wine lifetime_check_test.exe
#include "mtl_lifetime.h"
#include "mtl_submission.h"
#include "../util/log/log.h"
#include "../util/util_env.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using namespace dxvk::backend;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

// ---- the GPU timeline: the test decides what has completed ------------------------------

static std::atomic<uint64_t> g_completed = { 0u };

extern "C" obj_handle_t MTLDevice_newSharedEvent(obj_handle_t) { return 0x5eu; }
extern "C" uint64_t MTLSharedEvent_signaledValue(obj_handle_t) { return g_completed.load(); }
extern "C" bool MTLSharedEvent_waitUntilSignaledValue(obj_handle_t, uint64_t value, uint64_t timeout) {
  for (uint64_t t = 0u; t < timeout && g_completed.load() < value; t += 5u)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  return g_completed.load() >= value;
}
extern "C" void MTL4CommandQueue_signalEvent(obj_handle_t, obj_handle_t, uint64_t) { }
extern "C" void NSObject_release(obj_handle_t) { }
extern "C" void NSObject_retain(obj_handle_t) { }

static std::vector<std::string> g_lines;

namespace dxvk {
  void Logger::info(const std::string&) { }
  void Logger::warn(const std::string&) { }
  void Logger::err(const std::string& s) { g_lines.push_back(s); }
  namespace env {
    void setThreadName(const std::string&) { }
    std::string getEnvVar(const char* name) {
      return std::string(name) == "D9MT_LIFETIME_CHECK" ? "log" : "";
    }
  }
}

static bool has(size_t i, const std::string& text) {
  return i < g_lines.size() && g_lines[i].find(text) != std::string::npos;
}

int main() {
  CHECK(lifetime::mode() == 2);

  SubmissionTracker st(0u);
  lifetime::attach(&st);

  // 1. Bug A: a view released while the command buffer being recorded samples it
  const uint64_t rec = st.nextSerial();
  lifetime::noteUse(0x1000u, rec, lifetime::Use::Sampled);
  lifetime::checkRelease(0x1000u, "texture view");
  CHECK(g_lines.size() == 1u);
  CHECK(has(0, "D9MT-LIFETIME: texture view 0x1000 released while the command buffer being recorded"));
  CHECK(has(0, "uses it as sampled"));

  // 2. committed but not complete: reported; complete: not
  const uint64_t s1 = st.onSubmit(0u);
  lifetime::noteUse(0x2000u, s1, lifetime::Use::Attachment);
  lifetime::noteUse(0x3000u, s1, lifetime::Use::CopySrc);
  lifetime::checkRelease(0x2000u, "texture");
  CHECK(g_lines.size() == 2u);
  CHECK(has(1, "committed serial " + std::to_string(s1) + ", not complete"));
  CHECK(has(1, "uses it as attachment"));
  g_completed = s1;
  lifetime::checkRelease(0x3000u, "texture");
  CHECK(g_lines.size() == 2u);

  // 3. never used, or used by an older serial than the latest: the latest counts
  lifetime::checkRelease(0x4000u, "texture");
  const uint64_t s2 = st.onSubmit(0u);
  lifetime::noteUse(0x5000u, s2, lifetime::Use::CopyDst);
  lifetime::noteUse(0x5000u, s1, lifetime::Use::Sampled);  // an older stamp doesn't lower it
  lifetime::checkRelease(0x5000u, "texture");
  CHECK(g_lines.size() == 3u);
  CHECK(has(2, "uses it as copy destination"));

  // 4. the stamp goes with the release: the same handle again, unused, is clean
  lifetime::checkRelease(0x5000u, "texture");
  CHECK(g_lines.size() == 3u);

  // 5. a drawable's texture: "drawable", Core Animation named
  lifetime::markDrawable(0x6000u);
  lifetime::noteUse(0x6000u, st.nextSerial(), lifetime::Use::CopyDst);
  lifetime::checkRelease(0x6000u, "texture");
  CHECK(g_lines.size() == 4u);
  CHECK(has(3, "D9MT-LIFETIME drawable: texture 0x6000"));
  CHECK(has(3, "Core Animation still holds the drawable"));

  // 5b. two of d9mt's references to one texture (a drawable Core Animation hands out again while
  //     an older frame's wrapper still holds it): only the last release is checked
  lifetime::noteAcquire(0x6100u);
  lifetime::noteAcquire(0x6100u);
  lifetime::noteUse(0x6100u, st.nextSerial(), lifetime::Use::CopyDst);
  lifetime::checkRelease(0x6100u, "texture");
  CHECK(g_lines.size() == 4u);
  lifetime::checkRelease(0x6100u, "texture");
  CHECK(g_lines.size() == 5u);
  CHECK(has(4, "D9MT-LIFETIME: texture 0x6100"));
  g_lines.pop_back();

  // 6. the submit serial differs from the recorded one: once
  lifetime::checkSubmitSerial(7u, 7u);
  lifetime::checkSubmitSerial(7u, 8u);
  lifetime::checkSubmitSerial(9u, 10u);
  CHECK(g_lines.size() == 5u);
  CHECK(has(4, "recorded as serial 7 was submitted as 8"));

  // 7. the device goes (GPU drained): nothing stamped is in use any more
  lifetime::noteUse(0x7000u, st.nextSerial(), lifetime::Use::Sampled);
  g_completed = st.lastSubmittedSerial();
  st.drainAndStopPump();
  lifetime::detach(&st);
  lifetime::checkRelease(0x7000u, "texture");
  CHECK(g_lines.size() == 5u);

  for (const std::string& l : g_lines)
    std::printf("  %s\n", l.c_str());
  std::printf("%s (%d failures)\n", g_fail ? "FAILED" : "OK", g_fail);
  return g_fail ? 1 : 0;
}
