#include "../../bridge/winemetal.h"

#include "mtl_pso_cache.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <atomic>
#include <chrono>

#include "../util/log/log.h"
#include "../util/util_string.h"

#include "../mtl/mtl_state_cache.h"

#include "mtl_profiler.h"

namespace dxvk::backend {

  namespace {
    uint8_t wmtWriteMask(uint8_t v) {
      uint8_t m = 0u;
      if (v & 0x1u) m |= WMTColorWriteMaskRed;
      if (v & 0x2u) m |= WMTColorWriteMaskGreen;
      if (v & 0x4u) m |= WMTColorWriteMaskBlue;
      if (v & 0x8u) m |= WMTColorWriteMaskAlpha;
      return m;
    }
  }

  static_assert(sizeof(RenderPsoKey) == RenderPsoKey::StateOffset + statecache::KeyBytes,
    "the state cache records RenderPsoKey after its two cookies");

  // The validator's numbers (mtl_pso_key.h) are winemetal.h's.
  static_assert(WMTAttributeFormatUChar4 == 3 && WMTAttributeFormatUChar4Normalized == 9
             && WMTAttributeFormatChar4Normalized == 12 && WMTAttributeFormatShort2 == 16
             && WMTAttributeFormatShort4 == 18 && WMTAttributeFormatUShort2Normalized == 19
             && WMTAttributeFormatUShort4Normalized == 21 && WMTAttributeFormatShort2Normalized == 22
             && WMTAttributeFormatShort4Normalized == 24 && WMTAttributeFormatHalf2 == 25
             && WMTAttributeFormatHalf4 == 27 && WMTAttributeFormatFloat == 28
             && WMTAttributeFormatFloat2 == 29 && WMTAttributeFormatFloat3 == 30
             && WMTAttributeFormatFloat4 == 31 && WMTAttributeFormatInt1010102Normalized == 40
             && WMTAttributeFormatUInt1010102Normalized == 41
             && WMTAttributeFormatUChar4Normalized_BGRA == 42 && WMTAttributeFormatHalf == 53,
    "psoAttributeFormatProduced() must list attributeFormat()'s outputs");
  static_assert(WMTBlendFactorOneMinusSource1Alpha == 18 && WMTBlendOperationMax == 4,
    "psoStateValid() blend ranges");
  static_assert(WMTPrimitiveTopologyClassTriangle == 3, "psoStateValid() topology range");

  uint64_t RenderPsoKey::signature() {
    // Every field and sub-field offset: a field moving inside the key, or
    // inside an attribute/layout/color record, changes the signature.
    const uint64_t layout[] = {
      sizeof(RenderPsoKey),
      offsetof(RenderPsoKey, vsCookie),    offsetof(RenderPsoKey, fsCookie),
      offsetof(RenderPsoKey, attrs),       offsetof(RenderPsoKey, layouts),
      offsetof(RenderPsoKey, attrCount),   offsetof(RenderPsoKey, layoutCount),
      offsetof(RenderPsoKey, colors),      offsetof(RenderPsoKey, colorCount),
      offsetof(RenderPsoKey, sampleCount), offsetof(RenderPsoKey, topologyClass),
      offsetof(RenderPsoKey, alphaToCoverage),
      offsetof(RenderPsoKey, depthFormat), offsetof(RenderPsoKey, stencilFormat),
      std::size(RenderPsoKey{}.attrs), std::size(RenderPsoKey{}.layouts), std::size(RenderPsoKey{}.colors),
      sizeof(PsoVertexAttr),
      offsetof(PsoVertexAttr, format),        offsetof(PsoVertexAttr, offset),
      offsetof(PsoVertexAttr, bufferIndex),   offsetof(PsoVertexAttr, pad),
      sizeof(PsoVertexLayout),
      offsetof(PsoVertexLayout, stride),      offsetof(PsoVertexLayout, stepFunction),
      offsetof(PsoVertexLayout, pad),
      sizeof(PsoColorAttachment),
      offsetof(PsoColorAttachment, pixelFormat),    offsetof(PsoColorAttachment, blendEnabled),
      offsetof(PsoColorAttachment, writeMask),      offsetof(PsoColorAttachment, rgbSrcFactor),
      offsetof(PsoColorAttachment, rgbDstFactor),   offsetof(PsoColorAttachment, rgbBlendOp),
      offsetof(PsoColorAttachment, alphaSrcFactor), offsetof(PsoColorAttachment, alphaDstFactor),
      offsetof(PsoColorAttachment, alphaBlendOp),
      kPsoKeyMappingVersion,
    };
    return statecache::fnv1a(layout, sizeof(layout));
  }

  Rc<RenderPsoCache::Entry> RenderPsoCache::Registry::adoptOrInsert(
    const RenderPsoKey& key, const Rc<Entry>& fresh) {
    std::lock_guard lock(m_mutex);
    auto r = m_map.emplace(key, fresh);

    // A prewarm build that failed is not adopted: the draw builds its own
    // (a transient compiler failure shouldn't cost the pipeline for the
    // whole session; a deterministic one fails again and is cached as today).
    Rc<Entry>& e = r.first->second;
    if (!r.second && e->prewarmed() && e->isDone() && !e->handle())
      e = fresh;

    return e;
  }

