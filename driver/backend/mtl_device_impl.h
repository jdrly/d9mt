#pragma once

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "../mtl/mtl_device.h"
#include "../mtl/mtl_context.h"

#include "../mtl/mtl_state_cache.h"

#include "wmt_handle.h"
#include "mtl_pso_cache.h"
#include "mtl_residency.h"
#include "mtl_ring.h"
#include "mtl_submission.h"
#include "mtl_attachment_opt.h"

namespace dxvk::backend {

  struct Mtl4CompilerTag;     using WmtCompiler     = WmtRef<Mtl4CompilerTag>;
  struct Mtl4AllocatorTag;    using WmtAllocator    = WmtRef<Mtl4AllocatorTag>;
  struct Mtl4ArgTableTag;     using WmtArgTable     = WmtRef<Mtl4ArgTableTag>;

  class MtlAllocationImpl : public MtlAllocation {
  public:
    WmtBuffer owned;
    class MtlDeviceImpl* residencyDevice = nullptr;
    MtlMemCategory category = MtlMemCategory::Other;

    ~MtlAllocationImpl();
  };

  class MtlBufferImpl : public MtlBuffer {
  public:
    WmtBuffer owned;
    MtlBufferCreateInfo info = { };

    ~MtlBufferImpl();

    void bindDevice(MtlDevice* device) { m_device = device; }

    inline void recycleStorage(Rc<MtlAllocation>&& alloc);

    inline Rc<MtlAllocation> takeRecycledStorage();

  private:
    std::mutex m_recycleMutex;
    std::vector<Rc<MtlAllocation>> m_recycled;
  };

  class MtlTextureImpl : public MtlTexture {
  public:
    WmtTexture owned;
    uint64_t   gpuResourceId = 0u;
    uint64_t   bytes         = 0u;  // estimated, for accounting only
    MtlImageCreateInfo info  = { };
    class MtlDeviceImpl* residencyDevice = nullptr;
    // Attachment load/store optimization (mtl_attachment_opt.h), render
    // thread only. Stays Defined on images it doesn't track.
    attopt::AspectState attColor, attDepth, attStencil;
    uint64_t           attSampledEpoch = 0u;  // pass epoch of the last bind for sampling
    // The context's m_vtxEpoch when a pass that ended with the narrowed
    // barrier last rendered to it (MtlContextImpl::markVertexHazards).
    uint64_t           vtxEpoch = 0u;
    ~MtlTextureImpl();
  };

  class MtlTextureViewImpl : public MtlTextureView {
  public:
    WmtTexture owned;
    uint64_t   gpuResourceId = 0u;
    // Set for views from createImageView (not the presenter's drawable views): the
    // Metal view is released once the GPU is past every command buffer that may use it.
    class MtlDeviceImpl* residencyDevice = nullptr;
    ~MtlTextureViewImpl();
  };

  class MtlSamplerStateImpl : public MtlSamplerState {
  public:
    WmtSampler owned;
    uint64_t   gpuResourceId = 0u;
  };

  class MtlDeviceImpl : public MtlDevice, public MtlStateCacheBackend {

  public:

    explicit MtlDeviceImpl(const MtlMemoryProfile& profile);
    ~MtlDeviceImpl();

    Rc<MtlBuffer> createBuffer(
            const MtlBufferCreateInfo&  info) override;

    Rc<MtlTexture> createImage(
            const MtlImageCreateInfo&   info) override;

    Rc<MtlTextureView> createImageView(
            const Rc<MtlTexture>&       image,
            const MtlImageViewKey&      key) override;

    Rc<MtlSamplerState> createSampler(
            const MtlSamplerKey&        key) override;

    Rc<MtlShaderFunction> createShaderFunction(
            MtlShaderStage              stage,
            const void*                 metallib,
            size_t                      size,
            const void*                 fallback = nullptr,
            size_t                      fallbackSize = 0) override;

    void submitCompile(const Rc<MtlCompileTask>& task, bool urgent) override;

    size_t pendingCompiles() override;

    // State cache backend (mtl_state_cache.h).
    MtlStateCacheBackend* stateCacheBackend() override { return this; }
    uint64_t psoKeySignature() const override { return RenderPsoKey::signature(); }

