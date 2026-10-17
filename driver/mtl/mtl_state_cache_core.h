#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <utility>
#include <vector>

/**
 * Pipeline state cache: the parts without threads, IO or Metal
 * (docs/state-cache.md). File format, parse/serialize, merge, cap, the
 * shader -> entry index with its ready queue, and the render thread's
 * record ring. Plain C++17 so test/native/state_cache_test.cpp can run it
 * natively and under ThreadSanitizer.
 */
namespace dxvk::statecache {

  constexpr uint32_t FormatVersion = 1u;
  constexpr uint32_t HeaderSize    = 64u;
  constexpr uint32_t EntrySize     = 384u;  // 8-byte check + Entry
  constexpr uint32_t KeyBytes      = 304u;  // RenderPsoKey without its two library cookies
  constexpr uint32_t MaxEntries    = 4096u;
  /// Entries not used in this many sessions are kept (until the cap) but not prewarmed.
  constexpr uint32_t PrewarmSessions = 32u;

  enum class Origin : uint8_t {
    None          = 0u,
    D3D9          = 1u,   // a D3D9 shader: SHA-1 of bytecode + D3D9ShaderOptions
    FixedFunction = 2u,   // the fixed-function ubershader (sha all zero)
  };

  /// One recorded pipeline. On disk it follows an 8-byte check; in memory
  /// the counters are the merged values.
  struct Entry {
    uint8_t  vsOrigin     = 0u;
    uint8_t  psOrigin     = 0u;
    uint8_t  psLinked     = 0u;   // PS linked against the (D3D9) VS
    uint8_t  flags        = 0u;   // reserved, 0
    uint32_t lastSession  = 0u;
    uint32_t sessionsUsed = 0u;
    uint32_t firstUseMs   = 0u;   // earliest first use, ms after device creation
    uint8_t  vsSha[20]    = { };
    uint8_t  psSha[20]    = { };
    uint8_t  key[KeyBytes] = { };
    uint8_t  reserved[16] = { };
  };

  static_assert(sizeof(Entry) + 8u == EntrySize, "state cache entry layout");
  static_assert(offsetof(Entry, vsSha) == 16u && offsetof(Entry, key) == 56u, "state cache entry layout");

  /// Bytes that identify an entry (origins, link flag, shaders, key); the
  /// counters and reserved bytes are not part of it.
  uint64_t identityHash(const Entry& e);
  bool     sameIdentity(const Entry& a, const Entry& b);

  /// 64-bit FNV-1a; the per-entry and header checks.
  uint64_t fnv1a(const void* data, size_t size, uint64_t h = 0xcbf29ce484222325ull);

  /// Structural validity: known origins, a VS, link only against a D3D9 VS.
  bool entryValid(const Entry& e);

  enum class ParseStatus : uint32_t {
    Ok,             // entries usable (possibly empty)
    Empty,          // no data (no file)
    BadHeader,      // magic/check/sizes wrong: the file is dropped and rewritten
    OlderVersion,   // dropped (no converter for older formats yet)
    NewerVersion,   // written by a newer d9mt: neither used nor overwritten
    KeyMismatch,    // RenderPsoKey layout or mapping changed: entries dropped
  };

  struct ParseResult {
    ParseStatus status   = ParseStatus::Empty;
    uint32_t    session  = 0u;   // header session
    uint32_t    corrupt  = 0u;   // entries with a bad check or invalid content
    uint32_t    truncated = 0u;  // 1 if the file ended inside an entry or before entryCount
  };

  ParseResult parse(const uint8_t* data, size_t size, uint64_t keySignature,
                    std::vector<Entry>& out);

  std::vector<uint8_t> serialize(const std::vector<Entry>& entries,
                                 uint64_t keySignature, uint32_t session);

  /// Marks one use in `session` (once per session) and keeps the earliest first use.
  void noteUse(Entry& e, uint32_t session, uint32_t firstUseMs);

  /// Entry set keyed by identity.
  class Database {

  public:

    const std::vector<Entry>& entries() const { return m_entries; }
    size_t size() const { return m_entries.size(); }

    /// Inserts or, if the identity exists, merges the counters (max
    /// lastSession/sessionsUsed, min firstUseMs). True if inserted.
    bool merge(const Entry& e);

