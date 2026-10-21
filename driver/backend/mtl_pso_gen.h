#pragma once

// Content-addressed generations for the PSO MRU (MtlContextImpl::resolvePso). The MRU snapshot
// compares a generation per piece of PSO state (blend array, vertex layout, stream strides)
// instead of the state itself. A plain counter gives A->B->A three generations, so returning
// to A misses the MRU and rebuilds the whole RenderPsoKey. PsoGenRing remembers the last N
// distinct contents: content seen again gets its old generation back, new content a fresh one.
// Equal generations always mean equal content: a generation is never handed out twice.

#include <cstdint>

namespace dxvk::backend {

  template <typename T, uint32_t N>
  class PsoGenRing {

  public:

    // The generation of the content `key` describes: `equal(const T&, const K&)` compares a
    // remembered entry, `assign(T&, const K&)` stores a new one (reusing T's capacity).
    template <typename K, typename Equal, typename Assign>
    uint32_t intern(const K& key, Equal equal, Assign assign) {
      for (uint32_t i = 0u; i < N; i++) {
        if (m_gen[i] && equal(m_content[i], key))
          return m_gen[i];
      }
      const uint32_t slot = m_cursor;
      m_cursor = (m_cursor + 1u) % N;
      assign(m_content[slot], key);
      m_gen[slot] = ++m_next;
      return m_gen[slot];
    }

  private:

    T        m_content[N] = { };
    uint32_t m_gen[N]     = { };   // 0: empty slot
    uint32_t m_cursor     = 0u;
    uint32_t m_next       = 0u;    // generation 0 is the state at construction, never interned

  };

}