    Rc<MtlCompileTask> createPrewarmPso(
      const uint8_t*            pState,
      const MtlShaderFunction*  pVs,
      const MtlShaderFunction*  pFs) override;

    bool psoStateValid(const uint8_t* pState, const char** pWhy) override;
    bool psoSucceeded(MtlCompileTask* pPso) override;
    uint64_t psoBuildNs(MtlCompileTask* pPso) override;
    uint32_t urgentCompilesQueued() override;

    Rc<MtlContext> createContext() override;

    uint64_t getCompletedSerial() override;
    bool     isSerialComplete(uint64_t serial) override;
    uint64_t getNextSerial() override;
    void     waitForIdle() override;

    void     waitForResource(const MtlResource& resource) override;

    void*    hostAlloc(uint64_t size);

    bool supportsFormat(
            MtlFormat                   format,
            MtlUsage                    usage) const override;

    Rc<MtlAllocation> renameBuffer(MtlBuffer* buffer) override;

    Rc<MtlQuery> createGpuQuery(MtlQueryType type) override;
    Rc<MtlEvent> createGpuEvent() override;

    uint64_t allocatedBytes() override;

    obj_handle_t wmtDevice()   const { return m_device.handle(); }
    obj_handle_t wmtQueue()    const { return m_queue.handle(); }
    obj_handle_t wmtCompiler() const { return m_compiler.handle(); }

    SubmissionTracker& submissions() { return *m_tracker; }
    UploadRing&        uploadRing()  { return *m_ring; }

    MtlResidencyTracker& residency() { return *m_residency; }

    obj_handle_t getDepthStencilState(const MtlDepthStencilState& ds);

    bool valid() const { return bool(m_device); }

    uint64_t diagBuffersCreated() const { return m_diagBuffersCreated.load(); }
    uint64_t diagBuffersRenamed() const { return m_diagBuffersRenamed.load(); }
    uint64_t diagBuffersRecycled() const { return m_diagBuffersRecycled.load(); }
    uint64_t diagImagesCreated()  const { return m_diagImagesCreated.load(); }

    // Byte accounting by category; relaxed atomics touched only on resource
    // create/destroy/recycle, never per draw.
    void memAdd(MtlMemCategory c, uint64_t bytes) {
      m_memBytes[size_t(c)].fetch_add(int64_t(bytes), std::memory_order_relaxed);
      m_memCount[size_t(c)].fetch_add(1, std::memory_order_relaxed);
    }
    void memSub(MtlMemCategory c, uint64_t bytes) {
      m_memBytes[size_t(c)].fetch_sub(int64_t(bytes), std::memory_order_relaxed);
      m_memCount[size_t(c)].fetch_sub(1, std::memory_order_relaxed);
    }
    void memRecycled(int64_t delta) { m_recycledBytes.fetch_add(delta, std::memory_order_relaxed); }

    // Accounts a spare backing unless that would exceed the device-wide cap (0 = none).
    bool tryRecycle(uint64_t bytes, uint64_t cap) {
      int64_t cur = m_recycledBytes.load(std::memory_order_relaxed);
      do {
        if (cap && uint64_t(cur) + bytes > cap)
          return false;
      } while (!m_recycledBytes.compare_exchange_weak(cur, cur + int64_t(bytes), std::memory_order_relaxed));
      return true;
    }

    // 0 = off (D9MT_MEMSTATS), else the DIAG interval in frames.
    uint32_t memStatsInterval() const { return m_memStatsInterval; }
    std::string memStatsLine(uint64_t frame);

  private:

    void labelResource(obj_handle_t handle, MtlMemCategory c, const MtlImageCreateInfo* image, uint64_t bytes);

    std::atomic<int64_t> m_memBytes[size_t(MtlMemCategory::Count)] = { };
    std::atomic<int64_t> m_memCount[size_t(MtlMemCategory::Count)] = { };
    std::atomic<int64_t> m_recycledBytes { 0 };
    uint32_t             m_memStatsInterval = 0u;
    bool                 m_labels = false;

