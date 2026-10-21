#include "mtl_lifetime.h"

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <unordered_map>

#include "mtl_submission.h"

#include "../util/log/log.h"
#include "../util/util_env.h"
#include "../util/util_string.h"

namespace dxvk::backend::lifetime {

  namespace {

    struct Stamp {
      uint64_t serial   = 0u;
      uint32_t refs     = 0u;  // d9mt's references (noteAcquire), 0 = not counted
      Use      use      = Use::Sampled;
      bool     drawable = false;
    };

    std::mutex                               g_mutex;
    std::unordered_map<obj_handle_t, Stamp>  g_stamps;
    SubmissionTracker*                       g_tracker = nullptr;
    uint64_t                                 g_reported = 0u;
    uint64_t                                 g_drawables = 0u;
    bool                                     g_serialWarned = false;
    bool                                     g_teardown = false;

    const char* useName(Use use) {
      switch (use) {
        case Use::Sampled:    return "sampled";
        case Use::Attachment: return "attachment";
        case Use::CopySrc:    return "copy source";
        case Use::CopyDst:    return "copy destination";
      }
      return "?";
    }

    // the first 100 lines, then one every 1000 (with the count)
    bool shouldLog(uint64_t n) {
      return n <= 100u || n % 1000u == 0u;
    }

  }

  static int readMode() {
    // (at load: no logging here, the logger may not be up yet; attach says it)
    const std::string v = env::getEnvVar("D9MT_LIFETIME_CHECK");
    if (v.empty() || v == "0")
      return 0;
    return v == "log" ? 2 : 1;
  }

  extern const int g_mode = readMode();

  void attach(SubmissionTracker* tracker) {
    if (!enabled())
      return;
    Logger::warn(str::format("D9MT-LIFETIME: check on (", mode() == 1 ? "abort at the first" : "log only",
      "): a texture released before the GPU is past its last command buffer is reported"));
    std::lock_guard<std::mutex> lock(g_mutex);
    g_tracker = tracker;
    g_teardown = false;
  }

  void beginTeardown() {
    if (!enabled())
      return;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_teardown = true;
  }

  void detach(SubmissionTracker* tracker) {
    if (!enabled())
      return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_tracker == tracker) {
      g_tracker = nullptr;
      // the device drained the GPU: nothing stamped can still be in use
      g_stamps.clear();
    }
  }

  void noteAcquireSlow(obj_handle_t texture) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_stamps[texture].refs++;
  }

  void noteUseSlow(obj_handle_t texture, uint64_t serial, Use use) {
    std::lock_guard<std::mutex> lock(g_mutex);
    Stamp& s = g_stamps[texture];
    if (serial >= s.serial) {
      s.serial = serial;
      s.use    = use;
    }
  }

  void markDrawableSlow(obj_handle_t texture) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_stamps[texture].drawable = true;
  }

  void checkReleaseSlow(obj_handle_t texture, const char* what) {
    Stamp s;
    uint64_t completed = 0u, recording = 0u, n = 0u;
    bool unsubmitted = false;
    { std::lock_guard<std::mutex> lock(g_mutex);
      auto it = g_stamps.find(texture);
      if (it == g_stamps.end())
        return;
      // not d9mt's last reference: another wrapper still holds it
      if (it->second.refs > 1u) {
        it->second.refs--;
        return;
      }
      s = it->second;
      // A later object can get the same handle: it starts unstamped.
      g_stamps.erase(it);
      if (!g_tracker || !s.serial)
        return;
      completed = g_tracker->completedSerial();
      if (s.serial <= completed)
        return;
      recording = g_tracker->nextSerial();
      unsubmitted = g_teardown && s.serial >= recording;
      n = s.drawable ? ++g_drawables : ++g_reported;
    }

    const std::string where = s.serial >= recording
      ? str::format("the open serial ", s.serial, " (being recorded, or committed early by G1)")
      : str::format("committed serial ", s.serial, ", not complete");
    const std::string line = str::format("D9MT-LIFETIME", s.drawable ? " drawable" : "", ": ", what,
      " 0x", std::hex, texture, std::dec, " released while ", where, " uses it as ", useName(s.use),
      " (GPU completed ", completed, ", recording ", recording, ")",
      unsubmitted ? "; at context teardown, never submitted" : "",
      s.drawable ? "; Core Animation still holds the drawable" : "", n > 1u ? str::format(" [", n, "]") : "");

    if (s.drawable || mode() == 2 || unsubmitted) {
      if (shouldLog(n))
        Logger::err(line);
      return;
    }
    Logger::err(line);
    Logger::err("D9MT-LIFETIME: aborting (D9MT_LIFETIME_CHECK=1; =log reports and goes on)");
    std::fflush(stderr);
    std::abort();
  }

  void checkSubmitSerial(uint64_t recorded, uint64_t submitted) {
    if (!enabled() || recorded == submitted)
      return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_serialWarned)
      return;
    g_serialWarned = true;
    Logger::err(str::format("D9MT-LIFETIME: a command buffer recorded as serial ", recorded,
      " was submitted as ", submitted, ": stamps can be too early, the check may miss some"));
  }

}
