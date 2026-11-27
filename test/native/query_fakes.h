// Fakes for test/native/query_getdata_test.cpp: what driver/d3d9/d3d9_query.h and d3d9_query.cpp
// use of the Windows headers, the device and the Metal layer, so that both files compile
// unchanged. scripts/test-query-getdata.sh copies them next to forwarding headers that include
// this one in place of d3d9_device_child.h, d3d9_device.h and mtl/mtl_context.h.
//
// The fakes are hostile on purpose: every GPU query and event they create stays pending, every
// query end the device records stays unexecuted (the real device leaves it to the CS thread), and a
// carried flush is always open. A query whose answer depends on any of them never reaches S_OK.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <ostream>
#include <sstream>
#include <string>

#define likely(x)   __builtin_expect(bool(x), 1)
#define unlikely(x) __builtin_expect(bool(x), 0)
#define STDMETHODCALLTYPE

typedef uint32_t DWORD;
typedef int32_t  BOOL;
typedef uint64_t UINT64;
typedef int32_t  HRESULT;

#define S_OK                ((HRESULT)0)
#define S_FALSE             ((HRESULT)1)
#define D3D_OK              S_OK
#define E_NOINTERFACE       ((HRESULT)0x80004002u)
#define E_POINTER           ((HRESULT)0x80004003u)
#define D3DERR_DEVICELOST   ((HRESULT)0x88760868u)
#define D3DERR_NOTAVAILABLE ((HRESULT)0x8876086Au)
#define D3DERR_INVALIDCALL  ((HRESULT)0x8876086Cu)

#define D3DISSUE_END     (1u << 0)
#define D3DISSUE_BEGIN   (1u << 1)
#define D3DGETDATA_FLUSH (1u << 0)

#define MAKEFOURCC(a, b, c, d) \
  ((DWORD)(uint8_t)(a) | ((DWORD)(uint8_t)(b) << 8) | ((DWORD)(uint8_t)(c) << 16) | ((DWORD)(uint8_t)(d) << 24))

enum D3DQUERYTYPE {
  D3DQUERYTYPE_VCACHE            = 4,
  D3DQUERYTYPE_RESOURCEMANAGER   = 5,
  D3DQUERYTYPE_VERTEXSTATS       = 6,
  D3DQUERYTYPE_EVENT             = 8,
  D3DQUERYTYPE_OCCLUSION         = 9,
  D3DQUERYTYPE_TIMESTAMP         = 10,
  D3DQUERYTYPE_TIMESTAMPDISJOINT = 11,
  D3DQUERYTYPE_TIMESTAMPFREQ     = 12,
  D3DQUERYTYPE_PIPELINETIMINGS   = 13,
  D3DQUERYTYPE_INTERFACETIMINGS  = 14,
  D3DQUERYTYPE_VERTEXTIMINGS     = 15,
  D3DQUERYTYPE_PIXELTIMINGS      = 16,
  D3DQUERYTYPE_BANDWIDTHTIMINGS  = 17,
  D3DQUERYTYPE_CACHEUTILIZATION  = 18,
};

struct D3DDEVINFO_VCACHE { DWORD Pattern, OptMethod, CacheSize, MagicNumber; };
struct D3DDEVINFO_RESOURCEMANAGER { DWORD stats[8][8]; };
struct D3DDEVINFO_D3DVERTEXSTATS { DWORD NumRenderedTriangles, NumExtraClippingTriangles; };
struct D3DDEVINFO_D3D9PIPELINETIMINGS { float v[4]; };
struct D3DDEVINFO_D3D9INTERFACETIMINGS { float v[5]; };
struct D3DDEVINFO_D3D9STAGETIMINGS { float v[2]; };
struct D3DDEVINFO_D3D9BANDWIDTHTIMINGS { float v[5]; };
struct D3DDEVINFO_D3D9CACHEUTILIZATION { float v[2]; };

struct GUID {
  uint32_t id;
  bool operator==(const GUID& o) const { return id == o.id; }
};
typedef const GUID& REFIID;
inline std::ostream& operator<<(std::ostream& s, const GUID& g) { return s << "guid " << g.id; }
inline const GUID fake_uuid_IUnknown        = { 1u };
inline const GUID fake_uuid_IDirect3DQuery9 = { 2u };
#define __uuidof(T) fake_uuid_##T

struct IUnknown {
  virtual ~IUnknown() = default;
  virtual HRESULT QueryInterface(REFIID riid, void** ppvObject) = 0;
};

struct IDirect3DQuery9 : IUnknown {
  virtual D3DQUERYTYPE GetType() = 0;
  virtual DWORD GetDataSize() = 0;
  virtual HRESULT Issue(DWORD dwIssueFlags) = 0;
  virtual HRESULT GetData(void* pData, DWORD dwSize, DWORD dwGetDataFlags) = 0;
};

namespace dxvk {

  template <typename T> using Rc = std::shared_ptr<T>;
  template <typename T> T* ref(T* object) { return object; }

