#include "mtl_residency.h"

#include <windows.h>

#include "../util/log/log.h"

namespace dxvk::backend {

  bool (*MtlResidencyTracker::s_detaching)() = &this_thread::isInModuleDetachment;

  namespace {

    // The tracker's mutex, never waited for while the process detaches (#63). ExitProcess kills
    // every other thread before DLL detach, and a compile worker or prewarm thread killed inside
    // pipelineCreated (or the CS thread inside any other call) leaves the mutex held for good.
    // So when the lock is taken during detach, the owner is dead: the caller skips its work and
    // leaks what it would have released (the OS reclaims it at exit). Uncontended, this is one
    // try_lock, as cheap as a lock; the detach check runs only on contention.
    class DetachSafeLock {
    public:
      DetachSafeLock(std::mutex& mutex, bool (*detaching)())
      : m_mutex(mutex) {
        if (likely(m_mutex.try_lock()))
          m_owns = true;
        else if (!detaching()) {
          m_mutex.lock();
          m_owns = true;
        }
      }
      ~DetachSafeLock() {
        if (m_owns)
          m_mutex.unlock();
      }
      DetachSafeLock(const DetachSafeLock&) = delete;
      DetachSafeLock& operator = (const DetachSafeLock&) = delete;
      explicit operator bool () const { return m_owns; }
    private:
      std::mutex& m_mutex;
      bool        m_owns = false;
    };

  }

  MtlResidencyTracker::MtlResidencyTracker(obj_handle_t device, obj_handle_t queue, bool batchCommits)
  : m_queue(queue), m_batchCommits(batchCommits) {
    WMTResidencySetInfo info = {};
    m_set = WmtResidencySet::adopt(MTLDevice_newResidencySet(device, &info));

    if (!m_set) {
      Logger::err("MtlResidencyTracker: newResidencySet failed");
      return;
    }

    MTLResidencySet_requestResidency(m_set.handle());

    // Queue-level attach covers all command buffers committed to the queue (32-sets-per-queue limit applies).
    MTL4CommandQueue_addResidencySet(m_queue, m_set.handle());
  }

  MtlResidencyTracker::~MtlResidencyTracker() {
    serialRetired(~0ull);

    // Pipelines: out of the set and committed (with the retired allocations) before they are released.
    std::vector<obj_handle_t> psos;
    if (DetachSafeLock lock(m_mutex, s_detaching); lock) {
      if (m_set) {
        for (obj_handle_t pso : m_psos)
          MTLResidencySet_removeAllocation(m_set.handle(), pso);
      }
      if (!m_psos.empty())
        m_dirty = true;
      psos.swap(m_psos);
      m_psoCount = 0u;
    }
    beforeSubmit();
    releaseCommitted();
    for (obj_handle_t pso : psos)
      NSObject_release(pso);

    DetachSafeLock lock(m_mutex, s_detaching);
    if (!lock)
      return;  // detaching, the owner is dead: the set and the layer sets are leaked
    if (m_set)
      MTL4CommandQueue_removeResidencySet(m_queue, m_set.handle());
    for (obj_handle_t set : m_attachedLayerSets) {
      MTL4CommandQueue_removeResidencySet(m_queue, set);
      NSObject_release(set);
    }
    m_attachedLayerSets.clear();
  }

  void MtlResidencyTracker::allocCreated(obj_handle_t alloc, uint64_t bytes) {
    if (!alloc || !m_set)
      return;

    DetachSafeLock lock(m_mutex, s_detaching);
    if (!lock)
      return;
    MTLResidencySet_addAllocation(m_set.handle(), alloc);
    m_dirty = true;
    m_adds++;
    m_residentBytes += bytes;
  }

  void MtlResidencyTracker::pipelineCreated(obj_handle_t pso) {
    if (!pso || !m_set)
      return;

    DetachSafeLock lock(m_mutex, s_detaching);
    if (!lock)
      return;
    NSObject_retain(pso);
    MTLResidencySet_addAllocation(m_set.handle(), pso);
    m_psos.push_back(pso);
    m_dirty = true;
    m_psoCount++;
  }

