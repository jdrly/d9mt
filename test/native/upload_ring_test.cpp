// Native unit test for UploadRing chunk retirement (driver/backend/mtl_ring.cpp,
// compiled as is against stubbed bridge/residency calls):
//  - a chunk is never reused or freed before the last submission that
//    allocated from it has completed, also when onComplete() runs while the
//    command buffer being recorded still uses it (endFrame, 2275b84);
//  - it retires as soon as that submission completes, so the ring stays
//    bounded under steady streaming with submissions in flight (onSubmit used
//    to re-stamp every live chunk: ringMiB=3004 in a long session).
//   clang++ -std=c++17 -I test/native/stubs -I driver/backend test/native/upload_ring_test.cpp driver/backend/mtl_ring.cpp -o /tmp/urt && /tmp/urt
#include "mtl_ring.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace dxvk::backend;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

// ---- model state shared with the stubs ------------------------------------

static obj_handle_t g_nextHandle = 0x1000u;
static std::unordered_map<obj_handle_t, uint64_t> g_lastUse;  // serial of the last command buffer that allocated from it
static std::unordered_set<obj_handle_t> g_alive;
static uint64_t g_completed = 0u;
static bool     g_teardown = false;

extern "C" obj_handle_t MTLDevice_newBuffer(obj_handle_t, WMTBufferInfo* info) {
  const obj_handle_t h = g_nextHandle++;
  info->gpu_address = uint64_t(uintptr_t(info->memory.get()));
  g_alive.insert(h);
  return h;
}

extern "C" void NSObject_retain(obj_handle_t) { }

extern "C" void NSObject_release(obj_handle_t h) {
  // Chunk buffers go through MtlResidencyTracker::allocDestroyed.
  CHECK(h == 0u);
}

namespace dxvk::backend {

  MtlResidencyTracker::MtlResidencyTracker(obj_handle_t, obj_handle_t, bool) { }
  MtlResidencyTracker::~MtlResidencyTracker() { }
  void MtlResidencyTracker::allocCreated(obj_handle_t, uint64_t) { }

  void MtlResidencyTracker::allocDestroyed(obj_handle_t alloc, void* hostMem, uint64_t, uint64_t, bool) {
    CHECK(g_alive.erase(alloc) == 1u);
    if (!g_teardown) {
      auto it = g_lastUse.find(alloc);
      CHECK(it == g_lastUse.end() || it->second <= g_completed);  // no GPU read of freed memory
    }
    std::free(hostMem);
  }

}

// ---- harness: one CS thread recording command buffers, in-order GPU -------

struct Harness {

  struct Written { const uint8_t* ptr; uint64_t size; uint8_t tag; };

  MtlResidencyTracker residency{0u, 0u};
  UploadRing ring;
  uint64_t submitted = 0u;  // serial of the last submission
  uint64_t gpu       = 0u;  // serial the GPU has completed (in order)
  obj_handle_t front = 0u;
  std::deque<std::vector<Written>> inFlight;  // index 0 = submission gpu + 1
  std::vector<Written> recording;

  Harness(uint64_t chunkSize, uint32_t keepIdle = 0u, bool legacy = false)
  : ring((reset(), 1u), chunkSize, &residency, keepIdle, legacy) {
    front = g_nextHandle - 1u;  // the constructor's first chunk
  }

  ~Harness() { g_teardown = true; }

  static void reset() {
    g_lastUse.clear();
    g_alive.clear();
    g_completed = 0u;
    g_teardown = false;
  }

  uint64_t recordingSerial() const { return submitted + 1u; }

