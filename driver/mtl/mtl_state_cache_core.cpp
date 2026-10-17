#include "mtl_state_cache_core.h"

#include <algorithm>

namespace dxvk::statecache {

  namespace {

    constexpr char     Magic[8]      = { 'D', '9', 'M', 'T', 'S', 'T', 'C', '\0' };
    constexpr uint32_t CheckedHeader = 56u;   // bytes covered by the header check

    // Identity = origins + link flag, then both SHA-1s and the key.
    constexpr size_t IdHeadBytes = 3u;
    constexpr size_t IdTailOffset = offsetof(Entry, vsSha);
    constexpr size_t IdTailBytes  = offsetof(Entry, reserved) - IdTailOffset;

    template<typename T>
    void put(std::vector<uint8_t>& out, size_t offset, const T& v) {
      std::memcpy(out.data() + offset, &v, sizeof(v));
    }

    template<typename T>
    T get(const uint8_t* p) {
      T v;
      std::memcpy(&v, p, sizeof(v));
      return v;
    }

    /// True if a should come out of the ready queue after b.
    bool lowerPriority(const Entry& a, const Entry& b) {
      if (a.firstUseMs != b.firstUseMs)
        return a.firstUseMs > b.firstUseMs;
      if (a.sessionsUsed != b.sessionsUsed)
        return a.sessionsUsed < b.sessionsUsed;
      return a.lastSession < b.lastSession;
    }

  }


