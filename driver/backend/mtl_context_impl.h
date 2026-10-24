#pragma once

#include <array>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../mtl/mtl_context.h"
#include "../mtl/mtl_presenter.h"

#include "mtl_attachment_opt.h"
#include "mtl_canary_gate.h"
#include "mtl_cmd_chunk.h"
#include "mtl_device_impl.h"
#include "mtl_pso_cache.h"
#include "mtl_pso_gen.h"

namespace dxvk::backend {

  inline bool deferredPassesEnabled() {
    static const bool on = std::getenv("D9MT_DEFERRED") != nullptr;
    return on;
  }

  class MtlBackendQuery : public MtlQuery {
  public:
    void*        visCpu      = nullptr;
    // Visibility-buffer offsets of every segment this query was active in;
    // the result is their sum (see MtlContextImpl::newVisSegment).
    std::vector<uint64_t> slots;
    uint64_t     serial      = 0u;
    SubmissionTracker* tracker = nullptr;

    MtlGpuQueryStatus getData(MtlQueryData& out) override;
    void waitData() override;
    bool unsubmitted() const override { return tracker && serial && serial >= tracker->nextSerial(); }
  };

  class MtlBackendEvent : public MtlEvent {
  public:
    uint64_t     serial  = 0u;
    SubmissionTracker* tracker = nullptr;

    MtlGpuEventStatus test() override;
    bool unsubmitted() const override { return tracker && serial && serial >= tracker->nextSerial(); }
  };

  enum ContextDirtyBit : uint32_t {
    DirtyPso           = 1u << 0,
    DirtyVertexBuffers = 1u << 1,
    DirtyIndexBuffer   = 1u << 2,
    DirtyResources     = 1u << 3,
    DirtyPush          = 1u << 4,
    DirtyViewport      = 1u << 5,
    DirtyDepthStencil  = 1u << 6,
    DirtyBlendColor    = 1u << 7,
    DirtyStencilRef    = 1u << 8,
    DirtyDepthBias     = 1u << 9,
    DirtyRasterizer    = 1u << 10,
    DirtyEverything    = ~0u,
  };

  class MtlContextImpl : public MtlContext {

  public:

    explicit MtlContextImpl(Rc<MtlDeviceImpl> device);
    ~MtlContextImpl();

    void beginRecording() override;
    void endFrame() override;
    void flushCommandList(const char* reason, MtlSubmitStatus* status, bool deferrable = false) override;
    void synchronizeWsi(MtlPresenterSync sync) override;

    void bindShader(MtlShaderStage stage, Rc<MtlShaderFunction>&& fn) override;
    void setVertexLayout(uint32_t nAttr, const MtlVertexAttr* attrs,
                         uint32_t nLayout, const MtlVertexLayout* layouts) override;
    void setPrimitiveTopology(MtlPrimitiveTopology topology) override;
    void setViewports(uint32_t n, const MtlViewport* viewports) override;
    void setRasterizerState(const MtlRasterizerState& rs) override;
    void setMultisampleState(const MtlMultisampleState& ms) override;
    void setDepthStencilState(const MtlDepthStencilState& ds) override;
    void setBlendMode(uint32_t rt, const MtlBlendMode& blend) override;
    void setBlendColor(MtlColor4 color) override;
    void setStencilReference(uint32_t reference) override;
    void setDepthBias(MtlDepthBias bias) override;
    void setDepthBounds(MtlDepthBounds bounds) override;

    void bindVertexBuffer(uint32_t binding, MtlBufferSlice&& slice, uint32_t stride) override;
    void bindVertexBufferRange(uint32_t binding, MtlDeviceSize offset, MtlDeviceSize length, uint32_t stride) override;
    void bindIndexBuffer(MtlBufferSlice&& slice, MtlIndexType type) override;
    void bindUniformBuffer(MtlStageMask stages, uint32_t slot, MtlBufferSlice&& slice) override;
    void bindUniformBufferRange(MtlStageMask stages, uint32_t slot,
                                MtlDeviceSize offset, MtlDeviceSize length) override;
    void bindTexture(MtlStageMask stages, uint32_t slot, Rc<MtlTextureView>&& view) override;
    void bindSampler(MtlStageMask stages, uint32_t slot, Rc<MtlSamplerState>&& sampler) override;
    void pushData(MtlStageMask stages, uint32_t offset, uint32_t size, const void* data) override;