  RingSlice alloc(uint64_t size, uint64_t align = 256u) {
    RingSlice s = ring.allocate(size, align);
    CHECK(s.cpuPtr != nullptr && g_alive.count(s.buffer) == 1u);
    CHECK(s.offset % align == 0u);
    if (s.buffer != front) {
      // A chunk entering the bump slot, fresh or reused: nothing in flight may still read it.
      auto it = g_lastUse.find(s.buffer);
      CHECK(it == g_lastUse.end() || it->second <= g_completed);
      CHECK(s.offset == 0u);
      front = s.buffer;
    }
    g_lastUse[s.buffer] = recordingSerial();
    const uint8_t tag = uint8_t(recordingSerial() * 31u + recording.size());
    std::memset(s.cpuPtr, tag, size);
    recording.push_back({ static_cast<const uint8_t*>(s.cpuPtr), size, tag });
    return s;
  }

  void submit() {
    ring.onSubmit(++submitted);
    inFlight.push_back(std::move(recording));
    recording.clear();
  }

  // The GPU finishes submissions up to 'serial': their uploads must be intact.
  void gpuComplete(uint64_t serial) {
    CHECK(serial <= submitted);
    while (gpu < serial) {
      for (const Written& w : inFlight.front()) {
        bool intact = true;
        for (uint64_t i = 0u; i < w.size && intact; i++)
          intact = w.ptr[i] == w.tag;
        CHECK(intact);
      }
      inFlight.pop_front();
      gpu++;
    }
  }

  // MtlContextImpl calls onComplete(completedSerial) after each submit and in endFrame().
  void complete() {
    g_completed = gpu;
    ring.onComplete(gpu);
    checkExact();
  }

  void endFrame() {
    complete();
    ring.onFrame();
  }

  // Retired exactly when the last allocating submission completed: every
  // live chunk except the bump target is still used by a pending one.
  void checkExact() {
    size_t expect = 1u;
    for (obj_handle_t h : g_alive) {
      auto it = g_lastUse.find(h);
      if (h != front && it != g_lastUse.end() && it->second > g_completed)
        expect++;
    }
    CHECK(ring.liveChunks() == expect);
  }

};

static constexpr uint64_t Chunk = 64u << 10;

// 2275b84: a chunk the recording command buffer used and then rotated out of
// must survive an onComplete() that runs before that command buffer is submitted.
static void earlyRetireWhileRecording() {
  Harness h(Chunk);
  const obj_handle_t a = h.front;
  h.alloc(60u << 10);
  h.submit();
  h.gpuComplete(1u);
  h.complete();
  CHECK(h.ring.liveChunks() == 1u);

  h.alloc(1u << 10);            // CB 2 uses A ...
  h.alloc(60u << 10);           // ... and rotates it out (B)
  CHECK(h.front != a);
  h.complete();                 // endFrame() before CB 2 is submitted
  CHECK(h.ring.liveChunks() == 2u);
  h.alloc(60u << 10);           // rotates B out: must not get A back
  CHECK(h.front != a);
  CHECK(h.ring.liveChunks() == 3u);
  h.submit();
  h.complete();                 // GPU still at 1
  CHECK(h.ring.liveChunks() == 3u);
  h.gpuComplete(2u);
  h.complete();
  CHECK(h.ring.liveChunks() == 1u && h.ring.freeChunks() == 2u);
}

// A full chunk rotated out by a command buffer that didn't allocate from it
// retires once its own last submission completes, not one submission later.
static void exactRetireOfUnusedRotation() {
  Harness h(Chunk);
  const obj_handle_t a = h.front;
  h.alloc(Chunk);               // CB 1 fills A
  h.submit();
  h.alloc(1u << 10);            // CB 2 rotates A out without using it
  CHECK(h.front != a);
  h.complete();                 // GPU at 0: A still read by CB 1
  CHECK(h.ring.liveChunks() == 2u);
  h.gpuComplete(1u);
  h.complete();                 // CB 2 still recording, A is done
  CHECK(h.ring.liveChunks() == 1u && h.ring.freeChunks() == 1u);
  h.submit();
}

