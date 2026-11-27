// Unit test for D3D9DeviceEx::WaitForResource's contract, under AddressSanitizer and
// UndefinedBehaviorSanitizer. It compiles the driver's own function: scripts/test-wait-resource.sh
// cuts it out of d3d9_device.cpp (test/native/wait_resource_extract.py) into wait_resource.inc,
// included below, and runs it against a fake device.
//
// The fake's isResourceInUse answers from a script of readings (busy or idle; the last repeats),
// so every order in which the GPU can retire the resource between two readings is tried. The
// contract, from the D3D9 side: a call that returns true hands the caller the mapping, so
//  - the CS thread has run everything up to SequenceNumber (a draw issued before the Lock but
//    still queued on the CS thread hasn't stamped the resource yet, so no reading shows it);
//  - after that sync, the GPU was found idle, or the call waited for it after a flush.
// A call without DONOTWAIT always returns true. A DONOTWAIT call never blocks on the GPU and
// returns false exactly when its last reading was busy; then it forces a flush when the open,
// carried submission holds the resource, and hints one otherwise (no carry, or a carry open while
// an older submission holds it).
//
//   scripts/test-wait-resource.sh [d3d9_device.cpp]
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#define likely(x)   __builtin_expect(bool(x), 1)
#define unlikely(x) __builtin_expect(bool(x), 0)

static int g_fail = 0;
static long g_cases = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

namespace dxvk {

  using DWORD = uint32_t;
  constexpr DWORD D3DLOCK_DONOTWAIT = 0x00004000L;
  using high_resolution_clock = std::chrono::high_resolution_clock;

  namespace hitch {
    inline bool g_enabled = false;
    enum Cat : uint32_t { GpuWait };
    struct Profiler { };
    struct Scope { Scope(Profiler&, Cat) { } };
  }

  namespace stats {
    enum class FlushOrigin : uint32_t { Wait };
    enum class LockWait : uint32_t { Buffer, StaticBuffer, Readback };
    struct LockWaitCounts { void noteWait(LockWait, bool, uint64_t) { } };
  }

  enum class GpuFlushType : uint32_t { ImplicitWeakHint };

  struct MtlResource {
    uint64_t trackingSerial() const { return 1u; }
  };

  // What happened, in order: R0/R1 a reading (idle/busy), S a CS sync, F a flush, H a flush hint,
  // W a GPU wait.
  struct World {
    std::vector<bool> script;
    // No carry open; a carry open that holds the resource (its serial is the open one); a carry
    // open while an older submission holds the resource.
    enum class Carry { None, Holds, Older } carry = Carry::None;
    size_t            next = 0u;
    std::string       log;
    uint64_t          syncedTo = 0u;

    bool read() {
      const bool busy = script[next < script.size() ? next : script.size() - 1u];
      next++;
      log += busy ? "R1 " : "R0 ";
      return busy;
    }
  };

  struct FakeMtlDevice {
    World* w;
    struct FrontendStats { std::atomic<uint64_t> resourceGpuWaits = { 0u }; } fs;
    bool isResourceInUse(const MtlResource&) { return w->read(); }
    bool carryOpen() const { return w->carry != World::Carry::None; }
    // the resource's serial is 1: the open one, or an older one when the open one is 2
    uint64_t getNextSerial() const { return w->carry == World::Carry::Older ? 2u : 1u; }
    void waitForResource(const MtlResource&) { w->log += "W "; }
    FrontendStats& frontendStats() { return fs; }
  };

  struct D3D9DeviceEx {
    World*                w;
    FakeMtlDevice*        m_dxvkDevice;
    bool                  m_memStats = false;
    hitch::Profiler       m_hitch;
    stats::LockWaitCounts m_lockWaits;

    bool SynchronizeCsThread(uint64_t seq) { w->log += "S "; w->syncedTo = seq; return false; }
    void ExecuteFlush(stats::FlushOrigin, bool) { w->log += "F "; }
    void ConsiderFlush(GpuFlushType, stats::FlushOrigin) { w->log += "H "; }

