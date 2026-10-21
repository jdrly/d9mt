#include "mtl_context_impl.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <tuple>

#include "../util/log/log.h"
#include "../util/util_string.h"
#include "../util/util_d9mt_debug.h"

#include "mtl_profiler.h"

namespace dxvk::backend {

  namespace {
    WMTPrimitiveType wmtPrimitive(MtlPrimitiveTopology t) {
      switch (t) {
        case MtlPrimitiveTopology::PointList:     return WMTPrimitiveTypePoint;
        case MtlPrimitiveTopology::LineList:      return WMTPrimitiveTypeLine;
        case MtlPrimitiveTopology::LineStrip:     return WMTPrimitiveTypeLineStrip;
        case MtlPrimitiveTopology::TriangleStrip: return WMTPrimitiveTypeTriangleStrip;
        default:
        case MtlPrimitiveTopology::TriangleList:  return WMTPrimitiveTypeTriangle;
      }
    }
  }

  MtlContextImpl::MtlContextImpl(Rc<MtlDeviceImpl> device)
  : MtlContext(device), m_device(std::move(device)),
    m_psoCache(m_device->wmtCompiler(), m_device.ptr(), &m_device->m_psoRegistry) {
    // D9MT_ASYNC_SKIP=1 wins and is the same as a wait budget of 0.
    const int32_t budgetMs = m_device->memoryProfile().pipelineWaitBudgetMs;
    const bool workers = m_device->m_compileWorkers->maxThreads() != 0u;

    if (const char* v = std::getenv("D9MT_ASYNC_SKIP"); v && v[0] == '1') {
      m_asyncSkip = workers;
      Logger::warn(str::format("D9MT: async skip ", m_asyncSkip ? "on" : "ignored (no compile workers)"));
    } else if (budgetMs == 0) {
      m_asyncSkip = workers;
      Logger::warn(str::format("D9MT: pipeline wait budget 0 ms = async skip ",
        m_asyncSkip ? "on" : "ignored (no compile workers)"));
    } else if (budgetMs > 0) {
      if (workers)
        m_waitBudget.setBudgetMs(budgetMs);
      Logger::warn(str::format("D9MT: pipeline wait budget ", budgetMs, " ms per frame",
        workers ? "" : " ignored (no compile workers)"));
    }

    m_attOpt = m_device->memoryProfile().attachmentOpt && !deferredPassesEnabled();
    if (m_attOpt)
      Logger::info("D9MT: attachment load/store optimization on");
    else
      Logger::warn(str::format("D9MT: attachment load/store optimization off (",
        deferredPassesEnabled() ? "D9MT_DEFERRED" : "D9MT_NO_ATTACHMENT_OPT / d9mt.attachmentOpt", ")"));

    const char* clearDiscard = std::getenv("D9MT_CLEAR_DISCARD");
    m_clearDiscard = m_attOpt && !(clearDiscard && std::strcmp(clearDiscard, "0") == 0);
    Logger::info(m_clearDiscard
      ? "D9MT: a clear that ends a pass skips the pass's store of what it clears"
      : "D9MT: a clear that ends a pass keeps the pass's stores (D9MT_CLEAR_DISCARD=0 or no attachment optimization)");

    const char* passBarrier = std::getenv("D9MT_PASS_BARRIER");
    m_narrowBarrier = m_attOpt && !(passBarrier && std::strcmp(passBarrier, "all") == 0);
    Logger::info(m_narrowBarrier
      ? "D9MT: end-of-pass barrier before Fragment|Blit|Dispatch, vertex fences for vertex textures"
      : "D9MT: end-of-pass barrier before all stages (D9MT_PASS_BARRIER=all or no attachment optimization)");

    obj_handle_t dev = m_device->wmtDevice();

    WMTArgTableInfo ati = {};
    ati.max_buffers                = 31u;
    ati.max_textures               = 16u;
    ati.max_samplers               = 16u;
    ati.initialize_bindings        = 1u;
    ati.support_attribute_strides  = 1u;
    ArgTablePair pair;
    pair.table[0] = WmtArgTable::adopt(MTLDevice_newArgumentTable(dev, &ati));
    pair.table[1] = WmtArgTable::adopt(MTLDevice_newArgumentTable(dev, &ati));
    m_argTables.push_back(std::move(pair));

    m_allocators.push_back(WmtAllocator::adopt(MTLDevice_newCommandAllocator(dev)));
    m_initAllocators.emplace_back();
    m_allocatorSerial.push_back(0u);
    m_allocator     = m_allocators[0].handle();
  }

  MtlContextImpl::~MtlContextImpl() {
    if (m_pass.active)
      closePass();
    for (auto& kv : m_fxScalers)
      NSObject_release(kv.second.scaler);
    if (m_visBuffer) {
      m_device->residency().allocDestroyed(m_visBuffer.detach(), m_visCpu,
        m_device->submissions().nextSerial(), uint64_t(kVisSlots) * 8u);
    }
  }


  void MtlContextImpl::beginRecording() {
    prof::Scope profScope(prof::BeginRecording);
    m_dirty = DirtyEverything;

    /* A command buffer opened before this call (lazily, by ensureCommandBuffer)
     * is already encoding from m_allocator: switching slots now would stamp
     * the wrong slot's serial at submit and let its allocator be reset early. */
    if (m_cmdbuf)
      return;

    /* Reset requires the slot's prior command buffers to have COMPLETED
     * (MTL4CommandAllocator contract). Prefer any retired slot, grow the
     * pool when none is free, and only block once the pool is capped. */
    const uint64_t completed = m_device->submissions().completedSerial();
    const uint32_t n = uint32_t(m_allocators.size());
    uint32_t slot = ~0u;
    for (uint32_t i = 0u; i < n && slot == ~0u; i++) {
      uint32_t s = (m_allocatorCursor + i) % n;
      if (s != m_allocatorSlot && m_allocatorSerial[s] <= completed)
        slot = s;
    }
    bool fresh = false;
    if (slot == ~0u && n < std::clamp(m_device->memoryProfile().allocatorCap, 2u, kMaxAllocators)) {
      m_allocators.push_back(WmtAllocator::adopt(
        MTLDevice_newCommandAllocator(m_device->wmtDevice())));
      m_initAllocators.emplace_back();
      m_allocatorSerial.push_back(0u);
      slot = n;
      fresh = true;
    }
    if (slot == ~0u) {
      slot = m_allocatorCursor % n;
      if (slot == m_allocatorSlot)
        slot = (slot + 1u) % n;
      m_device->submissions().waitFor(m_allocatorSerial[slot]);
    }
    if (!fresh) {
      MTL4CommandAllocator_reset(m_allocators[slot].handle());
      if (m_initAllocators[slot])
        MTL4CommandAllocator_reset(m_initAllocators[slot].handle());
    }
    m_allocatorCursor = slot + 1u;
    m_allocator = m_allocators[slot].handle();
    m_allocatorSlot = slot;

    ensureCommandBuffer();
  }

  void MtlContextImpl::ensureCommandBuffer() {
    if (m_cmdbuf)
      return;

    m_cmdbuf = WmtCommandBuffer::adopt(
      MTLDevice_newMTL4CommandBuffer(m_device->wmtDevice()));
    MTL4CommandBuffer_beginCommandBuffer(m_cmdbuf.handle(), m_allocator);
    m_currentSerial = m_device->submissions().nextSerial();
  }

  static int  s_gpuCaptureRemaining = -2;
  static bool s_gpuCaptureTrigger   = false;
  static int  s_gpuCaptureCommits   = 3;

  static void gpuCaptureStart(obj_handle_t device, int commits) {
    obj_handle_t mgr = MTLCaptureManager_sharedCaptureManager();
    WMTCaptureInfo ci = {};
    ci.capture_object = device;
    ci.destination = WMTCaptureDestinationGPUTraceDocument;
    ci.output_url.set("/tmp/d9mt.gputrace");
    bool ok = MTLCaptureManager_startCapture(mgr, &ci);
    s_gpuCaptureRemaining = commits;
    Logger::info(str::format("D9MT-GPUCAPTURE start ok=", ok,
      " commits=", commits, " -> /tmp/d9mt.gputrace"));
  }

  static void gpuCaptureBeforeCommit(obj_handle_t device) {
    if (s_gpuCaptureRemaining == -2) {
      const char* v = std::getenv("D9MT_GPUCAPTURE");
      if (v && std::strcmp(v, "trigger") == 0) {
        s_gpuCaptureTrigger = true;
        s_gpuCaptureRemaining = 0;
        const char* n = std::getenv("D9MT_GPUCAPTURE_COMMITS");
        if (n && *n && std::atoi(n) > 0)
          s_gpuCaptureCommits = std::atoi(n);
        Logger::info(str::format("D9MT-GPUCAPTURE armed (touch C:\\d9mt-test\\GPUCAP, ",
          s_gpuCaptureCommits, " commits)"));
      } else {
        s_gpuCaptureRemaining = (v && *v) ? std::atoi(v) : -1;
        if (s_gpuCaptureRemaining > 0)
          gpuCaptureStart(device, s_gpuCaptureRemaining);
      }
      return;
    }
    if (s_gpuCaptureTrigger && s_gpuCaptureRemaining == 0) {
      std::FILE* f = std::fopen("C:\\d9mt-test\\GPUCAP", "rb");
      if (f) {
        std::fclose(f);
        std::remove("C:\\d9mt-test\\GPUCAP");
        Logger::info("D9MT-GPUCAPTURE trigger hit — starting capture");
        gpuCaptureStart(device, s_gpuCaptureCommits);
        Logger::info("D9MT-GPUCAPTURE startCapture returned");
      }
    }
  }

  static void gpuCaptureAfterCommit() {
    if (s_gpuCaptureRemaining > 0 && --s_gpuCaptureRemaining == 0) {
      MTLCaptureManager_stopCapture(MTLCaptureManager_sharedCaptureManager());
      Logger::info("D9MT-GPUCAPTURE stopped");
    }
  }

