// Native unit test for pipeline states in the residency set (#63; driver/backend/mtl_residency.cpp,
// compiled as is against stubbed bridge calls that model the set: what is added, what is
// committed, and every object's reference count):
//  - a pipeline added by pipelineCreated is in the set and committed by the next beforeSubmit,
//    not before (Metal reads a set's committed state at commit), and counted in residentPsos();
//  - the tracker holds its own reference: the pipeline outlives the caller's;
//  - serialRetired never takes a pipeline out (d9mt never evicts one);
//  - at teardown the pipelines leave the set, that removal is committed, and only then are they
//    released, each exactly once (no leak, no early release);
//  - pipelines added from several threads while another one commits are all counted and resident;
//  - without a residency set (newResidencySet failed) nothing is retained.
// DXVK's native headers (log.h includes the COM ones), as scripts/test-pso-key-hash.sh:
//   N=<DXVK or dxmt>/include/native; clang++ -std=c++17 -I driver/backend -I $N/windows \
//     -I $N/directx -include test/native/stubs/virtual_free.h test/native/residency_pso_test.cpp \
//     driver/backend/mtl_residency.cpp -o residency_pso_test && ./residency_pso_test
#include "mtl_residency.h"

#include <atomic>
#include <cstdio>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "../util/log/log.h"

using namespace dxvk::backend;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

// ---- the model: one residency set, reference counts -------------------------------------

static constexpr obj_handle_t kSet = 0x5e7u;
static std::mutex g_model;
static bool g_setFails = false;
static std::set<obj_handle_t> g_staged;      // the set's contents as last changed
static std::set<obj_handle_t> g_committed;   // the contents the GPU sees (as of the last commit)
static std::map<obj_handle_t, int> g_refs;   // our references (1 = the creator's)
static uint64_t g_commits = 0u;
static std::vector<std::string> g_errors;

static void created(obj_handle_t h) { std::lock_guard l(g_model); g_refs[h] = 1; }

extern "C" obj_handle_t MTLDevice_newResidencySet(obj_handle_t, const WMTResidencySetInfo*) {
  return g_setFails ? 0u : kSet;
}
extern "C" void MTLResidencySet_requestResidency(obj_handle_t) { }
extern "C" void MTL4CommandQueue_addResidencySet(obj_handle_t, obj_handle_t) { }
extern "C" void MTL4CommandQueue_removeResidencySet(obj_handle_t, obj_handle_t) { }
extern "C" obj_handle_t MetalLayer_residencySet(obj_handle_t) { return 0u; }

extern "C" void MTLResidencySet_addAllocation(obj_handle_t set, obj_handle_t a) {
  std::lock_guard l(g_model);
  CHECK(set == kSet);
  CHECK(g_refs[a] > 0);                      // never a released object
  g_staged.insert(a);
}
extern "C" void MTLResidencySet_removeAllocation(obj_handle_t set, obj_handle_t a) {
  std::lock_guard l(g_model);
  CHECK(set == kSet);
  CHECK(g_staged.erase(a) == 1u);
}
extern "C" void MTLResidencySet_commit(obj_handle_t set) {
  std::lock_guard l(g_model);
  CHECK(set == kSet);
  g_committed = g_staged;
  g_commits++;
}
extern "C" void NSObject_retain(obj_handle_t h) {
  std::lock_guard l(g_model);
  CHECK(g_refs[h] > 0);
  g_refs[h]++;
}
extern "C" void NSObject_release(obj_handle_t h) {
  std::lock_guard l(g_model);
  CHECK(g_refs[h] > 0);
  // The last reference: Metal frees the object. It must not be in the set the GPU sees.
  if (--g_refs[h] == 0)
    CHECK(g_committed.count(h) == 0u);
}

extern "C" int VirtualFree(void*, size_t, unsigned) { return 1; }  // stubs/virtual_free.h

namespace dxvk {
  void Logger::err(const std::string& message) { std::lock_guard l(g_model); g_errors.push_back(message); }
}

namespace dxvk::backend::lifetime {
  const int g_mode = 0;
  void checkReleaseSlow(obj_handle_t, const char*) { }
}

static int refs(obj_handle_t h) { std::lock_guard l(g_model); return g_refs[h]; }
static bool staged(obj_handle_t h) { std::lock_guard l(g_model); return g_staged.count(h) != 0u; }
static bool committed(obj_handle_t h) { std::lock_guard l(g_model); return g_committed.count(h) != 0u; }

