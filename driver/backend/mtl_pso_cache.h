#pragma once

#include <cstdint>
#include <cstring>
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
  };

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
    protected:
      void run() override;
    private:
      RenderPsoKey  m_key = { };
      obj_handle_t  m_compiler = 0u;
      obj_handle_t  m_vsLibrary = 0u;
      obj_handle_t  m_fsLibrary = 0u;
      const char*   m_vsEntry = nullptr;   // static strings
      const char*   m_fsEntry = nullptr;
      WmtRenderPso  m_pso;
    };

    RenderPsoCache(obj_handle_t compiler, MtlDevice* device);

    /// Looks up or starts building a PSO; never blocks.
    Entry* get(
            const RenderPsoKey& key,
            obj_handle_t        vsLibrary,
            obj_handle_t        fsLibrary,
            const char*         vsEntry = "main",
            const char*         fsEntry = "main");

    /// Same, but waits for the build.
    obj_handle_t getSync(
            const RenderPsoKey& key,
            obj_handle_t        vsLibrary,
            obj_handle_t        fsLibrary,
            const char*         vsEntry = "main",
            const char*         fsEntry = "main");

    size_t size() const { return m_cache.size(); }

  private:

    struct KeyHash {
      size_t operator () (const RenderPsoKey& k) const { return k.hash(); }
    };

    obj_handle_t m_compiler;
    MtlDevice*   m_device;

    RenderPsoKey m_lastKey = { };
    Entry*       m_last = nullptr;

    std::unordered_map<RenderPsoKey, Rc<Entry>, KeyHash> m_cache;

    static obj_handle_t build(
            obj_handle_t        compiler,
            const RenderPsoKey& key,
            obj_handle_t        vsLibrary,
            obj_handle_t        fsLibrary,
            const char*         vsEntry,
            const char*         fsEntry);

  };

}