  void MtlContextImpl::submit(bool waitCompleted) {
    prof::Scope profScope(prof::Submit);
    if (m_pass.active) {
      prof::bump(prof::CntPassCloseSubmit);
      closePass();
    }

    flushFramePasses();

    if (!m_cmdbuf)
      return;

    MTL4CommandBuffer_endCommandBuffer(m_cmdbuf.handle());

    obj_handle_t cbs[2] = { };
    uint32_t     cbCount = 0u;
    if (m_initCmdbuf) {
      /* Hoisted uploads run first; the producer barrier orders them ahead of
       * every later queue stage, including the main buffer's vertex fetch. */
      emitProducerBarrier(m_initEncoder, uint64_t(WMTStageBlit | WMTStageDispatch));
      MTLCommandEncoder_endEncoding(m_initEncoder);
      NSObject_release(m_initEncoder);
      m_initEncoder = 0u;
      MTL4CommandBuffer_endCommandBuffer(m_initCmdbuf.handle());
      cbs[cbCount++] = m_initCmdbuf.handle();
    }
    cbs[cbCount++] = m_cmdbuf.handle();

    m_device->residency().beforeSubmit();

    gpuCaptureBeforeCommit(m_device->wmtDevice());
    MTL4CommandQueue_commit(m_device->wmtQueue(), cbs, cbCount);
    gpuCaptureAfterCommit();
    if (unlikely(prof::g_profEnabled))
      m_rs[rstats::CmdBufs] += cbCount;

    uint64_t serial = m_device->submissions().onSubmit(m_device->wmtQueue());
    m_allocatorSerial[m_allocatorSlot] = serial;

    if (m_initCmdbuf) {
      auto doneInit = std::make_shared<WmtCommandBuffer>(std::move(m_initCmdbuf));
      m_initCmdbuf.reset();
      m_device->submissions().retireAfter(serial,
        [doneInit]() mutable { doneInit->reset(); });
    }

    m_device->uploadRing().onSubmit(serial);

    if (!m_keepAlive.empty()) {
      auto keepAlive = std::make_shared<std::vector<Rc<MtlResource>>>(
        std::move(m_keepAlive));
      for (auto& res : *keepAlive)
        res->setTrackingSerial(serial);
      m_device->submissions().retireAfter(serial,
        [keepAlive]() mutable { keepAlive->clear(); });
      m_keepAlive.clear();
    }

    auto doneCmd = std::make_shared<WmtCommandBuffer>(std::move(m_cmdbuf));
    m_device->submissions().retireAfter(serial,
      [doneCmd]() mutable { doneCmd->reset(); });

    if (waitCompleted)
      m_device->submissions().waitFor(serial);

    m_cmdbuf.reset();
    m_dirty = DirtyEverything;

    // Ring slices are only valid in the command buffer that allocated them
    // (UploadRing retires a chunk once its last allocating submission has
    // completed). Push constants that didn't change would otherwise keep
    // binding the old slice in later command buffers: upload them again.
    for (auto& b : m_bindings) {
      if (b.pushExtent)
        b.pushDirtyHi = b.pushExtent;  // the whole extent is uploaded anyway
      b.abGpuAddress = 0u;
    }

    const uint64_t completed = m_device->submissions().completedSerial();
    m_device->submissions().processRetirements();
    m_device->residency().serialRetired(completed);
    m_device->uploadRing().onComplete(completed);
  }

  void MtlContextImpl::flushCommandList(const char* reason, MtlSubmitStatus* status) {
    submit(false);
    beginRecording();
    if (status) {
      status->submissionId = m_currentSerial;
      status->completed    = false;
    }
  }

  void MtlContextImpl::endFrame() {
    m_frameBoundaryFlush = true;
    m_attFrame++;

    uint64_t completed = m_device->submissions().completedSerial();
    {
      prof::Scope profScope(prof::EndFrame);
      m_device->submissions().processRetirements();
      m_device->residency().serialRetired(completed);
      m_device->uploadRing().onComplete(completed);
      m_device->uploadRing().onFrame();
    }
    prof::onFrameEnd();
    if (unlikely(prof::g_profEnabled)) {
      m_rs[rstats::Frames]++;
      m_device->renderStats().publish(m_rs);
    }
    compile_stats::onFrameEnd(compile_stats::g_enabled ? m_device->pendingCompiles() : 0u);
    m_waitBudget.onFrameEnd();

    static uint64_t s_diagFrame = 0u;
    if (const uint32_t memEvery = m_device->memStatsInterval(); unlikely(memEvery)
     && (s_diagFrame + 1u) % memEvery == 0u)
      Logger::warn(m_device->memStatsLine(s_diagFrame + 1u));
    if ((++s_diagFrame & 0xFFu) == 0u) {
      Logger::info(str::format("DIAG frame=", s_diagFrame,
        " residencyNet=", m_device->residency().netResident(),
        " adds=", m_device->residency().totalAdds(),
        " removes=", m_device->residency().totalRemoves(),
        " pending=", m_device->residency().pendingRemovals(),
        " argTables=", m_argTables.size(),
        " retirements=", m_device->submissions().retirementCount(),
        " serial=", m_device->submissions().lastSubmittedSerial(),
        " completed=", completed,
        " bufNew=", m_device->diagBuffersCreated(),
        " bufRename=", m_device->diagBuffersRenamed(),
        " bufRecycle=", m_device->diagBuffersRecycled(),
        " imgNew=", m_device->diagImagesCreated(),
        " vbStale=", s_diagVbStale,
        " cbvStale=", s_diagCbvStale));
    }
  }

  void MtlContextImpl::synchronizeWsi(MtlPresenterSync sync) {
    prof::Scope profScope(prof::SynchronizeWsi);
    if (sync.drawable == MtlNullHandle) {
      Logger::warn("MtlContextImpl: synchronizeWsi without drawable");
      return;
    }

    if (m_pass.active) {
      prof::bump(prof::CntPassCloseSubmit);
      closePass();
    }
    ensureCommandBuffer();

    MTL4CommandQueue_waitForDrawable(m_device->wmtQueue(), sync.drawable);
    submit(false);

    MTL4CommandQueue_signalDrawable(m_device->wmtQueue(), sync.drawable);
    if (sync.minDuration > 0.0)
      MetalDrawable_presentAfterMinimumDuration(sync.drawable, sync.minDuration);
    else
      MetalDrawable_present(sync.drawable);
    NSObject_release(sync.drawable);
  }


  static bool sameAttachment(const Rc<MtlTextureView>& a,
                             const Rc<MtlTextureView>& b);

  void MtlContextImpl::bindRenderTargets(MtlRenderTargets&& targets, MtlFeedbackLoopMask feedbackLoop) {
    static const bool rtSkipOn = std::getenv("D9MT_RTSKIP") != nullptr;
    static const bool noSameViewSkipEnv = std::getenv("D9MT_NO_SAMEVIEW_SKIP") != nullptr;
    const bool noSameViewSkip = noSameViewSkipEnv
      || (d9mtdbg::flags() & d9mtdbg::NoSameViewSkip);

    /* DXVK semantics (DxvkContext::bindRenderTargets): re-binding the exact
     * same view objects is not a framebuffer change and must not end the
     * pass. The D3D9 frontend re-emits identical targets on every
     * Framebuffer-dirty event (Clear(), COLORWRITEENABLE / ZWRITEENABLE /
     * shader changes, redundant SetRenderTarget, ...). Everything that needs
     * a split still gets one:
     *  - read-only vs writable depth (GetDepthStencilView(bool)) and sRGB vs
     *    linear color are distinct view objects -> not "same";
     *  - the feedback-loop mask is compared;
     *  - clears, copies, blits, MSAA resolves (resolveTexture /
     *    resolveDepthStencil) and partial clears end the pass themselves
     *    (clearRenderTarget / prepareDirectEncode);
     *  - occlusion queries need no split: the visibility buffer is attached
     *    to every encoder and Begin/End re-arm the mode inside the pass.
     * Pending (not yet materialized) clears stay pending and fold into the
     * load action; every consumer of those targets flushes them first. */
    if (!noSameViewSkip && (m_pass.active || m_pass.pending)
     && uint32_t(feedbackLoop) == m_passFeedbackLoop
     && m_pass.targets.depthStencil == targets.depthStencil) {
      bool same = true;
      for (uint32_t i = 0u; same && i < MtlRenderTargets::MaxColorTargets; i++)
        same = m_pass.targets.color[i] == targets.color[i];
      if (same)
        return;
    }
    m_passFeedbackLoop = uint32_t(feedbackLoop);

    if (m_pass.active) {
      if (rtSkipOn) {
        auto sameRt = [](const Rc<MtlTextureView>& a, const Rc<MtlTextureView>& b) {
          if (!sameAttachment(a, b))
            return false;
          return a == nullptr || b == nullptr || a->key.format == b->key.format;
        };
        bool same = sameRt(m_pass.targets.depthStencil, targets.depthStencil);
        for (uint32_t i = 0u; same && i < MtlRenderTargets::MaxColorTargets; i++)
          same = sameRt(m_pass.targets.color[i], targets.color[i]);
        if (same)
          return;
      }
      prof::bump(prof::CntPassCloseRt);
      closePass();
    }
    else
      flushPendingClears();

    m_pass = PendingPass();
    m_pass.targets = std::move(targets);
    m_pass.pending = true;
    m_dirty |= DirtyPso | DirtyViewport;
  }

  static bool sameAttachment(const Rc<MtlTextureView>& a,
                             const Rc<MtlTextureView>& b) {
    if (a == b)                     return true;
    if (a == nullptr || b == nullptr) return false;
    return a->image.ptr()  == b->image.ptr()
        && a->key.baseMip  == b->key.baseMip
        && a->key.baseLayer == b->key.baseLayer;
  }

  void MtlContextImpl::clearRenderTarget(
    const Rc<MtlTextureView>& view, MtlAspectMask clear,
    MtlClearValue value, MtlAspectMask discard) {
    if (d9mtTrace() && (clear & MtlAspectColor))
      Logger::info(str::format("CLR color img=",
        (void*)(view ? view->image.ptr() : nullptr),
        " rgba=", value.color.r, ",", value.color.g, ",", value.color.b));
    static const bool noColorClear = std::getenv("D9MT_NOCOLORCLEAR") != nullptr;
    if (noColorClear && (clear & MtlAspectColor)) {
      clear = MtlAspectMask(clear & ~MtlAspectColor);
      if (!clear) return;
    }
    /* A same-view rebind (e.g. the one D3D9 Clear() emits) no longer ends
     * the pass, so an active pass must end here: the clear has to land after
     * the draws already recorded, either folded into the next load action or
     * as its own pass (never a second encoder while this one is open). */
    if (m_pass.active) {
      // The clear is the next command on the image: the pass needn't store
      // the aspects it overwrites. Per aspect: clearing depth alone keeps the
      // stencil store. Only images the attachment optimization tracks (one
      // subresource) take it (attopt::decide).
      if (m_clearDiscard && view != nullptr) {
        uint8_t mask = 0u;
        for (uint32_t i = 0; i < MtlRenderTargets::MaxColorTargets; i++) {
          if ((clear & MtlAspectColor) && sameAttachment(m_pass.targets.color[i], view))
            mask |= uint8_t(1u << i);
        }
        if (sameAttachment(m_pass.targets.depthStencil, view)) {
          if (clear & MtlAspectDepth)   mask |= uint8_t(1u << kAttDepth);
          if (clear & MtlAspectStencil) mask |= uint8_t(1u << kAttStencil);
        }
        m_pass.discardMask |= mask;
      }
      prof::bump(prof::CntPassCloseClear);
      closePass();
    }
    if (unlikely(prof::g_profEnabled))
      m_splitClears++;
    if (m_pass.pending) {
      for (uint32_t i = 0; i < MtlRenderTargets::MaxColorTargets; i++) {
        if (sameAttachment(m_pass.targets.color[i], view) && (clear & MtlAspectColor)) {
          m_pass.clearColor[i] = true;
          m_pass.clearValues[i] = value;
          if (unlikely(prof::g_profEnabled))
            m_rs[rstats::ClearsFolded]++;
          return;
        }
      }
      if (sameAttachment(m_pass.targets.depthStencil, view)) {
        if (clear & MtlAspectDepth)   { m_pass.clearDepth = true;   m_pass.depthClear = value.depth; }
        if (clear & MtlAspectStencil) { m_pass.clearStencil = true; m_pass.stencilClear = value.stencil; }
        if (unlikely(prof::g_profEnabled))
          m_rs[rstats::ClearsFolded]++;
        return;
      }
    }

    PendingPass saved = std::move(m_pass);
    m_pass = PendingPass();
    if (clear & MtlAspectColor) {
      m_pass.targets.color[0] = view;
      m_pass.clearColor[0] = true;
      m_pass.clearValues[0] = value;
    } else {
      m_pass.targets.depthStencil = view;
      if (clear & MtlAspectDepth)   { m_pass.clearDepth = true;   m_pass.depthClear = value.depth; }
      if (clear & MtlAspectStencil) { m_pass.clearStencil = true; m_pass.stencilClear = value.stencil; }
    }
    m_pass.pending = true;
    prof::bump(prof::CntPassCloseClear);
    if (unlikely(prof::g_profEnabled))
      m_rs[rstats::ClearPasses]++;
    materializePass();
    closePass();
    m_pass = std::move(saved);
  }

