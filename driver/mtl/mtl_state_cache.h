#pragma once

#include <atomic>
#include <chrono>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "mtl_state_cache_core.h"
#include "mtl_compile.h"
#include "mtl_types.h"

#include "../util/thread.h"
#include "../util/rc/util_rc.h"
#include "../util/rc/util_rc_ptr.h"

namespace dxvk {

  class MtlShaderFunction;

  /**
   * \brief A shader the prewarm can get functions from
   *
   * Implemented by D3D9ShaderProgram and the fixed-function module set.
   */
  class MtlStateCacheShader {

  public:

    /// The function linked against pPrev (nullptr = standalone), created
    /// if needed but never submitted to the compile workers: the prewarm
    /// runs it itself, or a draw does if it gets there first.
    virtual Rc<MtlShaderFunction> prewarmFunction(MtlStateCacheShader* pPrev) = 0;

  protected:

    ~MtlStateCacheShader() = default;

  };

  /**
   * \brief What the state cache needs from the backend
   */
  class MtlStateCacheBackend {

  public:

    /// Registers a PSO build for the recorded key bytes with the two
    /// functions' libraries. nullptr if that PSO is already registered
    /// (built, building or queued by a draw).
    virtual Rc<MtlCompileTask> createPrewarmPso(
      const uint8_t*            pState,
      const MtlShaderFunction*  pVs,
      const MtlShaderFunction*  pFs) = 0;

    /// The recorded key bytes are something the draw path can produce (see
    /// backend::psoStateValid). Invalid entries are dropped at load, never
    /// built: Metal terminates the process on invalid descriptors.
    virtual bool psoStateValid(const uint8_t* pState, const char** pWhy) = 0;

    /// Done and produced a pipeline.
    virtual bool psoSucceeded(MtlCompileTask* pPso) = 0;

    /// Time the build took (0 if unknown).
    virtual uint64_t psoBuildNs(MtlCompileTask* pPso) = 0;

    /// Urgent compile tasks queued on the worker pool.
    virtual uint32_t urgentCompilesQueued() = 0;

  protected:

    ~MtlStateCacheBackend() = default;

  };

  /// How a PSO miss of the render thread's cache was served.
  enum class MtlPsoUse : uint8_t {
    Miss,   // not prewarmed: built on demand
    Hit,    // prewarmed and done
    Late,   // prewarmed, still building
  };

  struct MtlStateCacheConfig {
    std::string file;              // platform path of the cache file
    std::string displayPath;       // for log lines (the Unix path when known)
    uint64_t    keySignature = 0u;
    uint32_t    threads      = 2u; // prewarm threads; 0 = record and load only
    uint32_t    maxEntries   = statecache::DefaultMaxEntries;
    bool        noValidate   = false; // tests of the crash guard only (test builds: D9MT_STATE_CACHE_TEST_NO_VALIDATE=1)
  };

  /**
   * \brief Pipeline state cache (docs/state-cache.md)
   *
   * Records the PSOs the render thread misses on, keyed by D3D9-side shader
   * identity plus the derived key bytes, persists them per executable, and
   * in later sessions rebuilds them in the background once the game has
   * created the shaders they need. Thread roles:
   *  - announce(): the thread creating shaders (D3D9 app thread)
   *  - record():   the render (CS) thread only; no allocation, no lock, no IO
   *  - one writer thread: load, record intake, merge and atomic writes
   *  - prewarm threads (lowest priority): function and PSO builds; they
   *    yield while urgent compiles are queued or the render thread waits
   */
  class MtlStateCache : public RcObject {

  public:

    /// Resolves the file from D9MT_STATE_CACHE / TMPDIR (docs/state-cache.md
    /// §4). False if the cache is off or there is no usable location.
    static bool resolveConfig(MtlStateCacheConfig& config, std::string& reason);

    MtlStateCache(MtlStateCacheBackend* pBackend, const MtlStateCacheConfig& config);

    ~MtlStateCache();

    /// A new shader exists. pShader must stay valid until shutdown():
    /// the cache keeps only this pointer (D3D9 shader modules live as long
    /// as the device, the fixed-function set is a device member).
    void announce(
            MtlShaderStage        stage,
      const MtlShaderStateId&     id,
            MtlStateCacheShader*  pShader);

