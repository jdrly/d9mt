#include "mtl_device_impl.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "mtl_context_impl.h"
#include "../mtl/mtl_context.h"
#include "../util/log/log.h"
#include "../util/util_env.h"
#include "../util/util_string.h"

namespace dxvk::backend {

  namespace {

    constexpr uint64_t UploadRingChunkSize = 4u << 20;

    WMTPixelFormat wmtFormat(MtlFormat format) {
      return WMTPixelFormat(uint32_t(format));
    }

    WMTTextureType wmtTextureType(const MtlImageCreateInfo& info) {
      if (info.cube)
        return info.arrayLayers > 6u ? WMTTextureTypeCubeArray : WMTTextureTypeCube;

      switch (info.type) {
        case MtlImageType::e1D:
          return info.arrayLayers > 1u ? WMTTextureType1DArray : WMTTextureType1D;
        case MtlImageType::e3D:
          return WMTTextureType3D;
        default:
        case MtlImageType::e2D:
          if (info.sampleCount > 1u)
            return WMTTextureType2DMultisample;
          return info.arrayLayers > 1u ? WMTTextureType2DArray : WMTTextureType2D;
      }
    }

    WMTResourceOptions storageOptions(MtlCpuAccess access) {
      switch (access) {
        case MtlCpuAccess::Write:
          return WMTResourceOptions(
              WMTResourceStorageModeShared
            | WMTResourceCPUCacheModeWriteCombined
            | WMTResourceHazardTrackingModeUntracked);
        case MtlCpuAccess::Read:
          return WMTResourceOptions(
              WMTResourceStorageModeShared
            | WMTResourceHazardTrackingModeUntracked);
        default:
        case MtlCpuAccess::None:
          return WMTResourceOptions(
              WMTResourceStorageModePrivate
            | WMTResourceHazardTrackingModeUntracked);
      }
    }

    // D9MT_TEX_PFV=1: PixelFormatView on every texture, as before (A/B).
    bool pixelFormatViewOnAll() {
      static const bool on = [] {
        const char* v = std::getenv("D9MT_TEX_PFV");
        return v != nullptr && v[0] == '1';
      }();
      return on;
    }

    uint32_t textureUsage(MtlUsage usage, MtlFormat format) {
      uint32_t result = 0u;

      if (usage & MtlUsage::Sampled)
        result |= WMTTextureUsageShaderRead;
      if (usage & MtlUsage::RenderTarget)
        result |= WMTTextureUsageRenderTarget | WMTTextureUsageShaderRead;
      if (usage & MtlUsage::DepthStencil)
        result |= WMTTextureUsageRenderTarget;

      // PixelFormatView turns off lossless compression on Apple GPUs. d9mt's
      // views keep the image's format or switch it between linear and sRGB,
      // with or without a swizzle, none of which needs the option
      // (MTLTextureUsage.pixelFormatView). Block-compressed formats keep it:
      // they are never losslessly compressed anyway (ColorFill no longer
      // views them, #14).
      if (pixelFormatViewOnAll() || (lookupFormatInfo(format)->flags & MtlFormatFlagCompressed))
        result |= WMTTextureUsagePixelFormatView;
      return result;
    }

    // Estimate only: block-aligned mip chain, no Metal padding.
    uint64_t textureBytes(const MtlImageCreateInfo& info) {
      const MtlFormatInfo* f = lookupFormatInfo(info.format);
      if (!f || !f->elementSize)
        return 0u;
      const uint64_t bw = std::max(1u, f->blockWidth), bh = std::max(1u, f->blockHeight);
      uint64_t total = 0u;
      for (uint32_t m = 0u; m < std::max(1u, info.mipLevels); m++) {
        uint64_t w = std::max(1u, info.extent.width  >> m);
        uint64_t h = std::max(1u, info.extent.height >> m);
        uint64_t d = info.type == MtlImageType::e3D ? std::max(1u, info.extent.depth >> m) : 1u;
        total += ((w + bw - 1u) / bw) * ((h + bh - 1u) / bh) * d * f->elementSize;
      }
      return total * std::max(1u, info.arrayLayers) * std::max(1u, info.sampleCount);
    }

  }