    bool WaitForResource(const MtlResource& Resource, uint64_t SequenceNumber, DWORD MapFlags,
                         stats::LockWait What);
  };

#include "wait_resource.inc"

}

using namespace dxvk;

static std::vector<std::string> events(const std::string& log) {
  std::vector<std::string> e;
  size_t p = 0u;
  while (p < log.size()) {
    const size_t q = log.find(' ', p);
    e.push_back(log.substr(p, q - p));
    p = q + 1u;
  }
  return e;
}

// The contract for one call; prints the case when it doesn't hold.
static void check(const std::vector<bool>& script, bool profiled, bool doNotWait, World::Carry carry) {
  g_cases++;
  World w;
  w.script = script;
  w.carry = carry;
  const bool carried = carry == World::Carry::Holds;
  FakeMtlDevice mtl;
  mtl.w = &w;
  D3D9DeviceEx dev;
  dev.w = &w;
  dev.m_dxvkDevice = &mtl;
  hitch::g_enabled = profiled;
  constexpr uint64_t Seq = 42u;
  const bool done = dev.WaitForResource(MtlResource(), Seq, doNotWait ? D3DLOCK_DONOTWAIT : 0u,
                                        stats::LockWait::Buffer);

  const std::vector<std::string> e = events(w.log);
  int lastSync = -1, firstFlush = -1;
  for (int i = 0; i < int(e.size()); i++) {
    if (e[i] == "S") lastSync = i;
    if (e[i] == "F" && firstFlush < 0) firstFlush = i;
  }
  bool ok = true;
  int lastRead = -1;
  for (int i = 0; i < int(e.size()); i++)
    if (e[i][0] == 'R') lastRead = i;
  const bool lastBusy = lastRead >= 0 && e[lastRead] == "R1";
  if (!doNotWait && !done)
    ok = false;   // only a DONOTWAIT poll may fail
  if (doNotWait) {
    if (done == lastBusy)
      ok = false;   // a poll fails exactly when the resource was busy at its last reading
    if (!done && (e.empty() || e.back() != (carried ? "F" : "H")))
      ok = false;   // a busy poll forces a flush of the carry that holds it, else hints one
  }
  for (int i = 0; i < int(e.size()); i++)
    if (e[i] == "W" && (doNotWait || firstFlush < 0 || firstFlush > i))
      ok = false;   // a DONOTWAIT call blocked, or a wait came before any flush
  if (done) {
    if (lastSync < 0 || w.syncedTo != Seq)
      ok = false;   // handed out the mapping without running the CS thread up to Seq
    else {
      // After the last sync: a GPU wait, or readings that are all idle (at least one).
      bool waited = false, sawRead = false, sawBusy = false;
      for (int i = lastSync + 1; i < int(e.size()); i++) {
        waited |= e[i] == "W";
        sawRead |= e[i][0] == 'R';
        sawBusy |= e[i] == "R1" && !waited;
      }
      if (!waited && (!sawRead || sawBusy))
        ok = false;
    }
  }
  if (!ok) {
    std::string s;
    for (bool b : script) s += b ? "busy " : "idle ";
    std::printf("FAIL readings [%s] profiled=%d donotwait=%d carry=%d: returned %s, events: %s\n",
                s.c_str(), int(profiled), int(doNotWait), int(carry), done ? "true" : "false", w.log.c_str());
    g_fail++;
  }
}

int main() {
  // Every script of 1 to 4 readings, profiled or not, with and without DONOTWAIT, and each carry
  // state.
  for (size_t n = 1u; n <= 4u; n++)
    for (uint32_t bits = 0u; bits < (1u << n); bits++) {
      std::vector<bool> script;
      for (size_t i = 0u; i < n; i++)
        script.push_back((bits >> i) & 1u);
      for (bool profiled : { false, true })
        for (bool doNotWait : { false, true })
          for (World::Carry carry : { World::Carry::None, World::Carry::Holds, World::Carry::Older })
            check(script, profiled, doNotWait, carry);
    }
  hitch::g_enabled = false;

  std::printf("%s: %ld cases, %d failures\n", g_fail ? "FAILED" : "OK", g_cases, g_fail);
  return g_fail ? 1 : 0;
}