  void MtlContextImpl::flushPendingClears() {
    if (m_pass.active || !m_pass.pending)
      return;
    bool any = m_pass.clearDepth || m_pass.clearStencil;
    for (bool c : m_pass.clearColor)
      any = any || c;
    if (!any)
      return;
    prof::bump(prof::CntPassCloseClear);
    if (unlikely(prof::g_profEnabled))
      m_rs[rstats::ClearPasses]++;
    materializePass();
    closePass();
  }

  void MtlContextImpl::materializePass() {
    if (m_pass.active)
      return;

    ensureCommandBuffer();
    ensureVisBuffer();

    if (deferredPassesEnabled()) {
      m_pass.encoder = 0;
      m_pass.active  = true;
      m_pass.pending = false;
      m_pass.encodedDraws = 0u;
      m_dirty = DirtyEverything;
      m_argTablesBound = false;
      m_lastEncodedPso = nullptr;
      m_dynValid       = false;
      armVisibility();
      return;
    }

    if (m_attOpt) {
      // Load and store actions are chosen when the pass ends (createPassEncoder).
      m_pass.encoder      = 0;
      m_pass.active       = true;
      m_pass.pending      = false;
      m_pass.encodedDraws = 0u;
      m_pass.writtenMask  = 0u;
      m_pass.discardDs    = false;
      m_pass.discardMask  = 0u;
      m_pass.epoch        = ++m_attEpoch;
      if (unlikely(prof::g_profEnabled)) {
        m_pass.openSerial = m_currentSerial;
        m_pass.openXfers  = m_splitXfers;
        m_pass.openClears = m_splitClears;
      }
      m_dirty = DirtyEverything;
      m_argTablesBound = false;
      m_lastEncodedPso = nullptr;
      m_dynValid       = false;
      armVisibility();
      return;
    }

    const bool count = prof::g_profEnabled;
    WMTRenderPassInfo info = {};
    info.visibility_buffer = m_visBuffer.handle();
    for (uint32_t i = 0; i < MtlRenderTargets::MaxColorTargets; i++) {
      const auto& view = m_pass.targets.color[i];
      if (view == nullptr)
        continue;
      auto* impl = static_cast<MtlTextureViewImpl*>(view.ptr());
      info.colors[i].texture = impl->owned.handle();
      info.colors[i].load_action = m_pass.clearColor[i]
        ? WMTLoadActionClear : WMTLoadActionLoad;
      info.colors[i].store_action = WMTStoreActionStore;
      if (unlikely(count)) {
        m_rs[m_pass.clearColor[i] ? rstats::LoadClear : rstats::LoadLoad]++;
        m_rs[rstats::StoreStore]++;
      }
      if (d9mtTrace() && i == 0)
        Logger::info(str::format("MAT color img=", (void*)impl->image.ptr(),
          " load=", m_pass.clearColor[i] ? "CLEAR" : "LOAD"));
      if (m_pass.clearColor[i]) {
        const auto& v = m_pass.clearValues[i];
        info.colors[i].clear_color = { v.color.r, v.color.g, v.color.b, v.color.a };
      }
    }

    if (m_pass.targets.depthStencil != nullptr) {
      auto* impl = static_cast<MtlTextureViewImpl*>(m_pass.targets.depthStencil.ptr());
      info.depth.texture = impl->owned.handle();
      info.depth.load_action = m_pass.clearDepth ? WMTLoadActionClear : WMTLoadActionLoad;
      info.depth.store_action = WMTStoreActionStore;
      info.depth.clear_depth = m_pass.depthClear;
      if (unlikely(count)) {
        m_rs[m_pass.clearDepth ? rstats::LoadClear : rstats::LoadLoad]++;
        m_rs[rstats::StoreStore]++;
      }
      const auto* fmtInfo = lookupFormatInfo(impl->key.format);
      if (fmtInfo->flags & MtlFormatFlagStencil) {
        info.stencil.texture = impl->owned.handle();
        info.stencil.load_action = m_pass.clearStencil ? WMTLoadActionClear : WMTLoadActionLoad;
        info.stencil.store_action = WMTStoreActionStore;
        info.stencil.clear_stencil = m_pass.stencilClear;
        if (unlikely(count)) {
          m_rs[m_pass.clearStencil ? rstats::LoadClear : rstats::LoadLoad]++;
          m_rs[rstats::StoreStore]++;
        }
      }
    }

    m_pass.encoder = newRenderEncoder(m_cmdbuf.handle(), &info);
    m_pass.active = true;
    m_pass.pending = false;
    m_dirty = DirtyEverything;
    m_argTablesBound = false;
    m_lastEncodedPso = nullptr;
    m_dynValid       = false;
    armVisibility();
  }

  MtlContextImpl::ArgTablePair MtlContextImpl::createArgumentTablePair() const {
    WMTArgTableInfo ati = {};
    ati.max_buffers                = 31u;
    ati.max_textures               = 16u;
    ati.max_samplers               = 16u;
    ati.initialize_bindings        = 1u;
    ati.support_attribute_strides  = 1u;

    ArgTablePair pair;
    pair.table[0] = WmtArgTable::adopt(MTLDevice_newArgumentTable(m_device->wmtDevice(), &ati));
    pair.table[1] = WmtArgTable::adopt(MTLDevice_newArgumentTable(m_device->wmtDevice(), &ati));
    return pair;
  }

  void MtlContextImpl::selectArgumentTablesForPass() {
    /* Metal snapshots an argument table's contents at each encoded draw, so
     * one pair can be mutated and reused for every draw in a pass; rotation
     * only needs to happen at the pass boundary. Growth is always by
     * push_back at the end, never insert-at-cursor — inserting scrambles the
     * per-slot serials so the round-robin never lands on a retired slot and
     * the pool grows unbounded. */
    prof::bump(prof::CntArgRotate);
    const uint64_t completed = m_device->submissions().completedSerial();
    const size_t   n         = m_argTables.size();

    for (size_t scan = 0u; scan < n; ++scan) {
      const size_t idx = (m_argTableCursor + scan) % n;
      if (m_argTables[idx].serial <= completed) {
        m_currentArgTable       = idx;
        m_argTableCursor        = idx + 1u;
        m_argTables[idx].serial = m_currentSerial;
        return;
      }
    }

    m_argTables.push_back(createArgumentTablePair());
    m_currentArgTable       = m_argTables.size() - 1u;
    m_argTableCursor        = m_currentArgTable + 1u;
    m_argTables[m_currentArgTable].serial = m_currentSerial;
  }

  void MtlContextImpl::bindArgumentTables() {
    ArgTablePair& pair = m_argTables[m_currentArgTable];
    auto* vs = m_cmdChunk.append<wmtcmd_mtl4_setargtable>(
      uint16_t(WMTMtl4RenderCommandSetArgumentTable));
    vs->table  = pair.table[0].handle();
    vs->stages = uint64_t(WMTRenderStageVertex);
    auto* fs = m_cmdChunk.append<wmtcmd_mtl4_setargtable>(
      uint16_t(WMTMtl4RenderCommandSetArgumentTable));
    fs->table  = pair.table[1].handle();
    fs->stages = uint64_t(WMTRenderStageFragment);
  }

  void MtlContextImpl::flushCmdChunk() {
    if (m_cmdChunk.empty())
      return;
    if (!m_pass.encoder) {
      // Commands must be encoded before the pass ends (a full chunk or a
      // barrier): decide now, without skipping stores.
      if (!m_attOpt || !m_pass.active)
        return;
      createPassEncoder(true);
    }
    prof::Scope profScope(prof::FlushCmdChunk);
    prof::bump(prof::CntChunkFlush);
    resolvePsoPatches();
    MTL4RenderCommandEncoder_encodeCommands(m_pass.encoder, m_cmdChunk.head());
    m_cmdChunk.reset();
  }

  void MtlContextImpl::closePass() {
    if (!m_pass.active)
      return;

    if (deferredPassesEnabled()) {
      recordPassEntry();
      return;
    }

    if (m_attOpt) {
      const bool encoded = m_pass.encoder != 0 || createPassEncoder(false);
      if (encoded) {
        flushCmdChunk();
        if (unlikely(prof::g_profEnabled))
          splitNoteClosed();
        if (m_narrowBarrier) {
          emitProducerBarrier(m_pass.encoder, uint64_t(WMTStageFragment),
            uint64_t(WMTStageFragment | WMTStageBlit | WMTStageDispatch));
          markVertexHazards();
        } else {
          emitProducerBarrier(m_pass.encoder, uint64_t(WMTStageFragment));
        }
        MTLCommandEncoder_endEncoding(m_pass.encoder);
        NSObject_release(m_pass.encoder);
      } else {
        // No draws and nothing to store: only visibility-mode commands.
        m_cmdChunk.reset();
      }
      attApplyAfter();
      m_pass.encoder = 0;
      m_pass.active = false;
      m_pass.pending = true;
      m_argTablesBound = false;
      for (auto& c : m_pass.clearColor) c = false;
      m_pass.clearDepth = m_pass.clearStencil = false;
      for (auto& r : m_pass.resolveTo) r = nullptr;
      m_pass.discardDs = false;
      m_pass.discardMask = 0u;
      return;
    }

    flushCmdChunk();

    /* Metal 4 does no implicit render-pass transition: a later encoder that
     * samples or copies this attachment must be ordered after these writes. */
    emitProducerBarrier(m_pass.encoder, uint64_t(WMTStageFragment));
    MTLCommandEncoder_endEncoding(m_pass.encoder);
    NSObject_release(m_pass.encoder);
    m_pass.encoder = 0;
    m_pass.active = false;
    m_pass.pending = true;
    m_argTablesBound = false;
    for (auto& c : m_pass.clearColor) c = false;
    m_pass.clearDepth = m_pass.clearStencil = false;
  }


  void MtlContextImpl::splitNoteClosed() {
    // Present ends the frame's last pass: not a split a flush rule could avoid.
    m_split.valid = !m_frameBoundaryFlush;
    for (uint32_t i = 0u; i < 5u; i++) {
      const Rc<MtlTextureView>& v = i < kAttDepth ? m_pass.targets.color[i] : m_pass.targets.depthStencil;
      m_split.img[i] = v != nullptr ? v->image.ptr() : nullptr;
      m_split.sub[i] = v != nullptr ? (v->key.baseMip << 16u) | v->key.baseLayer : 0u;
    }
    m_split.serial = m_currentSerial;
    m_split.xfers  = m_splitXfers;
    m_split.clears = m_splitClears;
  }


  void MtlContextImpl::splitCheck(uint64_t loadBytes) {
    if (!m_split.valid)
      return;
    m_split.valid = false;
    if (!loadBytes)
      return;
    for (uint32_t i = 0u; i < 5u; i++) {
      const Rc<MtlTextureView>& v = i < kAttDepth ? m_pass.targets.color[i] : m_pass.targets.depthStencil;
      if (m_split.img[i] != (v != nullptr ? v->image.ptr() : nullptr)
       || m_split.sub[i] != (v != nullptr ? (v->key.baseMip << 16u) | v->key.baseLayer : 0u))
        return;
    }
    m_rs[rstats::Splits]++;
    m_rs[rstats::SplitBytes] += loadBytes;
    if (m_split.serial != m_pass.openSerial)
      m_rs[rstats::SplitsSubmit]++;
    else if (m_split.xfers != m_pass.openXfers)
      m_rs[rstats::SplitsXfer]++;
    else if (m_split.clears != m_pass.openClears)
      m_rs[rstats::SplitsClear]++;
  }


