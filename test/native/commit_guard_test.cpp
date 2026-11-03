// Native unit test of the context's commit path at process detach (#63). The driver's own
// commitRecorded, abandonRecording, commitPrefix and submit (driver/backend/mtl_context_record.cpp;
// test/native/commit_guard_extract.py cuts them out by their anchors) are compiled against a fake
// device that counts what reaches the queue, the submission tracker and the canaries:
//  - normal submit: the residency commit, then one queue commit, a serial, the retirement batch,
//    the wait, the canary check;
//  - the residency commit couldn't be made (process detach, a thread ExitProcess ended holds the
//    tracker's lock): no queue commit, no serial (no signalEvent), no retirement, no wait, no
//    canary check, no retirement processing; the submissions are abandoned, one "detach: skipped
//    submit" line is logged, and the command buffers and what they keep alive are leaked;
//  - a carried prefix (commitPrefix) that can't commit: abandoned the same way, no retireAfter;
//    the destructor's completing submit(true) that follows does nothing either, logs nothing more;
//  - a normal prefix: committed, its command buffer retired after the serial;
//  - the GPU hung: no queue commit, the rest of submit as before #63;
//  - the destructor (also extracted): its allocators, whose command memory a committed prefix may
//    still run from, are released with the context normally, and leaked once the submissions were
//    abandoned (by its own completing submit(true), or before it).
// It builds against a tree without #63 too (commit_path_sig.h), where the detach cases fail.
//   scripts/test-residency.sh (builds it with the extraction; also against another tree's driver/)
#include <array>
#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "../../bridge/winemetal.h"
#include "commit_path_sig.h"

#define unlikely(x) __builtin_expect(bool(x), 0)

static int g_fail = 0;

struct Counts {
  uint32_t queueCommits = 0u, residencyCommits = 0u, onSubmit = 0u, retireBatch = 0u, retireAfter = 0u;
  uint32_t waits = 0u, processed = 0u, serialRetired = 0u, canaryChecks = 0u, canaryArms = 0u;
  uint32_t abandons = 0u, logs = 0u, cmdbufReleases = 0u, newCmdbufs = 0u, ringSubmits = 0u, allocReleases = 0u;
};
static Counts g;

static obj_handle_t g_nextCmdbuf = 0xc100u;

extern "C" void MTL4CommandBuffer_endCommandBuffer(obj_handle_t) { }
extern "C" void MTL4CommandBuffer_beginCommandBuffer(obj_handle_t, obj_handle_t) { }
extern "C" obj_handle_t MTLDevice_newMTL4CommandBuffer(obj_handle_t) { g.newCmdbufs++; return g_nextCmdbuf++; }
extern "C" void MTLCommandEncoder_endEncoding(obj_handle_t) { }
extern "C" void NSObject_release(obj_handle_t) { }
extern "C" void MTL4CommandQueue_commit(obj_handle_t, const obj_handle_t*, uint64_t) { g.queueCommits++; }

namespace dxvk {
  namespace str { template<typename... T> std::string format(const T&...) { return std::string(); } }
  // driver/util/util_fatal_log.h (#155): the commit path marks its step for the fatal-exception log
  namespace fatal { inline void breadcrumb(const char*) { } }
  struct Logger {
    static void err(const std::string&) { }
    static void warn(const std::string& m) {
      g.logs++;
      if (m.find("detach: skipped submit") == std::string::npos) {
        g_fail++;
        std::printf("FAIL unexpected log line: %s\n", m.c_str());
      }
    }
  };
}

namespace dxvk::backend {

  namespace prof {
    enum Zone { Submit };
    enum Counter { CntPassCloseSubmit };
    struct Scope { explicit Scope(Zone) { } };
    static inline void bump(Counter) { }
    static bool g_profEnabled = false;
  }
  namespace rstats { enum { CmdBufs = 0 }; }
  namespace lifetime {
    static inline void checkSubmitSerial(uint64_t, uint64_t) { }
    static inline void beginTeardown() { }
  }

  static void gpuCaptureBeforeCommit(obj_handle_t) { }
  static void gpuCaptureAfterCommit() { }

  enum : uint32_t { DirtyEverything = ~0u };

