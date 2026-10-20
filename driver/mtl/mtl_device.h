#pragma once

#include <atomic>
#include <cstddef>

#include "mtl_types.h"
#include "mtl_resource.h"
#include "mtl_memory_profile.h"
#include "mtl_render_stats.h"
#include "mtl_restore_stats.h"

#include "../util/rc/util_rc.h"
#include "../util/rc/util_rc_ptr.h"
#include "../util/config/config.h"

namespace dxvk {

  class MtlContext;
  class MtlQuery;
  class MtlEvent;
  class MtlShaderFunction;
  class MtlCompileTask;
  class MtlStateCache;
  class MtlStateCacheBackend;
  enum class MtlQueryType : uint32_t;

  // What a resource is for, set by the frontend. Only feeds the byte
  // counters (D9MT_MEMSTATS) and resource labels (D9MT_LABELS).
  enum class MtlMemCategory : uint8_t {
    Other,
    TexManaged, TexDefault, TexSysmem, TexDynamic, TexRT, TexDS,
    TexMapManaged,  // CPU copy of a MANAGED texture
    TexMap,         // CPU copy of any other texture (lock/readback buffer)
    VbIbGpu, VbIbStaging, VbIbDirect,
    ConstBuf, Staging, UP,
    Count
  };

  const char* memCategoryName(MtlMemCategory c);

  struct MtlBufferCreateInfo {
    uint64_t     size       = 0u;
    MtlUsage     usage      = MtlUsage::None;
    MtlCpuAccess cpuAccess  = MtlCpuAccess::None;
    MtlMemCategory category = MtlMemCategory::Other;
  };

  struct MtlImageCreateInfo {
    MtlImageType  type        = MtlImageType::e2D;
    MtlFormat     format      = MtlFormat::Invalid;
    MtlExtent3D   extent      = { };
    uint32_t      mipLevels   = 1u;
    uint32_t      arrayLayers = 1u;
    uint32_t      sampleCount = 1u;
    MtlUsage      usage       = MtlUsage::None;
    bool          cube        = false;
    MtlMemCategory category   = MtlMemCategory::Other;
  };

  struct MtlSamplerKey {
    MtlSamplerFilter  magFilter  = MtlSamplerFilter::Nearest;
    MtlSamplerFilter  minFilter  = MtlSamplerFilter::Nearest;
    MtlSamplerMipMode mipMode    = MtlSamplerMipMode::Nearest;
    MtlSamplerAddressMode addressU = MtlSamplerAddressMode::Repeat;
    MtlSamplerAddressMode addressV = MtlSamplerAddressMode::Repeat;
    MtlSamplerAddressMode addressW = MtlSamplerAddressMode::Repeat;
    uint32_t          maxAnisotropy = 1u;
    float             mipLodBias = 0.f;
    float             minLod     = 0.f;
    float             maxLod     = 16.f;
    MtlCompareFunction compareOp = MtlCompareFunction::Never;
    bool              compareEnable = false;
    MtlBorderColor    borderColor = MtlBorderColor::TransparentBlack;

    size_t hash() const;
    bool eq(const MtlSamplerKey& other) const;
  };

  class MtlDevice : public RcObject {

  public:

    MtlDevice();
    virtual ~MtlDevice();

    virtual Rc<MtlBuffer> createBuffer(
            const MtlBufferCreateInfo&  info);

    virtual Rc<MtlTexture> createImage(
            const MtlImageCreateInfo&   info);

    virtual Rc<MtlTextureView> createImageView(
            const Rc<MtlTexture>&       image,
            const MtlImageViewKey&      key);

    virtual Rc<MtlSamplerState> createSampler(
            const MtlSamplerKey&        key);

    // fallback: a metallib to load if Metal rejects the first one, e.g. the Compat build of
    // an embedded metallib for macOS versions without the newest Metal language.
    virtual Rc<MtlShaderFunction> createShaderFunction(
            MtlShaderStage              stage,
            const void*                 metallib,
            size_t                      size,
            const void*                 fallback = nullptr,
            size_t                      fallbackSize = 0);

    // Queues a compile task on the device's worker pool. Without workers
    // (D9MT_COMPILE_THREADS=0, stub device) tasks run inline on first wait().
    // Urgent tasks (needed by a draw) go ahead of prefetch work.
    virtual void submitCompile(const Rc<MtlCompileTask>& task, bool urgent);

    // Tasks queued but not yet started.
    virtual size_t pendingCompiles();

    virtual Rc<MtlContext> createContext();

    virtual uint64_t getCompletedSerial();

    // serial <= getCompletedSerial(), without reading the GPU when a cached value or the
    // submission counter already answers it.
    virtual bool isSerialComplete(uint64_t serial);

    virtual uint64_t getNextSerial();

    // G1: a deferrable flush kept a pass open and its submission isn't complete-able until
    // the backend's next real submit (docs/g1-early-commit.md §4.5). Any thread.
    bool carryOpen() const { return m_carryOpen.load(std::memory_order_acquire); }
    void setCarryOpen(bool open) { m_carryOpen.store(open, std::memory_order_release); }

    virtual void waitForIdle();

    virtual bool supportsFormat(
            MtlFormat                   format,
            MtlUsage                    usage) const;

    virtual const MtlDeviceInfo& deviceInfo() const;

    virtual Rc<MtlAllocation> renameBuffer(MtlBuffer* buffer);