  void MtlContextImpl::markVertexHazards() {
    auto mark = [this] (MtlTexture* img) {
      if (img != nullptr)
        static_cast<MtlTextureImpl*>(img)->vtxEpoch = m_vtxEpoch;
    };
    for (const auto& v : m_pass.targets.color)
      mark(v != nullptr ? v->image.ptr() : nullptr);
    if (m_pass.targets.depthStencil != nullptr)
      mark(m_pass.targets.depthStencil->image.ptr());
    for (const auto& r : m_pass.resolveTo)
      mark(r.ptr());
    m_vtxCheck = true;
  }


  void MtlContextImpl::vertexFence(uint64_t beforeStages) {
    // Its own (empty) encoder: no render encoder may be open. The barrier
    // waits for the fragment stage of every earlier encoder on the queue.
    ensureCommandBuffer();
    obj_handle_t enc = MTL4CommandBuffer_computeCommandEncoder(m_cmdbuf.handle());
    emitProducerBarrier(enc, uint64_t(WMTStageFragment), beforeStages);
    MTLCommandEncoder_endEncoding(enc);
    NSObject_release(enc);
    prof::bump(prof::CntVtxFence);
  }


  void MtlContextImpl::resolveVertexHazard() {
    m_vtxCheck = false;
    bool hazard = false;
    for (const auto& t : m_bindings[0].textures) {
      if (t != nullptr && t->image != nullptr
       && static_cast<const MtlTextureImpl*>(t->image.ptr())->vtxEpoch == m_vtxEpoch) {
        hazard = true;
        break;
      }
    }
    if (likely(!hazard))
      return;
    if (m_pass.encoder) {
      // The pass already has its encoder (a chunk was encoded early): end
      // it, fence, and go on in a new one on the same targets. The draws
      // before this one didn't read the image in their vertex stage.
      static bool logged = false;
      if (!logged) {
        logged = true;
        Logger::info("D9MT: vertex texture rendered in an earlier pass, encoder already open: pass split for a vertex fence");
      }
      prof::bump(prof::CntPassCloseBarrier);
      closePass();
      vertexFence(uint64_t(WMTStageVertex));
      materializePass();
    } else {
      // The pass's encoder is created when it ends, after the fence.
      vertexFence(uint64_t(WMTStageVertex));
    }
  }


  namespace {

    // D9MT_PROFILE: render-thread CPU time in the attachment optimization
    // (rstats::AttNs), outermost scope only (a restore can close a pass);
    // nothing without D9MT_PROFILE.
    class AttTimer {
    public:
      explicit AttTimer(uint64_t* acc) : m_acc(acc && !s_depth++ ? acc : nullptr), m_on(acc != nullptr) {
        if (m_acc)
          m_t0 = now();
      }
      ~AttTimer() { stop(); }
      // Ends the scope early (before work the pass needs either way).
      void stop() {
        if (m_acc)
          *m_acc += now() - m_t0;
        if (m_on)
          s_depth--;
        m_acc = nullptr;
        m_on  = false;
      }
    private:
      static uint64_t now() {
        return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count());
      }
      static inline thread_local uint32_t s_depth = 0u;
      uint64_t* m_acc;
      bool      m_on;
      uint64_t  m_t0 = 0u;
    };

    // Images the attachment optimization tracks: one subresource, so a
    // load/store action covers all of it. Everything else stays Defined.
    bool attImageEligible(const MtlTexture* img) {
      const MtlImageInfo& i = img->info();
      return i.type == MtlImageType::e2D && i.mipLevels == 1u && i.numLayers == 1u;
    }

    // Attachment views in the image's own format: clear values recorded
    // through them are what a re-clear of the image writes.
    bool attViewEligible(const MtlTextureView* v) {
      return v->image != nullptr && attImageEligible(v->image.ptr())
          && v->key.format == v->image->info().format
          && v->key.baseMip == 0u && v->key.baseLayer == 0u;
    }

    // Bytes one load or store of an aspect moves (estimate for STATS).
    uint64_t attBytes(const MtlTexture* img, uint32_t slot) {
      if (img == nullptr)
        return 0u;
      const MtlImageInfo& i = img->info();
      const MtlFormatInfo* fi = lookupFormatInfo(i.format);
      uint64_t bpp = fi->elementSize ? fi->elementSize : 4u;
      if (slot == 5u)                                    // stencil
        bpp = 1u;
      else if (slot == 4u && (fi->flags & MtlFormatFlagStencil))
        bpp = bpp > 4u ? 4u : bpp;                       // depth of a depth+stencil format
      return uint64_t(i.extent.width) * i.extent.height * std::max(i.sampleCount, 1u) * bpp;
    }

    WMTLoadAction wmtLoad(attopt::Load l) {
      switch (l) {
        case attopt::Load::Clear:    return WMTLoadActionClear;
        case attopt::Load::DontCare: return WMTLoadActionDontCare;
        default:                     return WMTLoadActionLoad;
      }
    }

    const char* attLoadName(attopt::Load l, bool fromState) {
      switch (l) {
        case attopt::Load::Clear:    return fromState ? "clear(state)" : "clear";
        case attopt::Load::DontCare: return "dontcare";
        default:                     return "load";
      }
    }