    void bindRenderTargets(MtlRenderTargets&& targets, MtlFeedbackLoopMask feedbackLoop) override;
    void emitGraphicsBarrier(MtlStageMask srcStages, MtlBarrierScope srcScope,
                             MtlStageMask dstStages, MtlBarrierScope dstScope) override;
    void clearRenderTarget(const Rc<MtlTextureView>& view, MtlAspectMask clear,
                           MtlClearValue value, MtlAspectMask discard) override;
    void clearImageView(const Rc<MtlTextureView>& view, MtlOrigin offset, MtlSize extent,
                        MtlAspectMask aspects, MtlClearValue value) override;

    void draw(uint32_t count, const MtlDrawArgs* args) override;
    void drawIndexed(uint32_t count, const MtlDrawIndexedArgs* args) override;

    void copyTexture(const Rc<MtlTexture>& dst, MtlSubresourceLayers dstSub, MtlOrigin dstOff,
                     const Rc<MtlTexture>& src, MtlSubresourceLayers srcSub, MtlOrigin srcOff, MtlSize extent) override;
    void copyBuffer(const Rc<MtlBuffer>& dst, MtlDeviceSize dstOff,
                    const Rc<MtlBuffer>& src, MtlDeviceSize srcOff, MtlDeviceSize bytes) override;
    void copyTextureToBuffer(const Rc<MtlBuffer>& dst, MtlDeviceSize off, MtlDeviceSize rowAlign,
                             MtlDeviceSize sliceAlign, MtlFormat format, const Rc<MtlTexture>& src,
                             MtlSubresourceLayers srcSub, MtlOrigin srcOff, MtlSize extent) override;
    void copyBufferToTexture(const Rc<MtlTexture>& dst, MtlSubresourceLayers dstSub, MtlOrigin dstOff, MtlSize extent,
                             const Rc<MtlBuffer>& src, MtlDeviceSize off, MtlDeviceSize rowAlign,
                             MtlDeviceSize sliceAlign, MtlFormat format) override;
    void blitTextureView(const Rc<MtlTextureView>& dst, const MtlOffset3D dstBox[2],
                         const Rc<MtlTextureView>& src, const MtlOffset3D srcBox[2],
                         MtlSamplerFilter filter) override;
    void resolveTexture(const Rc<MtlTexture>& dst, const Rc<MtlTexture>& src, MtlResolveRegion region,
                        MtlFormat format, MtlResolveMode color, MtlResolveMode stencil) override;
    void generateMipmaps(const Rc<MtlTextureView>& view, MtlSamplerFilter filter) override;

    void initBuffer(const Rc<MtlBuffer>& buffer) override;
    void initImage(const Rc<MtlTexture>& image) override;
    void discardImage(const Rc<MtlTexture>& image) override;
    void invalidateBuffer(const Rc<MtlBuffer>& buffer, Rc<MtlAllocation>&& slice) override;
    void uploadBuffer(const Rc<MtlBuffer>& dst, MtlDeviceSize dstOff,
                      const Rc<MtlBuffer>& staging, MtlDeviceSize srcOff, MtlDeviceSize size) override;
    bool ensureTextureUsage(const Rc<MtlTexture>& image, MtlTextureUsage usage) override;

    void beginQuery(const Rc<MtlQuery>& query) override;
    void endQuery(const Rc<MtlQuery>& query) override;
    void writeTimestamp(const Rc<MtlQuery>& query) override;

    void signal(const Rc<MtlSignal>& signal, uint64_t value) override;
    void signalGpuEvent(const Rc<MtlEvent>& event) override;

    void track(Rc<MtlResource>&& resource) override;

    void beginDebugLabel(const char* label) override;
    void endDebugLabel() override;
    void insertDebugLabel(const char* label) override;
    void setApiTag(uint32_t api) override;

    void setPresenter(Rc<MtlPresenter> presenter) { m_presenter = std::move(presenter); }