    std::atomic<uint64_t> m_diagBuffersCreated { 0u };
    std::atomic<uint64_t> m_diagBuffersRenamed { 0u };
    std::atomic<uint64_t> m_diagBuffersRecycled { 0u };
    std::atomic<uint64_t> m_diagImagesCreated  { 0u };

    WmtDevice   m_device;
    WmtQueue    m_queue;
    WmtCompiler m_compiler;

    // Declaration order is load-bearing: destruction runs in reverse, so the
    // ring hands its chunks to the tracker before the tracker detaches, and
    // the tracker still has a live queue underneath it.
    std::unique_ptr<SubmissionTracker>   m_tracker;
    std::unique_ptr<MtlResidencyTracker> m_residency;
    std::unique_ptr<UploadRing>          m_ring;

    // Keyed by the whole state: two states with the same hash must not share an object.
    struct SamplerKeyHash { size_t operator () (const MtlSamplerKey& k) const { return k.hash(); } };
    struct SamplerKeyEq   { bool operator () (const MtlSamplerKey& a, const MtlSamplerKey& b) const { return a.eq(b); } };
    struct DssHash { size_t operator () (const MtlDepthStencilState& ds) const; };
    struct DssEq   { bool operator () (const MtlDepthStencilState& a, const MtlDepthStencilState& b) const; };

    std::mutex m_samplerMutex;
    std::unordered_map<MtlSamplerKey, Rc<MtlSamplerState>, SamplerKeyHash, SamplerKeyEq> m_samplerCache;

    std::mutex m_dssoMutex;
    std::unordered_map<MtlDepthStencilState, WmtDepthStencil, DssHash, DssEq> m_dssoCache;

    // Stopped first in ~MtlDeviceImpl, while the compiler is still alive.
    std::unique_ptr<MtlCompileWorkers> m_compileWorkers;

    // PSOs shared with the state cache prewarm (only used while it is on).
    RenderPsoCache::Registry m_psoRegistry;

    friend class MtlContextImpl;

  };

  // Defined inline (weak comdat) so every TU constructing MtlTextureImpl
  // shares one definition instead of emitting conflicting strong symbols.
  inline MtlTextureImpl::~MtlTextureImpl() {
    if (residencyDevice && owned) {
      residencyDevice->memSub(info.category, bytes);
      residencyDevice->residency().allocDestroyed(
        owned.detach(), nullptr,
        residencyDevice->submissions().nextSerial(), bytes);
    }
  }

  // Metal 4 command buffers don't retain what they use: a draw's argument table holds the
  // view's resource ID and a pass its handle, so the view outlives the last Rc until the
  // GPU is past the command buffer being recorded (released on the retire thread).
  inline MtlTextureViewImpl::~MtlTextureViewImpl() {
    if (residencyDevice && owned) {
      const obj_handle_t h = owned.detach();
      SubmissionTracker& st = residencyDevice->submissions();
      st.retireAfter(st.nextSerial(), [h] () { NSObject_release(h); });
    }
  }

  inline void MtlBufferImpl::recycleStorage(Rc<MtlAllocation>&& alloc) {
    if (alloc == nullptr)
      return;
    auto* device = static_cast<MtlDeviceImpl*>(m_device);
    const MtlMemoryProfile& profile = device->memoryProfile();
    std::lock_guard<std::mutex> lock(m_recycleMutex);
    if (m_recycled.size() < profile.recycledPerBuffer
     && device->tryRecycle(alloc->size, profile.recycledCapBytes))
      m_recycled.push_back(std::move(alloc));
  }

  inline Rc<MtlAllocation> MtlBufferImpl::takeRecycledStorage() {
    std::lock_guard<std::mutex> lock(m_recycleMutex);
    if (m_recycled.empty())
      return nullptr;
    Rc<MtlAllocation> alloc = std::move(m_recycled.back());
    m_recycled.pop_back();
    static_cast<MtlDeviceImpl*>(m_device)->memRecycled(-int64_t(alloc->size));
    return alloc;
  }

  inline MtlBufferImpl::~MtlBufferImpl() {
    for (const auto& alloc : m_recycled) if (m_device)
      static_cast<MtlDeviceImpl*>(m_device)->memRecycled(-int64_t(alloc->size));
  }

}