  // WmtRef<CommandBufferTag>: counts releases of a live handle.
  class WmtCommandBuffer {
  public:
    WmtCommandBuffer() = default;
    static WmtCommandBuffer adopt(obj_handle_t h) { WmtCommandBuffer r; r.m_h = h; return r; }
    WmtCommandBuffer(WmtCommandBuffer&& o) noexcept : m_h(std::exchange(o.m_h, 0u)) { }
    WmtCommandBuffer& operator = (WmtCommandBuffer&& o) noexcept { reset(); m_h = std::exchange(o.m_h, 0u); return *this; }
    ~WmtCommandBuffer() { reset(); }
    void reset() { if (m_h) { g.cmdbufReleases++; m_h = 0u; } }
    obj_handle_t detach() { return std::exchange(m_h, 0u); }
    obj_handle_t handle() const { return m_h; }
    explicit operator bool () const { return m_h != 0u; }
  private:
    obj_handle_t m_h = 0u;
  };

  // WmtRef<Mtl4AllocatorTag>: what the context owns; counts releases of a live handle.
  class WmtAllocator {
  public:
    WmtAllocator() = default;
    static WmtAllocator adopt(obj_handle_t h) { WmtAllocator r; r.m_h = h; return r; }
    WmtAllocator(WmtAllocator&& o) noexcept : m_h(std::exchange(o.m_h, 0u)) { }
    WmtAllocator& operator = (WmtAllocator&& o) noexcept { reset(); m_h = std::exchange(o.m_h, 0u); return *this; }
    ~WmtAllocator() { reset(); }
    void reset() { if (m_h) { g.allocReleases++; m_h = 0u; } }
    obj_handle_t detach() { return std::exchange(m_h, 0u); }
    obj_handle_t handle() const { return m_h; }
    explicit operator bool () const { return m_h != 0u; }
  private:
    obj_handle_t m_h = 0u;
  };

  struct WmtBuffer {
    obj_handle_t h = 0u;
    obj_handle_t detach() { return std::exchange(h, 0u); }
    explicit operator bool () const { return h != 0u; }
  };

  struct FakeResource {
    uint64_t serial = 0u;
    void setTrackingSerial(uint64_t s) { serial = s; }
  };

  struct RetireBatch {
    WmtCommandBuffer initCmdbuf, cmdbuf;
    std::vector<std::shared_ptr<FakeResource>> keepAlive;
    bool empty() const { return !initCmdbuf && !cmdbuf && keepAlive.empty(); }
  };

  struct FakeSubmissions {
    bool hung = false, isAbandoned = false;
    uint64_t next = 1u;
    std::vector<std::function<void()>> retirements;
    bool gpuHung() const { return hung; }
    bool abandoned() const { return isAbandoned; }
    void abandon() { g.abandons++; isAbandoned = true; }
    uint64_t onSubmit(obj_handle_t) { g.onSubmit++; return next++; }
    uint64_t nextSerial() const { return next; }
    std::unique_ptr<RetireBatch> retireBatch(uint64_t, std::unique_ptr<RetireBatch>&&) { g.retireBatch++; return std::make_unique<RetireBatch>(); }
    void retireAfter(uint64_t, std::function<void()> f) { g.retireAfter++; retirements.push_back(std::move(f)); }
    void waitFor(uint64_t) { g.waits++; }
    uint64_t completedSerial() { return next - 1u; }
    void processRetirements(uint64_t) { g.processed++; }
  };
  struct FakeResidency {
    bool commits = true;
    // Before #63 its caller discards the result.
    bool beforeSubmit() { g.residencyCommits++; return commits; }
    void serialRetired(uint64_t) { g.serialRetired++; }
    void allocDestroyed(obj_handle_t, void*, uint64_t, uint64_t) { }
  };
  struct FakeRing {
    void onSubmit(uint64_t) { g.ringSubmits++; }
    void onComplete(uint64_t) { }
  };
  struct FakeDevice {
    FakeSubmissions subs;
    FakeResidency   res;
    FakeRing        ring;
    FakeSubmissions& submissions() { return subs; }
    FakeResidency&   residency()   { return res; }
    FakeRing&        uploadRing()  { return ring; }
    obj_handle_t wmtDevice() const { return 0xd1u; }
    obj_handle_t wmtQueue()  const { return 0x9u; }
    void setCarryOpen(bool) { }
  };

  struct MtlContextImpl {
    struct { bool active = false; } m_pass;
    struct StageBinding { uint32_t pushExtent = 0u, pushDirtyHi = 0u; uint64_t abGpuAddress = 0u; };