  bool RenderPsoCache::Registry::insertIfAbsent(const Rc<Entry>& e) {
    std::lock_guard lock(m_mutex);
    return m_map.emplace(e->m_key, e).second;
  }

  RenderPsoCache::RenderPsoCache(obj_handle_t compiler, MtlDevice* device, Registry* registry)
  : m_compiler(compiler), m_device(device), m_registry(registry) { }

  RenderPsoCache::Entry* RenderPsoCache::get(
    const RenderPsoKey& key, obj_handle_t vsLibrary, obj_handle_t fsLibrary,
    const char* vsEntry, const char* fsEntry,
    const MtlShaderFunction* vsFn, const MtlShaderFunction* fsFn) {
    static const bool s_noPsoCache = std::getenv("D9MT_NOPSOCACHE") != nullptr;
    if (s_noPsoCache) {
      m_last = nullptr;
      m_cache.erase(key);
    }

    if (m_last && m_lastKey == key)
      return m_last;

    auto entry = m_cache.find(key);
    if (entry == m_cache.end()) {
      prof::bump(prof::CntPsoMiss);

      Rc<Entry> e = new Entry();
      e->m_key       = key;
      e->m_compiler  = m_compiler;
      e->m_vsLibrary = vsLibrary;
      e->m_fsLibrary = fsLibrary;
      e->m_vsEntry   = vsEntry;
      e->m_fsEntry   = fsEntry;

      // State cache: a prewarmed entry for this key replaces ours; either
      // way the miss is recorded (once per key: m_cache keeps it).
      MtlStateCache* stateCache = s_noPsoCache ? nullptr : m_device->stateCache();
      MtlPsoUse use = MtlPsoUse::Miss;

      if (unlikely(stateCache != nullptr) && m_registry) {
        e = m_registry->adoptOrInsert(key, e);
        if (e->prewarmed()) {
          use = e->isDone() ? MtlPsoUse::Hit : MtlPsoUse::Late;

          // A prewarm thread may be building it at the lowest priority
          // while this draw is about to wait for it: raise that thread.
          if (use == MtlPsoUse::Late)
            stateCache->boostPrewarm(e.ptr());
        }
      }

      entry = m_cache.emplace(key, e).first;

      if (!e->isDone())
        m_device->submitCompile(e, true);

      if (unlikely(stateCache != nullptr))
        stateCache->record(key.stateBytes(), vsFn, fsFn, e.ptr(), use);
    }

    m_lastKey = key;
    m_last = entry->second.ptr();
    return m_last;
  }

  obj_handle_t RenderPsoCache::getSync(
    const RenderPsoKey& key, obj_handle_t vsLibrary, obj_handle_t fsLibrary,
    const char* vsEntry, const char* fsEntry) {
    Entry* e = get(key, vsLibrary, fsLibrary, vsEntry, fsEntry);
    compile_stats::waitOnRenderThread(e);
    return e->handle();
  }

  Rc<RenderPsoCache::Entry> RenderPsoCache::createPrewarmEntry(
    obj_handle_t compiler, const RenderPsoKey& key,
    obj_handle_t vsLibrary, obj_handle_t fsLibrary,
    const char* vsEntry, const char* fsEntry) {
    Rc<Entry> e = new Entry();
    e->m_key       = key;
    e->m_compiler  = compiler;
    e->m_vsLibrary = vsLibrary;
    e->m_fsLibrary = fsLibrary;
    e->m_vsEntry   = vsEntry;
    e->m_fsEntry   = fsEntry;
    e->m_prewarmed = true;
    return e;
  }

  void RenderPsoCache::Entry::run() {
    const uint64_t t0 = compile_stats::nowNs();
    m_pso = WmtRenderPso::adopt(build(m_compiler, m_key,
      m_vsLibrary, m_fsLibrary, m_vsEntry, m_fsEntry, m_prewarmed));
    m_buildNs = compile_stats::nowNs() - t0;

    // Prewarm builds run in the background: counted by the state cache
    // (STATECACHE lines), not as the render path's PSO builds.
    if (compile_stats::g_enabled && !m_prewarmed) {
      static std::atomic<uint64_t> s_total = { 0u };
      const uint64_t dt = m_buildNs;
      const uint64_t n = ++s_total;
      compile_stats::g_psos.add(dt);
      const double ms = double(dt) * 1e-6;
      if (ms >= 1.0)
        Logger::info(str::format("PSOBUILD ms=", ms, " n=", n));
    }
  }