  struct DxvkError {
    explicit DxvkError(std::string m) : msg(std::move(m)) { }
    std::string msg;
  };

  namespace str {
    template <typename... Args> std::string format(const Args&... args) {
      std::ostringstream s;
      (s << ... << args);
      return s.str();
    }
  }

  struct Logger { static void warn(const std::string&) { } };
  inline bool logQueryInterfaceError(REFIID, REFIID) { return false; }

  namespace bit { inline uint32_t popcnt(uint32_t v) { return uint32_t(__builtin_popcount(v)); } }
  namespace hitch {
    enum Kind { Query };
    struct Profiler { };
    struct Scope { Scope(Profiler&, Kind) { } };
  }
  namespace d9mtdbg { constexpr uint32_t OccVisible = 1u << 0, OccSync = 1u << 1; }
  namespace stats { enum class FlushOrigin { Query }; }
  enum class GpuFlushType { ImplicitSynchronization };

  enum class MtlQueryType : uint32_t { Occlusion, Timestamp };
  enum class MtlGpuQueryStatus : uint32_t { Invalid, Pending, Available, Failed };
  enum class MtlGpuEventStatus : uint32_t { Invalid, Pending, Signaled };

  union MtlQueryData {
    struct { uint64_t samplesPassed; } occlusion;
    struct { uint64_t time; }          timestamp;
  };

  // Everything the device does on a query's behalf, counted.
  struct FakeCounts {
    uint32_t gpuQueries = 0, gpuEvents = 0, begins = 0, ends = 0;
    uint32_t flushes = 0, considerFlushes = 0, syncs = 0;
  };

  // A GPU query or event whose submission never completes.
  struct MtlQuery {
    MtlQueryType type = MtlQueryType::Occlusion;
    MtlGpuQueryStatus getData(MtlQueryData& out) { out = MtlQueryData{}; return MtlGpuQueryStatus::Pending; }
    void waitData() { }
    bool unsubmitted() const { return true; }
  };

  struct MtlEvent {
    MtlGpuEventStatus test() { return MtlGpuEventStatus::Pending; }
    bool unsubmitted() const { return true; }
  };

  struct MtlContext {
    void beginQuery(const Rc<MtlQuery>&) { }
    void endQuery(const Rc<MtlQuery>&) { }
    void writeTimestamp(const Rc<MtlQuery>&) { }
    void signalGpuEvent(const Rc<MtlEvent>&) { }
  };

  struct MtlDevice {
    FakeCounts* counts    = nullptr;
    uint64_t    frequency = 0u;

    Rc<MtlQuery> createGpuQuery(MtlQueryType type) {
      counts->gpuQueries++;
      Rc<MtlQuery> q = std::make_shared<MtlQuery>();
      q->type = type;
      return q;
    }
    Rc<MtlEvent> createGpuEvent() { counts->gpuEvents++; return std::make_shared<MtlEvent>(); }
    uint64_t getTimestampFrequency() const { return frequency; }
  };

  class D3D9Query;
  struct D3D9DeviceLock { ~D3D9DeviceLock() { } };  // not trivial, like the real lock: no unused-variable warning

  class D3D9DeviceEx {
  public:
    explicit D3D9DeviceEx(uint64_t frequency) : m_device(std::make_shared<MtlDevice>()) {
      m_device->counts    = &counts;
      m_device->frequency = frequency;
    }

    FakeCounts counts;
    bool       lost = false;

    static uint32_t DebugNewQueryId() { return 0u; }
    Rc<MtlDevice> GetDXVKDevice() const { return m_device; }
    D3D9DeviceLock LockDevice() { return { }; }
    hitch::Profiler& Hitch() { return m_hitch; }
    bool IsDeviceLost() const { return lost; }
    bool SupportsVCacheQuery() const { return false; }

    uint32_t DebugFlags() const { return 0u; }
    bool DebugHooks() const { return false; }
    uint64_t DebugFrame() const { return 0u; }
    void DebugOnQueryIssue(uint32_t, bool, bool, uint32_t) { }
    void DebugOnQueryData(uint32_t, HRESULT, DWORD, uint32_t, uint64_t, bool, const char*) { }
    void DebugSyncQuery() { counts.syncs++; }

    bool CarryOpen() const { return true; }
    void Flush(stats::FlushOrigin) { counts.flushes++; }
    void ConsiderFlush(GpuFlushType, stats::FlushOrigin) { counts.considerFlushes++; }

    // The real device records the query's Begin and End for the CS thread (EmitCs), which runs
    // them later; here they never run.
    void Begin(D3D9Query*) { counts.begins++; }
    void End(D3D9Query*) { counts.ends++; }

  private:
    Rc<MtlDevice>   m_device;
    hitch::Profiler m_hitch;
  };

  template <typename Base>
  class D3D9DeviceChild : public Base {
  public:
    D3D9DeviceChild(D3D9DeviceEx* pDevice) : m_parent(pDevice) { }
  protected:
    D3D9DeviceEx* m_parent;
  };

}