  private:

    static constexpr uint32_t kVertexBufferBaseIndex = 3u;
    static constexpr uint32_t kPushConstantIndex = 20u;
    static constexpr uint32_t kAbIndex = 21u;
    /* The fixed-function fragment shader's texture slots: t2d 0-7, tcube 8-15
     * (scripts/build-driver.sh pins them). */
    static constexpr uint32_t kNullCubeFirstSlot = 8u;
    static constexpr uint32_t kPushSharedBlockSize = 32u;
    static constexpr size_t kCmdChunkFlushBytes = 1u << 22;

    struct StageBindings {
      std::array<Rc<MtlTextureView>,  16> textures;
      std::array<Rc<MtlSamplerState>, 16> samplers;
      struct CbvBinding {
        MtlBufferSlice slice;
        MtlDeviceSize  rangeOffset = 0;
        MtlDeviceSize  rangeLength = 0;
      };
      std::array<CbvBinding, 8> cbvs;
      std::array<uint8_t, 4096> pushData;
      uint32_t pushDirtyLo = 0, pushDirtyHi = 0;
      uint32_t pushExtent = 0;
      uint64_t pushGpuAddress = 0u;
      uint32_t dirtyTextures = ~0u;
      uint32_t dirtySamplers = ~0u;
      uint32_t dirtyCbvs     = ~0u;
      uint64_t abGpuAddress = 0u;
      std::vector<uint64_t> abShadowWords;
      uint64_t abFnId = 0u;
      uint8_t abActive = 0u;
    };

    struct ArgTableShadow {
      std::array<uint64_t, 31> bufferGpuAddress = { };
      std::array<uint64_t, 16> textureId = { };
      std::array<uint64_t, 16> samplerId = { };
    };

    struct ArgTablePair {
      std::array<WmtArgTable, 2>    table;
      std::array<ArgTableShadow, 2> shadow;
      uint64_t serial = 0u;
    };

    struct VertexStream {
      MtlBufferSlice slice;
      uint32_t stride = 0;
    };

    struct BlitArgTable {
      WmtArgTable table;
      uint64_t serial = 0u;
    };

    // Attachment load/store optimization: one aspect of the pass's targets
    // (color 0-3, depth, stencil) and what was decided for it.
    struct AttSlot {
      attopt::SlotInput    in;
      attopt::SlotDecision d;
      bool                 used = false;
    };
    static constexpr uint32_t kAttDepth = 4u, kAttStencil = 5u;

    struct PendingPass {
      MtlRenderTargets targets;
      bool active = false;
      bool pending = false;
      std::array<bool, 4> clearColor = { };
      std::array<MtlClearValue, 4> clearValues = { };
      bool clearDepth = false, clearStencil = false;
      float depthClear = 1.f; uint32_t stencilClear = 0;
      obj_handle_t encoder = 0;
      uint64_t encodedDraws = 0;
      // m_attOpt only: the encoder is created when the pass ends (or its
      // commands must be encoded before), once its load/store actions are known.
      std::array<AttSlot, 6> att = { };
      std::array<Rc<MtlTexture>, 4> resolveTo = { };  // MSAA color resolves folded into the store
      uint64_t epoch = 0u;         // m_attEpoch when the pass started
      uint8_t  writtenMask = 0u;   // aspects (AttSlot index bits) a draw may have written
      bool     discardDs = false;  // the depth-stencil is discarded right after the pass
      uint8_t  discardMask = 0u;   // aspects (AttSlot index bits) a full clear overwrites right after the pass
      // D9MT_PROFILE: m_currentSerial / m_splitXfers / m_splitClears when the pass started (splitCheck)
      uint64_t openSerial = 0u, openXfers = 0u, openClears = 0u;
    };

    struct FrameEntry {
      FrameEntry() = default;
      // Moves must stay noexcept: std::function's move ctor isn't noexcept
      // under C++17, so an implicit move here would push vector growth onto
      // the copy path, deep-copying a parked CmdChunk's freed-block chain.
      FrameEntry(FrameEntry&&) noexcept = default;
      FrameEntry& operator = (FrameEntry&&) noexcept = default;