  uint64_t fnv1a(const void* data, size_t size, uint64_t h) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0u; i < size; i++)
      h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
  }


  uint64_t identityHash(const Entry& e) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&e);
    return fnv1a(p + IdTailOffset, IdTailBytes, fnv1a(p, IdHeadBytes));
  }


  bool sameIdentity(const Entry& a, const Entry& b) {
    const uint8_t* pa = reinterpret_cast<const uint8_t*>(&a);
    const uint8_t* pb = reinterpret_cast<const uint8_t*>(&b);
    return !std::memcmp(pa, pb, IdHeadBytes)
        && !std::memcmp(pa + IdTailOffset, pb + IdTailOffset, IdTailBytes);
  }


  bool entryValid(const Entry& e) {
    if (e.vsOrigin != uint8_t(Origin::D3D9) && e.vsOrigin != uint8_t(Origin::FixedFunction))
      return false;
    if (e.psOrigin > uint8_t(Origin::FixedFunction) || e.psLinked > 1u)
      return false;
    if (e.psLinked && (e.psOrigin != uint8_t(Origin::D3D9) || e.vsOrigin != uint8_t(Origin::D3D9)))
      return false;
    return true;
  }


  ParseResult parse(const uint8_t* data, size_t size, uint64_t keySignature,
                    std::vector<Entry>& out) {
    ParseResult r;
    out.clear();

    if (!data || !size)
      return r;   // Empty

    if (size < 12u || std::memcmp(data, Magic, sizeof(Magic))) {
      r.status = ParseStatus::BadHeader;
      return r;
    }

    // The version decides how to read the rest, so it comes first: a newer
    // file may have a different header.
    const uint32_t version = get<uint32_t>(data + 8u);
    if (version > FormatVersion) {
      r.status = ParseStatus::NewerVersion;
      return r;
    }

    if (version < FormatVersion) {
      r.status = ParseStatus::OlderVersion;
      return r;
    }

    if (size < HeaderSize
     || get<uint32_t>(data + 12u) != HeaderSize
     || get<uint32_t>(data + 16u) != EntrySize
     || get<uint64_t>(data + CheckedHeader) != fnv1a(data, CheckedHeader)) {
      r.status = ParseStatus::BadHeader;
      return r;
    }

    r.session = get<uint32_t>(data + 32u);

    if (get<uint64_t>(data + 24u) != keySignature) {
      r.status = ParseStatus::KeyMismatch;
      return r;
    }

    const uint32_t count = get<uint32_t>(data + 20u);
    const size_t   avail = (size - HeaderSize) / EntrySize;
    const size_t   n     = std::min<size_t>(count, avail);

    if (n < count || (size - HeaderSize) % EntrySize)
      r.truncated = 1u;

    out.reserve(n);

    for (size_t i = 0u; i < n; i++) {
      const uint8_t* p = data + HeaderSize + i * EntrySize;
      Entry e;
      std::memcpy(&e, p + 8u, sizeof(e));

      if (get<uint64_t>(p) != fnv1a(&e, sizeof(e)) || !entryValid(e)) {
        r.corrupt++;
        continue;
      }

      out.push_back(e);
    }

    r.status = ParseStatus::Ok;
    return r;
  }


  std::vector<uint8_t> serialize(const std::vector<Entry>& entries,
                                 uint64_t keySignature, uint32_t session) {
    std::vector<uint8_t> out(HeaderSize + entries.size() * EntrySize, uint8_t(0u));

    std::memcpy(out.data(), Magic, sizeof(Magic));
    put<uint32_t>(out, 8u,  FormatVersion);
    put<uint32_t>(out, 12u, HeaderSize);
    put<uint32_t>(out, 16u, EntrySize);
    put<uint32_t>(out, 20u, uint32_t(entries.size()));
    put<uint64_t>(out, 24u, keySignature);
    put<uint32_t>(out, 32u, session);
    put<uint64_t>(out, CheckedHeader, fnv1a(out.data(), CheckedHeader));

    for (size_t i = 0u; i < entries.size(); i++) {
      const size_t offset = HeaderSize + i * EntrySize;
      put<uint64_t>(out, offset, fnv1a(&entries[i], sizeof(Entry)));
      std::memcpy(out.data() + offset + 8u, &entries[i], sizeof(Entry));
    }

    return out;
  }


  void noteUse(Entry& e, uint32_t session, uint32_t firstUseMs) {
    if (e.lastSession != session) {
      e.lastSession = std::max(e.lastSession, session);
      if (e.sessionsUsed != ~0u)
        e.sessionsUsed++;
    }

    e.firstUseMs = std::min(e.firstUseMs, firstUseMs);
  }


  int32_t Database::find(const Entry& e, uint64_t h) const {
    auto range = m_index.equal_range(h);
    for (auto it = range.first; it != range.second; ++it) {
      if (sameIdentity(m_entries[it->second], e))
        return int32_t(it->second);
    }
    return -1;
  }


  bool Database::merge(const Entry& e) {
    const uint64_t h = identityHash(e);
    const int32_t  i = find(e, h);

    if (i < 0) {
      m_index.emplace(h, uint32_t(m_entries.size()));
      m_entries.push_back(e);
      return true;
    }

    Entry& d = m_entries[uint32_t(i)];
    d.lastSession  = std::max(d.lastSession,  e.lastSession);
    d.sessionsUsed = std::max(d.sessionsUsed, e.sessionsUsed);
    d.firstUseMs   = std::min(d.firstUseMs,   e.firstUseMs);
    return false;
  }


  bool Database::use(const Entry& e, uint32_t session) {
    const uint64_t h = identityHash(e);
    const int32_t  i = find(e, h);

    if (i < 0) {
      Entry n = e;
      n.lastSession  = session;
      n.sessionsUsed = 1u;
      m_index.emplace(h, uint32_t(m_entries.size()));
      m_entries.push_back(n);
      return true;
    }

    noteUse(m_entries[uint32_t(i)], session, e.firstUseMs);
    return false;
  }


  bool Database::erase(const Entry& e) {
    const int32_t i = find(e, identityHash(e));
    if (i < 0)
      return false;

    m_entries.erase(m_entries.begin() + i);
    rebuildIndex();
    return true;
  }


  uint32_t Database::applyCap(uint32_t max) {
    if (m_entries.size() <= max)
      return 0u;

    const uint32_t dropped = uint32_t(m_entries.size() - max);

    // Most recently used first; stable so equal entries keep file order.
    std::stable_sort(m_entries.begin(), m_entries.end(), [] (const Entry& a, const Entry& b) {
      if (a.lastSession != b.lastSession)
        return a.lastSession > b.lastSession;
      return a.sessionsUsed > b.sessionsUsed;
    });

    m_entries.resize(max);
    rebuildIndex();
    return dropped;
  }


  void Database::rebuildIndex() {
    m_index.clear();
    for (uint32_t i = 0u; i < m_entries.size(); i++)
      m_index.emplace(identityHash(m_entries[i]), i);
  }


  ShaderRef vsRef(const Entry& e) {
    ShaderRef r;
    r.stage  = 0u;
    r.origin = e.vsOrigin;
    std::memcpy(r.sha, e.vsSha, sizeof(r.sha));
    return r;
  }


  ShaderRef psRef(const Entry& e) {
    ShaderRef r;
    r.stage  = 1u;
    r.origin = e.psOrigin;
    std::memcpy(r.sha, e.psSha, sizeof(r.sha));
    return r;
  }


  void PrewarmIndex::build(const std::vector<Entry>& entries, uint32_t session) {
    m_pending.clear();
    m_byShader.clear();
    m_ready.clear();

    for (const Entry& e : entries) {
      if (!entryValid(e) || e.lastSession > session || session - e.lastSession > PrewarmSessions)
        continue;

      const uint32_t idx = uint32_t(m_pending.size());
      Pending p;
      p.entry = e;

      const ShaderRef refs[2] = { vsRef(e), psRef(e) };
      for (uint32_t s = 0u; s < 2u; s++) {
        if (refs[s].origin == uint8_t(Origin::None))
          continue;
        m_byShader.emplace(refs[s], idx);
        if (!m_announced.count(refs[s]))
          p.missing++;
      }

      m_pending.push_back(p);

      if (!p.missing)
        pushReady(idx);
    }
  }


  uint32_t PrewarmIndex::announce(const ShaderRef& ref) {
    if (!m_announced.emplace(ref, true).second)
      return 0u;

    uint32_t ready = 0u;
    auto range = m_byShader.equal_range(ref);

    for (auto it = range.first; it != range.second; ++it) {
      Pending& p = m_pending[it->second];
      if (p.missing && !--p.missing) {
        pushReady(it->second);
        ready++;
      }
    }

    return ready;
  }


  void PrewarmIndex::pushReady(uint32_t idx) {
    m_ready.push_back(idx);
    std::push_heap(m_ready.begin(), m_ready.end(), [this] (uint32_t a, uint32_t b) {
      return lowerPriority(m_pending[a].entry, m_pending[b].entry);
    });
  }


  bool PrewarmIndex::pop(Entry& out) {
    if (m_ready.empty())
      return false;

    std::pop_heap(m_ready.begin(), m_ready.end(), [this] (uint32_t a, uint32_t b) {
      return lowerPriority(m_pending[a].entry, m_pending[b].entry);
    });

    out = m_pending[m_ready.back()].entry;
    m_ready.pop_back();
    return true;
  }

}