// ---- tests ------------------------------------------------------------------------------

static void testAddCommitTeardown() {
  const obj_handle_t psoA = 0xa001u, psoB = 0xa002u, buf = 0xb001u;
  created(psoA); created(psoB); created(buf);

  {
    MtlResidencyTracker tracker(0xde5u, 0x9u);

    tracker.pipelineCreated(psoA);
    CHECK(staged(psoA));
    CHECK(!committed(psoA));                 // only the next submit's commit makes it visible
    CHECK(refs(psoA) == 2);                  // the tracker's own reference
    CHECK(tracker.residentPsos() == 1u);
    CHECK(tracker.netResident() == 0);       // pipelines aren't counted as allocations

    tracker.beforeSubmit();
    CHECK(committed(psoA));

    tracker.pipelineCreated(0u);             // a failed build: nothing
    CHECK(tracker.residentPsos() == 1u);

    // The creator lets go (the PSO cache drops its entry, e.g. D9MT_NOPSOCACHE): still alive.
    NSObject_release(psoA);
    CHECK(refs(psoA) == 1);
    CHECK(committed(psoA));

    tracker.pipelineCreated(psoB);
    tracker.allocCreated(buf, 64u);
    tracker.beforeSubmit();
    CHECK(committed(psoB) && committed(buf));

    // Retiring every serial takes the buffer out, never a pipeline.
    tracker.allocDestroyed(buf, nullptr, 3u, 64u);
    tracker.serialRetired(10u);
    tracker.beforeSubmit();
    tracker.releaseCommitted();
    CHECK(!committed(buf));
    CHECK(refs(buf) == 0);
    CHECK(committed(psoA) && committed(psoB));
    CHECK(tracker.residentPsos() == 2u);
  }

  // Teardown: out of the set, committed, then released (NSObject_release checks the order).
  CHECK(!staged(psoA) && !staged(psoB));
  CHECK(!committed(psoA) && !committed(psoB));
  CHECK(refs(psoA) == 0);                    // creator's dropped above, tracker's at teardown
  CHECK(refs(psoB) == 1);                    // only the creator's is left
  NSObject_release(psoB);
}

static void testConcurrentAdds() {
  constexpr uint32_t kThreads = 4u, kPerThread = 500u;
  std::vector<obj_handle_t> psos;
  for (uint32_t i = 0u; i < kThreads * kPerThread; i++) {
    psos.push_back(0x100000u + i);
    created(psos.back());
  }

  {
    MtlResidencyTracker tracker(0xde5u, 0x9u);
    std::atomic<bool> stop = { false };
    std::thread submitter([&] { while (!stop.load()) tracker.beforeSubmit(); });
    std::vector<std::thread> workers;
    for (uint32_t t = 0u; t < kThreads; t++) {
      workers.emplace_back([&, t] {
        for (uint32_t i = 0u; i < kPerThread; i++)
          tracker.pipelineCreated(psos[t * kPerThread + i]);
      });
    }
    for (auto& w : workers)
      w.join();
    stop = true;
    submitter.join();

    tracker.beforeSubmit();
    CHECK(tracker.residentPsos() == kThreads * kPerThread);
    uint32_t inSet = 0u;
    for (obj_handle_t p : psos)
      inSet += committed(p) ? 1u : 0u;
    CHECK(inSet == kThreads * kPerThread);
  }

  for (obj_handle_t p : psos) {
    CHECK(!committed(p));
    CHECK(refs(p) == 1);
    NSObject_release(p);
  }
}

static void testNoSet() {
  const obj_handle_t pso = 0xc001u;
  created(pso);
  g_setFails = true;
  {
    MtlResidencyTracker tracker(0xde5u, 0x9u);
    tracker.pipelineCreated(pso);
    CHECK(refs(pso) == 1);
    CHECK(tracker.residentPsos() == 0u);
  }
  g_setFails = false;
  CHECK(!g_errors.empty());                  // "newResidencySet failed" was logged
  NSObject_release(pso);
}

int main() {
  testAddCommitTeardown();
  testConcurrentAdds();
  testNoSet();

  for (const auto& r : g_refs)
    CHECK(r.second == 0);                    // nothing leaked or over-released
  std::printf("%s (%llu commits)\n", g_fail ? "FAIL" : "ok", (unsigned long long)g_commits);
  return g_fail ? 1 : 0;
}