      enum class Kind : uint8_t { Render, Direct };
      Kind kind = Kind::Render;

      MtlRenderTargets targets;
      std::array<bool, 4> clearColor = { };
      std::array<MtlClearValue, 4> clearValues = { };
      bool clearDepth = false, clearStencil = false;
      float depthClear = 1.f;
      uint32_t stencilClear = 0u;
      uint64_t draws = 0u;
      std::vector<CmdChunk> chunks;
      std::array<uint8_t, 4> colorStore = { };
      uint8_t depthStore = 0u, stencilStore = 0u;

      std::function<void(obj_handle_t cmdbuf)> encode;
    };

    Rc<MtlDeviceImpl>  m_device;

    /* Bound wherever nothing is (Metal API validation asserts on a slot a function
     * declares and the table never set, e.g. the FF ubershader's null stream, its
     * vertex-blend buffer and its cube textures): a zeroed buffer, a 1x1 2D and a
     * 1x1 cube texture, a default sampler. Written into every argument table once at
     * creation and wherever a slot is unbound, so the table never holds 0. */
    Rc<MtlBuffer>       m_nullBuffer;
    Rc<MtlTexture>      m_nullImage2D, m_nullImageCube;
    Rc<MtlTextureView>  m_nullView2D, m_nullViewCube;
    Rc<MtlSamplerState> m_nullSampler;
    uint64_t            m_nullAddress = 0u, m_null2DId = 0u, m_nullCubeId = 0u, m_nullSamplerId = 0u;
    Rc<MtlPresenter>   m_presenter;

    WmtCommandBuffer   m_cmdbuf;
    PendingPass        m_pass;
    uint32_t           m_passFeedbackLoop = 0u;

    CmdChunk           m_cmdChunk;

    std::vector<FrameEntry> m_frameEntries;
    std::vector<CmdChunk>   m_chunkPool;
    std::unordered_set<const void*> m_frameSampled;
    std::unordered_set<const void*> m_everTransferRead;
    bool m_frameBoundaryFlush = false;

    // Pool grows on demand: a fixed ring of 3 forced CPU/GPU lockstep at
    // ~12 command buffers per frame (recording buffer N waited for N-3).
    static constexpr uint32_t kMaxAllocators = 32u;
    std::vector<WmtAllocator> m_allocators;
    std::vector<WmtAllocator> m_initAllocators;
    std::vector<uint64_t>     m_allocatorSerial;
    uint32_t     m_allocatorCursor = 0u;
    uint32_t     m_allocatorSlot = 0u;
    obj_handle_t m_allocator    = 0u;

    // Out-of-order upload command buffer, committed ahead of m_cmdbuf so
    // staging->VB/IB copies don't split the active render pass.
    WmtCommandBuffer m_initCmdbuf;
    obj_handle_t     m_initEncoder = 0u;
    bool tryHoistBufferCopy(MtlBufferImpl* d, MtlBufferImpl* s,
                            const Rc<MtlBuffer>& dst, const Rc<MtlBuffer>& src,
                            MtlDeviceSize dstOff, MtlDeviceSize srcOff, MtlDeviceSize bytes);

    std::vector<ArgTablePair> m_argTables;
    uint32_t                 m_argTableCursor = 0u;

    uint64_t     m_lastStampSerial = ~0ull;
    Rc<RenderPsoCache::Entry> m_lastEncodedPso;
    // D9MT_ASYNC_SKIP=1: drop draws whose shaders/PSO are still compiling
    // instead of waiting (no stalls, but objects pop in a few frames late).
    // Needs compile workers, or nothing would ever finish.
    bool         m_asyncSkip = false;
    // d9mt.pipelineWaitBudgetMs / D9MT_PSO_WAIT_MS > 0: wait for such
    // shaders/PSOs at most what is left of the frame's budget, then drop the
    // draws like m_asyncSkip. Off by default (wait as long as it takes).
    MtlCompileWaitBudget m_waitBudget;
    Rc<MtlCompileTask> m_lastBoosted[2];
    void boostPendingShaders();
    bool         m_dynValid        = false;
    MtlViewport  m_lastViewport    = {};
    MtlRect      m_lastScissor     = {};
    obj_handle_t m_lastDsso        = 0u;
    uint32_t     m_lastStencilRef  = 0u;
    MtlColor4    m_lastBlendColor  = {};
    uint64_t     m_lastCullMode    = 0u;
    uint64_t     m_lastWinding     = 0u;
    MtlDepthBias m_lastDepthBias   = {};

