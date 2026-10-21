#pragma once

#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "wmt_handle.h"
#include "mtl_pso_key.h"

#include "../mtl/mtl_device.h"
#include "../mtl/mtl_compile.h"

namespace dxvk::backend {

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

    // Not noexcept on purpose: libstdc++ then keeps each node's hash, so a rehash or a bucket
    // walk never hashes a key again.
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
      /// Render thread: fresh replaces old for key, if old is registered.
      void replace(const RenderPsoKey& key, const Entry* old, const Rc<Entry>& fresh);
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

    /// Render thread, failure path only: pso is a prewarm build a draw
    /// adopted that then failed (done, no pipeline). A fresh build of the
    /// same key replaces it here and in the registry, so the draws aren't
    /// skipped for the rest of the session. Returns the entry to use now
    /// (the fresh one, or whatever already replaced pso), or nullptr if pso
    /// is not a failed prewarm build.
    Entry* replaceFailedPrewarm(Entry* pso);

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