  // D9MT_COMPILE_THREADS=<n> overrides min(4, ncpu/2); 0 = no workers.
  static uint32_t compileThreadCount() {
    std::string env = env::getEnvVar("D9MT_COMPILE_THREADS");
    uint32_t ncpu = dxvk::thread::hardware_concurrency();
    uint32_t n = env.empty()
      ? std::clamp(ncpu / 2u, 1u, 4u)
      : uint32_t(std::strtoul(env.c_str(), nullptr, 10));
    Logger::info(str::format("D9MT: compile workers: ", n, " (ncpu ", ncpu, ")"));
    return n;
  }

  MtlDeviceImpl::MtlDeviceImpl(const MtlMemoryProfile& profile)
  : m_compileWorkers(std::make_unique<MtlCompileWorkers>(compileThreadCount())) {
    m_memProfile = profile;

    obj_handle_t devices = WMTCopyAllDevices();
    // objectAtIndex: is +0 and the array holds the device: retain it before the array goes
    // (adopting it released one reference too many at teardown).
    obj_handle_t device = NSArray_object(devices, 0u);
    if (device)
      m_device = WmtDevice::retain(device);
    NSObject_release(devices);

    if (!device) {
      Logger::err("MtlDeviceImpl: no Metal device");
      return;
    }
    m_queue = WmtQueue::adopt(MTLDevice_newMTL4CommandQueue(m_device.handle()));
    m_compiler = WmtCompiler::adopt(MTLDevice_newMTL4Compiler(m_device.handle()));

    m_tracker = std::make_unique<SubmissionTracker>(m_device.handle());
    lifetime::attach(m_tracker.get());
    m_residency = std::make_unique<MtlResidencyTracker>(m_device.handle(), m_queue.handle(), !profile.legacy);
    m_ring = std::make_unique<UploadRing>(m_device.handle(), UploadRingChunkSize, m_residency.get(),
                                         profile.ringKeep, profile.legacy);

    m_info.maxBufferLength = MTLDevice_recommendedMaxWorkingSetSize(m_device.handle());

    m_memStatsInterval = profile.statsInterval;
    m_labels = profile.labels;

    constexpr uint64_t MiB = 1ull << 20;
    Logger::warn(str::format("D9MT: memory profile ", memoryTierName(profile.tier),
      profile.autoTier ? " (auto)" : "", ": RAM ", profile.physRam / MiB, " MiB",
      ", working set ", m_info.maxBufferLength / MiB, " MiB",
      ", reportedVram ", profile.reportedVramMB, " MB",
      ", budget ", profile.budget ? str::format(profile.budget / MiB, " MiB") : std::string("none"),
      ", staging ", profile.stagingBlocks, " blocks/", profile.stagingInFlightMB, " MiB in flight",
      ", recycled ", profile.recycledPerBuffer, "/buffer cap ",
        profile.recycledCapBytes ? str::format(profile.recycledCapBytes / MiB, " MiB") : std::string("none"),
      ", ring keep ", profile.ringKeep ? str::format(profile.ringKeep) : std::string("all"),
      ", allocators ", profile.allocatorCap,
      ", writeOnlyDirect ", profile.writeOnlyDirect ? "on" : "off",
      profile.writeOnlyDirect ? (profile.writeOnlyDemoteLocks
        ? str::format(" (demote after ", profile.writeOnlyDemoteLocks, " in-place locks)")
        : std::string(" (never demote)")) : std::string(),
      profile.legacy ? ", legacy pools" : "",
      profile.statsInterval ? str::format(", memstats every ", profile.statsInterval, " frames") : std::string(),
      profile.labels ? ", labels" : ""));
    Logger::info(pixelFormatViewOnAll()
      ? "D9MT: PixelFormatView on every texture (D9MT_TEX_PFV=1): no lossless compression"
      : "D9MT: PixelFormatView only on block-compressed textures (lossless compression on)");
  }

