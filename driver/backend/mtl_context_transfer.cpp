#include "mtl_context_impl.h"
#include "../util/util_bit.h"
#include "../util/util_string.h"
#include "../util/util_d9mt_debug.h"

#include <algorithm>
#include <cstring>

#include "../util/log/log.h"

#include "mtl_profiler.h"

#include "mtl_blit_metallib.h"

#include "../d3d9/d3d9_caps.h"

namespace dxvk::backend {

  namespace {

    WMTPrimitiveTopologyClass topologyClass(MtlPrimitiveTopology t) {
      switch (t) {
        case MtlPrimitiveTopology::PointList:
          return WMTPrimitiveTopologyClassPoint;
        case MtlPrimitiveTopology::LineList:
        case MtlPrimitiveTopology::LineStrip:
          return WMTPrimitiveTopologyClassLine;
        default:
          return WMTPrimitiveTopologyClassTriangle;
      }
    }

    WMTAttributeFormat attributeFormat(MtlFormat f) {
      switch (f) {
        case MtlFormat::R32Float:     return WMTAttributeFormatFloat;
        case MtlFormat::RG32Float:    return WMTAttributeFormatFloat2;
        case MtlFormat::RGB32Float:   return WMTAttributeFormatFloat3;
        case MtlFormat::RGBA32Float:  return WMTAttributeFormatFloat4;
        case MtlFormat::RGBA8Unorm:   return WMTAttributeFormatUChar4Normalized;
        case MtlFormat::BGRA8Unorm:   return WMTAttributeFormatUChar4Normalized_BGRA;
        case MtlFormat::RGBA8Snorm:   return WMTAttributeFormatChar4Normalized;
        case MtlFormat::RG16Float:    return WMTAttributeFormatHalf2;
        case MtlFormat::RGBA16Float:  return WMTAttributeFormatHalf4;
        case MtlFormat::R16Float:     return WMTAttributeFormatHalf;
        case MtlFormat::RGBA16Snorm:  return WMTAttributeFormatShort4Normalized;
        case MtlFormat::RGBA16Unorm:  return WMTAttributeFormatUShort4Normalized;
        case MtlFormat::RG16Snorm:    return WMTAttributeFormatShort2Normalized;
        case MtlFormat::RG16Unorm:    return WMTAttributeFormatUShort2Normalized;
        case MtlFormat::RGB10A2Unorm: return WMTAttributeFormatUInt1010102Normalized;
        case MtlFormat::RGBA8Uint:    return WMTAttributeFormatUChar4;
        case MtlFormat::RG16Sint:     return WMTAttributeFormatShort2;
        case MtlFormat::RGBA16Sint:   return WMTAttributeFormatShort4;
        case MtlFormat::RGB10A2Uint:  return WMTAttributeFormatUInt1010102Normalized;
        case MtlFormat::RGB10A2Snorm_Vtx: return WMTAttributeFormatInt1010102Normalized;
        default:                      return WMTAttributeFormatFloat4;
      }
    }

  }

  MtlGpuQueryStatus MtlBackendQuery::getData(MtlQueryData& out) {
    if (tracker && serial && !tracker->isComplete(serial))
      return MtlGpuQueryStatus::Pending;

    out = MtlQueryData{};
    if (type == MtlQueryType::Occlusion) {
      uint64_t samples = 0u;
      if (visCpu) {
        for (uint64_t off : slots)
          samples += *reinterpret_cast<const volatile uint64_t*>(
            reinterpret_cast<const char*>(visCpu) + off);
      }
      out.occlusion.samplesPassed = samples;
      if (d9mtTrace())
        Logger::info(str::format("OCC result q=", (void*)this, " segments=", slots.size(),
          " samples=", samples));
    } else
      out.timestamp.time = serial;
    return MtlGpuQueryStatus::Available;
  }

  void MtlBackendQuery::waitData() {
    if (tracker && serial)
      tracker->waitFor(serial);
  }

  MtlGpuEventStatus MtlBackendEvent::test() {
    if (tracker && serial && !tracker->isComplete(serial))
      return MtlGpuEventStatus::Pending;
    return MtlGpuEventStatus::Signaled;
  }

  bool MtlContextImpl::resolvePso() {
    // The state cache's validator (mtl_pso_key.h) checks recorded buffer
    // indices against these: streams at kVertexBufferBaseIndex + 0..15, the
    // null stream (D3D9DeviceEx::NullStreamIdx = caps::MaxStreams) after.
    static_assert(kVertexBufferBaseIndex == kPsoFirstVertexBuffer
               && kPsoLastVertexBuffer == kVertexBufferBaseIndex + caps::MaxStreams,
      "update kPsoFirst/LastVertexBuffer (and kPsoKeyMappingVersion)");

    prof::Scope profScope(prof::ResolvePso);
    if (m_vs == nullptr)
      return false;

    // Shaders compile on the worker pool; the PSO key needs their libraries.
    if (unlikely(!m_vs->ready() || (m_fs != nullptr && !m_fs->ready()))) {
      if (m_asyncSkip) {
        boostPendingShaders();

        if (compile_stats::g_enabled)
          compile_stats::g_skips.add(0u);
        return false;
      }

      // Wait budget: the waits below never run a queued task inline, so
      // queue it ahead first. Out of budget: skip, as with m_asyncSkip.
      if (unlikely(m_waitBudget.enabled())) {
        boostPendingShaders();

        if (!m_waitBudget.wait(m_vs->task.ptr())
         || (m_fs != nullptr && !m_waitBudget.wait(m_fs->task.ptr()))) {
          if (compile_stats::g_enabled)
            compile_stats::g_skips.add(0u);
          return false;
        }
      }

      compile_stats::waitOnRenderThread(m_vs->task.ptr());
      if (m_fs != nullptr)
        compile_stats::waitOnRenderThread(m_fs->task.ptr());
    }

    static const bool mruOff = std::getenv("D9MT_NO_PSOMRU") != nullptr;
    uint32_t passSamples = 0u;
    for (uint32_t i = 0u; i < MtlRenderTargets::MaxColorTargets && !passSamples; i++) {
      const Rc<MtlTextureView>& v = m_pass.targets.color[i];
      if (v != nullptr && v->image != nullptr)
        passSamples = v->image->info().sampleCount;
    }
    if (!passSamples && m_pass.targets.depthStencil != nullptr
     && m_pass.targets.depthStencil->image != nullptr)
      passSamples = m_pass.targets.depthStencil->image->info().sampleCount;
    if (!passSamples)
      passSamples = m_msaa.sampleCount ? m_msaa.sampleCount : 1u;
    if (unlikely(passSamples > 1u)) {
      static bool warned = false;
      if (!warned) {
        warned = true;
        Logger::warn(str::format("MtlContextImpl::resolvePso: MSAA pass, PSO raster sample count ", passSamples));
      }
    }

    PsoSnapshot snap = {};
    snap.vs = m_vs.ptr();
    snap.fs = m_fs.ptr();
    for (uint32_t i = 0u; i < MtlRenderTargets::MaxColorTargets; i++) {
      const Rc<MtlTextureView>& v = m_pass.targets.color[i];
      snap.colorFmt[i] = v != nullptr ? uint32_t(v->key.format) : 0u;
    }
    snap.layoutGen  = m_layoutGen;
    snap.strideGen  = m_strideGen;
    snap.blendGen   = m_blendGen;
    snap.msaaPacked = (passSamples << 1)
                    | (m_msaa.alphaToCoverage ? 1u : 0u);
    snap.topoClass  = uint32_t(topologyClass(m_topology));

    if (!mruOff) {
      for (uint32_t i = 0u; i < 4u; i++) {
        if (m_psoMruPso[i] != nullptr
         && std::memcmp(&m_psoMruKey[i], &snap, sizeof(snap)) == 0)
          return emitPso(m_psoMruPso[i].ptr());
      }
    }
    prof::bump(prof::CntPsoKeyBuild);

    obj_handle_t vsLibrary = m_vs->library;
    obj_handle_t fsLibrary = m_fs != nullptr ? obj_handle_t(m_fs->library) : 0u;
    if (!vsLibrary)
      return false;

    RenderPsoKey key = {};
    key.vsCookie = uint64_t(m_vs->library);
    key.fsCookie = m_fs != nullptr ? uint64_t(m_fs->library) : 0u;

    uint32_t nAttr = std::min<uint32_t>(uint32_t(m_vertexAttrs.size()), 20u);
    for (uint32_t i = 0u; i < nAttr; i++) {
      const MtlVertexAttr& src = m_vertexAttrs[i];
      PsoVertexAttr& a = key.attrs[i];
      a.format      = uint32_t(attributeFormat(src.format));
      a.offset      = uint16_t(src.offset);
      a.bufferIndex = uint8_t(kVertexBufferBaseIndex + src.binding);
    }
    key.attrCount = uint8_t(nAttr);

    uint32_t nLayout = std::min<uint32_t>(uint32_t(m_vertexLayouts.size()), 20u);
    for (uint32_t i = 0u; i < nLayout; i++) {
      const MtlVertexLayout& src = m_vertexLayouts[i];
      PsoVertexLayout& l = key.layouts[i];
      /* A 0 stride here makes Metal assert "buffer at index N has no
       * stride"; prefer the live per-stream stride when present. */
      uint32_t strideVal = src.stride;
      if (src.binding < m_vertexStreams.size()
       && m_vertexStreams[src.binding].stride != 0u)
        strideVal = m_vertexStreams[src.binding].stride;
      l.stride       = uint16_t(strideVal);
      l.stepFunction = src.perInstance ? 2u : 1u;
      l.pad          = uint8_t(kVertexBufferBaseIndex + src.binding);
    }
    key.layoutCount = uint8_t(nLayout);

    uint8_t colorCount = 0u;
    for (uint32_t i = 0u; i < MtlRenderTargets::MaxColorTargets; i++) {
      const Rc<MtlTextureView>& view = m_pass.targets.color[i];
      if (view == nullptr)
        continue;
      PsoColorAttachment& c = key.colors[i];
      const MtlBlendMode& b = m_blend[i];
      c.pixelFormat    = uint32_t(view->key.format);
      c.blendEnabled   = b.enable ? 1u : 0u;
      c.writeMask      = uint8_t(b.writeMask & 0xfu);
      c.rgbSrcFactor   = uint8_t(b.colorSrc);
      c.rgbDstFactor   = uint8_t(b.colorDst);
      c.rgbBlendOp     = uint8_t(b.colorOp);
      c.alphaSrcFactor = uint8_t(b.alphaSrc);
      c.alphaDstFactor = uint8_t(b.alphaDst);
      c.alphaBlendOp   = uint8_t(b.alphaOp);
      colorCount = uint8_t(i + 1u);
    }
    key.colorCount = colorCount;

    key.sampleCount     = uint8_t(passSamples);
    key.topologyClass   = uint8_t(topologyClass(m_topology));
    key.alphaToCoverage = m_msaa.alphaToCoverage ? 1u : 0u;

    const char* vsEntry = m_vs->entryName ? m_vs->entryName : "main";
    const char* fsEntry = (m_fs != nullptr && m_fs->entryName) ? m_fs->entryName : "main";
    RenderPsoCache::Entry* pso = m_psoCache.get(key, vsLibrary, fsLibrary, vsEntry, fsEntry,
      m_vs.ptr(), m_fs.ptr());

    m_psoMruKey[m_psoMruCursor] = snap;
    m_psoMruPso[m_psoMruCursor] = pso;
    m_psoMruCursor = (m_psoMruCursor + 1u) & 3u;

    return emitPso(pso);
  }