    FakeDevice*      m_device = nullptr;
    WmtCommandBuffer m_cmdbuf, m_initCmdbuf;
    obj_handle_t     m_initEncoder = 0u;
    obj_handle_t     m_allocator = 0xa1u;
    std::vector<uint64_t> m_allocatorSerial = std::vector<uint64_t>(4u, 0u);
    uint32_t         m_allocatorSlot = 0u;
    uint64_t         m_currentSerial = 1u;
    uint64_t         m_frameLastSerial = 0u;
    uint32_t         m_carries = 0u;
    uint32_t         m_dirty = 0u;
    bool             m_prefixWork = true, m_serialPrefixCommitted = false, m_completePending = false, m_frameEnding = false;
    uint64_t         m_rs[1] = { };
    std::array<StageBinding, 2> m_bindings = { };
    std::vector<std::shared_ptr<FakeResource>> m_keepAlive;
    std::unique_ptr<RetireBatch> m_retire = std::make_unique<RetireBatch>();

    // What the destructor reaches.
    std::vector<WmtAllocator> m_allocators, m_initAllocators;
    struct FxScaler { obj_handle_t scaler = 0u; };
    std::map<int, FxScaler> m_fxScalers;
    static constexpr uint32_t kVisSlots = 4u, kCanarySlots = 4u;
    WmtBuffer m_visBuffer, m_canaryBuffer;
    void*     m_visCpu = nullptr;
    uint64_t* m_canaryCpu = nullptr;
    std::shared_ptr<int> m_nullBuffer, m_nullImage2D, m_nullImageCube, m_nullImage3D, m_nullView2D, m_nullViewCube, m_nullView3D, m_nullSampler;
    uint64_t  m_canaryLost = 0u;
    enum class CanaryState : uint8_t { Unused, Armed, On, Failed };
    CanaryState m_canaryState = CanaryState::On;

    ~MtlContextImpl();

    RetireBatch& retireBatch() { return *m_retire; }
    void emitProducerBarrier(obj_handle_t, uint64_t, uint64_t = 0u) { }
    void closePass() { }
    void flushFramePasses() { }
    void armCanary() { g.canaryArms++; }
    void checkCanaries(uint64_t) { g.canaryChecks++; }

    COMMIT_RECORDED_RET commitRecorded();
#if HAS_ABANDON_RECORDING
    void abandonRecording();
#endif
    void commitPrefix();
    void submit(bool waitCompleted);
  };

#include "commit_path.inc"
#include "context_dtor.inc"

}

using namespace dxvk::backend;

