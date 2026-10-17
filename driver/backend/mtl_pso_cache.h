#pragma once

#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "wmt_handle.h"

#include "../mtl/mtl_device.h"
#include "../mtl/mtl_compile.h"

namespace dxvk::backend {

  struct PsoVertexAttr {
    uint32_t format = 0u;
    uint16_t offset = 0u;
    uint8_t  bufferIndex = 0u;
    uint8_t  pad = 0u;
  };

  struct PsoVertexLayout {
    uint16_t stride = 0u;
    uint8_t  stepFunction = 0u;
    uint8_t  pad = 0u;
  };

  struct PsoColorAttachment {
    uint32_t pixelFormat = 0u;
    uint8_t  blendEnabled = 0u;
    uint8_t  writeMask = 0xfu;
    uint8_t  rgbSrcFactor = 1u;
    uint8_t  rgbDstFactor = 0u;
    uint8_t  rgbBlendOp = 0u;
    uint8_t  alphaSrcFactor = 1u;
    uint8_t  alphaDstFactor = 0u;
    uint8_t  alphaBlendOp = 0u;
  };

  // Packed POD, hashed bytewise — must be zero-initialized before filling so
  // padding bytes don't leak into the hash.
  struct RenderPsoKey {
    uint64_t vsCookie = 0u;
    uint64_t fsCookie = 0u;

    // 20 slots: the fixed-function ubershader declares all 18 FF vertex
    // registers, and Metal requires every declared attribute to be present
    // in the vertex descriptor (unlike Vulkan, which tolerates gaps).
    PsoVertexAttr   attrs[20] = { };
    PsoVertexLayout layouts[20] = { };
    uint8_t attrCount = 0u;
    uint8_t layoutCount = 0u;

    PsoColorAttachment colors[4] = { };
    uint8_t colorCount = 0u;

    uint8_t sampleCount = 1u;
    uint8_t topologyClass = 0u;
    uint8_t alphaToCoverage = 0u;

    uint32_t depthFormat = 0u;
    uint32_t stencilFormat = 0u;

    size_t hash() const {
      const uint8_t* p = reinterpret_cast<const uint8_t*>(this);
      uint64_t h = 0xcbf29ce484222325ull;
      for (size_t i = 0u; i < sizeof(*this); i++)
        h = (h ^ p[i]) * 0x100000001b3ull;
      return size_t(h ^ (h >> 32));
    }

    bool operator == (const RenderPsoKey& other) const {
      return !std::memcmp(this, &other, sizeof(*this));
    }

    /// The state cache records the key without its two cookies.
    static constexpr size_t StateOffset = 16u;

    const uint8_t* stateBytes() const {
      return reinterpret_cast<const uint8_t*>(this) + StateOffset;
    }

    /// Layout and mapping signature for the state cache file: changes
    /// whenever a field moves or kPsoKeyMappingVersion is bumped.
    static uint64_t signature();
  };

  /// Bump in the same commit as any change to what the key's derived bytes
  /// mean: attributeFormat(), the D3DFORMAT -> Metal format table, the blend
  /// factor/op mapping, kVertexBufferBaseIndex. Stale state cache entries are
  /// dropped instead of being prewarmed for nothing.
  constexpr uint32_t kPsoKeyMappingVersion = 1u;

  /**
   * \brief Render PSO cache (CS thread only)
   *
   * Builds run as compile tasks on the device's worker pool, so new PSOs
   * compile in parallel with each other and with command recording. An
   * entry is never removed while referenced; its handle is valid (possibly
   * 0 on failure) once the task is done.
   */
  class RenderPsoCache {

  public:

    class Entry : public MtlCompileTask {
      friend class RenderPsoCache;
    public:
      obj_handle_t handle() const { return m_pso.handle(); }
      /// Built by the state cache prewarm (immutable after creation).
      bool prewarmed() const { return m_prewarmed; }
      /// Build time; valid once isDone().
      uint64_t buildNs() const { return m_buildNs; }
    protected:
      void run() override;
    private:
      RenderPsoKey  m_key = { };
      obj_handle_t  m_compiler = 0u;
      obj_handle_t  m_vsLibrary = 0u;
      obj_handle_t  m_fsLibrary = 0u;
      const char*   m_vsEntry = nullptr;   // static strings
      const char*   m_fsEntry = nullptr;
      bool          m_prewarmed = false;
      uint64_t      m_buildNs = 0u;
      WmtRenderPso  m_pso;
    };

    struct KeyHash {
      size_t operator () (const RenderPsoKey& k) const { return k.hash(); }
    };

    /**
     * \brief Device-wide PSO map shared with the state cache prewarm
     *
     * Only used while the state cache is on: the render thread's misses
     * look here first (adopting a prewarmed entry) and register their own
     * builds, and the prewarm only builds keys nobody registered yet.
     * Never touched on the render thread's hit paths.
     */
    class Registry {
    public:
      /// Render thread: the registered entry for key, or registers fresh.
      Rc<Entry> adoptOrInsert(const RenderPsoKey& key, const Rc<Entry>& fresh);
      /// Prewarm: registers e unless the key is taken. True if registered.
      bool insertIfAbsent(const Rc<Entry>& e);
    private:
      std::mutex m_mutex;
      std::unordered_map<RenderPsoKey, Rc<Entry>, KeyHash> m_map;
    };

    RenderPsoCache(obj_handle_t compiler, MtlDevice* device, Registry* registry);

    /// Looks up or starts building a PSO; never blocks. vsFn/fsFn: the
    /// functions behind the libraries, for the state cache (nullptr for
    /// internal pipelines, which are not recorded).
    Entry* get(
            const RenderPsoKey& key,
            obj_handle_t        vsLibrary,
            obj_handle_t        fsLibrary,
            const char*         vsEntry = "main",
            const char*         fsEntry = "main",
            const MtlShaderFunction* vsFn = nullptr,
            const MtlShaderFunction* fsFn = nullptr);

    /// A not yet built entry for the state cache prewarm.
    static Rc<Entry> createPrewarmEntry(
            obj_handle_t        compiler,
            const RenderPsoKey& key,
            obj_handle_t        vsLibrary,
            obj_handle_t        fsLibrary,
            const char*         vsEntry,
            const char*         fsEntry);

    /// Same, but waits for the build.
    obj_handle_t getSync(
            const RenderPsoKey& key,
            obj_handle_t        vsLibrary,
            obj_handle_t        fsLibrary,
            const char*         vsEntry = "main",
            const char*         fsEntry = "main");

    size_t size() const { return m_cache.size(); }

  private:

    obj_handle_t m_compiler;
    MtlDevice*   m_device;
    Registry*    m_registry;

    RenderPsoKey m_lastKey = { };
    Entry*       m_last = nullptr;

    std::unordered_map<RenderPsoKey, Rc<Entry>, KeyHash> m_cache;

    static obj_handle_t build(
            obj_handle_t        compiler,
            const RenderPsoKey& key,
            obj_handle_t        vsLibrary,
            obj_handle_t        fsLibrary,
            const char*         vsEntry,
            const char*         fsEntry,
            bool                quiet);

  };

}