  void MtlContextImpl::boostPendingShaders() {
    // Move still-queued prefetch compiles ahead (once per function).
    const MtlShaderFunction* fns[2] = { m_vs.ptr(), m_fs.ptr() };

    for (uint32_t i = 0u; i < 2u; i++) {
      if (fns[i] != nullptr && !fns[i]->ready() && fns[i]->task != m_lastBoosted[i]) {
        m_lastBoosted[i] = fns[i]->task;
        m_device->submitCompile(fns[i]->task, true);
      }
    }
  }

  bool MtlContextImpl::emitPso(RenderPsoCache::Entry* pso) {
    const bool done = pso->isDone();

    if (done && !pso->handle()) {
      // A state cache prewarm build this key adopted, which then failed:
      // build the draw's own instead of skipping its draws all session.
      if (unlikely(pso->prewarmed())) {
        if (RenderPsoCache::Entry* fresh = m_psoCache.replaceFailedPrewarm(pso); fresh && fresh != pso) {
          for (auto& mru : m_psoMruPso) {
            if (mru.ptr() == pso)
              mru = fresh;
          }
          return emitPso(fresh);
        }
      }
      return false;
    }

    if (!done && m_asyncSkip) {
      if (compile_stats::g_enabled)
        compile_stats::g_skips.add(0u);
      return false;
    }

    if (pso == m_lastEncodedPso.ptr())
      return true;
    m_lastEncodedPso = pso;

    auto* cmd = m_cmdChunk.append<wmtcmd_mtl4_setpso>(
      uint16_t(WMTMtl4RenderCommandSetPSO));

    // Still compiling: patched (or dropped) before the chunk is encoded.
    if (done)
      cmd->pso = pso->handle();
    else
      m_psoPatches.push_back({ cmd, pso });
    return true;
  }

  void MtlContextImpl::resolvePsoPatches() {
    if (likely(m_psoPatches.empty()))
      return;

    bool failed = false;

    for (auto& p : m_psoPatches) {
      // Still building and this frame's wait budget is spent: drop the bind
      // and its draws for now, like a failed build but not cached as one.
      // The PSO keeps building; the next draw binds it again.
      if (unlikely(m_waitBudget.enabled()) && !m_waitBudget.wait(p.pso.ptr())) {
        failed = true;
        uint32_t draws = dropPsoBind(p.cmd);

        if (compile_stats::g_enabled && draws)
          compile_stats::g_skips.add(0u, draws);
        continue;
      }

      compile_stats::waitOnRenderThread(p.pso.ptr());
      p.cmd->pso = p.pso->handle();

      if (p.cmd->pso)
        continue;

      // Build failed: drop the bind and the draws that relied on it, as a
      // synchronous failure would have.
      failed = true;
      dropPsoBind(p.cmd);
    }

    m_psoPatches.clear();

    if (failed) {
      m_lastEncodedPso = nullptr;
      m_dirty |= DirtyPso;
    }
  }

  uint32_t MtlContextImpl::dropPsoBind(wmtcmd_mtl4_setpso* cmd) {
    uint32_t draws = 0u;
    cmd->type = WMTMtl4RenderCommandNop;

    for (auto* c = static_cast<wmtcmd_base*>(cmd->next.get()); c != nullptr;
         c = static_cast<wmtcmd_base*>(c->next.get())) {
      if (c->type == WMTMtl4RenderCommandSetPSO)
        break;
      if (c->type == WMTMtl4RenderCommandDraw || c->type == WMTMtl4RenderCommandDrawIndexed) {
        c->type = WMTMtl4RenderCommandNop;
        draws++;
      }
    }

    return draws;
  }

  void MtlContextImpl::appendTableAddress(ArgTableShadow& shadow, obj_handle_t table, uint64_t gpuAddress, uint32_t index) {
    if (shadow.bufferGpuAddress[index] == gpuAddress)
      return;
    shadow.bufferGpuAddress[index] = gpuAddress;
    auto* cmd = m_cmdChunk.append<wmtcmd_mtl4_table_setaddress>(
      uint16_t(WMTMtl4RenderCommandTableSetAddress));
    cmd->table       = table;
    cmd->gpu_address = gpuAddress;
    cmd->index       = index;
  }

  void MtlContextImpl::appendTableTexture(ArgTableShadow& shadow, obj_handle_t table, uint64_t gpuResourceId, uint32_t index) {
    if (shadow.textureId[index] == gpuResourceId)
      return;
    shadow.textureId[index] = gpuResourceId;
    auto* cmd = m_cmdChunk.append<wmtcmd_mtl4_table_setresource>(
      uint16_t(WMTMtl4RenderCommandTableSetTexture));
    cmd->table           = table;
    cmd->gpu_resource_id = gpuResourceId;
    cmd->index           = index;
  }

  void MtlContextImpl::appendTableSampler(ArgTableShadow& shadow, obj_handle_t table, uint64_t gpuResourceId, uint32_t index) {
    if (shadow.samplerId[index] == gpuResourceId)
      return;
    shadow.samplerId[index] = gpuResourceId;
    auto* cmd = m_cmdChunk.append<wmtcmd_mtl4_table_setresource>(
      uint16_t(WMTMtl4RenderCommandTableSetSamplerState));
    cmd->table           = table;
    cmd->gpu_resource_id = gpuResourceId;
    cmd->index           = index;
  }

  void MtlContextImpl::flushVertexBuffers() {
    prof::Scope profScope(prof::FlushVertexBuffers);
    ArgTablePair&   pair   = m_argTables[m_currentArgTable];
    obj_handle_t    table  = pair.table[0].handle();
    ArgTableShadow& shadow = pair.shadow[0];
    const uint32_t streamMask = m_dirtyVertexStreams
      & ((1u << m_vertexStreams.size()) - 1u);
    m_dirtyVertexStreams = 0u;
    for (uint32_t i : bit::BitMask(streamMask)) {
      VertexStream& s = m_vertexStreams[i];
      if (!s.slice.defined() || s.slice.storage() == nullptr) {
        appendTableAddress(shadow, table, 0u, kVertexBufferBaseIndex + i);
        continue;
      }
      if (unlikely(s.slice.storage() != s.slice.buffer()->storage()))
        s_diagVbStale++;
      s.slice.buffer()->setTrackingSerial(m_currentSerial);
      appendTableAddress(shadow, table, s.slice.gpuAddress(), kVertexBufferBaseIndex + i);
    }
  }

  void MtlContextImpl::flushResources() {
    prof::Scope profScope(prof::FlushResources);
    ArgTablePair& pair = m_argTables[m_currentArgTable];
    const bool psoChanged = (m_dirty & DirtyPso) != 0u;
    for (uint32_t st = 0u; st < 2u; st++) {
      StageBindings&  b      = m_bindings[st];
      obj_handle_t    table  = pair.table[st].handle();
      ArgTableShadow& shadow = pair.shadow[st];

      const Rc<MtlShaderFunction>& fn = (st == 0u) ? m_vs : m_fs;

      const bool abShader = fn != nullptr && !fn->abWords.empty();
      if (abShader != (b.abActive != 0u)) {
        b.dirtyCbvs = b.dirtyTextures = b.dirtySamplers = ~0u;
        b.abActive = abShader ? 1u : 0u;
      }

      if (abShader) {
        const uint64_t fnId = uint64_t(fn->library);
        const bool layoutChanged = b.abFnId != fnId;
        const uint32_t dirtyCbvs = layoutChanged ? ~0u : b.dirtyCbvs;
        const uint32_t dirtyTexs = layoutChanged ? ~0u : b.dirtyTextures;
        const uint32_t dirtySmps = layoutChanged ? ~0u : b.dirtySamplers;
        const bool rebuild = layoutChanged || b.abGpuAddress == 0u
          || (dirtyCbvs | dirtyTexs | dirtySmps) != 0u;
        b.dirtyCbvs = b.dirtyTextures = b.dirtySamplers = 0u;

        if (rebuild) {
          prof::bump(prof::CntAbRebuild);
          const uint32_t words = uint32_t(fn->abWords.size());
          if (b.abShadowWords.size() != words) {
            b.abShadowWords.assign(words, 0u);
            b.abFnId = 0u;
          }
          const bool full = layoutChanged || b.abFnId != fnId;
          b.abFnId = fnId;
          uint64_t* sw = b.abShadowWords.data();
          for (uint32_t i = 0u; i < words; i++) {
            const MtlShaderFunction::AbWord& e = fn->abWords[i];
            uint64_t v;
            switch (e.kind) {
              case MtlShaderFunction::AbWordKind::Cbv: {
                if (!full && !(dirtyCbvs & (1u << e.reg)))
                  continue;
                v = 0u;
                if (e.reg < b.cbvs.size()) {
                  StageBindings::CbvBinding& cbv = b.cbvs[e.reg];
                  if (cbv.slice.defined() && cbv.slice.storage() != nullptr
                   && uintptr_t(cbv.slice.storage().ptr()) >= 0x10000u)
                    v = cbv.slice.gpuAddress(cbv.rangeOffset);
                }
              } break;
              case MtlShaderFunction::AbWordKind::Texture: {
                if (!full && !(dirtyTexs & (1u << e.reg)))
                  continue;
                v = 0u;
                if (e.reg < b.textures.size() && b.textures[e.reg] != nullptr)
                  v = static_cast<MtlTextureViewImpl*>(
                    b.textures[e.reg].ptr())->gpuResourceId;
              } break;
              case MtlShaderFunction::AbWordKind::Sampler: {
                if (!full && !(dirtySmps & (1u << e.reg)))
                  continue;
                v = 0u;
                if (e.reg < b.samplers.size() && b.samplers[e.reg] != nullptr)
                  v = static_cast<MtlSamplerStateImpl*>(
                    b.samplers[e.reg].ptr())->gpuResourceId;
              } break;
              default:
                v = 0u;
                break;
            }
            sw[i] = v;
          }
          RingSlice rs = m_device->uploadRing().allocate(
            uint64_t(words) * 8u, 16u);
          if (rs.cpuPtr) {
            std::memcpy(rs.cpuPtr, sw, uint64_t(words) * 8u);
            b.abGpuAddress = rs.gpuAddress;
          }
        }
        appendTableAddress(shadow, table, b.abGpuAddress, kAbIndex);

        if (b.pushDirtyHi > 0u) {
          uint64_t sz = b.pushExtent;
          RingSlice prs = m_device->uploadRing().allocate(sz, 256u);
          if (prs.cpuPtr) {
            std::memcpy(prs.cpuPtr, b.pushData.data(), sz);
            b.pushGpuAddress = prs.gpuAddress;
          }
          b.pushDirtyLo = b.pushDirtyHi = 0u;
        }
        appendTableAddress(shadow, table, b.pushGpuAddress, kPushConstantIndex);
        continue;
      }

      const uint32_t cbvMask = (psoChanged ? ~0u : b.dirtyCbvs)
        & ((1u << b.cbvs.size()) - 1u);
      const uint32_t texMask = b.dirtyTextures
        & ((1u << b.textures.size()) - 1u);
      const uint32_t smpMask = b.dirtySamplers
        & ((1u << b.samplers.size()) - 1u);
      b.dirtyCbvs = b.dirtyTextures = b.dirtySamplers = 0u;

      for (uint32_t slot : bit::BitMask(cbvMask)) {
        StageBindings::CbvBinding& cbv = b.cbvs[slot];
        int32_t airSlot = (fn != nullptr && slot < fn->cbvSlotMap.size())
          ? int32_t(fn->cbvSlotMap[slot]) : -1;
        if (airSlot < 0)
          continue;

        if (!cbv.slice.defined() || cbv.slice.storage() == nullptr
         || uintptr_t(cbv.slice.storage().ptr()) < 0x10000u) {
          appendTableAddress(shadow, table, 0u, uint32_t(airSlot));
          continue;
        }
        if (unlikely(cbv.slice.storage() != cbv.slice.buffer()->storage()))
          s_diagCbvStale++;
        appendTableAddress(shadow, table, cbv.slice.gpuAddress(cbv.rangeOffset),
          uint32_t(airSlot));
      }

      for (uint32_t slot : bit::BitMask(texMask)) {
        const Rc<MtlTextureView>& view = b.textures[slot];
        if (view == nullptr) {
          appendTableTexture(shadow, table, 0u, slot);
          continue;
        }
        auto* tv = static_cast<MtlTextureViewImpl*>(view.ptr());
        appendTableTexture(shadow, table, tv->gpuResourceId, slot);
      }

      for (uint32_t slot : bit::BitMask(smpMask)) {
        const Rc<MtlSamplerState>& sampler = b.samplers[slot];
        if (sampler == nullptr) {
          appendTableSampler(shadow, table, 0u, slot);
          continue;
        }
        auto* sp = static_cast<MtlSamplerStateImpl*>(sampler.ptr());
        appendTableSampler(shadow, table, sp->gpuResourceId, slot);
      }

      if (b.pushDirtyHi > 0u) {
        uint64_t sz = b.pushExtent;
        RingSlice rs = m_device->uploadRing().allocate(sz, 256u);
        if (rs.cpuPtr) {
          std::memcpy(rs.cpuPtr, b.pushData.data(), sz);
          b.pushGpuAddress = rs.gpuAddress;
        }
        b.pushDirtyLo = b.pushDirtyHi = 0u;
      }
      appendTableAddress(shadow, table, b.pushGpuAddress, kPushConstantIndex);
    }
  }