#define EXPECT(what, field, want) do { if (g.field != (want)) { g_fail++; \
  std::printf("FAIL %s: %s %u (want %u)\n", what, #field, g.field, uint32_t(want)); } } while (0)

struct Setup {
  FakeDevice dev;
  MtlContextImpl ctx;
  std::weak_ptr<FakeResource> kept;
  explicit Setup(bool residencyCommits, bool withInit) {
    dev.res.commits = residencyCommits;
    ctx.m_device = &dev;
    ctx.m_cmdbuf = WmtCommandBuffer::adopt(0xc001u);
    if (withInit) {
      ctx.m_initCmdbuf = WmtCommandBuffer::adopt(0xc000u);
      ctx.m_initEncoder = 0xe0u;
    }
    auto res = std::make_shared<FakeResource>();
    kept = res;
    ctx.m_keepAlive.push_back(std::move(res));
    g = Counts();
  }
};

int main() {
  for (bool withInit : { false, true }) {
    const char* tag = withInit ? " (with hoisted uploads)" : "";
    char what[128];

    { // A normal submit.
      std::snprintf(what, sizeof(what), "submit%s", tag);
      Setup s(true, withInit);
      s.ctx.submit(true);
      EXPECT(what, residencyCommits, 1u); EXPECT(what, queueCommits, 1u); EXPECT(what, onSubmit, 1u);
      EXPECT(what, retireBatch, 1u); EXPECT(what, waits, 1u); EXPECT(what, canaryChecks, 1u);
      EXPECT(what, abandons, 0u); EXPECT(what, logs, 0u);
    }

    { // The residency commit can't be made: the submit is abandoned at once.
      std::snprintf(what, sizeof(what), "detach submit%s", tag);
      Setup s(false, withInit);
      s.ctx.submit(true);
      EXPECT(what, queueCommits, 0u); EXPECT(what, onSubmit, 0u); EXPECT(what, retireBatch, 0u);
      EXPECT(what, waits, 0u); EXPECT(what, canaryChecks, 0u); EXPECT(what, processed, 0u);
      EXPECT(what, serialRetired, 0u); EXPECT(what, ringSubmits, 0u);
      EXPECT(what, abandons, 1u); EXPECT(what, logs, 1u); EXPECT(what, cmdbufReleases, 0u);
      if (s.kept.expired()) { g_fail++; std::printf("FAIL %s: a kept-alive resource was released\n", what); }
    }

    { // A carried prefix that can't commit, then the destructor's completing submit(true).
      std::snprintf(what, sizeof(what), "detach prefix%s", tag);
      Setup s(false, withInit);
      s.ctx.commitPrefix();
      EXPECT(what, queueCommits, 0u); EXPECT(what, retireAfter, 0u); EXPECT(what, abandons, 1u);
      EXPECT(what, logs, 1u); EXPECT(what, cmdbufReleases, 0u); EXPECT(what, newCmdbufs, 1u);
      std::snprintf(what, sizeof(what), "detach prefix, then submit(true)%s", tag);
      s.ctx.submit(true);
      EXPECT(what, queueCommits, 0u); EXPECT(what, onSubmit, 0u); EXPECT(what, retireBatch, 0u);
      EXPECT(what, waits, 0u); EXPECT(what, canaryChecks, 0u); EXPECT(what, processed, 0u);
      EXPECT(what, logs, 1u); EXPECT(what, cmdbufReleases, 0u);
      if (s.kept.expired()) { g_fail++; std::printf("FAIL %s: a kept-alive resource was released\n", what); }
    }

    { // A normal prefix: committed, retired after the serial.
      std::snprintf(what, sizeof(what), "prefix%s", tag);
      Setup s(true, withInit);
      s.ctx.commitPrefix();
      EXPECT(what, queueCommits, 1u); EXPECT(what, retireAfter, withInit ? 2u : 1u);
      EXPECT(what, newCmdbufs, 1u); EXPECT(what, abandons, 0u); EXPECT(what, logs, 0u);
    }

    { // The GPU hung: nothing committed, the rest as before.
      std::snprintf(what, sizeof(what), "GPU hung%s", tag);
      Setup s(true, withInit);
      s.dev.subs.hung = true;
      s.ctx.submit(true);
      EXPECT(what, queueCommits, 0u); EXPECT(what, residencyCommits, 0u); EXPECT(what, onSubmit, 1u);
      EXPECT(what, waits, 1u); EXPECT(what, abandons, 0u); EXPECT(what, logs, 0u);
    }
  }

  // The destructor and the allocators it owns: released normally, leaked once abandoned.
  struct DtorCase { const char* what; bool residencyCommits, prefixPending, abandonedBefore; uint32_t wantReleases; };
  for (const DtorCase& d : { DtorCase { "context teardown", true, true, false, 3u },
                             DtorCase { "context teardown, nothing pending", true, false, false, 3u },
                             DtorCase { "detach teardown (its completing submit dropped)", false, true, false, 0u },
                             DtorCase { "detach teardown (abandoned before)", true, false, true, 0u } }) {
    g = Counts();
    { FakeDevice dev;
      dev.res.commits = d.residencyCommits;
      dev.subs.isAbandoned = d.abandonedBefore;
      auto ctx = std::make_unique<MtlContextImpl>();
      ctx->m_device = &dev;
      ctx->m_cmdbuf = WmtCommandBuffer::adopt(0xc001u);
      ctx->m_serialPrefixCommitted = d.prefixPending;
      ctx->m_allocators.push_back(WmtAllocator::adopt(0xa1u));
      ctx->m_allocators.push_back(WmtAllocator::adopt(0xa2u));
      ctx->m_initAllocators.push_back(WmtAllocator::adopt(0xa3u));
      ctx->m_initAllocators.emplace_back();
      ctx.reset();                           // the production destructor, then member teardown
    }
    if (g.allocReleases != d.wantReleases) {
      g_fail++;
      std::printf("FAIL %s: %u allocator releases (want %u)\n", d.what, g.allocReleases, d.wantReleases);
    }
  }

  std::printf("%s\n", g_fail ? "FAIL" : "ok");
  return g_fail ? 1 : 0;
}