    /// Render thread, once per miss of its PSO cache. pState: the PSO
    /// key bytes after the two library cookies (statecache::KeyBytes).
    void record(
      const uint8_t*            pState,
      const MtlShaderFunction*  pVs,
      const MtlShaderFunction*  pFs,
            MtlCompileTask*     pPso,
            MtlPsoUse           use);

    /// Render thread: a draw is about to wait for pPso, which a prewarm
    /// thread may be building at the lowest priority; that thread is raised
    /// to normal priority until the build is done.
    void boostPrewarm(MtlCompileTask* pPso);

    /// Render thread: a PSO miss that can't be recorded (internal shaders).
    void noteUnrecorded() {
      m_unrecorded.fetch_add(1u, std::memory_order_relaxed);
    }

    /// Stops the threads, writes the file and drops every reference to
    /// shaders and pipelines. Idempotent; call before the device goes away.
    void shutdown();

    /// For tests: true once the file was loaded and the index built.
    bool loaded() const { return m_loaded.load(std::memory_order_acquire); }

    /// For tests: blocks until the ready queue is empty and no prewarm
    /// job is running, or the timeout passes.
    bool waitPrewarmIdle(uint32_t timeoutMs);

    struct Stats {
      uint64_t hits = 0u, late = 0u, misses = 0u, unrecorded = 0u, ringDrops = 0u;
      uint64_t recorded = 0u, built = 0u, dup = 0u, failed = 0u, skipped = 0u;
      uint64_t writes = 0u, entries = 0u;
      uint64_t invalid = 0u, boosts = 0u;
      bool     guardTripped = false;   // the last session ended during prewarm builds
      uint64_t suspects = 0u;          // entries dropped for it (in flight then)
      uint32_t trips = 0u;             // consecutive sessions that ended that way (at load)
      bool     prewarmOff = false;     // GuardSkipTrips reached: no prewarm this session
      bool     fileDropped = false;    // GuardDropTrips reached: the file was dropped
      uint64_t journaled = 0u;         // records appended to the journal this session
      uint64_t journalReplayed = 0u;   // journal records merged at load (a session that ended without writing)
    };

    /// The journal's file IO (the writer thread's; public for the tests and the Wine append
    /// test, n013-perf/bench/journal_append.cpp). openJournal never truncates a file with our
    /// header; appendJournalRecords appends to an existing file only (false if it's gone).
    static statecache::JournalOpen openJournal(const std::string& path, uint64_t keySignature);
    static bool appendJournalRecords(const std::string& path, const std::vector<uint8_t>& bytes);

    Stats stats();

  private:

    struct Record {
      statecache::Entry   entry;
      Rc<MtlCompileTask>  pso;
      uint8_t             use = 0u;
    };

    struct ShaderSlot {
      MtlStateCacheShader*  shader = nullptr;
    };

    using Clock = std::chrono::steady_clock;

    MtlStateCacheBackend*   m_backend;
    MtlStateCacheConfig     m_config;
    const Clock::time_point m_t0 = Clock::now();

    // Render thread -> writer.
    statecache::SpscRing<Record, 1024u> m_ring;

    // Index, shaders, failures: m_mutex.
    dxvk::mutex                   m_mutex;
    dxvk::condition_variable      m_prewarmCond;
    statecache::PrewarmIndex      m_index;
    std::unordered_map<statecache::ShaderRef, ShaderSlot, statecache::ShaderRefHash> m_shaders;
    std::vector<statecache::Entry> m_failed;
    uint32_t                      m_activeJobs = 0u;
    bool                          m_indexBuilt = false;

    // Writer thread only.
    statecache::Database          m_db;
    std::vector<Record>           m_pendingRecords;
    uint32_t                      m_session  = 1u;
    bool                          m_readOnly = false;
    bool                          m_dirty    = false;
    Clock::time_point             m_lastChange = Clock::now();
    Clock::time_point             m_lastWrite = Clock::now();
    Clock::time_point             m_lastLog   = Clock::now();
    uint64_t                      m_loggedSum = 0u;
    uint32_t                      m_writeFailures = 0u;
    std::unordered_set<uint64_t>  m_dropped;   // identities of failed entries