  void MtlContextImpl::flushDynamicState() {
    prof::Scope profScope(prof::FlushDynamicState);
    const bool first = !m_dynValid;
    m_dynValid = true;

    if (first
     || std::memcmp(&m_lastViewport, &m_viewport, sizeof(MtlViewport)) != 0) {
      m_lastViewport = m_viewport;
      auto* cmd = m_cmdChunk.append<wmtcmd_mtl4_setviewport>(
        uint16_t(WMTMtl4RenderCommandSetViewport));
      cmd->viewport.originX = m_viewport.originX;
      cmd->viewport.originY = m_viewport.originY;
      cmd->viewport.width   = m_viewport.width;
      cmd->viewport.height  = m_viewport.height;
      cmd->viewport.znear   = m_viewport.znear;
      cmd->viewport.zfar    = m_viewport.zfar;
    }

    {
      MtlRect sc = m_viewport.scissor;
      if (sc.width == 0u || sc.height == 0u) {
        uint32_t w = 0u, h = 0u;
        for (uint32_t i = 0u; i < MtlRenderTargets::MaxColorTargets && !w; i++) {
          const Rc<MtlTextureView>& v = m_pass.targets.color[i];
          if (v != nullptr && v->image != nullptr) { w = v->image->extent.width; h = v->image->extent.height; }
        }
        if (!w && m_pass.targets.depthStencil != nullptr && m_pass.targets.depthStencil->image != nullptr) {
          w = m_pass.targets.depthStencil->image->extent.width;
          h = m_pass.targets.depthStencil->image->extent.height;
        }
        if (!w) { w = uint32_t(m_viewport.width); h = uint32_t(m_viewport.height); }
        sc = { 0u, 0u, w, h };
      }
      if (first
       || std::memcmp(&m_lastScissor, &sc, sizeof(MtlRect)) != 0) {
        m_lastScissor = sc;
        auto* cmd = m_cmdChunk.append<wmtcmd_mtl4_setscissorrect>(
          uint16_t(WMTMtl4RenderCommandSetScissorRect));
        cmd->rect.x      = sc.x;
        cmd->rect.y      = sc.y;
        cmd->rect.width  = sc.width;
        cmd->rect.height = sc.height;
      }
    }

    {
      // The DSS lookup hashes the whole state and takes the device mutex;
      // m_depthStencil only changes through setDepthStencilState, which sets
      // DirtyDepthStencil, so a viewport/stencil-ref/bias-only flush keeps
      // the last state object.
      obj_handle_t dsso = m_lastDsso;
      if (first || (m_dirty & DirtyDepthStencil))
        dsso = m_device->getDepthStencilState(m_depthStencil);
      if (first || dsso != m_lastDsso) {
        m_lastDsso = dsso;
        auto* dss = m_cmdChunk.append<wmtcmd_mtl4_setdsso>(
          uint16_t(WMTMtl4RenderCommandSetDepthStencilState));
        dss->dsso = dsso;
      }
      if (first || m_stencilRef != m_lastStencilRef) {
        m_lastStencilRef = m_stencilRef;
        auto* sref = m_cmdChunk.append<wmtcmd_mtl4_setstencilref>(
          uint16_t(WMTMtl4RenderCommandSetStencilReferenceValue));
        sref->reference = m_stencilRef;
      }
    }

    if (first
     || std::memcmp(&m_lastBlendColor, &m_blendColor, sizeof(MtlColor4)) != 0) {
      m_lastBlendColor = m_blendColor;
      auto* cmd = m_cmdChunk.append<wmtcmd_mtl4_setblendcolor>(
        uint16_t(WMTMtl4RenderCommandSetBlendColor));
      cmd->red   = m_blendColor.r;
      cmd->green = m_blendColor.g;
      cmd->blue  = m_blendColor.b;
      cmd->alpha = m_blendColor.a;
    }

    {
      // Cull mode, winding and depth bias used to be re-sent on every
      // dynamic-state flush (any viewport, DSS, stencil-ref or blend-color
      // change): three translated objc_msgSends into AGX each time. Encoder
      // state doesn't carry over, so 'first' (m_dynValid, reset whenever
      // materializePass starts an encoder or a deferred pass) sends them all.
      const uint64_t cullMode = uint64_t(WMTCullMode(m_rasterizer.cullMode));
      if (first || cullMode != m_lastCullMode) {
        m_lastCullMode = cullMode;
        auto* cull = m_cmdChunk.append<wmtcmd_mtl4_setcullmode>(
          uint16_t(WMTMtl4RenderCommandSetCullMode));
        cull->mode = cullMode;
      }
      const uint64_t winding = uint64_t(WMTWinding(m_rasterizer.frontFace));
      if (first || winding != m_lastWinding) {
        m_lastWinding = winding;
        auto* wind = m_cmdChunk.append<wmtcmd_mtl4_setwinding>(
          uint16_t(WMTMtl4RenderCommandSetFrontFacingWinding));
        wind->winding = winding;
      }
      if (first || std::memcmp(&m_lastDepthBias, &m_depthBias, sizeof(MtlDepthBias)) != 0) {
        m_lastDepthBias = m_depthBias;
        auto* bias = m_cmdChunk.append<wmtcmd_mtl4_setdepthbias>(
          uint16_t(WMTMtl4RenderCommandSetDepthBias));
        bias->bias        = m_depthBias.constantFactor;
        bias->slope_scale = m_depthBias.slopeFactor;
        bias->clamp       = m_depthBias.clamp;
      }

      // Encoders start in Clip mode; only a change (or a clamp on a fresh
      // encoder) needs a command. Deferred passes may merge encoders, so
      // they always state it on the first flush.
      const bool clamp = !m_rasterizer.depthClip;
      if (first ? (clamp || deferredPassesEnabled()) : clamp != m_lastDepthClamp) {
        auto* dc = m_cmdChunk.append<wmtcmd_mtl4_setdepthclipmode>(
          uint16_t(WMTMtl4RenderCommandSetDepthClipMode));
        dc->mode = uint64_t(clamp ? WMTDepthClipModeClamp : WMTDepthClipModeClip);
      }
      m_lastDepthClamp = clamp;
    }
  }

  void MtlContextImpl::emitProducerBarrier(obj_handle_t encoder, uint64_t afterStages, uint64_t beforeStages) {
    if (!encoder)
      return;
    MTL4CommandEncoder_barrier(encoder, afterStages, beforeStages,
                               uint64_t(WMTVisibilityOptionDevice));
    // Everything rendered so far is ordered before later vertex stages.
    if ((afterStages & uint64_t(WMTStageFragment)) && (beforeStages & uint64_t(WMTStageVertex)))
      m_vtxEpoch++;
  }

  void MtlContextImpl::emitGraphicsBarrier(MtlStageMask, MtlBarrierScope,
                                           MtlStageMask, MtlBarrierScope) {
    if (m_pass.active) {
      if (deferredPassesEnabled()) {
        auto* cmd = m_cmdChunk.append<wmtcmd_mtl4_barrier>(
          uint16_t(WMTMtl4RenderCommandBarrier));
        cmd->after_stages  = uint64_t(WMTStageFragment);
        cmd->before_stages = uint64_t(WMTStageAll);
        cmd->visibility    = uint64_t(WMTVisibilityOptionDevice);
        return;
      }
      flushCmdChunk();
      emitProducerBarrier(m_pass.encoder, uint64_t(WMTStageFragment));
    }
  }

  void MtlContextImpl::copyBuffer(const Rc<MtlBuffer>& dst, MtlDeviceSize dstOff,
                                  const Rc<MtlBuffer>& src, MtlDeviceSize srcOff, MtlDeviceSize bytes) {
    auto* d = static_cast<MtlBufferImpl*>(dst.ptr());
    auto* s = static_cast<MtlBufferImpl*>(src.ptr());
    if (!d || !s)
      return;
    if (tryHoistBufferCopy(d, s, dst, src, dstOff, srcOff, bytes))
      return;
    prepareDirectEncode(uint32_t(prof::CntPassCloseXfer));
    d->setTrackingSerial(m_currentSerial);
    s->setTrackingSerial(m_currentSerial);
    const obj_handle_t sbuf = s->handle.buf;
    const obj_handle_t dbuf = d->handle.buf;
    encodeDirect([this, keepD = dst, keepS = src, sbuf, srcOff, dbuf, dstOff, bytes]
                 (obj_handle_t cmdbuf) {
      obj_handle_t enc = MTL4CommandBuffer_computeCommandEncoder(cmdbuf);
      MTL4ComputeCommandEncoder_copyFromBufferToBuffer(
        enc, sbuf, srcOff, dbuf, dstOff, bytes);
      emitProducerBarrier(enc, uint64_t(WMTStageBlit | WMTStageDispatch));
      MTLCommandEncoder_endEncoding(enc);
      NSObject_release(enc);
    });
  }

