// Native unit test: no command buffer reaches the queue unless the residency set's pending adds
// were committed first (#63). MtlContextImpl::commitRecorded (driver/backend/mtl_context_record.cpp)
// is compiled from the driver's own source text (test/native/commit_guard_extract.py cuts it out
// by its anchors) against a fake device whose residency tracker reports whether it committed:
//  - committed (the normal path): one queue commit of the recorded command buffers, after it;
//  - not committed (process detach, a dead thread holds the tracker's lock): no queue commit;
//  - the GPU hung: no queue commit, and no residency commit either (as before).
// The extracted method is the only place d9mt commits to its queue.
//   scripts/test-residency.sh (builds it with the extraction; also against another tree's driver/)
#include <cstdint>
#include <cstdio>
#include <initializer_list>

#include "../../bridge/winemetal.h"

#define unlikely(x) __builtin_expect(bool(x), 0)

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static uint32_t g_queueCommits = 0u, g_cmdbufsCommitted = 0u, g_residencyCalls = 0u, g_residencyCallsAtCommit = 0u;

extern "C" void MTL4CommandBuffer_endCommandBuffer(obj_handle_t) { }
extern "C" void MTLCommandEncoder_endEncoding(obj_handle_t) { }
extern "C" void NSObject_release(obj_handle_t) { }
extern "C" void MTL4CommandQueue_commit(obj_handle_t, const obj_handle_t*, uint64_t count) {
  g_queueCommits++;
  g_cmdbufsCommitted += uint32_t(count);
  g_residencyCallsAtCommit = g_residencyCalls;
}

namespace dxvk::backend {

  namespace prof { static bool g_profEnabled = false; }
  namespace rstats { enum { CmdBufs = 0 }; }

  static void gpuCaptureBeforeCommit(obj_handle_t) { }
  static void gpuCaptureAfterCommit() { }

  struct FakeResidency {
    bool committed = true;
    // The base commit's tracker returns nothing: its caller discards the result either way.
    bool beforeSubmit() { g_residencyCalls++; return committed; }
  };
  struct FakeSubmissions {
    bool hung = false;
    bool gpuHung() const { return hung; }
  };
  struct FakeDevice {
    FakeResidency   res;
    FakeSubmissions subs;
    FakeResidency&   residency()   { return res; }
    FakeSubmissions& submissions() { return subs; }
    obj_handle_t wmtDevice() const { return 0xd1u; }
    obj_handle_t wmtQueue()  const { return 0x9u; }
  };
  struct Handle {
    obj_handle_t h = 0u;
    obj_handle_t handle() const { return h; }
    explicit operator bool () const { return h != 0u; }
  };

  struct MtlContextImpl {
    FakeDevice*  m_device = nullptr;
    Handle       m_cmdbuf, m_initCmdbuf;
    obj_handle_t m_initEncoder = 0u;
    uint64_t     m_rs[1] = { };
    void emitProducerBarrier(obj_handle_t, uint64_t, uint64_t = 0u) { }
    void commitRecorded();
  };

#include "commit_recorded.inc"

}

using namespace dxvk::backend;

static void run(const char* what, bool committed, bool hung, bool withInit,
                uint32_t wantCommits, uint32_t wantResidencyCalls) {
  FakeDevice dev;
  dev.res.committed = committed;
  dev.subs.hung = hung;
  MtlContextImpl ctx;
  ctx.m_device = &dev;
  ctx.m_cmdbuf.h = 0xc1u;
  if (withInit) {
    ctx.m_initCmdbuf.h = 0xc0u;
    ctx.m_initEncoder = 0xe0u;
  }
  g_queueCommits = g_cmdbufsCommitted = g_residencyCalls = g_residencyCallsAtCommit = 0u;
  ctx.commitRecorded();

  const bool ok = g_queueCommits == wantCommits && g_residencyCalls == wantResidencyCalls
    && (!wantCommits || (g_cmdbufsCommitted == (withInit ? 2u : 1u) && g_residencyCallsAtCommit == 1u));
  if (!ok) {
    g_fail++;
    std::printf("FAIL %s: queue commits %u (want %u), residency commits %u (want %u), command buffers %u\n",
      what, g_queueCommits, wantCommits, g_residencyCalls, wantResidencyCalls, g_cmdbufsCommitted);
  }
}

int main() {
  for (bool withInit : { false, true }) {
    run("residency committed", true, false, withInit, 1u, 1u);
    run("residency commit skipped (detach)", false, false, withInit, 0u, 1u);
    run("GPU hung", true, true, withInit, 0u, 0u);
  }
  std::printf("%s\n", g_fail ? "FAIL" : "ok");
  return g_fail ? 1 : 0;
}