  void MtlDeviceImpl::labelResource(obj_handle_t handle, MtlMemCategory c,
                                    const MtlImageCreateInfo* image, uint64_t bytes) {
    char text[96];
    if (image) {
      std::snprintf(text, sizeof(text), "%s %ux%ux%u f%u m%u l%u s%u",
        memCategoryName(c), image->extent.width, image->extent.height, image->extent.depth,
        uint32_t(image->format), image->mipLevels, image->arrayLayers, image->sampleCount);
    } else {
      std::snprintf(text, sizeof(text), "%s %llu", memCategoryName(c), (unsigned long long)bytes);
    }
    obj_handle_t str = NSString_alloc_init(text, WMTUTF8StringEncoding);
    if (!str)
      return;
    /* No MTLResource label thunk in the bridge; this one is a plain
     * [obj setLabel:str], and MTLResource declares the same selector. */
    MTLCommandEncoder_setLabel(handle, str);
    NSObject_release(str);
  }

  std::string MtlDeviceImpl::memStatsLine(uint64_t frame) {
    constexpr double MiB = 1.0 / double(1u << 20);
    std::string line = str::format("DIAG MEM frame=", frame,
      " allocatedMiB=", uint64_t(double(MTLDevice_currentAllocatedSize(m_device.handle())) * MiB),
      " residentMiB=", uint64_t(double(m_residency->residentBytes()) * MiB),
      " resident=", m_residency->netResident(),
      " commits=", m_residency->totalCommits(),
      " pendingFreeMiB=", uint64_t(double(m_residency->pendingBytes()) * MiB),
      " ringMiB=", uint64_t(double(m_ring->allocatedBytes()) * MiB),
      " recycledMiB=", uint64_t(double(m_recycledBytes.load()) * MiB),
      " bufLockSync=", m_frontendStats.bufferLockWaits.load(), "/",
        m_frontendStats.bufferLockWaitUs.load() / 1000u, "ms",
      " bufLockFree=", m_frontendStats.bufferLockFree.load(),
      " gpuWaits=", m_frontendStats.resourceGpuWaits.load(),
      " woDemoted=", m_frontendStats.writeOnlyDemoted.load(),
      " stagingNew=", m_frontendStats.stagingNew.load(),
      " stagingReused=", m_frontendStats.stagingReused.load());
    for (uint32_t i = 0u; i < uint32_t(MtlMemCategory::Count); i++) {
      int64_t n = m_memCount[i].load(std::memory_order_relaxed);
      if (!n)
        continue;
      char part[64];
      std::snprintf(part, sizeof(part), " %s=%.1f/%lld", memCategoryName(MtlMemCategory(i)),
        double(m_memBytes[i].load(std::memory_order_relaxed)) * MiB, (long long)n);
      line += part;
    }
    return line;
  }

  uint64_t MtlDeviceImpl::allocatedBytes() {
    return m_device ? uint64_t(MTLDevice_currentAllocatedSize(m_device.handle())) : 0u;
  }

  void MtlDeviceImpl::submitCompile(const Rc<MtlCompileTask>& task, bool urgent) {
    m_compileWorkers->submit(task, urgent);
  }

  size_t MtlDeviceImpl::pendingCompiles() {
    return m_compileWorkers->pending();
  }

  Rc<MtlCompileTask> MtlDeviceImpl::createPrewarmPso(
    const uint8_t* pState, const MtlShaderFunction* pVs, const MtlShaderFunction* pFs) {
    // Never hand Metal a descriptor the draw path couldn't have built: it
    // terminates the process on invalid ones. (Load already dropped these.)
#ifdef D9MT_STATE_CACHE_TESTING
    // Test builds only: D9MT_STATE_CACHE_TEST_NO_VALIDATE=1 lets a forged
    // entry through, to test the state cache's crash-loop guard.
    static const bool s_testNoValidate = env::getEnvVar("D9MT_STATE_CACHE_TEST_NO_VALIDATE") == "1";
#else
    constexpr bool s_testNoValidate = false;
#endif
    if (!s_testNoValidate && !backend::psoStateValid(pState))
      return nullptr;

    // The key resolvePso() builds for these functions: the recorded bytes
    // plus this session's library handles and entry names.
    RenderPsoKey key = {};
    std::memcpy(reinterpret_cast<uint8_t*>(&key) + RenderPsoKey::StateOffset, pState, statecache::KeyBytes);
    key.vsCookie = uint64_t(pVs->library);
    key.fsCookie = pFs != nullptr ? uint64_t(pFs->library) : 0u;

    const char* vsEntry = pVs->entryName ? pVs->entryName : "main";
    const char* fsEntry = (pFs != nullptr && pFs->entryName) ? pFs->entryName : "main";

    Rc<RenderPsoCache::Entry> e = RenderPsoCache::createPrewarmEntry(m_compiler.handle(), key,
      pVs->library, pFs != nullptr ? obj_handle_t(pFs->library) : 0u, vsEntry, fsEntry);

    if (!m_psoRegistry.insertIfAbsent(e))
      return nullptr;
    return e;
  }