  bool MtlContextImpl::tryHoistBufferCopy(MtlBufferImpl* d, MtlBufferImpl* s,
                                          const Rc<MtlBuffer>& dst, const Rc<MtlBuffer>& src,
                                          MtlDeviceSize dstOff, MtlDeviceSize srcOff, MtlDeviceSize bytes) {
    /* DXVK's prepareOutOfOrderTransfer equivalent: a staging->VB/IB upload
     * whose destination this command buffer has not touched yet can run
     * before the whole command buffer, so it doesn't end the render pass.
     *  - dst: VB/IB only. Their GPU use is stamped per command buffer
     *    (flushVertexBuffers / prepareDraw / drawIndexed / copies); uniform
     *    buffers and textures are not stamped on use and stay in order.
     *  - src: CPU-write-only staging (the D3D9 staging pool). Such a buffer
     *    is never a GPU write target, so reading it early can't miss a GPU
     *    product; its own serial can't tell reads from writes.
     *  - one hoisted copy per dst per command buffer (the stamp below makes
     *    later ones go in order), so copies in the init encoder never alias.
     * Off by default (D9MT_XFER_HOIST=1 enables): the two-command-buffer
     * commit path has not been validated in-game yet. */
    static const bool on = [] {
      const char* v = std::getenv("D9MT_XFER_HOIST");
      return v != nullptr && v[0] == '1';
    }();
    if (!on)
      return false;
    const uint32_t vbib = uint32_t(MtlUsage::VertexBuffer) | uint32_t(MtlUsage::IndexBuffer);
    if (!(uint32_t(d->info.usage) & vbib)
     || (uint32_t(d->info.usage) & uint32_t(MtlUsage::UniformBuffer)))
      return false;
    if (s->info.usage != MtlUsage::Staging || s->info.cpuAccess != MtlCpuAccess::Write)
      return false;
    ensureCommandBuffer();
    if (d->trackingSerial() == m_currentSerial)
      return false;

    if (!m_initCmdbuf) {
      if (!m_initAllocators[m_allocatorSlot])
        m_initAllocators[m_allocatorSlot] = WmtAllocator::adopt(
          MTLDevice_newCommandAllocator(m_device->wmtDevice()));
      m_initCmdbuf = WmtCommandBuffer::adopt(
        MTLDevice_newMTL4CommandBuffer(m_device->wmtDevice()));
      MTL4CommandBuffer_beginCommandBuffer(m_initCmdbuf.handle(),
        m_initAllocators[m_allocatorSlot].handle());
      m_initEncoder = MTL4CommandBuffer_computeCommandEncoder(m_initCmdbuf.handle());
    }

    d->setTrackingSerial(m_currentSerial);
    s->setTrackingSerial(m_currentSerial);
    MTL4ComputeCommandEncoder_copyFromBufferToBuffer(
      m_initEncoder, s->handle.buf, srcOff, d->handle.buf, dstOff, bytes);
    m_keepAlive.push_back(dst);
    m_keepAlive.push_back(src);
    return true;
  }

  void MtlContextImpl::uploadBuffer(const Rc<MtlBuffer>& dst, MtlDeviceSize dstOff,
                                    const Rc<MtlBuffer>& staging, MtlDeviceSize srcOff, MtlDeviceSize size) {
    copyBuffer(dst, dstOff, staging, srcOff, size);
  }

  void MtlContextImpl::copyTexture(const Rc<MtlTexture>& dst, MtlSubresourceLayers dstSub, MtlOrigin dstOff,
                                   const Rc<MtlTexture>& src, MtlSubresourceLayers srcSub, MtlOrigin srcOff, MtlSize extent) {
    auto* d = static_cast<MtlTextureImpl*>(dst.ptr());
    auto* s = static_cast<MtlTextureImpl*>(src.ptr());
    if (!d || !s)
      return;
    prepareDirectEncode(uint32_t(prof::CntPassCloseXfer));
    if (m_attOpt) {
      attRead(s, rstore::Reason::Copy);
      attWrite(d, rstore::Reason::CopyDst);
    }
    d->setTrackingSerial(m_currentSerial);
    s->setTrackingSerial(m_currentSerial);
    noteTransferRead(s);
    WMTOrigin srcOrigin = { uint64_t(srcOff.x), uint64_t(srcOff.y), uint64_t(srcOff.z) };
    WMTSize   srcSize   = { extent.width, extent.height, extent.depth };
    WMTOrigin dstOrigin = { uint64_t(dstOff.x), uint64_t(dstOff.y), uint64_t(dstOff.z) };
    const obj_handle_t stex = s->texture.tex;
    const obj_handle_t dtex = d->texture.tex;
    encodeDirect([this, keepD = dst, keepS = src, stex, dtex,
                  srcSub, dstSub, srcOrigin, srcSize, dstOrigin]
                 (obj_handle_t cmdbuf) {
      obj_handle_t enc = MTL4CommandBuffer_computeCommandEncoder(cmdbuf);
      MTL4ComputeCommandEncoder_copyFromTextureToTexture(
        enc, stex, srcSub.baseLayer, srcSub.mipLevel, srcOrigin, srcSize,
        dtex, dstSub.baseLayer, dstSub.mipLevel, dstOrigin);
      emitProducerBarrier(enc, uint64_t(WMTStageBlit | WMTStageDispatch));
      MTLCommandEncoder_endEncoding(enc);
      NSObject_release(enc);
    });
  }

  bool MtlContextImpl::presentUpscale(const Rc<MtlTexture>& dst, const Rc<MtlTexture>& src) {
    static const bool enabled = std::getenv("D9MT_METALFX") != nullptr;
    if (!enabled || m_fxUnsupported)
      return false;
    auto* d = static_cast<MtlTextureImpl*>(dst.ptr());
    auto* s = static_cast<MtlTextureImpl*>(src.ptr());
    if (!d || !s)
      return false;
    const MtlSize se = src->extent, de = dst->extent;
    if (!se.width || !se.height || !de.width || !de.height
     || se.width > de.width || se.height > de.height)
      return false;

    if (!m_fxChecked) {
      m_fxChecked = true;
      if (!MTLDevice_supportsFXSpatialScaler(m_device->wmtDevice())) {
        m_fxUnsupported = true;
        Logger::warn("MtlContextImpl: MetalFX spatial scaler unsupported on this device");
        return false;
      }
    }

    const std::array<uint32_t, 6> key = {
      se.width, se.height, de.width, de.height,
      uint32_t(src->format), uint32_t(dst->format) };
    obj_handle_t scaler = 0u;
    auto it = m_fxScalers.find(key);
    if (it != m_fxScalers.end()) {
      scaler = it->second.scaler;
      it->second.lastUse = m_currentSerial;
    } else {
      WMTFXSpatialScalerInfo info = {};
      info.color_format  = WMTPixelFormat(uint32_t(src->format));
      info.output_format = WMTPixelFormat(uint32_t(dst->format));
      info.input_width   = se.width;
      info.input_height  = se.height;
      info.output_width  = de.width;
      info.output_height = de.height;
      scaler = MTLDevice_newMTL4SpatialScaler(
        m_device->wmtDevice(), m_device->wmtCompiler(), &info);
      if (!scaler) {
        m_fxUnsupported = true;
        Logger::warn("MtlContextImpl: MTL4 spatial scaler creation failed — falling back to blit");
        return false;
      }
      /* A new size (window resize) makes the old scalers dead weight: keep
       * the most recently used one, release the rest once the GPU is past them. */
      while (!m_device->memoryProfile().legacy && m_fxScalers.size() >= 2u) {
        auto lru = m_fxScalers.begin();
        for (auto i = m_fxScalers.begin(); i != m_fxScalers.end(); i++)
          if (i->second.lastUse < lru->second.lastUse)
            lru = i;
        obj_handle_t old = lru->second.scaler;
        m_fxScalers.erase(lru);
        m_device->submissions().retireAfter(m_currentSerial, [old]() { NSObject_release(old); });
      }
      m_fxScalers.emplace(key, FxScaler { scaler, m_currentSerial });
    }

    prepareDirectEncode(uint32_t(prof::CntPassCloseXfer));
    if (m_attOpt) {
      attRead(s, rstore::Reason::Present);
      attWrite(d, rstore::Reason::Present);
    }
    d->setTrackingSerial(m_currentSerial);
    s->setTrackingSerial(m_currentSerial);
    noteTransferRead(s);
    // MetalFX's own stages are unknown: hold all of them for what the passes rendered.
    if (m_narrowBarrier)
      vertexFence(uint64_t(WMTStageAll));
    const obj_handle_t stex = s->texture.tex;
    const obj_handle_t dtex = d->texture.tex;
    encodeDirect([keepD = dst, keepS = src, scaler, stex, dtex]
                 (obj_handle_t cmdbuf) {
      MTL4CommandBuffer_encodeSpatialScale(cmdbuf, scaler, stex, dtex);
    });
    return true;
  }

  namespace {
    void computeBufferImagePitch(
        MtlFormat format, MtlSize extent,
        MtlDeviceSize rowAlign, MtlDeviceSize sliceAlign,
        MtlDeviceSize& bytesPerRow, MtlDeviceSize& bytesPerImage) {
      const MtlFormatInfo* fi = lookupFormatInfo(format);
      const uint32_t elementSize = fi->elementSize ? fi->elementSize : 4u;
      const uint32_t bw = fi->blockWidth  ? fi->blockWidth  : 1u;
      const uint32_t bh = fi->blockHeight ? fi->blockHeight : 1u;
      auto alignUp = [](MtlDeviceSize v, MtlDeviceSize a) {
        return a > 1u ? (v + a - 1u) / a * a : v;
      };
      bytesPerRow   = alignUp(MtlDeviceSize((extent.width + bw - 1u) / bw) * elementSize, rowAlign);
      bytesPerImage = extent.depth > 1u
        ? alignUp(bytesPerRow * ((extent.height + bh - 1u) / bh), sliceAlign)
        : 0u;
    }
  }