    const char* attContentName(attopt::Content c) {
      switch (c) {
        case attopt::Content::Cleared:   return "cleared";
        case attopt::Content::Lazy:      return "lazy";
        case attopt::Content::Undefined: return "undefined";
        default:                         return "defined";
      }
    }

  }


  obj_handle_t MtlContextImpl::newRenderEncoder(obj_handle_t cmdbuf, WMTRenderPassInfo* info) {
    if (unlikely(prof::g_profEnabled))
      m_rs[rstats::RenderPasses]++;
    return MTL4CommandBuffer_renderCommandEncoder(cmdbuf, info);
  }


  bool MtlContextImpl::chunkHasNoWork() const {
    for (auto* c = const_cast<wmtcmd_base*>(m_cmdChunk.head()); c != nullptr;
         c = static_cast<wmtcmd_base*>(c->next.get())) {
      if (c->type != WMTMtl4RenderCommandNop
       && c->type != WMTMtl4RenderCommandSetVisibilityResultMode)
        return false;
    }
    return true;
  }


  bool MtlContextImpl::createPassEncoder(bool early) {
    AttTimer attTimer(unlikely(prof::g_profEnabled) ? &m_rs[rstats::AttNs] : nullptr);
    const uint32_t dbg   = d9mtdbg::flags();
    const bool     optOn = !(dbg & d9mtdbg::NoAttachOpt);
    const bool     full  = !early;

    const Rc<MtlTextureView>& dsv = m_pass.targets.depthStencil;
    const MtlTexture* imgs[5] = { };
    for (uint32_t i = 0u; i < MtlRenderTargets::MaxColorTargets; i++)
      imgs[i] = m_pass.targets.color[i] != nullptr ? m_pass.targets.color[i]->image.ptr() : nullptr;
    imgs[4] = dsv != nullptr ? dsv->image.ptr() : nullptr;
    // An image bound to two slots (not expected) is left to Load/Store.
    auto unique = [&imgs] (uint32_t i) {
      for (uint32_t j = 0u; j < 5u; j++) {
        if (j != i && imgs[j] == imgs[i])
          return false;
      }
      return true;
    };

    bool needed = early || m_pass.encodedDraws != 0u || !m_psoPatches.empty() || !chunkHasNoWork();
    attopt::AspectState dummy;

    auto decideSlot = [&] (uint32_t s, MtlTextureImpl* img, attopt::AspectState& st,
                           bool eligible, bool clear) -> AttSlot& {
      AttSlot& a = m_pass.att[s];
      a = AttSlot();
      a.used             = true;
      a.in.content       = st.content;
      a.in.explicitClear = clear;
      a.in.eligible      = eligible;
      a.in.optOn         = optOn;
      a.in.fullKnowledge = full;
      a.in.written       = ((m_pass.writtenMask >> s) & 1u) != 0u;
      a.in.discardAfter  = ((m_pass.discardMask >> s) & 1u) != 0u || (s >= kAttDepth && m_pass.discardDs);
      // The bindings scan only matters when a store could be skipped.
      a.in.sampled = eligible && full && optOn && !a.in.written && attSampled(img);
      a.in.readHint = attopt::readRecently(st, m_attFrame);
      a.d = attopt::decide(a.in);
      if (a.d.restoreFirst) {
        attRestore(img, rstore::Reason::RtView,
          s < kAttDepth ? MtlAspectColor : s == kAttDepth ? MtlAspectDepth : MtlAspectStencil);
        a.in.content = st.content;
        a.d = attopt::decide(a.in);
      }
      return a;
    };

    WMTRenderPassInfo info = {};
    info.visibility_buffer = m_visBuffer.handle();

    for (uint32_t i = 0u; i < MtlRenderTargets::MaxColorTargets; i++) {
      m_pass.att[i] = AttSlot();
      const auto& view = m_pass.targets.color[i];
      if (view == nullptr)
        continue;
      auto* vimpl = static_cast<MtlTextureViewImpl*>(view.ptr());
      auto* img   = static_cast<MtlTextureImpl*>(view->image.ptr());
      attopt::AspectState& st = img ? img->attColor : dummy;
      const bool eligible = img && attViewEligible(view.ptr()) && unique(i);
      AttSlot& a = decideSlot(i, img, st, eligible, m_pass.clearColor[i]);

      info.colors[i].texture     = vimpl->owned.handle();
      info.colors[i].load_action = wmtLoad(a.d.load);
      const bool store = a.d.store == attopt::Store::Store;
      if (m_pass.resolveTo[i] != nullptr) {
        info.colors[i].store_action = store
          ? WMTStoreActionStoreAndMultisampleResolve : WMTStoreActionMultisampleResolve;
        info.colors[i].resolve_texture = static_cast<MtlTextureImpl*>(m_pass.resolveTo[i].ptr())->texture.tex;
        needed = true;
      } else {
        info.colors[i].store_action = store ? WMTStoreActionStore : WMTStoreActionDontCare;
      }
      if (a.d.load == attopt::Load::Clear) {
        if (a.d.loadFromState) {
          const float* c = st.value.c;
          info.colors[i].clear_color = { c[0], c[1], c[2], c[3] };
        } else {
          const auto& v = m_pass.clearValues[i];
          info.colors[i].clear_color = { v.color.r, v.color.g, v.color.b, v.color.a };
        }
      }
      needed = needed || attopt::slotNeedsPass(a.in, a.d);
    }

    for (uint32_t s = kAttDepth; s <= kAttStencil; s++)
      m_pass.att[s] = AttSlot();

    if (dsv != nullptr) {
      auto* vimpl = static_cast<MtlTextureViewImpl*>(dsv.ptr());
      auto* img   = static_cast<MtlTextureImpl*>(dsv->image.ptr());
      const bool eligible = img && attViewEligible(dsv.ptr()) && unique(4u);

      attopt::AspectState& sd = img ? img->attDepth : dummy;
      AttSlot& a = decideSlot(kAttDepth, img, sd, eligible, m_pass.clearDepth);
      info.depth.texture      = vimpl->owned.handle();
      info.depth.load_action  = wmtLoad(a.d.load);
      info.depth.store_action = a.d.store == attopt::Store::Store ? WMTStoreActionStore : WMTStoreActionDontCare;
      info.depth.clear_depth  = a.d.loadFromState ? sd.value.c[0] : m_pass.depthClear;
      needed = needed || attopt::slotNeedsPass(a.in, a.d);

      if (lookupFormatInfo(vimpl->key.format)->flags & MtlFormatFlagStencil) {
        attopt::AspectState& ss = img ? img->attStencil : dummy;
        AttSlot& b = decideSlot(kAttStencil, img, ss, eligible, m_pass.clearStencil);
        info.stencil.texture       = vimpl->owned.handle();
        info.stencil.load_action   = wmtLoad(b.d.load);
        info.stencil.store_action  = b.d.store == attopt::Store::Store ? WMTStoreActionStore : WMTStoreActionDontCare;
        info.stencil.clear_stencil = b.d.loadFromState ? ss.value.stencil : m_pass.stencilClear;
        needed = needed || attopt::slotNeedsPass(b.in, b.d);
      }
    }

    // No draws, no resolve and nothing to store: the pass is a no-op.
    const bool skip = full && optOn && !needed;

    if (unlikely(prof::g_profEnabled)) {
      int64_t saved = 0;
      for (uint32_t s = 0u; s < m_pass.att.size(); s++) {
        const AttSlot& a = m_pass.att[s];
        if (!a.used)
          continue;
        saved += attopt::bytesSaved(a.in, a.d, attBytes(imgs[s < kAttDepth ? s : 4u], s), skip);
        if (skip)
          continue;
        m_rs[a.d.load == attopt::Load::Load  ? rstats::LoadLoad
           : a.d.load == attopt::Load::Clear ? rstats::LoadClear : rstats::LoadDontCare]++;
        m_rs[a.d.store == attopt::Store::Store ? rstats::StoreStore : rstats::StoreDontCare]++;
        if (a.d.store == attopt::Store::DontCare)
          prof::bump(prof::CntStoreDontCare);
      }
      m_rs[rstats::BytesSaved] += uint64_t(saved);
      if (skip) {
        m_rs[rstats::PassesSkipped]++;
      } else {
        uint64_t loadBytes = 0u;
        for (uint32_t s = 0u; s < m_pass.att.size(); s++) {
          if (m_pass.att[s].used && m_pass.att[s].d.load == attopt::Load::Load)
            loadBytes += attBytes(imgs[s < kAttDepth ? s : 4u], s);
        }
        splitCheck(loadBytes);
      }
    }

    if (unlikely(dbg & d9mtdbg::AttTrace)) {
      if (!m_attTraceOn) {
        m_attTraceOn = true;
        m_attTraceBudget = 400u;
      }
      if (m_attTraceBudget) {
        m_attTraceBudget--;
        std::string line = str::format("D9MT-ATT pass=", m_pass.epoch,
          skip ? " skipped" : early ? " early" : "", " draws=", m_pass.encodedDraws,
          optOn ? "" : " (no_attach_opt)");
        static const char* kSlot[6] = { "c0", "c1", "c2", "c3", "d", "s" };
        for (uint32_t s = 0u; s < m_pass.att.size(); s++) {
          const AttSlot& a = m_pass.att[s];
          if (!a.used)
            continue;
          const MtlTexture* img = imgs[s < kAttDepth ? s : 4u];
          line += str::format(" ", kSlot[s], "=", img ? img->info().extent.width : 0u, "x",
            img ? img->info().extent.height : 0u, "/", img ? uint32_t(img->info().format) : 0u,
            a.in.eligible ? "" : "(untracked)", " ", attContentName(a.in.content),
            a.in.written ? " w" : "", a.in.sampled ? " sampled" : "", a.in.readHint ? " hint" : "",
            " ", attLoadName(a.d.load, a.d.loadFromState), "/",
            a.d.store == attopt::Store::Store ? "store" : "dontcare",
            s < kAttDepth && m_pass.resolveTo[s] != nullptr ? "+resolve" : "");
        }
        Logger::warn(line);
      }
    } else {
      m_attTraceOn = false;
    }

    // The encoder itself is created with or without the optimization.
    attTimer.stop();
    if (skip)
      return false;

    m_pass.encoder = newRenderEncoder(m_cmdbuf.handle(), &info);
    for (const auto& r : m_pass.resolveTo) {
      if (r != nullptr)
        m_keepAlive.push_back(r);
    }
    return true;
  }


  void MtlContextImpl::attApplyAfter() {
    AttTimer attTimer(unlikely(prof::g_profEnabled) ? &m_rs[rstats::AttNs] : nullptr);
    for (uint32_t s = 0u; s < m_pass.att.size(); s++) {
      AttSlot& a = m_pass.att[s];
      if (!a.used)
        continue;
      a.used = false;
      const Rc<MtlTextureView>& view = s < kAttDepth ? m_pass.targets.color[s] : m_pass.targets.depthStencil;
      auto* img = view != nullptr ? static_cast<MtlTextureImpl*>(view->image.ptr()) : nullptr;
      if (img == nullptr || !attImageEligible(img))
        continue;
      a.in.written      = ((m_pass.writtenMask >> s) & 1u) != 0u;
      a.in.discardAfter = ((m_pass.discardMask >> s) & 1u) != 0u || (s >= kAttDepth && m_pass.discardDs);
      const attopt::After after = attopt::stateAfter(a.in, a.d);
      attopt::AspectState& st = s < kAttDepth ? img->attColor
                              : s == kAttDepth ? img->attDepth : img->attStencil;
      st.content = after.content;
      if (after.passValue) {
        if (s < kAttDepth) {
          const auto& v = m_pass.clearValues[s];
          st.value.c[0] = v.color.r; st.value.c[1] = v.color.g;
          st.value.c[2] = v.color.b; st.value.c[3] = v.color.a;
        } else if (s == kAttDepth) {
          st.value.c[0] = m_pass.depthClear;
        } else {
          st.value.stencil = m_pass.stencilClear;
        }
      }
    }
  }


  uint8_t MtlContextImpl::attComputeWriteMask() const {
    uint8_t mask = 0u;
    for (uint32_t i = 0u; i < MtlRenderTargets::MaxColorTargets; i++) {
      if (m_pass.targets.color[i] != nullptr && (m_blend[i].writeMask & 0xFu))
        mask |= uint8_t(1u << i);
    }
    if (m_pass.targets.depthStencil != nullptr) {
      if (m_depthStencil.depthWrite)
        mask |= uint8_t(1u << kAttDepth);
      if (m_depthStencil.stencilEnable)
        mask |= uint8_t(1u << kAttStencil);
    }
    return mask;
  }


  bool MtlContextImpl::attSampled(const MtlTexture* img) const {
    if (img == nullptr)
      return false;
    if (static_cast<const MtlTextureImpl*>(img)->attSampledEpoch == m_pass.epoch)
      return true;
    for (const auto& b : m_bindings) {
      for (const auto& t : b.textures) {
        if (t != nullptr && t->image.ptr() == img)
          return true;
      }
    }
    return false;
  }


  void MtlContextImpl::attBindSampled(const MtlTextureView* view) {
    auto* t = static_cast<MtlTextureImpl*>(view->image.ptr());
    // A bind while no pass is open counts for the pass that opens next.
    t->attSampledEpoch = m_pass.active ? m_pass.epoch : m_attEpoch + 1u;
    // Sampling reads the view's aspects only: D3D9 samples the depth of a
    // depth-stencil texture (shadow maps, INTZ), never its stencil, so a
    // stencil cleared but never stored stays Lazy instead of being re-cleared.
    attRead(t, rstore::Reason::Sample, view->key.aspects ? view->key.aspects : MtlAspectColor);
  }


  void MtlContextImpl::attRead(const MtlTexture* img, rstore::Reason why, MtlAspectMask aspects) {
    auto* t = static_cast<MtlTextureImpl*>(const_cast<MtlTexture*>(img));
    if (t == nullptr)
      return;
    using attopt::Content;
    auto known = [] (const attopt::AspectState& st) {
      return st.content == Content::Lazy || st.content == Content::Cleared;
    };
    if (likely(!known(t->attColor) && !known(t->attDepth) && !known(t->attStencil)))
      return;

    // Only the aspects the image has: initImage marks all three Cleared.
    const MtlFormatInfo* fi = lookupFormatInfo(t->format);
    MtlAspectMask has = 0u;
    if (fi->flags & MtlFormatFlagDepth)   has |= MtlAspectDepth;
    if (fi->flags & MtlFormatFlagStencil) has |= MtlAspectStencil;
    if (!has)                             has  = MtlAspectColor;
    aspects &= has;

    MtlAspectMask lazy = 0u;
    if ((aspects & MtlAspectColor)   && attopt::noteRead(t->attColor,   m_attFrame)) lazy |= MtlAspectColor;
    if ((aspects & MtlAspectDepth)   && attopt::noteRead(t->attDepth,   m_attFrame)) lazy |= MtlAspectDepth;
    if ((aspects & MtlAspectStencil) && attopt::noteRead(t->attStencil, m_attFrame)) lazy |= MtlAspectStencil;
    if (lazy)
      attRestore(t, why, lazy);
  }


  void MtlContextImpl::attWrite(const MtlTexture* img, rstore::Reason why) {
    if (img == nullptr)
      return;
    // Partial writes keep the rest of the contents: make memory hold them first.
    attRead(img, why);
    auto* t = static_cast<MtlTextureImpl*>(const_cast<MtlTexture*>(img));
    t->attColor.content = t->attDepth.content = t->attStencil.content = attopt::Content::Defined;
  }


  void MtlContextImpl::attRestore(MtlTextureImpl* t, rstore::Reason why, MtlAspectMask aspects) {
    AttTimer attTimer(unlikely(prof::g_profEnabled) ? &m_rs[rstats::AttNs] : nullptr);
    // Something reads an image whose clear was never stored: store it now.
    // Its own pass, before whatever reads; an encoder already open ends first.
    // Only `aspects` are re-cleared; another Lazy aspect is loaded and stored
    // unchanged and stays Lazy (memory never holds it).
    if (m_pass.encoder) {
      prof::bump(prof::CntPassCloseBlit);
      closePass();
    }
    if (unlikely(prof::g_profEnabled))
      m_splitXfers++;
    ensureCommandBuffer();
    t->setTrackingSerial(m_currentSerial);

    const MtlFormatInfo* fi = lookupFormatInfo(t->format);
    const bool hasDepth   = (fi->flags & MtlFormatFlagDepth) != 0u;
    const bool hasStencil = (fi->flags & MtlFormatFlagStencil) != 0u;
    auto lazy = [] (const attopt::AspectState& st) { return st.content == attopt::Content::Lazy; };
    const bool clearColor   = (aspects & MtlAspectColor)   && lazy(t->attColor);
    const bool clearDepth   = (aspects & MtlAspectDepth)   && lazy(t->attDepth);
    const bool clearStencil = (aspects & MtlAspectStencil) && lazy(t->attStencil);

    WMTRenderPassInfo info = {};
    info.render_target_width  = t->extent.width;
    info.render_target_height = t->extent.height;
    uint64_t bytes = 0u;
    if (hasDepth || hasStencil) {
      if (hasDepth) {
        info.depth.texture      = t->owned.handle();
        info.depth.load_action  = clearDepth ? WMTLoadActionClear : WMTLoadActionLoad;
        info.depth.store_action = WMTStoreActionStore;
        info.depth.clear_depth  = t->attDepth.value.c[0];
        bytes += attBytes(t, 4u);
      }
      if (hasStencil) {
        info.stencil.texture       = t->owned.handle();
        info.stencil.load_action   = clearStencil ? WMTLoadActionClear : WMTLoadActionLoad;
        info.stencil.store_action  = WMTStoreActionStore;
        info.stencil.clear_stencil = t->attStencil.value.stencil;
        bytes += attBytes(t, 5u);
      }
    } else {
      const float* c = t->attColor.value.c;
      info.colors[0].texture      = t->owned.handle();
      info.colors[0].load_action  = clearColor ? WMTLoadActionClear : WMTLoadActionLoad;
      info.colors[0].store_action = WMTStoreActionStore;
      info.colors[0].clear_color  = { c[0], c[1], c[2], c[3] };
      bytes += attBytes(t, 0u);
    }

    obj_handle_t enc = newRenderEncoder(m_cmdbuf.handle(), &info);
    emitProducerBarrier(enc, uint64_t(WMTStageFragment));
    MTLCommandEncoder_endEncoding(enc);
    NSObject_release(enc);
    m_keepAlive.push_back(Rc<MtlTexture>(t));

    uint8_t restored = 0u;
    if (clearColor)   { t->attColor.content   = attopt::Content::Cleared; restored |= rstore::AspectColor; }
    if (clearDepth)   { t->attDepth.content   = attopt::Content::Cleared; restored |= rstore::AspectDepth; }
    if (clearStencil) { t->attStencil.content = attopt::Content::Cleared; restored |= rstore::AspectStencil; }

    if (unlikely(prof::g_profEnabled)) {
      m_rs[rstats::Restores]++;
      m_rs[rstats::BytesSaved] -= bytes;
      rstore::Key k;
      k.format  = uint32_t(t->format);
      k.width   = t->extent.width;
      k.height  = t->extent.height;
      k.aspects = restored;
      k.reason  = why;
      k.api     = rstore::Api(m_apiTag < uint32_t(rstore::Api::Count) ? m_apiTag : 0u);
      m_device->restoreLog().add(k);
    }

    // att_trace: the restores share the pass decisions' line budget.
    if (unlikely(d9mtdbg::flags() & d9mtdbg::AttTrace) && m_attTraceOn && m_attTraceBudget) {
      m_attTraceBudget--;
      rstore::Key k;
      k.format  = uint32_t(t->format);
      k.width   = t->extent.width;
      k.height  = t->extent.height;
      k.aspects = restored;
      k.reason  = why;
      k.api     = rstore::Api(m_apiTag < uint32_t(rstore::Api::Count) ? m_apiTag : 0u);
      Logger::warn(str::format("D9MT-ATT restore after pass=", m_attEpoch, " ", rstore::describe(k),
        m_apiTag ? "" : " (api tags need D9MT_PROFILE)"));
    }
  }


  void MtlContextImpl::setApiTag(uint32_t api) {
    m_apiTag = api;
  }


  void MtlContextImpl::discardImage(const Rc<MtlTexture>& image) {
    if (!m_attOpt || image == nullptr || !attImageEligible(image.ptr()))
      return;
    auto* t = static_cast<MtlTextureImpl*>(image.ptr());
    const Rc<MtlTextureView>& dsv = m_pass.targets.depthStencil;
    if (m_pass.active && dsv != nullptr && dsv->image.ptr() == t) {
      // The open pass needn't store it.
      m_pass.discardDs = true;
      return;
    }
    t->attColor.content = t->attDepth.content = t->attStencil.content = attopt::Content::Undefined;
  }


  void MtlContextImpl::bindShader(MtlShaderStage stage, Rc<MtlShaderFunction>&& fn) {
    Rc<MtlShaderFunction>& slot = (stage == MtlShaderStage::Vertex ? m_vs : m_fs);
    if (slot == fn)
      return;
    slot = std::move(fn);
    m_dirty |= DirtyPso;
  }

  void MtlContextImpl::setVertexLayout(
    uint32_t nAttr, const MtlVertexAttr* attrs,
    uint32_t nLayout, const MtlVertexLayout* layouts) {
    if (m_vertexAttrs.size() == nAttr && m_vertexLayouts.size() == nLayout
     && std::memcmp(m_vertexAttrs.data(), attrs, nAttr * sizeof(MtlVertexAttr)) == 0
     && std::memcmp(m_vertexLayouts.data(), layouts, nLayout * sizeof(MtlVertexLayout)) == 0)
      return;
    m_vertexAttrs.assign(attrs, attrs + nAttr);
    m_vertexLayouts.assign(layouts, layouts + nLayout);
    m_layoutGen++;
    m_dirty |= DirtyPso;
  }

  void MtlContextImpl::setPrimitiveTopology(MtlPrimitiveTopology topology) {
    // Not DirtyPso: the pipeline doesn't depend on the topology (RenderPsoKey::topologyClass is
    // always 0, and build() leaves Metal's input topology unspecified), and each draw encodes
    // m_topology itself. So a draw that only changes topology skips resolvePso and the
    // resource flush that DirtyPso also triggers.
    m_topology = topology;
  }

  void MtlContextImpl::setViewports(uint32_t n, const MtlViewport* viewports) {
    if (!n)
      return;
    if (std::memcmp(&m_viewport, &viewports[0], sizeof(MtlViewport)) == 0)
      return;
    m_viewport = viewports[0];
    m_dirty |= DirtyViewport;
  }

  void MtlContextImpl::setRasterizerState(const MtlRasterizerState& rs) {
    if (std::memcmp(&m_rasterizer, &rs, sizeof(rs)) == 0)
      return;
    m_rasterizer = rs;
    m_dirty |= DirtyRasterizer;
  }

  void MtlContextImpl::setMultisampleState(const MtlMultisampleState& ms) {
    if (std::memcmp(&m_msaa, &ms, sizeof(ms)) == 0)
      return;
    m_msaa = ms;
    m_dirty |= DirtyPso;
  }

  void MtlContextImpl::setDepthStencilState(const MtlDepthStencilState& ds) {
    if (std::memcmp(&m_depthStencil, &ds, sizeof(ds)) == 0)
      return;
    m_depthStencil = ds;
    m_dirty |= DirtyDepthStencil;
  }

  void MtlContextImpl::setBlendMode(uint32_t rt, const MtlBlendMode& blend) {
    if (rt >= 4)
      return;
    if (std::memcmp(&m_blend[rt], &blend, sizeof(blend)) == 0)
      return;
    m_blend[rt] = blend;
    m_blendGen++;
    m_dirty |= DirtyPso;
  }

  void MtlContextImpl::setBlendColor(MtlColor4 color) {
    if (std::memcmp(&m_blendColor, &color, sizeof(color)) == 0)
      return;
    m_blendColor = color;
    m_dirty |= DirtyBlendColor;
  }

  void MtlContextImpl::setStencilReference(uint32_t reference) {
    if (m_stencilRef == reference)
      return;
    m_stencilRef = reference;
    m_dirty |= DirtyStencilRef;
  }

  void MtlContextImpl::setDepthBias(MtlDepthBias bias) {
    if (std::memcmp(&m_depthBias, &bias, sizeof(bias)) == 0)
      return;
    m_depthBias = bias;
    m_dirty |= DirtyDepthBias;
  }

  void MtlContextImpl::setDepthBounds(MtlDepthBounds bounds) {
    /* accepted but intentionally a no-op: Metal has no depth-bounds test */
  }

  void MtlContextImpl::bindVertexBuffer(uint32_t binding, MtlBufferSlice&& slice, uint32_t stride) {
    auto& stream = m_vertexStreams[binding];
    if (stream.stride != stride) {
      m_strideGen++;
      m_dirty |= DirtyPso;
    }
    stream.slice = std::move(slice);
    stream.stride = stride;
    m_dirtyVertexStreams |= 1u << binding;
    m_dirty |= DirtyVertexBuffers;
  }

  void MtlContextImpl::bindVertexBufferRange(uint32_t binding, MtlDeviceSize offset, MtlDeviceSize length, uint32_t stride) {
    auto& stream = m_vertexStreams[binding];
    if (!stream.slice.defined())
      return;
    if (stream.stride != stride) {
      m_strideGen++;
      m_dirty |= DirtyPso;
    }
    stream.slice = MtlBufferSlice(stream.slice.buffer(), stream.slice.storage(),
                                  offset, length);
    stream.stride = stride;
    m_dirtyVertexStreams |= 1u << binding;
    m_dirty |= DirtyVertexBuffers;
  }

  void MtlContextImpl::bindIndexBuffer(MtlBufferSlice&& slice, MtlIndexType type) {
    m_indexBuffer = std::move(slice);
    m_indexType = type;
    m_dirty |= DirtyIndexBuffer;
  }

  void MtlContextImpl::bindUniformBuffer(MtlStageMask stages, uint32_t slot, MtlBufferSlice&& slice) {
    for (uint32_t s = 0; s < 2; s++) {
      if (stages & (1u << s)) {
        auto& cbv = m_bindings[s].cbvs[slot];
        cbv.slice = slice;
        cbv.rangeOffset = 0;
        cbv.rangeLength = slice.length();
        m_bindings[s].dirtyCbvs |= 1u << slot;
      }
    }
    m_dirty |= DirtyResources;
  }

  void MtlContextImpl::bindUniformBufferRange(MtlStageMask stages, uint32_t slot,
                                              MtlDeviceSize offset, MtlDeviceSize length) {
    for (uint32_t s = 0; s < 2; s++) {
      if (stages & (1u << s)) {
        auto& cbv = m_bindings[s].cbvs[slot];
        cbv.rangeOffset = offset;
        cbv.rangeLength = length;
        m_bindings[s].dirtyCbvs |= 1u << slot;
      }
    }
    m_dirty |= DirtyResources;
  }

  void MtlContextImpl::bindTexture(MtlStageMask stages, uint32_t slot, Rc<MtlTextureView>&& view) {
    if (unlikely(slot >= m_bindings[0].textures.size())) {
      static bool warned = false;
      if (!warned) { warned = true; Logger::warn("MtlContextImpl: dropped texture bind beyond stage table (DMAP?)"); }
      return;
    }
    bool changed = false;
    for (uint32_t s = 0; s < 2; s++) {
      if (stages & (1u << s)) {
        if (m_bindings[s].textures[slot] == view)
          continue;
        m_bindings[s].textures[slot] = view;
        m_bindings[s].dirtyTextures |= 1u << slot;
        changed = true;
      }
    }
    if (changed) {
      m_dirty |= DirtyResources;
      if (m_narrowBarrier && (stages & MtlStageVertex) && view != nullptr)
        m_vtxCheck = true;
      if (deferredPassesEnabled() && view != nullptr && view->image != nullptr)
        m_frameSampled.insert(view->image.ptr());
      if (m_attOpt && view != nullptr && view->image != nullptr)
        attBindSampled(view.ptr());
    }
  }

  void MtlContextImpl::bindSampler(MtlStageMask stages, uint32_t slot, Rc<MtlSamplerState>&& sampler) {
    if (unlikely(slot >= m_bindings[0].samplers.size())) {
      static bool warned = false;
      if (!warned) { warned = true; Logger::warn("MtlContextImpl: dropped sampler bind beyond stage table (DMAP?)"); }
      return;
    }
    bool changed = false;
    for (uint32_t s = 0; s < 2; s++) {
      if (stages & (1u << s)) {
        if (m_bindings[s].samplers[slot] == sampler)
          continue;
        m_bindings[s].samplers[slot] = sampler;
        m_bindings[s].dirtySamplers |= 1u << slot;
        changed = true;
      }
    }
    if (changed)
      m_dirty |= DirtyResources;
  }

  void MtlContextImpl::pushData(MtlStageMask stages, uint32_t offset, uint32_t size, const void* data) {
    const bool  isShared   = (uint32_t(stages) & (uint32_t(stages) - 1u)) != 0u;
    const uint32_t baseOff = isShared ? 0u : kPushSharedBlockSize;
    for (uint32_t s = 0; s < 2; s++) {
      if (stages & (1u << s)) {
        auto& b = m_bindings[s];
        std::memcpy(b.pushData.data() + baseOff + offset, data, size);
        b.pushDirtyLo = b.pushDirtyLo ? std::min(b.pushDirtyLo, baseOff + offset) : baseOff + offset;
        b.pushDirtyHi = std::max(b.pushDirtyHi, baseOff + offset + size);
        b.pushExtent  = std::max(b.pushExtent, baseOff + offset + size);
      }
    }
    m_dirty |= DirtyPush;
  }


  bool MtlContextImpl::ensureFanIndices() {
    if (m_fanIndices != nullptr)
      return true;
    MtlBufferCreateInfo bi = {};
    bi.size      = uint64_t(3u * (kFanMaxVerts - 2u)) * 4u;
    bi.usage     = MtlUsage::IndexBuffer;
    bi.cpuAccess = MtlCpuAccess::Write;
    Rc<MtlBuffer> buf = m_device->createBuffer(bi);
    uint32_t* p = buf != nullptr ? static_cast<uint32_t*>(buf->mapPtr(0u)) : nullptr;
    if (p == nullptr) {
      Logger::err("MtlContextImpl: fan index pattern buffer creation failed");
      return false;
    }
    for (uint32_t t = 0u; t + 2u < kFanMaxVerts; t++) {
      *p++ = 0u;
      *p++ = t + 1u;
      *p++ = t + 2u;
    }
    m_fanIndices = std::move(buf);
    return true;
  }

  void MtlContextImpl::draw(uint32_t count, const MtlDrawArgs* args) {
    if (!prepareDraw())
      return;
    prof::Scope profScope(prof::EncodeDraw);
    if (unlikely(m_topology == MtlPrimitiveTopology::TriangleFan)) {
      if (!ensureFanIndices())
        return;
      if (m_fanIndices->trackingSerial() != m_currentSerial)
        m_fanIndices->setTrackingSerial(m_currentSerial);
      for (uint32_t i = 0; i < count; i++) {
        if (args[i].vertexCount < 3u)
          continue;
        const uint32_t verts = std::min(args[i].vertexCount, kFanMaxVerts);
        auto* cmd = m_cmdChunk.append<wmtcmd_mtl4_draw_indexed>(
          uint16_t(WMTMtl4RenderCommandDrawIndexed));
        cmd->primitive           = uint64_t(WMTPrimitiveTypeTriangle);
        cmd->index_count         = 3u * (verts - 2u);
        cmd->index_type          = uint64_t(WMTIndexTypeUInt32);
        cmd->index_buffer        = m_fanIndices->gpuAddress(0u);
        cmd->index_buffer_length = m_fanIndices->size;
        cmd->instance_count      = args[i].instanceCount ? args[i].instanceCount : 1u;
        cmd->base_vertex         = int64_t(args[i].firstVertex);
        cmd->base_instance       = args[i].firstInstance;
      }
      return;
    }
    for (uint32_t i = 0; i < count; i++) {
      auto* cmd = m_cmdChunk.append<wmtcmd_mtl4_draw>(
        uint16_t(WMTMtl4RenderCommandDraw));
      cmd->primitive      = uint64_t(wmtPrimitive(m_topology));
      cmd->vertex_start   = args[i].firstVertex;
      cmd->vertex_count   = args[i].vertexCount;
      cmd->instance_count = args[i].instanceCount ? args[i].instanceCount : 1u;
      cmd->base_instance  = args[i].firstInstance;
    }
  }

  void MtlContextImpl::drawIndexed(uint32_t count, const MtlDrawIndexedArgs* args) {
    if (!prepareDraw())
      return;
    if (unlikely(m_topology == MtlPrimitiveTopology::TriangleFan)) {
      static bool warned = false;
      if (!warned) {
        warned = true;
        Logger::warn("MtlContextImpl: indexed triangle fan drawn as list (index remap unimplemented)");
      }
    }
    if (m_indexBuffer.storage() == nullptr)
      return;
    prof::Scope profScope(prof::EncodeDraw);
    if (m_indexBuffer.buffer() != nullptr
     && m_indexBuffer.buffer()->trackingSerial() != m_currentSerial)
      m_indexBuffer.buffer()->setTrackingSerial(m_currentSerial);
    const uint64_t idxSize = (m_indexType == MtlIndexType::Uint16) ? 2u : 4u;
    const WMTIndexType idxType = (m_indexType == MtlIndexType::Uint16)
      ? WMTIndexTypeUInt16 : WMTIndexTypeUInt32;

    for (uint32_t i = 0; i < count; i++) {
      uint64_t indexOffset = uint64_t(args[i].firstIndex) * idxSize;
      uint64_t gpuAddr     = m_indexBuffer.gpuAddress(indexOffset);
      uint64_t accessible  = m_indexBuffer.addressableLength(indexOffset);
      auto* cmd = m_cmdChunk.append<wmtcmd_mtl4_draw_indexed>(
        uint16_t(WMTMtl4RenderCommandDrawIndexed));
      cmd->primitive           = uint64_t(wmtPrimitive(m_topology));
      cmd->index_count         = args[i].indexCount;
      cmd->index_type          = uint64_t(idxType);
      cmd->index_buffer        = gpuAddr;
      cmd->index_buffer_length = accessible;
      cmd->instance_count      = args[i].instanceCount ? args[i].instanceCount : 1u;
      cmd->base_vertex         = int64_t(args[i].vertexOffset);
      cmd->base_instance       = args[i].firstInstance;
    }
  }

  bool MtlContextImpl::prepareDraw() {
    if (!m_pass.pending && !m_pass.active)
      return false;
    if (unlikely(m_viewport.scissorEmpty))
      return false;
    prof::Scope profScope(prof::PrepareDraw);
    if (unlikely(!m_pass.active))
      materializePass();
    if (unlikely(m_vtxCheck))
      resolveVertexHazard();
    const uint32_t dirtyIn = m_dirty;

    if (!deferredPassesEnabled()
     && m_cmdChunk.approxBytes() >= kCmdChunkFlushBytes)
      flushCmdChunk();

    if ((m_dirty & DirtyPso) && !resolvePso())
      return false;

    if (!m_argTablesBound) {
      selectArgumentTablesForPass();
      bindArgumentTables();
      m_argTablesBound = true;
      m_dirty |= DirtyVertexBuffers | DirtyResources | DirtyPush;
      m_dirtyVertexStreams = ~0u;
      for (auto& b : m_bindings) {
        b.dirtyTextures = ~0u;
        b.dirtySamplers = ~0u;
        b.dirtyCbvs     = ~0u;
      }
    }

    if (m_dirty & DirtyVertexBuffers)
      flushVertexBuffers();
    if (m_dirty & (DirtyPso | DirtyResources | DirtyPush))
      flushResources();
    if (m_dirty & (DirtyViewport | DirtyDepthStencil | DirtyBlendColor
                 | DirtyStencilRef | DirtyDepthBias | DirtyRasterizer))
      flushDynamicState();
    m_pass.encodedDraws++;

    if (unlikely(m_currentSerial != m_lastStampSerial)) {
      for (auto& stream : m_vertexStreams) {
        if (stream.slice.defined() && stream.slice.buffer() != nullptr)
          stream.slice.buffer()->setTrackingSerial(m_currentSerial);
      }
      if (m_indexBuffer.defined() && m_indexBuffer.buffer() != nullptr)
        m_indexBuffer.buffer()->setTrackingSerial(m_currentSerial);
      m_lastStampSerial = m_currentSerial;
    }

    // Aspects the pass may write: a store can only be skipped without.
    if (m_attOpt) {
      if (dirtyIn & (DirtyPso | DirtyDepthStencil))
        m_attWriteMask = attComputeWriteMask();
      m_pass.writtenMask |= m_attWriteMask;
    }

    m_dirty = 0u;
    prof::bump(prof::CntDraws);
    if (unlikely(prof::g_profEnabled))
      m_rs[rstats::Draws]++;
    return true;
  }


  CmdChunk MtlContextImpl::takeChunk() {
    if (m_chunkPool.empty())
      return CmdChunk();
    CmdChunk c = std::move(m_chunkPool.back());
    m_chunkPool.pop_back();
    return c;
  }

  void MtlContextImpl::recordPassEntry() {
    FrameEntry e;
    e.kind         = FrameEntry::Kind::Render;
    e.targets      = m_pass.targets;
    e.clearColor   = m_pass.clearColor;
    e.clearValues  = m_pass.clearValues;
    e.clearDepth   = m_pass.clearDepth;
    e.clearStencil = m_pass.clearStencil;
    e.depthClear   = m_pass.depthClear;
    e.stencilClear = m_pass.stencilClear;
    e.draws        = m_pass.encodedDraws;
    resolvePsoPatches();
    e.chunks.push_back(std::move(m_cmdChunk));
    m_cmdChunk = takeChunk();
    m_frameEntries.push_back(std::move(e));

    m_pass.encoder = 0;
    m_pass.active  = false;
    m_pass.pending = true;
    m_pass.encodedDraws = 0u;
    m_argTablesBound = false;
    for (auto& c : m_pass.clearColor) c = false;
    m_pass.clearDepth = m_pass.clearStencil = false;
  }

  void MtlContextImpl::prepareDirectEncode(uint32_t passCloseCounter) {
    if (m_pass.active) {
      prof::bump(prof::Counter(passCloseCounter));
      closePass();
    }
    else
      flushPendingClears();
    if (unlikely(prof::g_profEnabled))
      (passCloseCounter == uint32_t(prof::CntPassCloseClear) ? m_splitClears : m_splitXfers)++;
    ensureCommandBuffer();
  }

  void MtlContextImpl::encodeDirect(std::function<void(obj_handle_t)>&& fn) {
    if (!deferredPassesEnabled()) {
      fn(m_cmdbuf.handle());
      return;
    }
    FrameEntry e;
    e.kind   = FrameEntry::Kind::Direct;
    e.encode = std::move(fn);
    m_frameEntries.push_back(std::move(e));
  }

  void MtlContextImpl::noteTransferRead(const MtlTexture* tex) {
    if (!deferredPassesEnabled() || tex == nullptr)
      return;
    m_frameSampled.insert(tex);
    m_everTransferRead.insert(tex);
  }

  void MtlContextImpl::flushFramePasses() {
    const bool frameBoundary = m_frameBoundaryFlush;
    m_frameBoundaryFlush = false;

    if (m_frameEntries.empty()) {
      m_frameSampled.clear();
      return;
    }

    if (!m_cmdbuf) {
      Logger::err("MtlContextImpl::flushFramePasses: entries without cmdbuf, dropped");
      m_frameEntries.clear();
      m_frameSampled.clear();
      return;
    }

    static const bool ruleB   = std::getenv("D9MT_NO_RULE_B") == nullptr;
    static const bool ruleC   = std::getenv("D9MT_NO_RULE_C") == nullptr;
    static const bool ruleE   = std::getenv("D9MT_NO_RULE_E") == nullptr;
    static const bool ruleF   = std::getenv("D9MT_NO_RULE_F") == nullptr;
    static const bool mergeOn = std::getenv("D9MT_NO_PASSMERGE") == nullptr;

    auto& entries = m_frameEntries;

    using AttKey = std::tuple<const void*, uint32_t, uint32_t>;
    auto attKeyOf = [](const Rc<MtlTextureView>& v) {
      return AttKey((const void*)v->image.ptr(), v->key.baseMip, v->key.baseLayer);
    };
    auto sameRt = [](const Rc<MtlTextureView>& a, const Rc<MtlTextureView>& b) {
      if (!sameAttachment(a, b))
        return false;
      return a == nullptr || b == nullptr || a->key.format == b->key.format;
    };
    auto isSampled = [&](const Rc<MtlTextureView>& v) {
      return v != nullptr && v->image != nullptr
          && m_frameSampled.find(v->image.ptr()) != m_frameSampled.end();
    };
    auto anyClear = [](const FrameEntry& e) {
      bool c = e.clearDepth || e.clearStencil;
      for (bool b : e.clearColor) c = c || b;
      return c;
    };
    auto chunksEmpty = [](const FrameEntry& e) {
      for (const auto& c : e.chunks)
        if (!c.empty()) return false;
      return true;
    };

    if (ruleE) {
      for (size_t i = 0u; i < entries.size(); ) {
        FrameEntry& p = entries[i];
        if (p.kind != FrameEntry::Kind::Render || p.draws != 0u
         || !chunksEmpty(p) || !anyClear(p)) {
          i++;
          continue;
        }
        bool blocked = false;
        for (uint32_t s = 0u; s < MtlRenderTargets::MaxColorTargets && !blocked; s++)
          blocked = p.clearColor[s] && isSampled(p.targets.color[s]);
        if ((p.clearDepth || p.clearStencil) && isSampled(p.targets.depthStencil))
          blocked = true;
        size_t target = SIZE_MAX;
        for (size_t j = i + 1u; j < entries.size() && !blocked && target == SIZE_MAX; j++) {
          const FrameEntry& c = entries[j];
          if (c.kind != FrameEntry::Kind::Render) {
            blocked = true;
            break;
          }
          bool touches = false, matches = true;
          for (uint32_t s = 0u; s < MtlRenderTargets::MaxColorTargets; s++) {
            if (!p.clearColor[s])
              continue;
            bool touchedHere = false;
            for (uint32_t cs = 0u; cs < MtlRenderTargets::MaxColorTargets; cs++)
              touchedHere = touchedHere
                || (c.targets.color[cs] != nullptr
                 && sameAttachment(c.targets.color[cs], p.targets.color[s]));
            touches = touches || touchedHere;
            matches = matches && sameRt(c.targets.color[s], p.targets.color[s])
                   && !c.clearColor[s];
          }
          if (p.clearDepth || p.clearStencil) {
            bool touchedHere = c.targets.depthStencil != nullptr
              && sameAttachment(c.targets.depthStencil, p.targets.depthStencil);
            touches = touches || touchedHere;
            matches = matches && sameRt(c.targets.depthStencil, p.targets.depthStencil)
                   && !(p.clearDepth   && c.clearDepth)
                   && !(p.clearStencil && c.clearStencil)
                   && !c.clearDepth && !c.clearStencil;
          }
          if (!touches)
            continue;
          if (matches)
            target = j;
          else
            blocked = true;
        }
        if (target != SIZE_MAX) {
          FrameEntry& t = entries[target];
          for (uint32_t s = 0u; s < MtlRenderTargets::MaxColorTargets; s++) {
            if (p.clearColor[s]) {
              t.clearColor[s]  = true;
              t.clearValues[s] = p.clearValues[s];
            }
          }
          if (p.clearDepth)   { t.clearDepth = true;   t.depthClear = p.depthClear; }
          if (p.clearStencil) { t.clearStencil = true; t.stencilClear = p.stencilClear; }
          for (auto& ch : entries[i].chunks) {
            ch.reset();
            m_chunkPool.push_back(std::move(ch));
          }
          entries.erase(entries.begin() + i);
        } else {
          i++;
        }
      }
    }

    if (mergeOn) {
      for (size_t i = 0u; i + 1u < entries.size(); ) {
        FrameEntry& a = entries[i];
        FrameEntry& b = entries[i + 1u];
        bool merge = a.kind == FrameEntry::Kind::Render
                  && b.kind == FrameEntry::Kind::Render
                  && !anyClear(b)
                  && sameRt(a.targets.depthStencil, b.targets.depthStencil);
        for (uint32_t s = 0u; merge && s < MtlRenderTargets::MaxColorTargets; s++)
          merge = sameRt(a.targets.color[s], b.targets.color[s]);
        if (merge) {
          for (auto& ch : b.chunks)
            a.chunks.push_back(std::move(ch));
          a.draws += b.draws;
          entries.erase(entries.begin() + i + 1u);
        } else {
          i++;
        }
      }
    }

    for (auto& e : entries) {
      if (e.kind != FrameEntry::Kind::Render)
        continue;
      e.colorStore.fill(uint8_t(WMTStoreActionStore));
      e.depthStore   = uint8_t(WMTStoreActionStore);
      e.stencilStore = uint8_t(WMTStoreActionStore);
    }

    if (ruleB && frameBoundary) {
      std::unordered_set<const void*> handled;
      for (size_t ri = entries.size(); ri-- > 0u; ) {
        FrameEntry& e = entries[ri];
        if (e.kind != FrameEntry::Kind::Render || e.targets.depthStencil == nullptr)
          continue;
        const Rc<MtlTextureView>& dsv = e.targets.depthStencil;
        if (dsv->image == nullptr || !handled.insert(dsv->image.ptr()).second)
          continue;
        const MtlTexture* img = dsv->image.ptr();
        if ((img->info().usage & MtlTextureUsageShaderRead) != 0u)
          continue;
        if (m_frameSampled.find(img) != m_frameSampled.end())
          continue;
        e.depthStore   = uint8_t(WMTStoreActionDontCare);
        e.stencilStore = uint8_t(WMTStoreActionDontCare);
        prof::bump(prof::CntStoreDontCare);
      }
    }

    if (ruleC) {
      std::map<AttKey, std::pair<size_t, uint32_t>> nextUse;
      for (size_t ri = entries.size(); ri-- > 0u; ) {
        FrameEntry& e = entries[ri];
        if (e.kind != FrameEntry::Kind::Render)
          continue;
        for (uint32_t s = 0u; s < MtlRenderTargets::MaxColorTargets; s++) {
          const Rc<MtlTextureView>& v = e.targets.color[s];
          if (v == nullptr || v->image == nullptr)
            continue;
          AttKey key = attKeyOf(v);
          auto it = nextUse.find(key);
          if (it != nextUse.end()) {
            const auto [nj, ns] = it->second;
            if (entries[nj].clearColor[ns]
             && m_frameSampled.find(v->image.ptr()) == m_frameSampled.end()
             && m_everTransferRead.find(v->image.ptr()) == m_everTransferRead.end()) {
              e.colorStore[s] = uint8_t(WMTStoreActionDontCare);
              prof::bump(prof::CntStoreDontCare);
            }
          }
          nextUse[key] = { ri, s };
        }
      }
    }

    if (ruleF) {
      for (size_t i = 0u; i < entries.size(); ) {
        FrameEntry& e = entries[i];
        bool cull = e.kind == FrameEntry::Kind::Render
                 && e.draws == 0u && chunksEmpty(e);
        if (cull) {
          for (uint32_t s = 0u; s < MtlRenderTargets::MaxColorTargets; s++)
            cull = cull && !(e.targets.color[s] != nullptr
                          && e.colorStore[s] == uint8_t(WMTStoreActionStore));
          if (e.targets.depthStencil != nullptr
           && (e.depthStore   == uint8_t(WMTStoreActionStore)
            || e.stencilStore == uint8_t(WMTStoreActionStore)))
            cull = false;
        }
        if (cull) {
          for (auto& ch : e.chunks) {
            ch.reset();
            m_chunkPool.push_back(std::move(ch));
          }
          entries.erase(entries.begin() + i);
        } else {
          i++;
        }
      }
    }

    uint32_t encoders = 0u;
    for (auto& e : entries) {
      if (e.kind == FrameEntry::Kind::Direct) {
        e.encode(m_cmdbuf.handle());
        e.encode = nullptr;
        continue;
      }

      WMTRenderPassInfo info = {};
      info.visibility_buffer = m_visBuffer.handle();
      for (uint32_t s = 0u; s < MtlRenderTargets::MaxColorTargets; s++) {
        const auto& view = e.targets.color[s];
        if (view == nullptr)
          continue;
        auto* impl = static_cast<MtlTextureViewImpl*>(view.ptr());
        info.colors[s].texture      = impl->owned.handle();
        info.colors[s].load_action  = e.clearColor[s]
          ? WMTLoadActionClear : WMTLoadActionLoad;
        info.colors[s].store_action = WMTStoreAction(e.colorStore[s]);
        if (e.clearColor[s]) {
          const auto& v = e.clearValues[s];
          info.colors[s].clear_color = { v.color.r, v.color.g, v.color.b, v.color.a };
        }
        if (d9mtTrace() && s == 0u)
          Logger::info(str::format("MAT(replay) color img=", (void*)impl->image.ptr(),
            " load=", e.clearColor[s] ? "CLEAR" : "LOAD",
            " store=", e.colorStore[s] == uint8_t(WMTStoreActionStore) ? "STORE" : "DONTCARE"));
      }
      if (e.targets.depthStencil != nullptr) {
        auto* impl = static_cast<MtlTextureViewImpl*>(e.targets.depthStencil.ptr());
        info.depth.texture      = impl->owned.handle();
        info.depth.load_action  = e.clearDepth ? WMTLoadActionClear : WMTLoadActionLoad;
        info.depth.store_action = WMTStoreAction(e.depthStore);
        info.depth.clear_depth  = e.depthClear;
        const auto* fmtInfo = lookupFormatInfo(impl->key.format);
        if (fmtInfo->flags & MtlFormatFlagStencil) {
          info.stencil.texture       = impl->owned.handle();
          info.stencil.load_action   = e.clearStencil ? WMTLoadActionClear : WMTLoadActionLoad;
          info.stencil.store_action  = WMTStoreAction(e.stencilStore);
          info.stencil.clear_stencil = e.stencilClear;
        }
      }

      obj_handle_t enc = newRenderEncoder(m_cmdbuf.handle(), &info);
      for (auto& ch : e.chunks) {
        if (!ch.empty())
          MTL4RenderCommandEncoder_encodeCommands(enc, ch.head());
        ch.reset();
        m_chunkPool.push_back(std::move(ch));
      }
      e.chunks.clear();
      emitProducerBarrier(enc, uint64_t(WMTStageFragment));
      MTLCommandEncoder_endEncoding(enc);
      NSObject_release(enc);
      encoders++;

      for (uint32_t s = 0u; s < MtlRenderTargets::MaxColorTargets; s++) {
        if (e.targets.color[s] != nullptr)
          m_keepAlive.push_back(e.targets.color[s]);
      }
      if (e.targets.depthStencil != nullptr)
        m_keepAlive.push_back(e.targets.depthStencil);
    }
    prof::bump(prof::CntFramePasses, encoders);

    entries.clear();
    m_frameSampled.clear();
  }

}