    uint64_t s_diagVbStale  = 0u;
    uint64_t s_diagCbvStale = 0u;
    uint32_t                 m_currentArgTable = 0u;
    bool                     m_argTablesBound = false;

    uint32_t           m_dirty = DirtyEverything;

    Rc<MtlShaderFunction> m_vs, m_fs;
    std::vector<MtlVertexAttr>   m_vertexAttrs;
    std::vector<MtlVertexLayout> m_vertexLayouts;
    MtlPrimitiveTopology m_topology = { };
    std::array<MtlBlendMode, 4> m_blend = { };
    MtlMultisampleState m_msaa = { };
    MtlRasterizerState  m_rasterizer = { };
    MtlDepthStencilState m_depthStencil = { };
    MtlViewport m_viewport = { };
    MtlColor4   m_blendColor = { };
    uint32_t    m_stencilRef = 0;
    MtlDepthBias m_depthBias = { };

    std::array<StageBindings, 2> m_bindings;
    std::array<VertexStream, 16> m_vertexStreams;
    uint32_t                     m_dirtyVertexStreams = ~0u;
    MtlBufferSlice m_indexBuffer;
    MtlIndexType   m_indexType = { };

    RenderPsoCache m_psoCache;

    std::vector<Rc<MtlResource>> m_keepAlive;

    // Releases recorded for the open command buffer; handed to the tracker at submit.
    std::unique_ptr<RetireBatch> m_retire;
    RetireBatch& retireBatch() {
      if (unlikely(m_retire == nullptr))
        m_retire = std::make_unique<RetireBatch>();
      return *m_retire;
    }

    uint64_t m_currentSerial = 0;

    // GPU canary (D9MT_GPU_CANARY, on by default): the last encoder of every command buffer
    // fills its serial's 8-byte slot in a shared buffer. A command buffer whose serial
    // completed with its slot still empty was discarded by the GPU. After a GPU page fault
    // that is always true of the command buffers queued after it ("innocent victims", several
    // per fault), and of the faulting one itself if the fault came before the fill ran (no
    // barrier orders the fill after the command buffer's other work: one would keep the next
    // command buffer from overlapping with this one).
    // Nothing else reports that to d9mt (Metal 4 command buffers have no status, the shipped
    // bridge no commit feedback); macOS only logs IOGPUMetalError in its unified log. Logged
    // as D9MT-GPU-LOST in wine.log.
    static constexpr uint32_t kCanarySlots = 512u;
    static constexpr uint64_t kCanaryFilled = 0xa5a5a5a5a5a5a5a5ull;
    // command buffers without a present copy: a canary encoder of their own on every 4th, and
    // only if no canary was armed for CanaryGate::kOwnGapNs (mtl_canary_gate.h)
    static constexpr uint32_t kCanaryEvery = 4u;
    struct CanarySlot {
      uint64_t    serial = 0u;  // armed for this serial, 0 = not armed
      uint32_t    frame  = 0u;
      const char* reason = nullptr;
      uint64_t    offset = 0u;  // in the canary buffer (the test fault's is elsewhere)
    };
    WmtBuffer   m_canaryBuffer;
    uint64_t*   m_canaryCpu     = nullptr;
    std::array<CanarySlot, kCanarySlots> m_canarySlots = { };
    uint64_t    m_canaryChecked = 0u;  // serials up to here are checked
    uint64_t    m_canaryLost    = 0u;
    uint64_t    m_canaryLostUnlogged = 0u;  // after the first 20: counted for the 10 s summary
    std::chrono::steady_clock::time_point m_canaryLostAt = { };
    // Unused: nothing armed yet; Armed: armed, no mark seen yet; On: a mark arrived (logged);
    // Failed: no buffer, or no mark in 64 command buffers (logged, no canary any more)
    enum class CanaryState : uint8_t { Unused, Armed, On, Failed };
    CanaryState m_canaryState   = CanaryState::Unused;
    uint64_t    m_canaryArmed   = 0u;  // D9MT_GPU_CANARY_TEST_FAULT's count
    uint32_t    m_canaryOwn     = 0u;  // command buffers that could have had an encoder of their own
    CanaryGate  m_canaryGate;          // when the last canary of any kind was armed
    const char* m_submitReason  = nullptr;