  void MtlContextImpl::copyBufferToTexture(const Rc<MtlTexture>& dst, MtlSubresourceLayers dstSub, MtlOrigin dstOff, MtlSize extent,
                                           const Rc<MtlBuffer>& src, MtlDeviceSize off, MtlDeviceSize rowAlign,
                                           MtlDeviceSize sliceAlign, MtlFormat format) {
    auto* d = static_cast<MtlTextureImpl*>(dst.ptr());
    auto* s = static_cast<MtlBufferImpl*>(src.ptr());
    if (!d || !s)
      return;
    prepareDirectEncode(uint32_t(prof::CntPassCloseXfer));
    if (m_attOpt)
      attWrite(d, rstore::Reason::Upload);
    d->setTrackingSerial(m_currentSerial);
    s->setTrackingSerial(m_currentSerial);
    WMTSize   size   = { extent.width, extent.height, extent.depth };
    WMTOrigin origin = { uint64_t(dstOff.x), uint64_t(dstOff.y), uint64_t(dstOff.z) };
    MtlDeviceSize bytesPerRow = 0u, bytesPerImage = 0u;
    computeBufferImagePitch(format != MtlFormat::Invalid ? format : d->format,
      extent, rowAlign, sliceAlign, bytesPerRow, bytesPerImage);
    const obj_handle_t sbuf = s->handle.buf;
    const obj_handle_t dtex = d->texture.tex;
    encodeDirect([this, keepD = dst, keepS = src, sbuf, dtex, off,
                  bytesPerRow, bytesPerImage, size, dstSub, origin]
                 (obj_handle_t cmdbuf) {
      obj_handle_t enc = MTL4CommandBuffer_computeCommandEncoder(cmdbuf);
      MTL4ComputeCommandEncoder_copyFromBufferToTexture(
        enc, sbuf, off, bytesPerRow, bytesPerImage, size,
        dtex, dstSub.baseLayer, dstSub.mipLevel, origin);
      emitProducerBarrier(enc, uint64_t(WMTStageBlit | WMTStageDispatch));
      MTLCommandEncoder_endEncoding(enc);
      NSObject_release(enc);
    });
  }

  void MtlContextImpl::copyTextureToBuffer(const Rc<MtlBuffer>& dst, MtlDeviceSize off, MtlDeviceSize rowAlign,
                                           MtlDeviceSize sliceAlign, MtlFormat format, const Rc<MtlTexture>& src,
                                           MtlSubresourceLayers srcSub, MtlOrigin srcOff, MtlSize extent) {
    auto* d = static_cast<MtlBufferImpl*>(dst.ptr());
    auto* s = static_cast<MtlTextureImpl*>(src.ptr());
    if (!d || !s)
      return;
    prepareDirectEncode(uint32_t(prof::CntPassCloseXfer));
    if (m_attOpt)
      attRead(s, rstore::Reason::Readback);
    d->setTrackingSerial(m_currentSerial);
    s->setTrackingSerial(m_currentSerial);
    noteTransferRead(s);
    WMTOrigin origin = { uint64_t(srcOff.x), uint64_t(srcOff.y), uint64_t(srcOff.z) };
    WMTSize   size   = { extent.width, extent.height, extent.depth };
    MtlDeviceSize bytesPerRow = 0u, bytesPerImage = 0u;
    computeBufferImagePitch(format != MtlFormat::Invalid ? format : s->format,
      extent, rowAlign, sliceAlign, bytesPerRow, bytesPerImage);
    const obj_handle_t stex = s->texture.tex;
    const obj_handle_t dbuf = d->handle.buf;
    encodeDirect([this, keepD = dst, keepS = src, stex, dbuf, off,
                  bytesPerRow, bytesPerImage, size, srcSub, origin]
                 (obj_handle_t cmdbuf) {
      obj_handle_t enc = MTL4CommandBuffer_computeCommandEncoder(cmdbuf);
      MTL4ComputeCommandEncoder_copyFromTextureToBuffer(
        enc, stex, srcSub.baseLayer, srcSub.mipLevel, origin, size,
        dbuf, off, bytesPerRow, bytesPerImage);
      emitProducerBarrier(enc, uint64_t(WMTStageBlit | WMTStageDispatch));
      MTLCommandEncoder_endEncoding(enc);
      NSObject_release(enc);
    });
  }

  void MtlContextImpl::resolveTexture(const Rc<MtlTexture>& dst, const Rc<MtlTexture>& src,
                                      MtlResolveRegion region, MtlFormat format,
                                      MtlResolveMode color, MtlResolveMode stencil) {
    (void)stencil;
    auto* d = static_cast<MtlTextureImpl*>(dst.ptr());
    auto* s = static_cast<MtlTextureImpl*>(src.ptr());
    if (!d || !s)
      return;

    const MtlFormatInfo* fi = lookupFormatInfo(
      format != MtlFormat::Invalid ? format : s->format);
    const bool isDs = (fi->flags & (MtlFormatFlagDepth | MtlFormatFlagStencil)) != 0u;

    if (src->info().sampleCount <= 1u) {
      static bool warned = false;
      if (!warned) {
        warned = true;
        Logger::warn(str::format("MtlContextImpl::resolveTexture: single-sampled source, plain copy (",
          isDs ? "depth-stencil" : "color", ", fmt=", uint32_t(s->format), ")"));
      }
      if (unlikely(s->format != d->format))
        return;
      MtlSize extent = region.extent;
      extent.depth = std::max(extent.depth, 1u);
      copyTexture(dst, region.dstSubresource, region.dstOffset,
                  src, region.srcSubresource, region.srcOffset, extent);
      return;
    }

    if (isDs) {
      resolveDepthStencil(dst, src, region, fi, color);
      return;
    }

    const MtlExtent3D srcExtent = src->mipLevelExtent(region.srcSubresource.mipLevel);
    if (unlikely(region.srcOffset.x || region.srcOffset.y
              || region.dstOffset.x || region.dstOffset.y
              || region.extent.width  < srcExtent.width
              || region.extent.height < srcExtent.height)) {
      static bool warned = false;
      if (!warned) {
        warned = true;
        Logger::warn("MtlContextImpl::resolveTexture: partial region resolved as full subresource");
      }
    }

    // The pass rendering the source is still open and has no encoder yet:
    // resolve with its store action instead of loading every sample again
    // in a pass of its own.
    if (m_attOpt && m_pass.active && !m_pass.encoder
     && !(d9mtdbg::flags() & d9mtdbg::NoAttachOpt)
     && !region.srcOffset.x && !region.srcOffset.y && !region.dstOffset.x && !region.dstOffset.y
     && region.extent.width >= srcExtent.width && region.extent.height >= srcExtent.height
     && !region.srcSubresource.mipLevel && !region.srcSubresource.baseLayer
     && !region.dstSubresource.mipLevel && !region.dstSubresource.baseLayer
     && d->format == s->format && dst->info().sampleCount <= 1u) {
      for (uint32_t i = 0u; i < MtlRenderTargets::MaxColorTargets; i++) {
        const Rc<MtlTextureView>& v = m_pass.targets.color[i];
        if (v == nullptr || v->image.ptr() != src.ptr() || m_pass.resolveTo[i] != nullptr
         || v->key.format != s->format || v->key.baseMip || v->key.baseLayer)
          continue;
        d->setTrackingSerial(m_currentSerial);
        s->setTrackingSerial(m_currentSerial);
        // Overwritten completely: no restore of a skipped clear needed.
        d->attColor.content = d->attDepth.content = d->attStencil.content = attopt::Content::Defined;
        m_pass.resolveTo[i] = dst;
        if (unlikely(prof::g_profEnabled)) {
          const MtlImageInfo& si = src->info();
          const MtlFormatInfo* sfi = lookupFormatInfo(si.format);
          m_rs[rstats::ResolveFolded]++;
          m_rs[rstats::BytesSaved] += uint64_t(si.extent.width) * si.extent.height
            * std::max(si.sampleCount, 1u) * (sfi->elementSize ? sfi->elementSize : 4u);
        }
        prof::bump(prof::CntPassCloseBlit);
        closePass();
        return;
      }
    }

    prepareDirectEncode(uint32_t(prof::CntPassCloseBlit));
    if (m_attOpt) {
      attRead(s, rstore::Reason::Resolve);
      attWrite(d, rstore::Reason::Resolve);
    }

    d->setTrackingSerial(m_currentSerial);
    s->setTrackingSerial(m_currentSerial);
    noteTransferRead(s);

    WMTRenderPassInfo info = {};
    info.colors[0].texture         = s->texture.tex;
    info.colors[0].load_action     = WMTLoadActionLoad;
    info.colors[0].store_action    = WMTStoreActionMultisampleResolve;
    info.colors[0].level           = region.srcSubresource.mipLevel;
    info.colors[0].slice           = region.srcSubresource.baseLayer;
    info.colors[0].resolve_texture = d->texture.tex;
    info.colors[0].resolve_level   = region.dstSubresource.mipLevel;
    info.colors[0].resolve_slice   = region.dstSubresource.baseLayer;
    info.render_target_width       = srcExtent.width;
    info.render_target_height      = srcExtent.height;
    encodeDirect([this, keepD = dst, keepS = src, info](obj_handle_t cmdbuf) mutable {
      obj_handle_t enc = newRenderEncoder(cmdbuf, &info);
      emitProducerBarrier(enc, uint64_t(WMTStageFragment));
      MTLCommandEncoder_endEncoding(enc);
      NSObject_release(enc);
    });
  }

  void MtlContextImpl::resolveDepthStencil(const Rc<MtlTexture>& dst, const Rc<MtlTexture>& src,
                                           const MtlResolveRegion& region, const MtlFormatInfo* fi,
                                           MtlResolveMode depthMode) {
    auto* d = static_cast<MtlTextureImpl*>(dst.ptr());
    auto* s = static_cast<MtlTextureImpl*>(src.ptr());
    const bool hasDepth   = (fi->flags & MtlFormatFlagDepth) != 0u;
    const bool hasStencil = (fi->flags & MtlFormatFlagStencil) != 0u;

    if (unlikely(s->format != d->format || dst->info().sampleCount > 1u
              || !(dst->info().usage & MtlTextureUsageRenderTarget))) {
      static bool warned = false;
      if (!warned) {
        warned = true;
        Logger::warn(str::format("MtlContextImpl::resolveDepthStencil: incompatible dst (srcFmt=",
          uint32_t(s->format), " dstFmt=", uint32_t(d->format), " dstSamples=",
          dst->info().sampleCount, " dstUsage=", uint32_t(dst->info().usage), "), skipped"));
      }
      return;
    }

    static bool logged = false;
    if (!logged) {
      logged = true;
      Logger::warn(str::format("MtlContextImpl::resolveDepthStencil: MSAA depth resolve via render pass (samples=",
        src->info().sampleCount, " fmt=", uint32_t(s->format), " stencil=", hasStencil ? 1 : 0, ")"));
    }

    const MtlExtent3D srcExtent = src->mipLevelExtent(region.srcSubresource.mipLevel);

    prepareDirectEncode(uint32_t(prof::CntPassCloseBlit));
    if (m_attOpt) {
      attRead(s, rstore::Reason::Resolve);
      attWrite(d, rstore::Reason::Resolve);
    }

    d->setTrackingSerial(m_currentSerial);
    s->setTrackingSerial(m_currentSerial);
    noteTransferRead(s);

    WMTRenderPassInfo info = {};
    if (hasDepth) {
      info.depth.texture          = s->texture.tex;
      info.depth.load_action      = WMTLoadActionLoad;
      info.depth.store_action     = WMTStoreActionMultisampleResolve;
      info.depth.level            = region.srcSubresource.mipLevel;
      info.depth.slice            = region.srcSubresource.baseLayer;
      info.ds_resolve.depth_texture = d->texture.tex;
    }
    if (hasStencil) {
      info.stencil.texture        = s->texture.tex;
      info.stencil.load_action    = WMTLoadActionLoad;
      info.stencil.store_action   = WMTStoreActionMultisampleResolve;
      info.stencil.level          = region.srcSubresource.mipLevel;
      info.stencil.slice          = region.srcSubresource.baseLayer;
      info.ds_resolve.stencil_texture = d->texture.tex;
    }
    info.ds_resolve.level          = region.dstSubresource.mipLevel;
    info.ds_resolve.slice          = region.dstSubresource.baseLayer;
    info.ds_resolve.depth_filter   = uint8_t(
        depthMode == MtlResolveMode::Min ? WMTMultisampleDepthResolveFilterMin
      : depthMode == MtlResolveMode::Max ? WMTMultisampleDepthResolveFilterMax
      : WMTMultisampleDepthResolveFilterSample0);
    info.ds_resolve.stencil_filter = uint8_t(WMTMultisampleStencilResolveFilterSample0);
    info.render_target_width       = srcExtent.width;
    info.render_target_height      = srcExtent.height;
    encodeDirect([this, keepD = dst, keepS = src, info](obj_handle_t cmdbuf) mutable {
      obj_handle_t enc = newRenderEncoder(cmdbuf, &info);
      emitProducerBarrier(enc, uint64_t(WMTStageFragment));
      MTLCommandEncoder_endEncoding(enc);
      NSObject_release(enc);
    });
  }

