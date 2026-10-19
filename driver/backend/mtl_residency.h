#pragma once

// Drawables never enter this set — pinning them would block CoreAnimation
// from recycling them and stall nextDrawable permanently. Removals defer
// until the GPU retires the referencing serial, since Metal 4 command
// buffers don't retain resources.

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include "wmt_handle.h"

namespace dxvk::backend {

  struct Mtl4ResidencySetTag; using WmtResidencySet = WmtRef<Mtl4ResidencySetTag>;

  class MtlResidencyTracker {

  public:

    // batchCommits = false (legacy): serialRetired() commits its removals itself.
    MtlResidencyTracker(obj_handle_t device, obj_handle_t queue, bool batchCommits = true);

    ~MtlResidencyTracker();

    MtlResidencyTracker             (const MtlResidencyTracker&) = delete;
    MtlResidencyTracker& operator = (const MtlResidencyTracker&) = delete;

    void allocCreated(obj_handle_t alloc, uint64_t bytes = 0u);

    void allocDestroyed(obj_handle_t alloc, void* hostMem, uint64_t lastUseSerial, uint64_t bytes = 0u);

    // Commits pending adds/removes: once per submit, and only if the set changed.
    void beforeSubmit();

    // Removes allocations whose last use has retired from the set. They are
    // released (object, then host memory) by the first serialRetired() or
    // releaseCommitted() after the next commit, so one commit covers adds
    // and removes alike and releasing stays off the pre-commit path.
    void serialRetired(uint64_t completedSerial);

    void releaseCommitted();

    bool attachLayer(obj_handle_t layer);
    void detachLayer(obj_handle_t layer);

    int64_t  netResident()     const { return int64_t(m_adds.load()) - int64_t(m_removes.load()); }
    uint64_t totalAdds()       const { return m_adds.load(); }
    uint64_t totalRemoves()    const { return m_removes.load(); }
    uint64_t pendingRemovals() const { return m_pendingCount.load(); }
    uint64_t totalCommits()    const { return m_commits.load(); }
    // Bytes added minus bytes removed / still waiting for removal (as passed by callers).
    uint64_t residentBytes()   const { return m_residentBytes.load(); }
    uint64_t pendingBytes()    const { return m_pendingBytes.load(); }

  private:

    struct PendingRemoval {
      obj_handle_t alloc   = 0u;
      void*        hostMem = nullptr;
      uint64_t     serial  = 0u;
      uint64_t     bytes   = 0u;
    };

    bool retireLocked(uint64_t completedSerial);
    void commitLocked();

    obj_handle_t    m_queue = 0u;
    bool            m_batchCommits = true;
    WmtResidencySet m_set;

    std::mutex                  m_mutex;
    bool                        m_dirty = false;
    std::vector<PendingRemoval> m_pending;
    std::vector<PendingRemoval> m_removed;    // out of the set, awaiting commit
    std::vector<PendingRemoval> m_committed;  // removal committed, awaiting release
    std::vector<obj_handle_t>   m_attachedLayerSets;

    std::atomic<uint64_t> m_adds{0u};
    std::atomic<uint64_t> m_removes{0u};
    std::atomic<uint64_t> m_pendingCount{0u};
    std::atomic<uint64_t> m_residentBytes{0u};
    std::atomic<uint64_t> m_pendingBytes{0u};
    std::atomic<uint64_t> m_commits{0u};

  };

}