    static constexpr uint32_t kVisSlots = 65536u;
    WmtBuffer m_visBuffer;
    void*     m_visCpu   = nullptr;
    uint64_t  m_visSlot  = 0u;
    // Metal counts into one offset at a time and the mode is per-encoder.
    // Every change of the active query set starts a new segment slot that
    // all active queries own, so overlapping queries each see every sample
    // drawn while they were active. The segment is re-armed on each new
    // render encoder.
    std::vector<Rc<MtlQuery>> m_activeOcclusion;
    uint64_t  m_visSegment = 0u;
    bool      m_lastDepthClamp = false;

    Rc<MtlShaderFunction>      m_blitVs, m_blitFs;
    Rc<MtlShaderFunction>      m_fillFs;
    Rc<MtlShaderFunction>      m_depthClearVs;
    std::array<Rc<MtlSamplerState>, 2> m_blitSamplers;
    bool                       m_blitInitFailed = false;
    std::vector<BlitArgTable>  m_blitArgTables;
    uint32_t                   m_blitArgCursor = 0u;

    // Metal has no fan primitive: one shared index buffer holds the fan
    // pattern, and a fan of N vertices consumes its first 3*(N-2) indices.
    static constexpr uint32_t  kFanMaxVerts = 16386u;
    Rc<MtlBuffer>              m_fanIndices;
    bool ensureFanIndices();

    bool presentUpscale(const Rc<MtlTexture>& dst, const Rc<MtlTexture>& src) override;
    struct FxScaler { obj_handle_t scaler; uint64_t lastUse; };
    std::map<std::array<uint32_t, 6>, FxScaler> m_fxScalers;
    bool m_fxChecked = false, m_fxUnsupported = false;
    void clearDepthStencilRect(const Rc<MtlTextureView>& view, MtlOrigin offset,
                               MtlSize extent, MtlAspectMask aspects, MtlClearValue value);
    void resolveDepthStencil(const Rc<MtlTexture>& dst, const Rc<MtlTexture>& src,
                             const MtlResolveRegion& region, const MtlFormatInfo* fi,
                             MtlResolveMode depthMode);

    void ensureCommandBuffer();
    obj_handle_t newRenderEncoder(obj_handle_t cmdbuf, WMTRenderPassInfo* info);
    void materializePass();
    void flushPendingClears();
    void closePass();
    void flushCmdChunk();

    // Attachment load/store optimization (mtl_attachment_opt.h). Off with
    // D9MT_NO_ATTACHMENT_OPT=1 / d9mt.attachmentOpt = False and under
    // D9MT_DEFERRED (deferred passes decide their own load/store actions).
    bool         m_attOpt = false;
    bool         m_clearDiscard = false;  // a clear that ends a pass discards what it clears (D9MT_CLEAR_DISCARD=0: off)
    uint64_t     m_attEpoch = 0u;
    uint8_t      m_attWriteMask = 0u;   // aspects the current blend/depth-stencil state writes
    uint32_t     m_attTraceBudget = 0u;
    bool         m_attTraceOn = false;
    uint32_t     m_attFrame = 1u;       // 1-based frame counter for the read hint (attopt::readRecently)
    uint32_t     m_apiTag = 0u;         // D9MT_PROFILE: rstore::Api of the commands being recorded
    bool         createPassEncoder(bool early);
    void         attApplyAfter();
    uint8_t      attComputeWriteMask() const;
    bool         attSampled(const MtlTexture* img) const;
    // `why`: the operation that needs memory to hold the contents, for the
    // RESTORE attribution. `aspects`: the aspects it reads (sampling a
    // depth-stencil image reads depth only).
    void         attBindSampled(const MtlTextureView* view);
    void         attRestore(MtlTextureImpl* img, rstore::Reason why, MtlAspectMask aspects);
    void         attRead(const MtlTexture* img, rstore::Reason why,
                         MtlAspectMask aspects = MtlAspectColor | MtlAspectDepth | MtlAspectStencil);
    void         attWrite(const MtlTexture* img, rstore::Reason why);
    bool         chunkHasNoWork() const;