  bool MtlContextImpl::ensureBlitPipeline() {
    if (m_blitVs != nullptr && m_blitFs != nullptr)
      return true;
    if (m_blitInitFailed)
      return false;

    m_blitVs = m_device->createShaderFunction(
      MtlShaderStage::Vertex, g_blitMetallib, size_t(g_blitMetallibSize),
      g_blitMetallibCompat, size_t(g_blitMetallibCompatSize));
    if (m_blitVs != nullptr)
      m_blitVs->entryName = "blitVsMain";
    m_blitFs = m_device->createShaderFunction(
      MtlShaderStage::Fragment, g_blitMetallib, size_t(g_blitMetallibSize),
      g_blitMetallibCompat, size_t(g_blitMetallibCompatSize));
    if (m_blitFs != nullptr)
      m_blitFs->entryName = "blitFsMain";
    m_fillFs = m_device->createShaderFunction(
      MtlShaderStage::Fragment, g_blitMetallib, size_t(g_blitMetallibSize),
      g_blitMetallibCompat, size_t(g_blitMetallibCompatSize));
    if (m_fillFs != nullptr)
      m_fillFs->entryName = "fillFsMain";
    m_depthClearVs = m_device->createShaderFunction(
      MtlShaderStage::Vertex, g_blitMetallib, size_t(g_blitMetallibSize),
      g_blitMetallibCompat, size_t(g_blitMetallibCompatSize));
    if (m_depthClearVs != nullptr)
      m_depthClearVs->entryName = "depthClearVsMain";

    MtlSamplerKey key = {};
    key.addressU = key.addressV = key.addressW = MtlSamplerAddressMode::ClampToEdge;
    key.mipMode  = MtlSamplerMipMode::None;
    key.minFilter = key.magFilter = MtlSamplerFilter::Nearest;
    m_blitSamplers[0] = m_device->createSampler(key);
    key.minFilter = key.magFilter = MtlSamplerFilter::Linear;
    m_blitSamplers[1] = m_device->createSampler(key);

    if (m_blitVs == nullptr || m_blitFs == nullptr || m_fillFs == nullptr
     || m_depthClearVs == nullptr
     || m_blitSamplers[0] == nullptr || m_blitSamplers[1] == nullptr) {
      Logger::err("MtlContextImpl: meta-blit pipeline init failed");
      m_blitVs = nullptr;
      m_blitFs = nullptr;
      m_fillFs = nullptr;
      m_depthClearVs = nullptr;
      m_blitInitFailed = true;
      return false;
    }
    return true;
  }

  void MtlContextImpl::clearImageView(const Rc<MtlTextureView>& view, MtlOrigin offset,
                                      MtlSize extent, MtlAspectMask aspects,
                                      MtlClearValue value) {
    if (view == nullptr || view->image == nullptr)
      return;
    if (!(aspects & MtlAspectColor)) {
      clearDepthStencilRect(view, offset, extent, aspects, value);
      return;
    }

    const MtlExtent3D mipExtent = view->image->mipLevelExtent(view->key.baseMip);
    const bool fullRect = offset.x == 0u && offset.y == 0u
                       && extent.width  >= mipExtent.width
                       && extent.height >= mipExtent.height;

    if (fullRect) {
      /* End an active pass in either mode (a second encoder can't be opened
       * while it is live) and execute pending clears first, so an older
       * folded clear on this image can't land after this one. */
      if (m_pass.active) {
        prof::bump(prof::CntPassCloseClear);
        closePass();
      }
      flushPendingClears();
      PendingPass saved = std::move(m_pass);
      m_pass = PendingPass();
      m_pass.targets.color[0] = view;
      m_pass.clearColor[0]    = true;
      m_pass.clearValues[0]   = value;
      m_pass.pending          = true;
      prof::bump(prof::CntPassCloseClear);
      materializePass();
      closePass();
      m_pass = std::move(saved);
      return;
    }

    /* Metal load-action clears are scissor-blind, so a sub-rect fill needs
     * a scissored solid-color draw instead. */
    if (!ensureBlitPipeline())
      return;

    uint32_t x1 = std::min(offset.x + extent.width,  mipExtent.width);
    uint32_t y1 = std::min(offset.y + extent.height, mipExtent.height);
    if (offset.x >= x1 || offset.y >= y1)
      return;

    RenderPsoKey key = {};
    key.vsCookie              = uint64_t(m_blitVs->library);
    key.fsCookie              = uint64_t(m_fillFs->library);
    key.colors[0].pixelFormat = uint32_t(view->key.format);
    key.colorCount            = 1u;
    key.sampleCount           = uint8_t(std::max(view->image->info().sampleCount, 1u));
    key.topologyClass         = uint8_t(WMTPrimitiveTopologyClassTriangle);
    obj_handle_t pso = m_psoCache.getSync(
      key, m_blitVs->library, m_fillFs->library,
      m_blitVs->entryName, m_fillFs->entryName);
    if (!pso)
      return;

    prepareDirectEncode(uint32_t(prof::CntPassCloseClear));
    if (m_attOpt)
      attWrite(view->image.ptr(), rstore::Reason::PartialClear);

    auto* tv = static_cast<MtlTextureViewImpl*>(view.ptr());
    view->image->setTrackingSerial(m_currentSerial);
    m_keepAlive.push_back(view);

    WMTRenderPassInfo info = {};
    info.colors[0].texture      = tv->owned.handle();
    info.colors[0].load_action  = WMTLoadActionLoad;
    info.colors[0].store_action = WMTStoreActionStore;
    info.render_target_width    = mipExtent.width;
    info.render_target_height   = mipExtent.height;

    WMTViewport vp = {};
    vp.originX = double(offset.x);
    vp.originY = double(offset.y);
    vp.width   = double(x1 - offset.x);
    vp.height  = double(y1 - offset.y);
    vp.znear   = 0.0;
    vp.zfar    = 1.0;
    WMTScissorRect sc = {};
    sc.x      = offset.x;
    sc.y      = offset.y;
    sc.width  = x1 - offset.x;
    sc.height = y1 - offset.y;

    float params[8] = {
      0.f, 0.f, 0.f, 0.f,
      value.color.r, value.color.g, value.color.b, value.color.a,
    };
    RingSlice rs = m_device->uploadRing().allocate(sizeof(params), 256u);
    if (rs.cpuPtr)
      std::memcpy(rs.cpuPtr, params, sizeof(params));

    obj_handle_t table = selectBlitArgTable();
    MTL4ArgumentTable_setAddress(table, rs.gpuAddress, 0u);

    encodeDirect([this, keepView = view, info, pso, vp, sc, table]
                 (obj_handle_t cmdbuf) mutable {
      obj_handle_t enc = newRenderEncoder(cmdbuf, &info);
      MTL4RenderCommandEncoder_setRenderPipelineState(enc, pso);
      MTL4RenderCommandEncoder_setViewport(enc, vp);
      MTL4RenderCommandEncoder_setScissorRect(enc, sc);
      MTL4RenderCommandEncoder_setArgumentTable(
        enc, table, uint64_t(WMTRenderStageVertex | WMTRenderStageFragment));
      MTL4RenderCommandEncoder_drawPrimitives(
        enc, WMTPrimitiveTypeTriangle, 0u, 3u, 1u, 0u);
      emitProducerBarrier(enc, uint64_t(WMTStageFragment));
      MTLCommandEncoder_endEncoding(enc);
      NSObject_release(enc);
    });
    m_dirty = DirtyEverything;
  }

