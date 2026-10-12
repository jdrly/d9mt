#pragma once

#include "../mtl/mtl_device.h"
#include "../cs/dxvk_cs.h"

#include "d3d9_buffer_tracking.h"
#include "d3d9_device_child.h"
#include "d3d9_format.h"

namespace dxvk {

  enum D3D9_COMMON_BUFFER_MAP_MODE {
    D3D9_COMMON_BUFFER_MAP_MODE_BUFFER,
    D3D9_COMMON_BUFFER_MAP_MODE_DIRECT
  };

  struct D3D9_BUFFER_DESC {
    D3DRESOURCETYPE Type;
    UINT            Size;
    DWORD           Usage;
    D3D9Format      Format;
    D3DPOOL         Pool;
    DWORD           FVF;
  };

  enum D3D9_COMMON_BUFFER_TYPE {
    D3D9_COMMON_BUFFER_TYPE_MAPPING,
    D3D9_COMMON_BUFFER_TYPE_STAGING,
    D3D9_COMMON_BUFFER_TYPE_REAL
  };

  struct D3D9Range {
    D3D9Range() { Clear(); }

    D3D9Range(uint32_t min, uint32_t max)
    : min(min),
      max(max) {

    }

    inline bool IsDegenerate() const { return min == max; }

    inline void Conjoin(D3D9Range range) {
      if (IsDegenerate())
        *this = range;
      else {
        min = std::min(range.min, min);
        max = std::max(range.max, max);
      }
    }

    inline bool Overlaps(D3D9Range range) {
      if (IsDegenerate())
        return false;

      return range.max > min && range.min < max;
    }

    inline void Clear() { min = 0; max = 0; }

    uint32_t min = 0;
    uint32_t max = 0;
  };

  class D3D9CommonBuffer {
    static constexpr MtlDeviceSize BufferSliceAlignment = 64;
  public:

    D3D9CommonBuffer(
            D3D9DeviceEx*      pDevice,
      const D3D9_BUFFER_DESC*  pDesc);

    ~D3D9CommonBuffer();

    HRESULT Lock(
            UINT   OffsetToLock,
            UINT   SizeToLock,
            void** ppbData,
            DWORD  Flags);

    HRESULT Unlock();

    D3D9_COMMON_BUFFER_MAP_MODE DetermineMapMode(const D3D9Options* options) const;

    inline D3D9_COMMON_BUFFER_MAP_MODE GetMapMode() const {
      return m_mapMode;
    }

    template <D3D9_COMMON_BUFFER_TYPE Type>
    inline const Rc<MtlBuffer>& GetBuffer() const {
      if constexpr (Type == D3D9_COMMON_BUFFER_TYPE_MAPPING)
        return GetMapBuffer();
      else if constexpr (Type == D3D9_COMMON_BUFFER_TYPE_STAGING)
        return GetStagingBuffer();
      else
        return GetRealBuffer();
    }

    template <D3D9_COMMON_BUFFER_TYPE Type>
    inline MtlBufferSlice GetBufferSlice() const {
      return GetBufferSlice<Type>(0, m_desc.Size);
    }

    template <D3D9_COMMON_BUFFER_TYPE Type>
    inline MtlBufferSlice GetBufferSlice(MtlDeviceSize offset) const {
      return GetBufferSlice<Type>(offset, m_desc.Size - offset);
    }

    template <D3D9_COMMON_BUFFER_TYPE Type>
    inline MtlBufferSlice GetBufferSlice(MtlDeviceSize offset, MtlDeviceSize length) const {
     if (likely(length && offset < m_desc.Size)) {
        const Rc<MtlBuffer>& buffer = GetBuffer<Type>();
        MtlDeviceSize clamped = std::min<MtlDeviceSize>(m_desc.Size - offset, length);

        // The map buffer's storage() lags its renames until the CS thread catches up, so
        // slices must snapshot the allocation the frontend actually maps, not the current one.
        if (buffer == GetMapBuffer())
          return MtlBufferSlice(buffer, m_allocation, offset, clamped);

        // Demoted DIRECT buffer: same for the renames it had before demotion.
        if (buffer == m_buffer && m_realAllocation != nullptr)
          return MtlBufferSlice(buffer, m_realAllocation, offset, clamped);

        return MtlBufferSlice(buffer, offset, clamped);
     }

      return MtlBufferSlice();
    }

    inline Rc<MtlAllocation> DiscardMapSlice() {
      m_allocation = GetMapBuffer()->allocateStorage();
      if (TracksUses())
        m_tracker.onRename();
      return m_allocation;
    }

