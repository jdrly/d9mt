// Native unit test of the device's teardown after a dropped submit (#63). The driver's own
// MtlDeviceImpl destructor (driver/backend/mtl_device_impl.cpp; test/native/device_teardown_extract.py
// cuts it out by its anchors) runs on a fake device that owns sampler and depth-stencil caches,
// and the test counts their releases after member teardown:
//  - normal teardown: every sampler and depth-stencil state is released (after the drain);
//  - a context dropped a submit at process detach (the submissions are abandoned): none is, since
//    a committed prefix of the dropped serial, covered by no signal, may still reference them.
// It builds against a tree without #63 too, where the abandoned case fails.
//   scripts/test-residency.sh (builds it with the extraction; also against another tree's driver/)
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <memory>
#include <unordered_map>
#include <utility>

#define unlikely(x) __builtin_expect(bool(x), 0)

static int g_fail = 0;
static uint32_t g_samplerReleases = 0u, g_dssReleases = 0u, g_drains = 0u;

namespace dxvk::backend {

  struct FakeTracker {
    bool isAbandoned = false;
    bool abandoned() const { return isAbandoned; }
    void drainAndStopPump() { g_drains++; }
  };
  namespace lifetime { static inline void detach(FakeTracker*) { } }

  struct MtlStateCache { void shutdown() { } };
  struct FakeWorkers { };

  // Rc<MtlSamplerStateImpl>: the last reference releases the sampler.
  struct FakeSampler { ~FakeSampler() { g_samplerReleases++; } };
  // WmtDepthStencil: releases a live handle.
  struct FakeDss {
    uint64_t h = 0u;
    explicit FakeDss(uint64_t handle) : h(handle) { }
    FakeDss(FakeDss&& o) noexcept : h(std::exchange(o.h, 0u)) { }
    FakeDss& operator = (FakeDss&& o) noexcept { if (h) g_dssReleases++; h = std::exchange(o.h, 0u); return *this; }
    ~FakeDss() { if (h) g_dssReleases++; }
  };

  struct MtlDeviceImpl {
    std::unique_ptr<FakeTracker> m_tracker;
    std::unordered_map<int, std::shared_ptr<FakeSampler>> m_samplerCache;
    std::unordered_map<int, FakeDss> m_dssoCache;
    std::unique_ptr<FakeWorkers> m_compileWorkers = std::make_unique<FakeWorkers>();
    MtlStateCache* stateCache() const { return nullptr; }
    ~MtlDeviceImpl();
  };

#include "device_dtor.inc"

}

using namespace dxvk::backend;

int main() {
  for (bool abandoned : { false, true }) {
    g_samplerReleases = g_dssReleases = g_drains = 0u;
    { auto dev = std::make_unique<MtlDeviceImpl>();
      dev->m_tracker = std::make_unique<FakeTracker>();
      dev->m_tracker->isAbandoned = abandoned;
      dev->m_samplerCache.emplace(1, std::make_shared<FakeSampler>());
      dev->m_samplerCache.emplace(2, std::make_shared<FakeSampler>());
      dev->m_dssoCache.emplace(1, FakeDss(0xd501u));
      dev->m_dssoCache.emplace(2, FakeDss(0xd502u));
      dev.reset();                           // the production destructor, then member teardown
    }
    const char* what = abandoned ? "detach teardown (abandoned)" : "device teardown";
    const uint32_t want = abandoned ? 0u : 2u;
    if (g_samplerReleases != want || g_dssReleases != want || g_drains != 1u) {
      g_fail++;
      std::printf("FAIL %s: %u sampler and %u depth-stencil releases (want %u each), %u drains\n",
        what, g_samplerReleases, g_dssReleases, want, g_drains);
    }
  }
  std::printf("%s\n", g_fail ? "FAIL" : "ok");
  return g_fail ? 1 : 0;
}