  obj_handle_t RenderPsoCache::build(obj_handle_t compiler,
    const RenderPsoKey& key, obj_handle_t vsLibrary, obj_handle_t fsLibrary,
    const char* vsEntry, const char* fsEntry, bool quiet) {

    WMTMtl4RenderPipelineInfo info = {};
    info.vertex_library            = vsLibrary;
    info.fragment_library          = fsLibrary;
    // Metal reserves the name "main"; hand-authored FF ubershaders use custom entry names instead.
    info.vertex_entry_name.set(vsEntry ? vsEntry : "main");
    info.fragment_entry_name.set(fsLibrary ? (fsEntry ? fsEntry : "main") : nullptr);
    if (vsEntry && vsEntry[0] == 'f' && vsEntry[1] == 'f') {
      static std::atomic<bool> s_ffOnce = { false };
      if (!s_ffOnce.exchange(true)) {
        Logger::warn(str::format("D9MT-FF: building FF render PSO vsEntry=", vsEntry,
          " fsEntry=", (fsLibrary && fsEntry) ? fsEntry : "(none)",
          " attrs=", uint32_t(key.attrCount), " colors=", uint32_t(key.colorCount))); }
    }
    info.rasterization_enabled     = 1u;
    info.raster_sample_count       = key.sampleCount ? key.sampleCount : 1u;
    info.alpha_to_coverage_enabled = key.alphaToCoverage != 0u ? 1u : 0u;
    info.input_primitive_topology  = uint32_t(WMTPrimitiveTopologyClass(key.topologyClass));

    // Never past the key's arrays, whatever the counts say (a state cache
    // key is validated before it gets here, a draw's key is built in range).
    for (uint32_t i = 0u; i < key.colorCount && i < std::size(key.colors); i++) {
      const PsoColorAttachment& k = key.colors[i];
      WMTColorAttachmentBlendInfo& c = info.colors[i];
      c.pixel_format          = WMTPixelFormat(k.pixelFormat);
      c.blending_enabled      = k.blendEnabled != 0u;
      c.write_mask            = wmtWriteMask(k.writeMask);
      c.rgb_blend_operation   = WMTBlendOperation(k.rgbBlendOp);
      c.alpha_blend_operation = WMTBlendOperation(k.alphaBlendOp);
      c.src_rgb_blend_factor  = WMTBlendFactor(k.rgbSrcFactor);
      c.dst_rgb_blend_factor  = WMTBlendFactor(k.rgbDstFactor);
      c.src_alpha_blend_factor = WMTBlendFactor(k.alphaSrcFactor);
      c.dst_alpha_blend_factor = WMTBlendFactor(k.alphaDstFactor);
    }

    WMTRenderPipelineVertexInfo& vinfo = info.vertex;
    vinfo.num_attributes = std::min<uint32_t>(key.attrCount, uint32_t(std::size(key.attrs)));
    for (uint32_t i = 0u; i < vinfo.num_attributes; i++) {
      const PsoVertexAttr& k = key.attrs[i];
      WMTVertexAttributeInfo& a = vinfo.attributes[i];
      a.format       = WMTAttributeFormat(k.format);
      a.offset       = k.offset;
      a.buffer_index = k.bufferIndex;
    }

    uint32_t numLayouts = 0u;
    for (uint32_t i = 0u; i < key.layoutCount && i < std::size(key.layouts); i++) {
      const PsoVertexLayout& k = key.layouts[i];
      uint32_t idx = k.pad;
      if (idx >= 31u)
        continue;
      WMTVertexBufferLayoutInfo& l = vinfo.layouts[idx];
      l.stride        = k.stride;
      l.step_function = k.stepFunction == 2u
        ? WMTVertexStepFunctionPerInstance
        : WMTVertexStepFunctionPerVertex;
      l.step_rate     = 1u;
      numLayouts = std::max(numLayouts, idx + 1u);
    }
    vinfo.num_layouts = numLayouts;

    // D3D9 sm3 VS always declares point-size output; Metal rejects that against an explicit topology class.
    info.input_primitive_topology = WMTPrimitiveTopologyClassUnspecified;

    obj_handle_t err = 0u;
    obj_handle_t pso = MTL4Compiler_newRenderPipelineState(compiler, &info, &err);

    if (!pso) {
      if (err) {
        char buf[512] = {};
        obj_handle_t desc = NSObject_description(err);
        if (desc) {
          NSString_getCString(desc, buf, sizeof(buf) - 1u, WMTUTF8StringEncoding);
          NSObject_release(desc);
        }
        // A recorded pipeline that no longer builds is dropped from the
        // state cache; the draw builds its own if it needs it.
        if (quiet)
          Logger::warn(str::format("RenderPsoCache: state cache prewarm pipeline build failed (entry dropped): ", buf));
        else
          Logger::err(str::format("RenderPsoCache: pipeline build failed: ", buf));
        NSObject_release(err);
      } else if (quiet) {
        Logger::warn("RenderPsoCache: state cache prewarm pipeline build failed (entry dropped, no error object)");
      } else {
        Logger::err("RenderPsoCache: pipeline build failed (no error object)");
      }
    }
    return pso;
  }

}
