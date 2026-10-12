#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "../mtl/mtl_device.h"
#include "../mtl/mtl_context.h"

#include "wmt_handle.h"
#include "mtl_residency.h"
#include "mtl_ring.h"
#include "mtl_submission.h"

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
    static constexpr size_t MaxRecycledBackings = 4u;

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
    ~MtlTextureImpl();
  };

  class MtlTextureViewImpl : public MtlTextureView {
  public:
    WmtTexture owned;
    uint64_t   gpuResourceId = 0u;
  };

  class MtlSamplerStateImpl : public MtlSamplerState {
  public:
    WmtSampler owned;
    uint64_t   gpuResourceId = 0u;
  };

  class MtlDeviceImpl : public MtlDevice {

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
            size_t                      size) override;

    void submitCompile(const Rc<MtlCompileTask>& task, bool urgent) override;

    size_t pendingCompiles() override;

    Rc<MtlContext> createContext() override;

    uint64_t getCompletedSerial() override;
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

    std::mutex m_samplerMutex;
    std::unordered_map<size_t, Rc<MtlSamplerState>> m_samplerCache;

    std::mutex m_dssoMutex;
    std::unordered_map<size_t, WmtDepthStencil> m_dssoCache;

    // Stopped first in ~MtlDeviceImpl, while the compiler is still alive.
    std::unique_ptr<MtlCompileWorkers> m_compileWorkers;

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

  inline void MtlBufferImpl::recycleStorage(Rc<MtlAllocation>&& alloc) {
    if (alloc == nullptr)
      return;
    std::lock_guard<std::mutex> lock(m_recycleMutex);
    if (m_recycled.size() < MaxRecycledBackings) {
      static_cast<MtlDeviceImpl*>(m_device)->memRecycled(int64_t(alloc->size));
      m_recycled.push_back(std::move(alloc));
    }
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