    // D9MT_PROFILE: render-thread totals, published at endFrame (mtl_render_stats.h).
    uint64_t     m_rs[rstats::Count] = { };

    // D9MT_PROFILE: pass splits (rstats::Splits). The attachments of the
    // last pass that ended with an encoder (not at the frame-end submit),
    // and what had happened by then; a pass on exactly those attachments
    // that loads some of them is a split, attributed to what happened
    // between that end and its start.
    struct SplitWatch {
      bool        valid  = false;
      const void* img[5] = { };   // colour 0-3, depth-stencil
      uint32_t    sub[5] = { };   // baseMip << 16 | baseLayer
      uint64_t    serial = 0u;    // m_currentSerial
      uint64_t    xfers  = 0u;    // m_splitXfers
      uint64_t    clears = 0u;    // m_splitClears
    };
    SplitWatch   m_split;
    uint64_t     m_splitXfers  = 0u;  // copies, blits, resolves, restores, partial clears so far
    uint64_t     m_splitClears = 0u;  // full clears so far
    void         splitNoteClosed();
    void         splitCheck(uint64_t loadBytes);

    void recordPassEntry();
    CmdChunk takeChunk();
    void prepareDirectEncode(uint32_t passCloseCounter);
    void encodeDirect(std::function<void(obj_handle_t)>&& fn);
    void noteTransferRead(const MtlTexture* tex);
    void flushFramePasses();

    void appendTableAddress(ArgTableShadow& shadow, obj_handle_t table, uint64_t gpuAddress, uint32_t index);
    void appendTableTexture(ArgTableShadow& shadow, obj_handle_t table, uint64_t gpuResourceId, uint32_t index);
    void appendTableSampler(ArgTableShadow& shadow, obj_handle_t table, uint64_t gpuResourceId, uint32_t index);
    bool prepareDraw();
    bool resolvePso();
    bool emitPso(RenderPsoCache::Entry* pso);
    // Waits for PSOs still compiling and patches their SetPSO commands; must
    // run before m_cmdChunk is encoded or handed off.
    void resolvePsoPatches();
    // Turns a SetPSO and the draws after it (up to the next SetPSO) into
    // NOPs; returns the number of draws dropped.
    static uint32_t dropPsoBind(wmtcmd_mtl4_setpso* cmd);

    struct PsoPatch {
      wmtcmd_mtl4_setpso*       cmd;
      Rc<RenderPsoCache::Entry> pso;
    };
    std::vector<PsoPatch> m_psoPatches;

