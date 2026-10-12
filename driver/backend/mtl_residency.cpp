#include "mtl_residency.h"

#include <windows.h>

#include "../util/log/log.h"

namespace dxvk::backend {

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
    beforeSubmit();
    releaseCommitted();

    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_set)
      MTL4CommandQueue_removeResidencySet(m_queue, m_set.handle());
    for (obj_handle_t set : m_attachedLayerSets)
      MTL4CommandQueue_removeResidencySet(m_queue, set);
    m_attachedLayerSets.clear();
  }

  void MtlResidencyTracker::allocCreated(obj_handle_t alloc, uint64_t bytes) {
    if (!alloc || !m_set)
      return;

    std::lock_guard<std::mutex> lock(m_mutex);
    MTLResidencySet_addAllocation(m_set.handle(), alloc);
    m_dirty = true;
    m_adds++;
    m_residentBytes += bytes;
  }

  void MtlResidencyTracker::allocDestroyed(obj_handle_t alloc, void* hostMem, uint64_t lastUseSerial, uint64_t bytes) {
    if (!alloc) {
      if (hostMem)
        VirtualFree(hostMem, 0, MEM_RELEASE);
      return;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    m_pending.push_back({ alloc, hostMem, lastUseSerial, bytes });
    m_pendingCount = m_pending.size();
    m_pendingBytes += bytes;
  }

  void MtlResidencyTracker::beforeSubmit() {
    if (!m_set)
      return;

    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_dirty)
      commitLocked();
  }

  void MtlResidencyTracker::releaseCommitted() {
    std::vector<PendingRemoval> released;

    { std::lock_guard<std::mutex> lock(m_mutex);
      if (m_committed.empty())
        return;
      released.swap(m_committed);
    }

    // Order matters: set removal + commit (beforeSubmit) -> object release ->
    // host free, so the Metal buffer dies before its bytesNoCopy memory.
    for (const PendingRemoval& p : released) {
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

    { std::lock_guard<std::mutex> lock(m_mutex);
      if (!retireLocked(completedSerial) || m_batchCommits)
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

    std::lock_guard<std::mutex> lock(m_mutex);
    for (obj_handle_t s : m_attachedLayerSets) {
      if (s == set)
        return true;
    }
    MTL4CommandQueue_addResidencySet(m_queue, set);
    m_attachedLayerSets.push_back(set);
    return true;
  }

  void MtlResidencyTracker::detachLayer(obj_handle_t layer) {
    if (!layer)
      return;

    obj_handle_t set = MetalLayer_residencySet(layer);
    if (!set)
      return;

    std::lock_guard<std::mutex> lock(m_mutex);
    for (size_t i = 0u; i < m_attachedLayerSets.size(); i++) {
      if (m_attachedLayerSets[i] == set) {
        MTL4CommandQueue_removeResidencySet(m_queue, set);
        m_attachedLayerSets.erase(m_attachedLayerSets.begin() + i);
        return;
      }
    }
  }

}
