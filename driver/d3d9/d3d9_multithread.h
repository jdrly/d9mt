#pragma once

#include "d3d9_include.h"
#include "d3d9_hitch.h"
#include "d3d9_stats.h"
#include "d3d9_pace.h"
#include "d3d9_xfer_stats.h"

namespace dxvk {

  class D3D9Multithread;

  class D3D9DeviceLock {

  public:

    D3D9DeviceLock() { }

    D3D9DeviceLock(D3D9Multithread& mutex)
    : m_mutex(&mutex) { }

    D3D9DeviceLock(D3D9DeviceLock&& other)
    : m_mutex(other.m_mutex), m_hitch(other.m_hitch),
      m_hitchT0(other.m_hitchT0), m_hitchOuter(other.m_hitchOuter) {
      other.m_mutex = nullptr;
      other.m_hitch = nullptr;
    }

    D3D9DeviceLock& operator = (D3D9DeviceLock&& other) {
      Unlock();

      m_mutex      = other.m_mutex;
      m_hitch      = other.m_hitch;
      m_hitchT0    = other.m_hitchT0;
      m_hitchOuter = other.m_hitchOuter;
      other.m_mutex = nullptr;
      other.m_hitch = nullptr;
      return *this;
    }

    ~D3D9DeviceLock() {
      Unlock();
    }

    // D9MT_PROFILE: the lock's lifetime is time spent inside d9mt.
    void AttachHitch(hitch::Profiler* pProfiler, uint64_t T0, bool Outer) {
      m_hitch      = pProfiler;
      m_hitchT0    = T0;
      m_hitchOuter = Outer;
    }

  private:

    D3D9Multithread* m_mutex = nullptr;
    hitch::Profiler* m_hitch = nullptr;
    uint64_t         m_hitchT0 = 0u;
    bool             m_hitchOuter = false;

    void Unlock();

  };


  class D3D9Multithread {
    static constexpr uint32_t InvalidTid = -1u;

    friend D3D9DeviceLock;
  public:

    D3D9Multithread(BOOL Protected)
    : m_protected(Protected) { }

    D3D9DeviceLock AcquireLock() {
      if (likely(!m_protected))
        return D3D9DeviceLock();

      uint32_t expected = 0u;
      uint32_t threadId = dxvk::this_thread::get_id();

      if (likely(m_owner.compare_exchange_weak(expected, threadId, std::memory_order_acquire)))
        return D3D9DeviceLock(*this);

      if (expected == threadId)
        return D3D9DeviceLock();

      return LockContested(threadId);
    }

  private:

    alignas(CACHE_LINE_SIZE)
    std::atomic<uint32_t>   m_owner     = { 0u };
    BOOL                    m_protected = false;

    D3D9DeviceLock LockContested(uint32_t threadId);

    void Unlock() {
      m_owner.store(0u, std::memory_order_release);
    }

  };

  inline void D3D9DeviceLock::Unlock() {
    if (unlikely(m_hitch != nullptr)) {
      m_hitch->leave(m_hitchOuter, m_hitchT0, hitch::ticks());
      m_hitch = nullptr;
    }

    if (m_mutex)
      m_mutex->Unlock();
  }

}