  void MtlResidencyTracker::allocDestroyed(obj_handle_t alloc, void* hostMem, uint64_t lastUseSerial, uint64_t bytes) {
    if (!alloc) {
      if (hostMem)
        VirtualFree(hostMem, 0, MEM_RELEASE);
      return;
    }

    DetachSafeLock lock(m_mutex, s_detaching);
    if (!lock)
      return;  // detaching: alloc and hostMem are leaked
    m_pending.push_back({ alloc, hostMem, lastUseSerial, bytes });
    m_pendingCount = m_pending.size();
    m_pendingBytes += bytes;
  }

  void MtlResidencyTracker::beforeSubmit() {
    if (!m_set)
      return;

    DetachSafeLock lock(m_mutex, s_detaching);
    if (lock && m_dirty)
      commitLocked();
  }

  void MtlResidencyTracker::releaseCommitted() {
    std::vector<PendingRemoval> released;

    { DetachSafeLock lock(m_mutex, s_detaching);
      if (!lock || m_committed.empty())
        return;
      released.swap(m_committed);
    }

    // Order matters: set removal + commit (beforeSubmit) -> object release ->
    // host free, so the Metal buffer dies before its bytesNoCopy memory.
    for (const PendingRemoval& p : released) {
      lifetime::checkRelease(p.alloc, "texture");
      NSObject_release(p.alloc);
      if (p.hostMem)
        VirtualFree(p.hostMem, 0, MEM_RELEASE);
    }
  }

  void MtlResidencyTracker::serialRetired(uint64_t completedSerial) {
    if (!m_set)
      return;

    // Removed by an earlier call and committed since (normally by the
    // submit that just ran): release them now, off the pre-commit path.
    releaseCommitted();

    { DetachSafeLock lock(m_mutex, s_detaching);
      if (!lock || !retireLocked(completedSerial) || m_batchCommits)
        return;
      // Legacy: commit the removals right away, as before batching.
      commitLocked();
    }
    releaseCommitted();
  }

  bool MtlResidencyTracker::retireLocked(uint64_t completedSerial) {
    if (m_pending.empty())
      return false;

    size_t write = 0u, due = 0u;
    for (size_t i = 0u; i < m_pending.size(); i++) {
      const PendingRemoval& p = m_pending[i];
      if (p.serial <= completedSerial) {
        MTLResidencySet_removeAllocation(m_set.handle(), p.alloc);
        m_pendingBytes -= p.bytes;
        m_residentBytes -= p.bytes;
        m_removed.push_back(p);
        due++;
      } else {
        if (write != i)
          m_pending[write] = p;
        write++;
      }
    }
    m_pending.resize(write);
    m_pendingCount = write;

    if (due) {
      m_dirty = true;
      m_removes += due;
    }
    return due != 0u;
  }

  void MtlResidencyTracker::commitLocked() {
    MTLResidencySet_commit(m_set.handle());
    m_dirty = false;
    m_commits++;
    m_committed.insert(m_committed.end(), m_removed.begin(), m_removed.end());
    m_removed.clear();
  }

  bool MtlResidencyTracker::attachLayer(obj_handle_t layer) {
    if (!layer)
      return false;

    obj_handle_t set = MetalLayer_residencySet(layer);
    if (!set)
      return false;

    DetachSafeLock lock(m_mutex, s_detaching);
    if (!lock)
      return false;
    for (obj_handle_t s : m_attachedLayerSets) {
      if (s == set)
        return true;
    }
    // (the layer owns it: retained, so it outlives the layer until the queue lets go of it)
    NSObject_retain(set);
    MTL4CommandQueue_addResidencySet(m_queue, set);
    m_attachedLayerSets.push_back(set);
    return true;
  }

  obj_handle_t MtlResidencyTracker::detachLayer(obj_handle_t layer) {
    if (!layer)
      return 0u;

    obj_handle_t set = MetalLayer_residencySet(layer);
    if (!set)
      return 0u;

    DetachSafeLock lock(m_mutex, s_detaching);
    if (!lock)
      return 0u;
    for (size_t i = 0u; i < m_attachedLayerSets.size(); i++) {
      if (m_attachedLayerSets[i] == set) {
        m_attachedLayerSets.erase(m_attachedLayerSets.begin() + i);
        return set;
      }
    }
    return 0u;
  }

  void MtlResidencyTracker::removeLayerSet(obj_handle_t set) {
    if (!set)
      return;
    MTL4CommandQueue_removeResidencySet(m_queue, set);
    NSObject_release(set);
  }

}