    inline Rc<MtlAllocation> GetMappedSlice() const {
      return m_allocation;
    }

    inline DWORD GetMapFlags() const      { return m_mapFlags; }
    inline void SetMapFlags(DWORD Flags)  { m_mapFlags = Flags; }

    inline const D3D9_BUFFER_DESC* Desc() const { return &m_desc; }

    static HRESULT ValidateBufferProperties(const D3D9_BUFFER_DESC* pDesc, const bool IsExtended);

    inline D3D9Range& DirtyRange()  { return m_dirtyRange; }

    inline bool NeedsReadback() const     { return m_needsReadback; }

    inline void SetNeedsReadback(bool state) { m_needsReadback = state; }

    inline uint32_t IncrementLockCount() { return ++m_lockCount; }
    inline uint32_t DecrementLockCount() {
      if (m_lockCount == 0)
        return 0;

      return --m_lockCount;
    }
    inline uint32_t GetLockCount() const { return m_lockCount; }

    inline bool NeedsUpload() const { return (m_desc.Pool != D3DPOOL_DEFAULT || m_uploadAtDraw) && m_mapMode != D3D9_COMMON_BUFFER_MAP_MODE_DIRECT && !m_dirtyRange.IsDegenerate(); }

    void PreLoad();

    bool HasSequenceNumber() const {
      return m_mapMode != D3D9_COMMON_BUFFER_MAP_MODE_DIRECT || m_trackUses;
    }

    // DIRECT buffer whose uses are stamped per CS chunk (d3d9_buffer_tracking.h).
    // Without it (legacy profile), DIRECT locks synchronize with everything.
    bool TracksUses() const {
      return m_trackUses && m_mapMode == D3D9_COMMON_BUFFER_MAP_MODE_DIRECT;
    }

    void TrackMappingBufferSequenceNumber(uint64_t Seq) {
      if (TracksUses())
        m_tracker.onUse(Seq);
      else
        m_seq = Seq;
    }


    uint64_t GetMappingBufferSequenceNumber() const {
      if (TracksUses())
        return m_tracker.lastUse();
      return HasSequenceNumber() ? m_seq
        : DxvkCsThread::SynchronizeAll;
    }

    D3D9DirectBufferTracker& Tracker() { return m_tracker; }

    // DEFAULT + WRITEONLY, not DYNAMIC, mapped directly: may go back to BUFFER
    // mode when the game keeps locking it in place.
    bool IsDemotable() const {
      return m_demotable && m_mapMode == D3D9_COMMON_BUFFER_MAP_MODE_DIRECT;
    }

    // DIRECT -> BUFFER for the rest of the buffer's life: the current backing
    // (contents copied into a new staging buffer) becomes the GPU buffer and
    // later locks write the staging copy, uploaded on unlock. Must not be
    // locked, and must not need a readback.
    void DemoteToBufferMode();

    bool DoPerDrawUpload() const {
      return m_desc.Pool == D3DPOOL_SYSTEMMEM && (m_desc.Usage & D3DUSAGE_DYNAMIC) != 0;
    }

  private:

    Rc<MtlBuffer> CreateBuffer() const;
    Rc<MtlBuffer> CreateStagingBuffer() const;

    inline const Rc<MtlBuffer>& GetMapBuffer() const {
      return m_stagingBuffer != nullptr ? m_stagingBuffer : m_buffer;
    }

    inline const Rc<MtlBuffer>& GetStagingBuffer() const {
      return m_stagingBuffer;
    }

    inline const Rc<MtlBuffer>& GetRealBuffer() const {
      return m_buffer;
    }

    D3D9DeviceEx*               m_parent;
    const D3D9_BUFFER_DESC      m_desc;
    DWORD                       m_mapFlags = 0;
    bool                        m_needsReadback = false;
    D3D9_COMMON_BUFFER_MAP_MODE m_mapMode;
    bool                        m_uploadAtDraw = false;

    Rc<MtlBuffer>              m_buffer;
    Rc<MtlBuffer>              m_stagingBuffer;

    Rc<MtlAllocation>         m_allocation;
    Rc<MtlAllocation>         m_realAllocation;  // m_buffer's backing at demotion

    D3D9Range                   m_dirtyRange;

    uint32_t                    m_lockCount = 0;

    uint64_t                    m_seq = 0ull;

    bool                        m_trackUses = false;
    bool                        m_demotable = false;
    D3D9DirectBufferTracker     m_tracker;

  };

}