  bool MtlDeviceImpl::psoStateValid(const uint8_t* pState, const char** pWhy) {
    return backend::psoStateValid(pState, pWhy);
  }

  bool MtlDeviceImpl::psoSucceeded(MtlCompileTask* pPso) {
    auto* e = static_cast<RenderPsoCache::Entry*>(pPso);
    return e->isDone() && e->handle();
  }

  uint64_t MtlDeviceImpl::psoBuildNs(MtlCompileTask* pPso) {
    auto* e = static_cast<RenderPsoCache::Entry*>(pPso);
    return e->isDone() ? e->buildNs() : 0u;
  }

  uint32_t MtlDeviceImpl::urgentCompilesQueued() {
    return m_compileWorkers->urgentQueued();
  }

  MtlDeviceImpl::~MtlDeviceImpl() {
    // Normally done by ~D3D9DeviceEx; this covers a device whose D3D9 side
    // never got that far (e.g. its constructor threw after creating the
    // cache). Idempotent: prewarm threads must stop before the compiler goes.
    // At process exit (isInModuleDetachment) shutdown() only detaches its
    // threads: no join or lock on threads ExitProcess already killed.
    if (MtlStateCache* stateCache = this->stateCache())
      stateCache->shutdown();

    m_compileWorkers = nullptr;

    /* Retirement callbacks reach into the residency tracker, which is
     * destroyed after this body runs — the pump must be stopped and
     * drained here, before that member teardown happens. */
    if (m_tracker) {
      m_tracker->drainAndStopPump();
      lifetime::detach(m_tracker.get());
    }
  }