    /// Like merge() for an entry used in `session`: a new entry gets
    /// sessionsUsed 1, an existing one noteUse(). True if inserted.
    bool use(const Entry& e, uint32_t session);

    bool erase(const Entry& e);

    /// Drops the least recently used entries (lastSession, then
    /// sessionsUsed) above `max`. Returns how many were dropped.
    uint32_t applyCap(uint32_t max);

    void clear() { m_entries.clear(); m_index.clear(); }

  private:

    std::vector<Entry> m_entries;
    std::unordered_multimap<uint64_t, uint32_t> m_index;

    int32_t find(const Entry& e, uint64_t h) const;
    void    rebuildIndex();

  };

  /// A shader a pipeline needs: stage (0 = VS, 1 = PS), origin, SHA-1.
  struct ShaderRef {
    uint8_t stage  = 0u;
    uint8_t origin = 0u;
    uint8_t sha[20] = { };

    bool operator == (const ShaderRef& o) const {
      return stage == o.stage && origin == o.origin && !std::memcmp(sha, o.sha, sizeof(sha));
    }
  };

  struct ShaderRefHash {
    size_t operator () (const ShaderRef& r) const {
      return size_t(fnv1a(&r, sizeof(r)));
    }
  };

  ShaderRef vsRef(const Entry& e);
  ShaderRef psRef(const Entry& e);

  /// Which recorded pipelines have all their shaders. Shaders are announced
  /// as the game creates them; an entry becomes ready once every shader it
  /// needs was announced. Ready entries come out ordered by first use in
  /// earlier sessions (then sessionsUsed, then lastSession). Not thread-safe.
  class PrewarmIndex {

  public:

    /// Indexes the entries used within PrewarmSessions of `session`; the
    /// rest are skipped. Shaders announced before are kept.
    void build(const std::vector<Entry>& entries, uint32_t session);

    /// Records the shader; appends newly ready entries to the queue.
    /// Returns how many became ready.
    uint32_t announce(const ShaderRef& ref);

    bool announced(const ShaderRef& ref) const { return m_announced.count(ref) != 0u; }

    /// Next ready entry by priority; false if none.
    bool pop(Entry& out);

    size_t readyCount() const { return m_ready.size(); }
    size_t indexedCount() const { return m_pending.size(); }

  private:

    struct Pending {
      Entry    entry;
      uint32_t missing = 0u;
    };

    std::vector<Pending> m_pending;
    std::unordered_multimap<ShaderRef, uint32_t, ShaderRefHash> m_byShader;
    std::unordered_map<ShaderRef, bool, ShaderRefHash> m_announced;
    std::vector<uint32_t> m_ready;   // heap of indices into m_pending

    void pushReady(uint32_t idx);

  };

  /**
   * \brief Single-producer, single-consumer ring of fixed capacity
   *
   * Preallocated: push() and pop() never allocate. T is copy-assigned in,
   * moved out, and reset to T() on pop so no reference outlives the slot.
   */
  template<typename T, size_t N>
  class SpscRing {
    static_assert((N & (N - 1u)) == 0u, "power of two");

  public:

    /// Producer only. False (nothing stored) if full.
    bool push(const T& v) {
      const size_t head = m_head.load(std::memory_order_relaxed);
      const size_t tail = m_tail.load(std::memory_order_acquire);

      if (head - tail >= N)
        return false;

      m_slots[head & (N - 1u)] = v;
      m_head.store(head + 1u, std::memory_order_release);
      return true;
    }

    /// Consumer only.
    bool pop(T& out) {
      const size_t tail = m_tail.load(std::memory_order_relaxed);
      const size_t head = m_head.load(std::memory_order_acquire);

      if (tail == head)
        return false;

      T& slot = m_slots[tail & (N - 1u)];
      out = std::move(slot);
      slot = T();
      m_tail.store(tail + 1u, std::memory_order_release);
      return true;
    }

  private:

    alignas(64) std::atomic<size_t> m_head = { 0u };
    alignas(64) std::atomic<size_t> m_tail = { 0u };
    std::array<T, N> m_slots = { };

  };

}