  void MtlContextImpl::clearDepthStencilRect(const Rc<MtlTextureView>& view, MtlOrigin offset,
                                             MtlSize extent, MtlAspectMask aspects,
                                             MtlClearValue value) {
    const MtlFormatInfo* fi = lookupFormatInfo(view->key.format);
    const bool hasDepth   = (fi->flags & MtlFormatFlagDepth) != 0u;
    const bool hasStencil = (fi->flags & MtlFormatFlagStencil) != 0u;
    const bool clearDepth   = hasDepth   && (aspects & MtlAspectDepth) != 0u;
    const bool clearStencil = hasStencil && (aspects & MtlAspectStencil) != 0u;
    if (!clearDepth && !clearStencil)
      return;

    if (!ensureBlitPipeline())
      return;

    const MtlExtent3D mipExtent = view->image->mipLevelExtent(view->key.baseMip);
    uint32_t x1 = std::min(offset.x + extent.width,  mipExtent.width);
    uint32_t y1 = std::min(offset.y + extent.height, mipExtent.height);
    if (offset.x >= x1 || offset.y >= y1)
      return;

    RenderPsoKey key = {};
    key.vsCookie      = uint64_t(m_depthClearVs->library);
    key.fsCookie      = 0u;
    key.colorCount    = 0u;
    key.sampleCount   = uint8_t(std::max(view->image->info().sampleCount, 1u));
    key.topologyClass = uint8_t(WMTPrimitiveTopologyClassTriangle);
    key.depthFormat   = uint32_t(view->key.format);
    obj_handle_t pso = m_psoCache.getSync(
      key, m_depthClearVs->library, 0u, m_depthClearVs->entryName, nullptr);
    if (!pso)
      return;

    MtlDepthStencilState ds = {};
    ds.depthTest      = true;
    ds.depthCompareOp = MtlCompareFunction::Always;
    ds.depthWrite     = clearDepth;
    ds.stencilEnable  = clearStencil;
    if (clearStencil) {
      MtlStencilState s = {};
      s.compareOp = MtlCompareFunction::Always;
      s.failOp = s.depthFailOp = s.passOp = MtlStencilOperation::Replace;
      ds.front = ds.back = s;
    }
    obj_handle_t dsso = m_device->getDepthStencilState(ds);

    prepareDirectEncode(uint32_t(prof::CntPassCloseClear));
    if (m_attOpt)
      attWrite(view->image.ptr(), rstore::Reason::PartialClear);

    auto* tv = static_cast<MtlTextureViewImpl*>(view.ptr());
    view->image->setTrackingSerial(m_currentSerial);
    m_keepAlive.push_back(view);

    WMTRenderPassInfo info = {};
    if (hasDepth) {
      info.depth.texture      = tv->owned.handle();
      info.depth.load_action  = WMTLoadActionLoad;
      info.depth.store_action = WMTStoreActionStore;
      info.depth.level        = view->key.baseMip;
      info.depth.slice        = view->key.baseLayer;
    }
    if (hasStencil) {
      info.stencil.texture      = tv->owned.handle();
      info.stencil.load_action  = WMTLoadActionLoad;
      info.stencil.store_action = WMTStoreActionStore;
      info.stencil.level        = view->key.baseMip;
      info.stencil.slice        = view->key.baseLayer;
    }
    info.render_target_width  = mipExtent.width;
    info.render_target_height = mipExtent.height;

    WMTViewport vp = {};
    vp.originX = double(offset.x);
    vp.originY = double(offset.y);
    vp.width   = double(x1 - offset.x);
    vp.height  = double(y1 - offset.y);
    vp.znear   = 0.0;
    vp.zfar    = 1.0;
    WMTScissorRect sc = {};
    sc.x      = offset.x;
    sc.y      = offset.y;
    sc.width  = x1 - offset.x;
    sc.height = y1 - offset.y;

    float params[8] = {
      0.f, 0.f, 0.f, 0.f,
      value.depth, 0.f, 0.f, 0.f,
    };
    RingSlice rs = m_device->uploadRing().allocate(sizeof(params), 256u);
    if (rs.cpuPtr)
      std::memcpy(rs.cpuPtr, params, sizeof(params));

    obj_handle_t table = selectBlitArgTable();
    MTL4ArgumentTable_setAddress(table, rs.gpuAddress, 0u);

    const uint32_t stencilRef = value.stencil;
    encodeDirect([this, keepView = view, info, pso, dsso, clearStencil,
                  stencilRef, vp, sc, table]
                 (obj_handle_t cmdbuf) mutable {
      obj_handle_t enc = newRenderEncoder(cmdbuf, &info);
      MTL4RenderCommandEncoder_setRenderPipelineState(enc, pso);
      MTL4RenderCommandEncoder_setDepthStencilState(enc, dsso);
      if (clearStencil)
        MTL4RenderCommandEncoder_setStencilReferenceValue(enc, stencilRef);
      MTL4RenderCommandEncoder_setViewport(enc, vp);
      MTL4RenderCommandEncoder_setScissorRect(enc, sc);
      MTL4RenderCommandEncoder_setArgumentTable(
        enc, table, uint64_t(WMTRenderStageVertex | WMTRenderStageFragment));
      MTL4RenderCommandEncoder_drawPrimitives(
        enc, WMTPrimitiveTypeTriangle, 0u, 3u, 1u, 0u);
      emitProducerBarrier(enc, uint64_t(WMTStageFragment));
      MTLCommandEncoder_endEncoding(enc);
      NSObject_release(enc);
    });
    m_dirty = DirtyEverything;
  }

  obj_handle_t MtlContextImpl::selectBlitArgTable() {
    const uint64_t completed = m_device->submissions().completedSerial();
    const size_t   n         = m_blitArgTables.size();

    for (size_t scan = 0u; scan < n; ++scan) {
      const size_t idx = (m_blitArgCursor + scan) % n;
      if (m_blitArgTables[idx].serial <= completed) {
        m_blitArgCursor              = uint32_t(idx + 1u);
        m_blitArgTables[idx].serial  = m_currentSerial;
        return m_blitArgTables[idx].table.handle();
      }
    }

    WMTArgTableInfo ati = {};
    ati.max_buffers         = 1u;
    ati.max_textures        = 1u;
    ati.max_samplers        = 1u;
    ati.initialize_bindings = 1u;
    BlitArgTable entry;
    entry.table  = WmtArgTable::adopt(
      MTLDevice_newArgumentTable(m_device->wmtDevice(), &ati));
    entry.serial = m_currentSerial;
    m_blitArgTables.push_back(std::move(entry));
    m_blitArgCursor = uint32_t(m_blitArgTables.size());
    return m_blitArgTables.back().table.handle();
  }

  void MtlContextImpl::blitTextureView(const Rc<MtlTextureView>& dstView, const MtlOffset3D dstOffsets[2],
                                       const Rc<MtlTextureView>& srcView, const MtlOffset3D srcOffsets[2],
                                       MtlSamplerFilter filter) {
    if (dstView == nullptr || srcView == nullptr
     || dstView->image == nullptr || srcView->image == nullptr)
      return;

    const MtlFormatInfo* dstFi = lookupFormatInfo(dstView->key.format);
    if (unlikely(dstFi->flags & (MtlFormatFlagDepth | MtlFormatFlagStencil))) {
      Logger::err("MtlContextImpl::blitTextureView: depth-stencil destination unsupported");
      return;
    }

    if (unlikely(srcView->key.viewType != MtlTextureViewType::e2D)) {
      static bool warned = false;
      if (!warned) { warned = true; Logger::warn("MtlContextImpl::blitTextureView: non-2D source view, sampling as 2D"); }
    }

    if (!ensureBlitPipeline())
      return;

    int32_t dx0 = dstOffsets[0].x, dy0 = dstOffsets[0].y;
    int32_t dx1 = dstOffsets[1].x, dy1 = dstOffsets[1].y;
    float   sx0 = float(srcOffsets[0].x), sy0 = float(srcOffsets[0].y);
    float   sx1 = float(srcOffsets[1].x), sy1 = float(srcOffsets[1].y);
    if (dx0 > dx1) { std::swap(dx0, dx1); std::swap(sx0, sx1); }
    if (dy0 > dy1) { std::swap(dy0, dy1); std::swap(sy0, sy1); }
    if (dx0 == dx1 || dy0 == dy1)
      return;

    const MtlExtent3D srcExtent = srcView->image->mipLevelExtent(srcView->key.baseMip);
    const MtlExtent3D dstExtent = dstView->image->mipLevelExtent(dstView->key.baseMip);
    if (!srcExtent.width || !srcExtent.height)
      return;

    dx0 = std::max(dx0, 0);
    dy0 = std::max(dy0, 0);
    dx1 = std::min(dx1, int32_t(dstExtent.width));
    dy1 = std::min(dy1, int32_t(dstExtent.height));
    if (dx0 >= dx1 || dy0 >= dy1)
      return;

    RenderPsoKey key = {};
    key.vsCookie             = uint64_t(m_blitVs->library);
    key.fsCookie             = uint64_t(m_blitFs->library);
    key.colors[0].pixelFormat = uint32_t(dstView->key.format);
    key.colorCount           = 1u;
    key.sampleCount          = uint8_t(std::max(dstView->image->info().sampleCount, 1u));
    key.topologyClass        = uint8_t(WMTPrimitiveTopologyClassTriangle);
    obj_handle_t pso = m_psoCache.getSync(
      key, m_blitVs->library, m_blitFs->library,
      m_blitVs->entryName, m_blitFs->entryName);
    if (!pso)
      return;

    prepareDirectEncode(uint32_t(prof::CntPassCloseBlit));
    if (m_attOpt) {
      attRead(srcView->image.ptr(), rstore::Reason::Blit);
      attWrite(dstView->image.ptr(), rstore::Reason::Blit);
    }

    auto* dstTv = static_cast<MtlTextureViewImpl*>(dstView.ptr());
    auto* srcTv = static_cast<MtlTextureViewImpl*>(srcView.ptr());
    dstView->image->setTrackingSerial(m_currentSerial);
    srcView->image->setTrackingSerial(m_currentSerial);
    noteTransferRead(srcView->image.ptr());
    m_keepAlive.push_back(dstView);
    m_keepAlive.push_back(srcView);

    WMTRenderPassInfo info = {};
    info.colors[0].texture      = dstTv->owned.handle();
    info.colors[0].load_action  = WMTLoadActionLoad;
    info.colors[0].store_action = WMTStoreActionStore;
    info.render_target_width    = dstExtent.width;
    info.render_target_height   = dstExtent.height;

    WMTViewport vp = {};
    vp.originX = double(dx0);
    vp.originY = double(dy0);
    vp.width   = double(dx1 - dx0);
    vp.height  = double(dy1 - dy0);
    vp.znear   = 0.0;
    vp.zfar    = 1.0;
    WMTScissorRect sc = {};
    sc.x      = uint32_t(dx0);
    sc.y      = uint32_t(dy0);
    sc.width  = uint32_t(dx1 - dx0);
    sc.height = uint32_t(dy1 - dy0);

    float params[4] = {
      sx0 / float(srcExtent.width), sy0 / float(srcExtent.height),
      sx1 / float(srcExtent.width), sy1 / float(srcExtent.height),
    };
    RingSlice rs = m_device->uploadRing().allocate(sizeof(params), 256u);
    if (rs.cpuPtr)
      std::memcpy(rs.cpuPtr, params, sizeof(params));

    obj_handle_t table = selectBlitArgTable();
    MTL4ArgumentTable_setAddress(table, rs.gpuAddress, 0u);
    MTL4ArgumentTable_setTexture(table, srcTv->gpuResourceId, 0u);
    auto* smp = static_cast<MtlSamplerStateImpl*>(
      m_blitSamplers[filter == MtlSamplerFilter::Linear ? 1u : 0u].ptr());
    MTL4ArgumentTable_setSamplerState(table, smp->gpuResourceId, 0u);

    encodeDirect([this, keepDst = dstView, keepSrc = srcView, info, pso, vp, sc, table]
                 (obj_handle_t cmdbuf) mutable {
      obj_handle_t enc = newRenderEncoder(cmdbuf, &info);
      MTL4RenderCommandEncoder_setRenderPipelineState(enc, pso);
      MTL4RenderCommandEncoder_setViewport(enc, vp);
      MTL4RenderCommandEncoder_setScissorRect(enc, sc);
      MTL4RenderCommandEncoder_setArgumentTable(
        enc, table, uint64_t(WMTRenderStageVertex | WMTRenderStageFragment));
      MTL4RenderCommandEncoder_drawPrimitives(
        enc, WMTPrimitiveTypeTriangle, 0u, 3u, 1u, 0u);
      emitProducerBarrier(enc, uint64_t(WMTStageFragment));
      MTLCommandEncoder_endEncoding(enc);
      NSObject_release(enc);
    });

    m_dirty = DirtyEverything;
  }

  void MtlContextImpl::generateMipmaps(const Rc<MtlTextureView>& view, MtlSamplerFilter) {
    if (view == nullptr)
      return;
    auto* tv = static_cast<MtlTextureViewImpl*>(view.ptr());
    obj_handle_t tex = view->image != nullptr
      ? static_cast<MtlTextureImpl*>(view->image.ptr())->owned.handle()
      : tv->owned.handle();
    if (!tex)
      return;
    prepareDirectEncode(uint32_t(prof::CntPassCloseXfer));
    if (m_attOpt)
      attWrite(view->image.ptr(), rstore::Reason::MipGen);
    if (view->image != nullptr) {
      view->image->setTrackingSerial(m_currentSerial);
      noteTransferRead(view->image.ptr());
    }
    encodeDirect([this, keepView = view, tex](obj_handle_t cmdbuf) {
      obj_handle_t enc = MTL4CommandBuffer_computeCommandEncoder(cmdbuf);
      MTL4ComputeCommandEncoder_generateMipmaps(enc, tex);
      emitProducerBarrier(enc, uint64_t(WMTStageBlit | WMTStageDispatch));
      MTLCommandEncoder_endEncoding(enc);
      NSObject_release(enc);
    });
  }