  void* MtlDeviceImpl::hostAlloc(uint64_t size) {
    void* p = VirtualAlloc(nullptr, SIZE_T(size),
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (unlikely(!p)) {
      Logger::warn("MtlDeviceImpl: host allocation failed — draining GPU and retrying");
      waitForIdle();
      if (m_tracker)
        m_tracker->processRetirements();
      if (m_tracker && m_residency) {
        m_residency->serialRetired(m_tracker->completedSerial());
        m_residency->beforeSubmit();  // commit the removals and free them now
        m_residency->releaseCommitted();
      }
      p = VirtualAlloc(nullptr, SIZE_T(size),
          MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
      if (!p)
        Logger::err("MtlDeviceImpl: host allocation failed after drain");
    }
    // No memset: fresh VirtualAlloc pages are zero, and nothing recycles
    // host memory through here.
    return p;
  }

  MtlAllocationImpl::~MtlAllocationImpl() {
    if (residencyDevice) {
      residencyDevice->memSub(category, size);
      residencyDevice->residency().allocDestroyed(
        owned.detach(), mapPtr,
        residencyDevice->submissions().nextSerial(), size);
      mapPtr = nullptr;
      return;
    }
    owned.reset();
    if (mapPtr) {
      VirtualFree(mapPtr, 0, MEM_RELEASE);
      mapPtr = nullptr;
    }
  }

  Rc<MtlBuffer> MtlDeviceImpl::createBuffer(const MtlBufferCreateInfo& info) {
    WMTBufferInfo bufferInfo = {};
    bufferInfo.length = info.size;
    bufferInfo.options = storageOptions(info.cpuAccess);
    /* 32-bit process: CPU-visible backing must be allocated in our own
     * address space, not handed off to Metal. */
    bufferInfo.memory.set(info.cpuAccess != MtlCpuAccess::None
      ? hostAlloc(info.size)
      : nullptr);

    obj_handle_t handle = MTLDevice_newBuffer(m_device.handle(), &bufferInfo);

    if (!handle) {
      Logger::err("MtlDeviceImpl: createBuffer failed");
      return nullptr;
    }

    Rc<MtlBufferImpl> buffer = new MtlBufferImpl();
    buffer->owned = WmtBuffer::adopt(handle);
    buffer->size = info.size;
    buffer->info = info;
    buffer->bindDevice(this);

    Rc<MtlAllocationImpl> storage = new MtlAllocationImpl();
    storage->owned = WmtBuffer::retain(handle);
    storage->handle.buf = MtlObjectHandle(handle);
    storage->mapPtr = bufferInfo.memory.get();
    storage->gpuAddress = bufferInfo.gpu_address;
    storage->size = info.size;
    storage->residencyDevice = this;
    storage->category = info.category;
    memAdd(info.category, info.size);
    m_residency->allocCreated(handle, info.size);
    if (unlikely(m_labels))
      labelResource(handle, info.category, nullptr, info.size);
    buffer->assignStorage(std::move(storage));
    m_diagBuffersCreated++;
    return buffer;
  }

  Rc<MtlTexture> MtlDeviceImpl::createImage(const MtlImageCreateInfo& info) {
    WMTTextureInfo texInfo = {};
    texInfo.pixel_format = wmtFormat(info.format);
    texInfo.width = info.extent.width;
    texInfo.height = info.extent.height;
    texInfo.depth = info.extent.depth;
    texInfo.type = wmtTextureType(info);
    uint32_t arrayLength = info.arrayLayers > 0u ? info.arrayLayers : 1u;
    if (info.cube)
      arrayLength = arrayLength / 6u > 0u ? arrayLength / 6u : 1u;
    texInfo.array_length = arrayLength;
    texInfo.mipmap_level_count = info.mipLevels;
    texInfo.sample_count = info.sampleCount;
    texInfo.usage = WMTTextureUsage(textureUsage(info.usage, info.format));
    texInfo.options = storageOptions(MtlCpuAccess::None);

    obj_handle_t handle = MTLDevice_newTexture(m_device.handle(), &texInfo);

    if (!handle) {
      Logger::err("MtlDeviceImpl: createImage failed");
      return nullptr;
    }

    Rc<MtlTextureImpl> texture = new MtlTextureImpl();
    texture->owned = WmtTexture::adopt(handle);
    texture->texture.tex = MtlObjectHandle(handle);
    texture->texture.generation = 0u;
    texture->format = info.format;
    texture->extent = info.extent;
    texture->gpuResourceId = texInfo.gpu_resource_id;
    texture->info = info;
    texture->m_info.type        = info.type;
    texture->m_info.format      = info.format;
    texture->m_info.extent      = info.extent;
    texture->m_info.mipLevels   = info.mipLevels ? info.mipLevels : 1u;
    texture->m_info.numLayers   = info.arrayLayers ? info.arrayLayers : 1u;
    texture->m_info.sampleCount = info.sampleCount ? info.sampleCount : 1u;
    texture->m_info.usage       = MtlTextureUsage(textureUsage(info.usage, info.format));
    texture->residencyDevice = this;
    texture->m_device = this;  // MtlTexture::createView (StretchRect, ColorFill)
    texture->bytes = textureBytes(info);
    memAdd(info.category, texture->bytes);
    m_residency->allocCreated(handle, texture->bytes);
    if (unlikely(m_labels))
      labelResource(handle, info.category, &info, texture->bytes);
    m_diagImagesCreated++;
    return texture;
  }

  Rc<MtlTextureView> MtlDeviceImpl::createImageView(
    const Rc<MtlTexture>& image, const MtlImageViewKey& key) {
    auto* impl = static_cast<MtlTextureImpl*>(image.ptr());

    WMTTextureSwizzleChannels swizzle;
    if (key.usage & MtlTextureUsageRenderTarget) {
      /* Metal silently strips RenderTarget usage from a view with a
       * non-identity swizzle, so attachments must stay identity. */
      swizzle.r = WMTTextureSwizzleRed;
      swizzle.g = WMTTextureSwizzleGreen;
      swizzle.b = WMTTextureSwizzleBlue;
      swizzle.a = WMTTextureSwizzleAlpha;
    } else {
      swizzle.r = WMTTextureSwizzle(key.swizzle.r);
      swizzle.g = WMTTextureSwizzle(key.swizzle.g);
      swizzle.b = WMTTextureSwizzle(key.swizzle.b);
      swizzle.a = WMTTextureSwizzle(key.swizzle.a);
    }

    MtlImageCreateInfo viewShape = impl->info;
    viewShape.arrayLayers = key.layerCount;
    if (viewShape.cube
     && (key.layerCount == 0u || key.layerCount % 6u != 0u))
      viewShape.cube = false;

    uint64_t gpuResourceId = 0u;
    obj_handle_t handle = MTLTexture_newTextureView(
      impl->owned.handle(),
      wmtFormat(key.format),
      wmtTextureType(viewShape),
      key.baseMip, key.mipCount,
      key.baseLayer, key.layerCount,
      swizzle,
      &gpuResourceId);

    if (!handle) {
      Logger::err("MtlDeviceImpl: createImageView failed");
      return nullptr;
    }

    Rc<MtlTextureViewImpl> view = new MtlTextureViewImpl();
    view->owned = WmtTexture::adopt(handle);
    view->view.tex = MtlObjectHandle(handle);
    view->key = key;
    view->image = image;
    view->gpuResourceId = gpuResourceId;
    view->residencyDevice = this;
    return view;
  }

  Rc<MtlSamplerState> MtlDeviceImpl::createSampler(const MtlSamplerKey& key) {
    { std::lock_guard<std::mutex> lock(m_samplerMutex);
      auto entry = m_samplerCache.find(key);
      if (entry != m_samplerCache.end())
        return entry->second;
    }

    WMTSamplerInfo samplerInfo = {};
    samplerInfo.min_filter = WMTSamplerMinMagFilter(key.minFilter);
    samplerInfo.mag_filter = WMTSamplerMinMagFilter(key.magFilter);
    samplerInfo.mip_filter = WMTSamplerMipFilter(key.mipMode);
    samplerInfo.s_address_mode = WMTSamplerAddressMode(key.addressU);
    samplerInfo.t_address_mode = WMTSamplerAddressMode(key.addressV);
    samplerInfo.r_address_mode = WMTSamplerAddressMode(key.addressW);
    samplerInfo.border_color = WMTSamplerBorderColor(key.borderColor);
    samplerInfo.compare_function = key.compareEnable
      ? WMTCompareFunction(key.compareOp)
      : WMTCompareFunctionNever;
    samplerInfo.lod_min_clamp = key.minLod;
    samplerInfo.lod_max_clamp = key.maxLod;
    samplerInfo.max_anisotroy = key.maxAnisotropy;
    samplerInfo.normalized_coords = true;
    samplerInfo.support_argument_buffers = true;

    obj_handle_t handle = MTLDevice_newSamplerState(m_device.handle(), &samplerInfo);

    if (!handle) {
      Logger::err("MtlDeviceImpl: createSampler failed");
      return nullptr;
    }

    Rc<MtlSamplerStateImpl> sampler = new MtlSamplerStateImpl();
    sampler->owned = WmtSampler::adopt(handle);
    sampler->sampler = MtlObjectHandle(handle);
    sampler->gpuResourceId = samplerInfo.gpu_resource_id;

    std::lock_guard<std::mutex> lock(m_samplerMutex);
    return m_samplerCache.emplace(key, sampler).first->second;
  }

  Rc<MtlAllocation> MtlDeviceImpl::renameBuffer(MtlBuffer* buffer) {
    auto* impl = static_cast<MtlBufferImpl*>(buffer);

    if (Rc<MtlAllocation> recycled = impl->takeRecycledStorage()) {
      m_diagBuffersRecycled++;
      return recycled;
    }

    WMTBufferInfo bufferInfo = {};
    bufferInfo.length = impl->size;
    bufferInfo.options = storageOptions(impl->info.cpuAccess);
    bufferInfo.memory.set(impl->info.cpuAccess != MtlCpuAccess::None
      ? hostAlloc(impl->size)
      : nullptr);

    obj_handle_t handle = MTLDevice_newBuffer(m_device.handle(), &bufferInfo);

    if (!handle) {
      Logger::err("MtlDeviceImpl: renameBuffer failed");
      return nullptr;
    }

    Rc<MtlAllocationImpl> storage = new MtlAllocationImpl();
    storage->owned = WmtBuffer::adopt(handle);
    storage->handle.buf = MtlObjectHandle(handle);
    storage->mapPtr = bufferInfo.memory.get();
    storage->gpuAddress = bufferInfo.gpu_address;
    storage->size = impl->size;
    storage->residencyDevice = this;
    storage->category = impl->info.category;
    memAdd(impl->info.category, impl->size);
    m_residency->allocCreated(handle, impl->size);
    if (unlikely(m_labels))
      labelResource(handle, impl->info.category, nullptr, impl->size);
    m_diagBuffersRenamed++;
    return storage;
  }

  // A Metal library from metallib bytes, or 0 if Metal rejects them (for example a newer
  // Metal language than this macOS supports).
  static obj_handle_t newLibraryFromBytes(obj_handle_t device, const void* metallib, size_t size) {
    obj_handle_t data = DispatchData_alloc_init(uint64_t(uintptr_t(metallib)), uint64_t(size));
    if (!data) {
      Logger::err("MtlDeviceImpl::createShaderFunction: DispatchData_alloc_init failed");
      return 0u;
    }
    obj_handle_t err = 0u;
    obj_handle_t library = MTLDevice_newLibrary(device, data, &err);
    NSObject_release(data);
    if (err)
      NSObject_release(err);
    return library;
  }

  Rc<MtlShaderFunction> MtlDeviceImpl::createShaderFunction(
    MtlShaderStage stage, const void* metallib, size_t size, const void* fallback, size_t fallbackSize) {
    if (!metallib || !size)
      return nullptr;

    obj_handle_t library = newLibraryFromBytes(m_device.handle(), metallib, size);
    if (!library && fallback && fallbackSize) {
      static std::atomic<bool> logged = { false };
      if (!logged.exchange(true))
        Logger::info("MtlDeviceImpl::createShaderFunction: embedded metallib not supported by this macOS, using its Compat build");
      library = newLibraryFromBytes(m_device.handle(), fallback, fallbackSize);
    }

    if (!library) {
      Logger::err("MtlDeviceImpl::createShaderFunction: newLibraryWithData failed");
      return nullptr;
    }

    auto fn = new MtlShaderFunction();
    fn->stage   = stage;
    fn->library = MtlObjectHandle(library);
    return fn;
  }

  Rc<MtlContext> MtlDeviceImpl::createContext() {
    return new MtlContextImpl(Rc<MtlDeviceImpl>(this));
  }

  Rc<MtlQuery> MtlDeviceImpl::createGpuQuery(MtlQueryType type) {
    Rc<MtlBackendQuery> q = new MtlBackendQuery();
    q->type = type;
    return q;
  }

  Rc<MtlEvent> MtlDeviceImpl::createGpuEvent() {
    return new MtlBackendEvent();
  }

  // Field by field: the padding after the bools isn't initialized by the frontend.
  static void dssWords(const MtlDepthStencilState& ds, uint32_t (&w)[16]) {
    const MtlStencilState* s[2] = { &ds.front, &ds.back };
    w[0] = uint32_t(ds.depthCompareOp);
    w[1] = uint32_t(ds.depthTest) | uint32_t(ds.depthWrite) << 1 | uint32_t(ds.stencilEnable) << 2;
    for (uint32_t i = 0u; i < 2u; i++) {
      w[2 + 6 * i] = uint32_t(s[i]->failOp);
      w[3 + 6 * i] = uint32_t(s[i]->depthFailOp);
      w[4 + 6 * i] = uint32_t(s[i]->passOp);
      w[5 + 6 * i] = uint32_t(s[i]->compareOp);
      w[6 + 6 * i] = s[i]->readMask;
      w[7 + 6 * i] = s[i]->writeMask;
    }
    w[14] = w[15] = 0u;
  }

  size_t MtlDeviceImpl::DssHash::operator () (const MtlDepthStencilState& ds) const {
    uint32_t w[16];
    dssWords(ds, w);
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint32_t x : w)
      h = (h ^ x) * 0x100000001b3ull;
    return size_t(h ^ (h >> 32));
  }

  bool MtlDeviceImpl::DssEq::operator () (const MtlDepthStencilState& a, const MtlDepthStencilState& b) const {
    uint32_t wa[16], wb[16];
    dssWords(a, wa);
    dssWords(b, wb);
    return !std::memcmp(wa, wb, sizeof(wa));
  }

  obj_handle_t MtlDeviceImpl::getDepthStencilState(const MtlDepthStencilState& ds) {
    { std::lock_guard<std::mutex> lock(m_dssoMutex);
      auto entry = m_dssoCache.find(ds);
      if (entry != m_dssoCache.end())
        return entry->second.handle();
    }

    if (d9mtTrace())
      Logger::info(str::format("DSS new ztest=", ds.depthTest ? 1 : 0,
        " zfunc=", uint32_t(ds.depthCompareOp), " zwrite=", ds.depthWrite ? 1 : 0,
        " stencil=", ds.stencilEnable ? 1 : 0,
        " F(func=", uint32_t(ds.front.compareOp), " pass=", uint32_t(ds.front.passOp),
        " zfail=", uint32_t(ds.front.depthFailOp), ")",
        " B(func=", uint32_t(ds.back.compareOp), " pass=", uint32_t(ds.back.passOp),
        " zfail=", uint32_t(ds.back.depthFailOp), ")"));

    WMTDepthStencilInfo info = {};
    info.depth_compare_function = WMTCompareFunction(ds.depthTest
      ? ds.depthCompareOp : MtlCompareFunction::Always);
    info.depth_write_enabled    = ds.depthTest && ds.depthWrite;

    auto fillStencil = [&](WMTStencilInfo& s, const MtlStencilState& m) {
      s.enabled                  = ds.stencilEnable;
      s.depth_stencil_pass_op    = WMTStencilOperation(m.passOp);
      s.stencil_fail_op          = WMTStencilOperation(m.failOp);
      s.depth_fail_op            = WMTStencilOperation(m.depthFailOp);
      s.stencil_compare_function = WMTCompareFunction(m.compareOp);
      s.write_mask               = uint8_t(m.writeMask);
      s.read_mask                = uint8_t(m.readMask);
    };
    fillStencil(info.front_stencil, ds.front);
    fillStencil(info.back_stencil,  ds.back);

    obj_handle_t handle = MTLDevice_newDepthStencilState(m_device.handle(), &info);

    std::lock_guard<std::mutex> lock(m_dssoMutex);
    return m_dssoCache.emplace(ds, WmtDepthStencil::adopt(handle)).first->second.handle();
  }

  uint64_t MtlDeviceImpl::getCompletedSerial() {
    return m_tracker ? m_tracker->completedSerial() : 0u;
  }

  // Here, not in mtl_submission.cpp: renamed storage goes back through MtlBufferImpl.
  void RetireBatch::run() {
    for (auto& r : recycles) {
      static_cast<MtlBufferImpl*>(r.first.ptr())->recycleStorage(std::move(r.second));
      r.first = nullptr;
    }
    recycles.clear();
    for (obj_handle_t o : objects)
      NSObject_release(o);
    objects.clear();
    for (auto& s : signals)
      s.first->signal(s.second);
    signals.clear();
    initCmdbuf.reset();
    cmdbuf.reset();
    keepAlive.clear();
  }

  bool MtlDeviceImpl::isSerialComplete(uint64_t serial) {
    return m_tracker ? m_tracker->isComplete(serial) : serial == 0u;
  }

  uint64_t MtlDeviceImpl::getNextSerial() {
    return m_tracker ? m_tracker->nextSerial() : 1u;
  }

  void MtlDeviceImpl::waitForIdle() {
    if (m_tracker)
      m_tracker->waitFor(m_tracker->lastSubmittedSerial());
  }

  void MtlDeviceImpl::waitForResource(const MtlResource& resource) {
    if (m_tracker)
      m_tracker->waitFor(resource.trackingSerial());
  }

  bool MtlDeviceImpl::supportsFormat(MtlFormat format, MtlUsage usage) const {
    return format != MtlFormat::Invalid;
  }


}

namespace dxvk {

  Rc<MtlDevice> backendCreateDevice(const MtlMemoryProfile& profile) {
    Rc<backend::MtlDeviceImpl> device = new backend::MtlDeviceImpl(profile);
    return device->valid() ? Rc<MtlDevice>(device) : nullptr;
  }

}