// The front chunk is not retired, whatever its stamp; a front chunk that a
// command buffer didn't touch keeps the stamp of the last one that did.
static void frontStaysLive() {
  Harness h(Chunk);
  const obj_handle_t a = h.front;
  h.alloc(1u << 10);
  h.submit();
  h.submit();                   // empty command buffers
  h.submit();
  h.gpuComplete(3u);
  h.complete();
  CHECK(h.ring.liveChunks() == 1u);
  h.alloc(1u << 10);
  CHECK(h.front == a);
  h.submit();
}

// Steady streaming: ~13 submits per frame, two in flight behind the CS
// thread, every freed chunk kept (ultra). The ring must stay at its working set.
static void steadyStreamingBounded() {
  constexpr uint64_t Lag = 2u, PerFrame = 13u, AllocsPerCb = 40u, AllocSize = 4u << 10;
  Harness h(Chunk);
  uint64_t peak = 0u;
  for (uint32_t frame = 0u; frame < 300u; frame++) {
    for (uint64_t cb = 0u; cb < PerFrame; cb++) {
      for (uint64_t i = 0u; i < AllocsPerCb; i++)
        h.alloc(AllocSize);
      h.submit();
      h.gpuComplete(h.submitted > Lag ? h.submitted - Lag : 0u);
      h.complete();
    }
    h.endFrame();
    peak = std::max(peak, h.ring.allocatedBytes());
    // Lag + 1 command buffers in flight plus a partly used chunk each.
    const uint64_t bound = (Lag + 2u) * (AllocsPerCb * AllocSize + Chunk);
    if (h.ring.allocatedBytes() > bound) {
      CHECK(h.ring.allocatedBytes() <= bound);
      std::printf("  ring grew to %llu KiB by frame %u (bound %llu KiB)\n",
        (unsigned long long)(h.ring.allocatedBytes() >> 10), frame,
        (unsigned long long)(bound >> 10));
      return;
    }
  }
  std::printf("  steady streaming: peak ring %llu KiB\n", (unsigned long long)(peak >> 10));
}

// Random allocation sizes (some oversized), flushes, endFrame() calls while
// recording, GPU progress and waits.
static void simulate(uint32_t seed, uint32_t keepIdle, bool legacy) {
  std::mt19937 rng(seed);
  Harness h(Chunk, keepIdle, legacy);
  uint64_t peak = 0u;
  for (int step = 0; step < 100000; step++) {
    const uint32_t action = rng() % 100u;
    if (action < 80u) {
      const uint64_t size = 1u + (rng() % 50u ? rng() % (16u << 10) : rng() % (3u * Chunk));
      h.alloc(size, (rng() & 1u) ? 256u : 16u);
    } else if (action < 90u) {
      h.submit();
      if (rng() % 16u == 0u)
        h.gpuComplete(h.submitted);            // submit(waitCompleted = true)
      h.complete();
    } else if (action < 97u) {
      if (h.gpu < h.submitted)
        h.gpuComplete(h.gpu + 1u + rng() % (h.submitted - h.gpu));
    } else {
      h.endFrame();
    }
    peak = std::max(peak, h.ring.allocatedBytes());
  }
  h.submit();
  h.gpuComplete(h.submitted);
  h.complete();
  CHECK(h.ring.liveChunks() == 1u);
  std::printf("  seed %u keep %u%s: %llu submits, peak ring %llu KiB, %zu free chunks\n",
    seed, keepIdle, legacy ? " legacy" : "", (unsigned long long)h.submitted,
    (unsigned long long)(peak >> 10), h.ring.freeChunks());
}

int main() {
  earlyRetireWhileRecording();
  exactRetireOfUnusedRotation();
  frontStaysLive();
  steadyStreamingBounded();
  for (uint32_t seed : { 1u, 2u, 3u, 42u }) {
    simulate(seed, 0u, false);
    simulate(seed, 2u, false);
    simulate(seed, 0u, true);
  }
  std::printf("%s (%d failures)\n", g_fail ? "FAILED" : "OK", g_fail);
  return g_fail ? 1 : 0;
}