    struct PsoSnapshot {
      const void* vs = nullptr;
      const void* fs = nullptr;
      uint32_t colorFmt[4] = { };
      uint32_t layoutGen = 0u, strideGen = 0u, blendGen = 0u;
      uint32_t msaaPacked = 0u, topoClass = 0u, pad = 0u;
    };
    PsoSnapshot  m_psoMruKey[4] = { };
    Rc<RenderPsoCache::Entry> m_psoMruPso[4];
    uint32_t     m_psoMruCursor = 0u;
    uint32_t     m_layoutGen = 0u, m_strideGen = 0u, m_blendGen = 0u;
    // State changed since the last resolvePso (PsoGen* bits); its generation is interned there.
    enum : uint32_t { PsoGenBlend = 1u, PsoGenLayout = 2u, PsoGenStride = 4u };
    uint32_t     m_psoGenDirty = 0u;
    struct VertexLayoutState {
      std::vector<MtlVertexAttr>   attrs;
      std::vector<MtlVertexLayout> layouts;
    };
    PsoGenRing<std::array<MtlBlendMode, 4>, 8> m_blendGens;
    PsoGenRing<VertexLayoutState, 8>           m_layoutGens;
    PsoGenRing<std::array<uint32_t, 16>, 8>    m_strideGens;
    // Out of line and cold: resolvePso's MRU-hit path stays as it was.
    __attribute__((noinline, cold)) void internPsoGens();
    ArgTablePair createArgumentTablePair() const;
    void initNullBindings(ArgTablePair& pair) const;
    uint64_t nullTextureId(uint32_t stage, uint32_t slot) const {
      return stage == 1u && slot >= kNullCubeFirstSlot ? m_nullCubeId : m_null2DId;
    }
    void selectArgumentTablesForPass();
    void bindArgumentTables();
    void flushVertexBuffers();
    void flushResources();
    void flushDynamicState();
    void submit(bool waitCompleted);
    // A2, commit != complete: commitRecorded() ends and commits the recorded command buffers
    // with no serial; submit() adds the serial and its bookkeeping; commitPrefix() begins the
    // next command buffer under the same serial (G1, carryOpenPass).
    void commitRecorded();
    void commitPrefix();
    bool carryOpenPass();
    void completeCarry();
    static constexpr uint32_t kMaxCarriesPerSerial = 2u;
    bool     m_earlyCommit         = false;       // D9MT_EARLY_COMMIT (default on with attOpt)
    bool     m_earlyCommitAlways   = false;       // =always: no backlog gate (tests)
    uint64_t m_earlyCommitMinBytes = 8ull << 20;  // D9MT_EARLY_COMMIT_MIN_MIB
    uint32_t m_carries             = 0u;          // carries of the open serial
    bool     m_completePending     = false;       // a carried flush completes when its pass closes
    bool     m_frameEnding         = false;       // endFrame ran: the next submit is the frame's last
    uint64_t m_frameLastSerial     = 0u;          // that submit's serial (the backlog gate)
    bool     m_prefixWork          = false;       // the command buffer holds encoded work
    bool     m_serialPrefixCommitted = false;     // commitPrefix() ran for the open serial
    uint64_t canarySlot(const char* reason);
    void armCanary();
    void checkCanaries(uint64_t completed);

    void         ensureVisBuffer();
    void         armVisibility();
    void         newVisSegment();
    bool         removeActiveOcclusion(const MtlQuery* query);

    bool         ensureBlitPipeline();
    obj_handle_t selectBlitArgTable();

    // Metal 4 has no driver hazard tracking: a producer barrier on the
    // outgoing encoder orders its writes ahead of all later queue stages so
    // a subsequent encoder observes them. Metal waits for `afterStages` of
    // this encoder and of every earlier one on the queue (also in earlier
    // command buffers), and holds `beforeStages` of every later encoder.
    void         emitProducerBarrier(obj_handle_t encoder, uint64_t afterStages,
                                     uint64_t beforeStages = uint64_t(WMTStageAll));

    // Narrowed end-of-pass barrier (docs/gpu-4k.md, candidate 1): with the
    // attachment optimization, a draw pass ends with after Fragment before
    // Fragment|Blit|Dispatch, so the next pass's vertex work can run while
    // this pass's fragment work does. The one GPU-written input of a vertex
    // stage is a D3D9 vertex texture (vertex samplers); vertex and index
    // buffers come from the CPU or from copies, whose barriers hold every
    // stage. So an image such a pass rendered gets the current m_vtxEpoch
    // (MtlTextureImpl::vtxEpoch), every barrier after Fragment before Vertex
    // at the end of an encoder starts a new epoch (not the in-pass feedback
    // barrier: it orders later encoders only, not the pass's own later
    // draws), and a draw whose vertex stage has an image of the current
    // epoch bound gets a fence first (resolveVertexHazard).
    // D9MT_PASS_BARRIER=all: every pass ends with the barrier before All.
    bool         m_narrowBarrier = false;
    bool         m_vtxCheck      = false;  // look at the vertex-stage textures at the next draw
    uint64_t     m_vtxEpoch      = 1u;
    void         markVertexHazards();
    void         vertexFence(uint64_t beforeStages);
    void         resolveVertexHazard();

  };

}
