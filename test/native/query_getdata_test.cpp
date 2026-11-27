// Native test: a D3DQUERYTYPE_TIMESTAMPFREQ query answers GetData with S_OK and the device's
// timestamp frequency on the first call. A caller may create one every frame, Issue(END) it and
// poll GetData(D3DGETDATA_FLUSH) with no sleep until S_OK or D3DERR_DEVICELOST; an answer that stays
// anything else spins its thread for good.
//
// It compiles driver/d3d9/d3d9_query.h and d3d9_query.cpp unchanged against the hostile fakes of
// query_fakes.h (GPU queries and events never complete, recorded query ends never run), so an
// answer that waits on the GPU or the CS thread fails here. scripts/test-query-getdata.sh
// sets it up and also runs it on copies with a planted fault, each of which must fail a check
// (exit status 3, which no sanitizer uses).
//
//   scripts/test-query-getdata.sh [d3d9_query.cpp [d3d9_query.h]]
#include "d3d9/d3d9_query.h"

#include <cstdio>
#include <memory>

using namespace dxvk;

static int g_fail = 0;
static int g_checks = 0;
#define CHECK(c) do { g_checks++; if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

// Not the 1 GHz of MtlDevice, so a hard-coded answer doesn't pass.
constexpr uint64_t kFrequency = 24000000ull;
constexpr uint32_t kMaxPolls  = 1000u;
constexpr uint8_t  kCanary    = 0xA5u;

struct Poll { HRESULT hr; uint32_t calls; };

// The caller's loop: GetData until S_OK or D3DERR_DEVICELOST, at most kMaxPolls calls.
static Poll poll(D3D9Query& q, void* data, DWORD size, DWORD flags) {
  for (uint32_t n = 1u; n <= kMaxPolls; n++) {
    const HRESULT hr = q.GetData(data, size, flags);
    if (hr == S_OK || hr == D3DERR_DEVICELOST)
      return { hr, n };
  }
  return { S_FALSE, kMaxPolls };
}

// 8 bytes of answer followed by 8 canary bytes that GetData must leave alone.
struct Answer {
  uint8_t bytes[16];
  Answer() { std::memset(bytes, kCanary, sizeof(bytes)); }
  uint64_t value() const { uint64_t v; std::memcpy(&v, bytes, sizeof(v)); return v; }
  bool tailIntact() const {
    for (size_t i = 8; i < sizeof(bytes); i++)
      if (bytes[i] != kCanary) return false;
    return true;
  }
};

static std::unique_ptr<D3D9Query> make(D3D9DeviceEx& dev) {
  return std::make_unique<D3D9Query>(&dev, D3DQUERYTYPE_TIMESTAMPFREQ);
}

static bool nothingRecorded(const FakeCounts& c) {
  return c.gpuQueries == 0u && c.gpuEvents == 0u && c.begins == 0u && c.ends == 0u
      && c.flushes == 0u && c.considerFlushes == 0u && c.syncs == 0u;
}

int main() {
  // CreateQuery accepts the type.
  {
    D3D9DeviceEx dev(kFrequency);
    CHECK(D3D9Query::QuerySupported(&dev, D3DQUERYTYPE_TIMESTAMPFREQ) == D3D_OK);
  }

  // A fresh query each frame: Issue(END), then poll with FLUSH. Every frame answers on the first call.
  {
    D3D9DeviceEx dev(kFrequency);
    uint32_t firstCall = 0u;
    for (uint32_t frame = 0u; frame < 1000u; frame++) {
      auto q = make(dev);
      CHECK(q->GetDataSize() == sizeof(UINT64));
      CHECK(q->Issue(D3DISSUE_END) == D3D_OK);
      Answer a;
      const Poll p = poll(*q, a.bytes, sizeof(UINT64), D3DGETDATA_FLUSH);
      firstCall += p.hr == S_OK && p.calls == 1u && a.value() == kFrequency && a.tailIntact();
    }
    CHECK(firstCall == 1000u);
    CHECK(nothingRecorded(dev.counts));
  }

  // The same answer without FLUSH, without a prior Issue, after Issue(BEGIN), and again from
  // the answered (cached) query.
  {
    D3D9DeviceEx dev(kFrequency);
    for (DWORD flags : { DWORD(0u), DWORD(D3DGETDATA_FLUSH) }) {
      auto noIssue = make(dev);
      Answer a;
      const Poll p = poll(*noIssue, a.bytes, sizeof(UINT64), flags);
      CHECK(p.hr == S_OK && p.calls == 1u && a.value() == kFrequency && a.tailIntact());

      Answer again;
      CHECK(noIssue->GetData(again.bytes, sizeof(UINT64), flags) == S_OK);
      CHECK(again.value() == kFrequency && again.tailIntact());

      auto begun = make(dev);
      CHECK(begun->Issue(D3DISSUE_BEGIN) == D3D_OK);
      CHECK(begun->Issue(D3DISSUE_END) == D3D_OK);
      Answer b;
      const Poll pb = poll(*begun, b.bytes, sizeof(UINT64), flags);
      CHECK(pb.hr == S_OK && pb.calls == 1u && b.value() == kFrequency);
    }
    // A readiness probe with no buffer.
    auto probe = make(dev);
    CHECK(probe->Issue(D3DISSUE_END) == D3D_OK);
    CHECK(probe->GetData(nullptr, 0u, D3DGETDATA_FLUSH) == S_OK);
    CHECK(nothingRecorded(dev.counts));
  }

  // A lost device ends a FLUSH poll at once, with D3DERR_DEVICELOST.
  {
    D3D9DeviceEx dev(kFrequency);
    dev.lost = true;
    auto q = make(dev);
    CHECK(q->Issue(D3DISSUE_END) == D3D_OK);
    Answer a;
    const Poll p = poll(*q, a.bytes, sizeof(UINT64), D3DGETDATA_FLUSH);
    CHECK(p.hr == D3DERR_DEVICELOST && p.calls == 1u);
  }

  std::printf("query_getdata_test: %d checks, %d failed\n", g_checks, g_fail);
  return g_fail ? 3 : 0;
}