  void MtlContextImpl::initBuffer(const Rc<MtlBuffer>& buffer) {
    auto* b = static_cast<MtlBufferImpl*>(buffer.ptr());
    if (!b)
      return;
    prepareDirectEncode(uint32_t(prof::CntPassCloseXfer));
    b->setTrackingSerial(m_currentSerial);
    const obj_handle_t buf  = b->handle.buf;
    const uint64_t     size = b->size;
    encodeDirect([this, keep = buffer, buf, size](obj_handle_t cmdbuf) {
      obj_handle_t enc = MTL4CommandBuffer_computeCommandEncoder(cmdbuf);
      MTL4ComputeCommandEncoder_fillBuffer(enc, buf, 0u, size, 0u);
      emitProducerBarrier(enc, uint64_t(WMTStageBlit | WMTStageDispatch));
      MTLCommandEncoder_endEncoding(enc);
      NSObject_release(enc);
    });
  }

  void MtlContextImpl::initImage(const Rc<MtlTexture>& image) {
    auto* t = static_cast<MtlTextureImpl*>(image.ptr());
    if (!t)
      return;

    /* A render pass built on a non-attachment texture (sampled-only usage,
     * or a compressed format) is an invalid descriptor that faults the GPU
     * and poisons the queue — skip the clear for those; they get their
     * defined content from uploads instead. */
    const bool renderable = (uint32_t(t->info.usage)
      & (uint32_t(MtlUsage::RenderTarget) | uint32_t(MtlUsage::DepthStencil))) != 0u;
    if (!renderable)
      return;

    if (m_pass.active) {
      prof::bump(prof::CntPassCloseBlit);
      closePass();
    }
    ensureCommandBuffer();
    t->setTrackingSerial(m_currentSerial);

    const MtlFormatInfo* fi = lookupFormatInfo(t->format);
    const bool depthStencil = (fi->flags & (MtlFormatFlagDepth | MtlFormatFlagStencil)) != 0u;

    WMTRenderPassInfo info = {};
    info.render_target_width  = t->extent.width;
    info.render_target_height = t->extent.height;

    if (depthStencil) {
      info.depth.texture      = t->owned.handle();
      info.depth.load_action  = WMTLoadActionClear;
      info.depth.store_action = WMTStoreActionStore;
      info.depth.clear_depth  = 1.0f;
      if (fi->flags & MtlFormatFlagStencil) {
        info.stencil.texture      = t->owned.handle();
        info.stencil.load_action  = WMTLoadActionClear;
        info.stencil.store_action = WMTStoreActionStore;
        info.stencil.clear_stencil = 0u;
      }
    } else {
      info.colors[0].texture      = t->owned.handle();
      info.colors[0].load_action  = WMTLoadActionClear;
      info.colors[0].store_action = WMTStoreActionStore;
      info.colors[0].clear_color  = { 0.0, 0.0, 0.0, 0.0 };
    }

    encodeDirect([this, keep = image, info](obj_handle_t cmdbuf) mutable {
      obj_handle_t enc = newRenderEncoder(cmdbuf, &info);
      emitProducerBarrier(enc, uint64_t(WMTStageFragment));
      MTLCommandEncoder_endEncoding(enc);
      NSObject_release(enc);
    });

    // Memory now holds a known clear: passes can Clear-load it, and skip
    // storing it while nothing writes it.
    if (m_attOpt) {
      if (t->info.mipLevels <= 1u && t->info.arrayLayers <= 1u && !t->info.cube
       && t->info.type == MtlImageType::e2D) {
        t->attColor = t->attDepth = t->attStencil = attopt::AspectState();
        t->attColor.content = t->attDepth.content = t->attStencil.content = attopt::Content::Cleared;
        t->attDepth.value.c[0] = 1.0f;
      }
    }
  }

  void MtlContextImpl::invalidateBuffer(const Rc<MtlBuffer>& buffer, Rc<MtlAllocation>&& slice) {
    if (slice == nullptr)
      return;
    ensureCommandBuffer();

    /* The tracking serial answers "is the GPU still using what the CPU maps":
     * after a rename that is the new backing, which nothing recorded so far
     * uses (the old one is kept alive by retireAfter below). Every later use
     * re-stamps it: refreshed vertex streams are flagged dirty, the index
     * buffer is stamped per draw. So a non-DISCARD lock after a DISCARD no
     * longer waits for the old backing. */
    if (!m_device->memoryProfile().legacy)
      buffer->setTrackingSerial(0u);

    for (uint32_t i = 0u; i < m_vertexStreams.size(); i++) {
      auto& stream = m_vertexStreams[i];
      if (stream.slice.buffer() == buffer) {
        stream.slice.refreshStorage();
        m_dirtyVertexStreams |= 1u << i;
        m_dirty |= DirtyVertexBuffers;
      }
    }
    if (m_indexBuffer.buffer() == buffer)
      m_indexBuffer.refreshStorage();
    for (uint32_t st = 0u; st < 2u; st++) {
      for (uint32_t slot = 0u; slot < m_bindings[st].cbvs.size(); slot++) {
        auto& cbv = m_bindings[st].cbvs[slot];
        if (cbv.slice.buffer() == buffer) {
          cbv.slice.refreshStorage();
          m_bindings[st].dirtyCbvs |= 1u << slot;
          m_dirty |= DirtyResources;
        }
      }
    }

    Rc<MtlBuffer> owner = buffer;
    Rc<MtlAllocation> keep = std::move(slice);
    m_device->submissions().retireAfter(m_currentSerial, [owner, keep]() mutable {
      if (owner != nullptr)
        static_cast<MtlBufferImpl*>(owner.ptr())->recycleStorage(std::move(keep));
      owner = nullptr;
      keep  = nullptr;
    });
  }

  bool MtlContextImpl::ensureTextureUsage(const Rc<MtlTexture>&, MtlTextureUsage) {
    return true;
  }

  void MtlContextImpl::ensureVisBuffer() {
    if (m_visBuffer)
      return;
    // CPU-visible memory must live in our address space (32-bit process): Metal's own
    // contents pointer is unreachable from i386 code. 512 KiB is a whole number of pages.
    const uint64_t length = uint64_t(kVisSlots) * 8u;
    void* host = VirtualAlloc(nullptr, SIZE_T(length), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    WMTBufferInfo info = {};
    info.length  = length;
    info.options = WMTResourceOptions(WMTResourceStorageModeShared | WMTResourceHazardTrackingModeUntracked);
    info.memory.set(host);
    obj_handle_t h = MTLDevice_newBuffer(m_device->wmtDevice(), &info);
    m_visBuffer = WmtBuffer::adopt(h);
    m_visCpu    = host;
    m_device->residency().allocCreated(h, info.length);
  }

  void MtlContextImpl::beginQuery(const Rc<MtlQuery>& query) {
    auto* q = static_cast<MtlBackendQuery*>(query.ptr());
    if (!q || q->type != MtlQueryType::Occlusion)
      return;
    ensureVisBuffer();
    q->visCpu  = m_visCpu;
    q->tracker = &m_device->submissions();
    q->slots.clear();

    removeActiveOcclusion(q);
    m_activeOcclusion.push_back(query);
    newVisSegment();
    if (d9mtTrace())
      Logger::info(str::format("OCC begin q=", (void*)q, " slot=", m_visSegment,
        " passActive=", m_pass.active ? 1 : 0, " nested=", m_activeOcclusion.size() - 1u));
  }

  bool MtlContextImpl::removeActiveOcclusion(const MtlQuery* query) {
    for (auto it = m_activeOcclusion.begin(); it != m_activeOcclusion.end(); ++it) {
      if (it->ptr() == query) {
        m_activeOcclusion.erase(it);
        return true;
      }
    }
    return false;
  }

  void MtlContextImpl::newVisSegment() {
    /* Called whenever the active query set changes. Metal counts into a
     * single offset, so the set gets a fresh zeroed slot that every active
     * query adds to its own slot list (the GPU only accumulates into it;
     * passes use MTLVisibilityResultTypeAccumulate). */
    const bool emit = m_pass.active || deferredPassesEnabled();
    if (m_activeOcclusion.empty()) {
      if (emit) {
        auto* cmd = m_cmdChunk.append<wmtcmd_mtl4_setvisibilitymode>(
          uint16_t(WMTMtl4RenderCommandSetVisibilityResultMode));
        cmd->mode   = uint64_t(WMTVisibilityResultModeDisabled);
        cmd->offset = 0u;
      }
      return;
    }

    m_visSegment = (m_visSlot % kVisSlots) * 8u;
    m_visSlot++;
    if (m_visCpu)
      *reinterpret_cast<uint64_t*>(reinterpret_cast<char*>(m_visCpu) + m_visSegment) = 0u;
    for (const auto& a : m_activeOcclusion)
      static_cast<MtlBackendQuery*>(a.ptr())->slots.push_back(m_visSegment);

    if (emit)
      armVisibility();
  }

  void MtlContextImpl::armVisibility() {
    if (m_activeOcclusion.empty())
      return;
    auto* cmd = m_cmdChunk.append<wmtcmd_mtl4_setvisibilitymode>(
      uint16_t(WMTMtl4RenderCommandSetVisibilityResultMode));
    cmd->mode   = uint64_t(WMTVisibilityResultModeCounting);
    cmd->offset = m_visSegment;
  }

  void MtlContextImpl::endQuery(const Rc<MtlQuery>& query) {
    auto* q = static_cast<MtlBackendQuery*>(query.ptr());
    if (!q)
      return;
    ensureCommandBuffer();
    q->serial  = m_currentSerial;
    q->tracker = &m_device->submissions();

    if (q->type != MtlQueryType::Occlusion)
      return;

    if (removeActiveOcclusion(q))
      newVisSegment();
  }

  void MtlContextImpl::writeTimestamp(const Rc<MtlQuery>& query) {
    auto* q = static_cast<MtlBackendQuery*>(query.ptr());
    if (!q)
      return;
    ensureCommandBuffer();
    q->serial  = m_currentSerial;
    q->tracker = &m_device->submissions();
  }

  void MtlContextImpl::signal(const Rc<MtlSignal>& signal, uint64_t value) {
    ensureCommandBuffer();
    Rc<MtlSignal> keep = signal;
    m_device->submissions().retireAfter(m_currentSerial,
      [keep, value]() { keep->signal(value); });
  }

  void MtlContextImpl::signalGpuEvent(const Rc<MtlEvent>& event) {
    auto* e = static_cast<MtlBackendEvent*>(event.ptr());
    if (!e)
      return;
    ensureCommandBuffer();
    e->serial  = m_currentSerial;
    e->tracker = &m_device->submissions();
  }

  void MtlContextImpl::track(Rc<MtlResource>&& resource) {
    if (resource != nullptr)
      m_keepAlive.push_back(std::move(resource));
  }

  void MtlContextImpl::beginDebugLabel(const char*)  { }
  void MtlContextImpl::endDebugLabel()               { }
  void MtlContextImpl::insertDebugLabel(const char*) { }

}