    virtual MtlStageMask getShaderStages() const;

    bool isResourceInUse(const MtlResource& resource) {
      return !isSerialComplete(resource.trackingSerial());
    }

    // Blocks until the completed serial catches up to the resource's tracking
    // serial. An unsubmitted command buffer never signals, so calling this
    // without flushing first stalls forever.
    virtual void waitForResource(const MtlResource& resource);

    virtual Rc<MtlStagingBuffer> createStagingBuffer(MtlDeviceSize size);
    virtual MtlStagingBufferStats getStagingMemoryStatistics();

    virtual Rc<MtlQuery> createGpuQuery(MtlQueryType type);
    virtual Rc<MtlEvent> createGpuEvent();

    virtual uint64_t getTimestampFrequency() const;

    // Metal's currentAllocatedSize in bytes, 0 if unknown (STATS line).
    virtual uint64_t allocatedBytes();

    const MtlMemoryProfile& memoryProfile() const { return m_memProfile; }

    // Frontend counters for the DIAG MEM line. Lock counters are only
    // touched when memoryProfile().statsInterval != 0.
    struct FrontendStats {
      std::atomic<uint64_t> bufferLockWaits  = { 0u };  // buffer locks that had to wait for the CS thread and/or GPU
      std::atomic<uint64_t> bufferLockWaitUs = { 0u };
      std::atomic<uint64_t> bufferLockFree   = { 0u };  // in-place DIRECT locks that needed no wait
      std::atomic<uint64_t> writeOnlyDemoted = { 0u };  // WRITEONLY DIRECT buffers demoted to BUFFER mode
      std::atomic<uint64_t> resourceGpuWaits = { 0u };  // lock waits that blocked on the GPU (buffers + readbacks)
      std::atomic<uint64_t> stagingNew       = { 0u };  // 4 MiB staging blocks created / reused from the pool
      std::atomic<uint64_t> stagingReused    = { 0u };
    };
    FrontendStats& frontendStats() { return m_frontendStats; }

    // D9MT_PROFILE: render-thread totals for the STATS line (mtl_render_stats.h).
    rstats::Totals& renderStats() { return m_renderStats; }

    // D9MT_PROFILE: what the attachment optimization's restores were for
    // (mtl_restore_stats.h), for the RESTORE lines.
    rstore::Log& restoreLog() { return m_restoreLog; }

    // Pipeline state cache (mtl_state_cache.h). The backend side the cache
    // builds through (nullptr: not supported) and the signature of the PSO
    // key layout it records.
    virtual MtlStateCacheBackend* stateCacheBackend() { return nullptr; }
    virtual uint64_t psoKeySignature() const { return 0u; }

    // The cache, once the frontend has created it (nullptr: off). Set once
    // before the first draw; read by the render thread on PSO misses.
    MtlStateCache* stateCache() const {
      return m_stateCachePtr.load(std::memory_order_acquire);
    }

    void setStateCache(const Rc<MtlStateCache>& cache);

  protected:

    Rc<MtlStateCache>           m_stateCache;
    std::atomic<MtlStateCache*> m_stateCachePtr = { nullptr };

    FrontendStats    m_frontendStats;
    rstats::Totals   m_renderStats;
    rstore::Log      m_restoreLog;

    MtlDeviceInfo    m_info = { };
    MtlMemoryProfile m_memProfile = { };
    std::atomic<bool> m_carryOpen = { false };

  };

  inline Rc<MtlAllocation> MtlBuffer::allocateStorage() {
    return m_device ? m_device->renameBuffer(this) : nullptr;
  }

  inline Rc<MtlTextureView> MtlTexture::createView(const MtlImageViewKey& key) {
    return m_device ? m_device->createImageView(Rc<MtlTexture>(this), key) : nullptr;
  }

  inline MtlStagingBuffer::MtlStagingBuffer(const Rc<MtlDevice>& device, MtlDeviceSize size) {
    reinit(device, size);
  }

  inline void MtlStagingBuffer::reinit(const Rc<MtlDevice>& device, MtlDeviceSize size) {
    MtlBufferCreateInfo info = { };
    info.size      = size;
    info.usage     = MtlUsage::Staging;
    info.cpuAccess = MtlCpuAccess::Write;
    info.category  = MtlMemCategory::Staging;
    m_buffer = device ? device->createBuffer(info) : nullptr;
    m_size   = size;
    m_offset = 0u;
    m_stats  = { };
  }

  class MtlAdapter : public RcObject {
  public:
    explicit MtlAdapter(const MtlMemoryProfile& profile = { }) : m_memProfile(profile) { }
    virtual ~MtlAdapter() { }

    virtual const MtlDeviceInfo& deviceInfo() const;

    virtual bool supportsFormat(MtlFormat format, MtlUsage usage) const;

    virtual Rc<MtlDevice> createDevice();

  protected:
    MtlDeviceInfo    m_info = { };
    MtlMemoryProfile m_memProfile = { };
  };

  class MtlInstance : public RcObject {
  public:
    MtlInstance();
    virtual ~MtlInstance();

    virtual uint32_t adapterCount() const;

    virtual Rc<MtlAdapter> enumAdapters(uint32_t index);

    const Config& config() const { return m_config; }
    void setConfig(const Config& config) { m_config = config; }

  protected:
    Config           m_config;
    MtlMemoryProfile m_memProfile = { };
  };

}