    // Journal (docs/state-cache-journal.md): every entry used this session, once, appended
    // after the intake that recorded it; folded into the main file and removed by write().
    // Off for a read-only cache, a journal a newer d9mt wrote, or after an IO error.
    bool                           m_journalOn    = true;
    bool                           m_journalReady = false;   // prepared (header, alignment) since the last removal
    std::vector<statecache::Entry> m_journalPending;
    std::unordered_set<uint64_t>   m_journaled;              // identities journaled this session

    dxvk::mutex                   m_writerMutex;
    dxvk::condition_variable      m_writerCond;
    bool                          m_stopWriter = false;   // m_writerMutex

    std::atomic<bool>             m_stop    = { false };
    std::atomic<bool>             m_stopped = { false };
    std::atomic<bool>             m_loaded  = { false };

    std::atomic<uint64_t> m_hits = { 0u }, m_late = { 0u }, m_misses = { 0u };
    std::atomic<uint64_t> m_unrecorded = { 0u }, m_ringDrops = { 0u };
    std::atomic<uint64_t> m_recorded = { 0u }, m_built = { 0u }, m_dup = { 0u };
    std::atomic<uint64_t> m_failedCount = { 0u }, m_skipped = { 0u };
    std::atomic<uint64_t> m_pausedNs = { 0u }, m_buildNs = { 0u }, m_hitBuildNs = { 0u };
    std::atomic<uint64_t> m_writes = { 0u }, m_entries = { 0u };
    std::atomic<uint64_t> m_prewarmLogged = { 0u };
    std::atomic<uint64_t> m_invalid = { 0u }, m_boosts = { 0u };
    std::atomic<bool>     m_guardTripped = { false };
    std::atomic<uint64_t> m_suspects = { 0u };
    std::atomic<bool>     m_prewarmOff = { false };
    std::atomic<bool>     m_fileDropped = { false };
    std::atomic<uint32_t> m_loadTrips = { 0u };
    std::atomic<uint64_t> m_journalRecords = { 0u }, m_journalReplayed = { 0u };

    // Crash-loop guard: <file>.prewarm lists the entries whose prewarm jobs
    // are in flight (rewritten atomically at every job start, removed when
    // the queue drains and at a clean shutdown). Found at load: the last
    // session ended (crash, kill, or exit without releasing the device)
    // during the prewarm; only the listed entries are dropped, and the trip
    // count in <file>.trips goes up. Only a prewarm queue that drains resets
    // that count (sessions without prewarm leave it alone), so a crash the
    // list doesn't catch can't repeat forever: at GuardSkipTrips one session
    // runs without prewarm, at GuardDropTrips the file is dropped.
    static constexpr uint32_t     GuardSkipTrips = 3u;
    static constexpr uint32_t     GuardDropTrips = 6u;
    dxvk::mutex                   m_markerMutex;
    bool                          m_markerOnDisk = false;
    uint32_t                      m_trips = 0u;   // m_markerMutex; set by load() before any prewarm job

    // Per prewarm thread: the job in flight (for the marker) and the PSO it
    // is building (for boostPrewarm()).
    struct PrewarmSlot {
      std::atomic<uint64_t>        inflight = { 0u };      // identity hash, 0 = none
      std::atomic<MtlCompileTask*> running = { nullptr };
      std::atomic<bool>            boosted = { false };
      void*                        handle  = nullptr;   // Win32 thread handle
    };
    static constexpr uint32_t MaxPrewarmThreads = 4u;
    PrewarmSlot                   m_slots[MaxPrewarmThreads];
    uint32_t                      m_lastIntake = 0u;    // writer: records drained last time

    dxvk::thread              m_writer;
    std::vector<dxvk::thread> m_prewarmThreads;

    void writerMain();
    void load();
    void intake(bool final);
    bool ingest(Record& r);
    bool write();
    void appendJournal();
    std::string journalPath() const { return m_config.file + ".journal"; }
    void logUse(bool final);

    void prewarmMain(uint32_t index);
    bool prewarmOne(uint32_t index, const statecache::Entry& e, const ShaderSlot& vs, const ShaderSlot& ps);
    bool finishFunction(MtlShaderFunction* pFn);
    bool yieldToRenderThread();
    void writeMarker();
    void clearMarkerIfDrained();
    std::string markerPath() const { return m_config.file + ".prewarm"; }
    std::string tripsPath() const { return m_config.file + ".trips"; }

    double seconds() const {
      return std::chrono::duration<double>(Clock::now() - m_t0).count();
    }

  };

}
