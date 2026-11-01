// Native unit test: every pipeline state the PSO cache builds is in the queue's residency set,
// committed before the submit that can first use it (#63). Drives the production
// RenderPsoCache (driver/backend/mtl_pso_cache.cpp) and MtlResidencyTracker
// (driver/backend/mtl_residency.cpp), compiled as is, with a stub Metal 4 compiler and a model
// of the residency set behind the bridge calls. It only watches existing bridge calls
// (MTLResidencySet_addAllocation/commit), never the tracker's API, so the same file builds
// against the base commit, where it must fail: there, no PSO ever joins the set.
//  - draws (get): a successful build's handle is in the set the moment isDone() says so (a draw
//    encodes it only then), and committed by the next beforeSubmit (before the queue commit);
//  - internal pipelines (getSync): the same;
//  - the state cache prewarm (createPrewarmEntry): the same;
//  - a prewarm build a draw adopted that then failed (replaceFailedPrewarm): the replacement is;
//  - a failed build adds nothing.
// Run by scripts/test-residency.sh (DXVK's native headers; plain, ASan/UBSan and TSan).
#include "mtl_pso_cache.h"
#include "mtl_residency.h"

#include "../mtl/mtl_context.h"
#include "../mtl/mtl_state_cache.h"
#include "../util/log/log.h"

#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

using namespace dxvk;
using namespace dxvk::backend;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

// ---- the bridge: a stub compiler and a model of the residency set ------------------------

static constexpr obj_handle_t kSet = 0x5e7u, kCompiler = 0xc0u, kQueue = 0x9u;
static std::mutex g_model;
static std::set<obj_handle_t> g_staged, g_committed, g_built;
static std::map<obj_handle_t, int> g_refs;
static std::atomic<obj_handle_t> g_nextPso = { 0x10000u };
static std::set<uint64_t> g_failCookies;    // vsCookie values (vertex library handles) whose builds fail

extern "C" obj_handle_t MTLDevice_newResidencySet(obj_handle_t, const WMTResidencySetInfo*) {
  std::lock_guard l(g_model); g_refs[kSet] = 1; return kSet;
}
extern "C" void MTLResidencySet_requestResidency(obj_handle_t) { }
extern "C" void MTL4CommandQueue_addResidencySet(obj_handle_t, obj_handle_t) { }
extern "C" void MTL4CommandQueue_removeResidencySet(obj_handle_t, obj_handle_t) { }
extern "C" obj_handle_t MetalLayer_residencySet(obj_handle_t) { return 0u; }
extern "C" void MTLResidencySet_addAllocation(obj_handle_t, obj_handle_t a) { std::lock_guard l(g_model); g_staged.insert(a); }
extern "C" void MTLResidencySet_removeAllocation(obj_handle_t, obj_handle_t a) { std::lock_guard l(g_model); g_staged.erase(a); }
extern "C" void MTLResidencySet_commit(obj_handle_t) { std::lock_guard l(g_model); g_committed = g_staged; }
extern "C" void NSObject_retain(obj_handle_t h) { std::lock_guard l(g_model); g_refs[h]++; }
extern "C" void NSObject_release(obj_handle_t h) {
  std::lock_guard l(g_model);
  CHECK(g_refs[h] > 0);
  if (--g_refs[h] == 0)
    CHECK(g_committed.count(h) == 0u);       // freed while the GPU's view of the set holds it
}
extern "C" obj_handle_t NSObject_description(obj_handle_t) { return 0u; }
extern "C" uint32_t NSString_getCString(obj_handle_t, char*, uint64_t, enum WMTStringEncoding) { return 0u; }
extern "C" int VirtualFree(void*, size_t, unsigned) { return 1; }  // stubs/virtual_free.h

extern "C" obj_handle_t MTL4Compiler_newRenderPipelineState(obj_handle_t compiler, const WMTMtl4RenderPipelineInfo* info, obj_handle_t* err) {
  CHECK(compiler == kCompiler);
  *err = 0u;
  if (g_failCookies.count(uint64_t(info->vertex_library)))
    return 0u;
  const obj_handle_t h = g_nextPso++;
  std::lock_guard l(g_model);
  g_refs[h] = 1;
  g_built.insert(h);
  return h;
}

static bool staged(obj_handle_t h)    { std::lock_guard l(g_model); return g_staged.count(h) != 0u; }
static bool committed(obj_handle_t h) { std::lock_guard l(g_model); return g_committed.count(h) != 0u; }

// ---- the device: compile tasks run on a thread of their own, as on a worker --------------

// MtlDevice's base methods (driver/mtl/mtl_device_stub.cpp holds them, with WSI and config the
// test doesn't need): only what the PSO cache reaches is used.
namespace dxvk {
  MtlDevice::MtlDevice() { }
  MtlDevice::~MtlDevice() { }
  void MtlDevice::setStateCache(const Rc<MtlStateCache>& cache) {
    m_stateCache = cache;
    m_stateCachePtr.store(cache.ptr(), std::memory_order_release);
  }
  Rc<MtlBuffer> MtlDevice::createBuffer(const MtlBufferCreateInfo&) { return nullptr; }
  Rc<MtlTexture> MtlDevice::createImage(const MtlImageCreateInfo&) { return nullptr; }
  Rc<MtlTextureView> MtlDevice::createImageView(const Rc<MtlTexture>&, const MtlImageViewKey&) { return nullptr; }
  Rc<MtlSamplerState> MtlDevice::createSampler(const MtlSamplerKey&) { return nullptr; }
  Rc<MtlShaderFunction> MtlDevice::createShaderFunction(MtlShaderStage, const void*, size_t, const void*, size_t) { return nullptr; }
  void MtlDevice::submitCompile(const Rc<MtlCompileTask>&, bool) { }
  size_t MtlDevice::pendingCompiles() { return 0u; }
  Rc<MtlContext> MtlDevice::createContext() { return nullptr; }
  uint64_t MtlDevice::getCompletedSerial() { return 0u; }
  bool MtlDevice::isSerialComplete(uint64_t) { return true; }
  uint64_t MtlDevice::getNextSerial() { return 1u; }
  void MtlDevice::waitForIdle() { }
  bool MtlDevice::supportsFormat(MtlFormat, MtlUsage) const { return false; }
  const MtlDeviceInfo& MtlDevice::deviceInfo() const { static MtlDeviceInfo info = {}; return info; }
  Rc<MtlAllocation> MtlDevice::renameBuffer(MtlBuffer*) { return nullptr; }
  MtlStageMask MtlDevice::getShaderStages() const { return MtlStageMask(0); }
  void MtlDevice::waitForResource(const MtlResource&) { }
  Rc<MtlStagingBuffer> MtlDevice::createStagingBuffer(MtlDeviceSize) { return nullptr; }
  MtlStagingBufferStats MtlDevice::getStagingMemoryStatistics() { return {}; }
  Rc<MtlQuery> MtlDevice::createGpuQuery(MtlQueryType) { return nullptr; }
  Rc<MtlEvent> MtlDevice::createGpuEvent() { return nullptr; }
  uint64_t MtlDevice::getTimestampFrequency() const { return 1u; }
  uint64_t MtlDevice::allocatedBytes() { return 0u; }

  // Logging: kept, printed (the cache logs a failed build).
  void Logger::info(const std::string& m) { std::printf("info: %s\n", m.c_str()); }
  void Logger::warn(const std::string& m) { std::printf("warn: %s\n", m.c_str()); }
  void Logger::err(const std::string& m)  { std::printf("err: %s\n", m.c_str()); }

  namespace backend::lifetime {
    const int g_mode = 0;
    void checkReleaseSlow(obj_handle_t, const char*) { }
  }
}

struct TestDevice : public MtlDevice {
  std::vector<std::thread> threads;
  void submitCompile(const Rc<MtlCompileTask>& task, bool) override {
    Rc<MtlCompileTask> t = task;
    threads.emplace_back([t] { t->tryRun(); });
  }
  void join() { for (auto& t : threads) t.join(); threads.clear(); }
};

struct NullBackend : public MtlStateCacheBackend {
  Rc<MtlCompileTask> createPrewarmPso(const uint8_t*, const MtlShaderFunction*, const MtlShaderFunction*) override { return nullptr; }
  bool psoStateValid(const uint8_t*, const char**) override { return true; }
  bool psoSucceeded(MtlCompileTask*) override { return false; }
  uint64_t psoBuildNs(MtlCompileTask*) override { return 0u; }
  uint32_t urgentCompilesQueued() override { return 0u; }
};

// ---- the base commit's and this branch's signatures, both -------------------------------

template<typename C = RenderPsoCache>
static std::unique_ptr<C> makeCache(MtlDevice* dev, RenderPsoCache::Registry* reg, MtlResidencyTracker* res) {
  if constexpr (std::is_constructible_v<C, obj_handle_t, MtlDevice*, RenderPsoCache::Registry*, MtlResidencyTracker*>)
    return std::make_unique<C>(kCompiler, dev, reg, res);
  else
    return std::make_unique<C>(kCompiler, dev, reg);
}

template<typename C, typename = void>
struct PrewarmTakesResidency : std::false_type { };
template<typename C>
struct PrewarmTakesResidency<C, std::void_t<decltype(C::createPrewarmEntry(obj_handle_t(), (MtlResidencyTracker*)nullptr,
  std::declval<const RenderPsoKey&>(), obj_handle_t(), obj_handle_t(), "", ""))>> : std::true_type { };

template<typename C = RenderPsoCache>
static Rc<RenderPsoCache::Entry> makePrewarm(MtlResidencyTracker* res, const RenderPsoKey& key) {
  const obj_handle_t vs = obj_handle_t(key.vsCookie), fs = obj_handle_t(key.fsCookie);
  if constexpr (PrewarmTakesResidency<C>::value)
    return C::createPrewarmEntry(kCompiler, res, key, vs, fs, "main", "main");
  else
    return C::createPrewarmEntry(kCompiler, key, vs, fs, "main", "main");
}

static RenderPsoKey keyFor(uint64_t vsLibrary) {
  RenderPsoKey k = {};
  k.vsCookie = vsLibrary;
  k.fsCookie = vsLibrary + 1u;
  return k;
}

// The handle a draw would encode: resident as soon as the build is done, committed by the
// submit that encodes it.
static void checkResidentOnceDone(const char* what, RenderPsoCache::Entry* e, MtlResidencyTracker& res) {
  while (!e->isDone())
    std::this_thread::yield();
  const obj_handle_t h = e->handle();
  CHECK(h != 0u);
  const bool inSet = staged(h);
  if (!inSet)
    std::printf("FAIL %s: PSO %llx built but not in the residency set\n", what, (unsigned long long)h);
  CHECK(inSet);
  res.beforeSubmit();
  CHECK(committed(h));
}

int main() {
  const std::string dir = [] { char buf[] = "/tmp/d9mt-pso-res.XXXXXX"; const char* d = mkdtemp(buf); return std::string(d ? d : "/tmp"); }();
  {
    TestDevice dev;
    NullBackend backend;
    MtlStateCacheConfig cfg;
    cfg.file = dir + "/test.d9mtstate";
    cfg.displayPath = cfg.file;
    cfg.keySignature = RenderPsoKey::signature();
    cfg.threads = 0u;
    Rc<MtlStateCache> stateCache = new MtlStateCache(&backend, cfg);

    MtlResidencyTracker res(0xde5u, kQueue);
    RenderPsoCache::Registry registry;
    auto cache = makeCache(&dev, &registry, &res);

    // A draw's miss.
    RenderPsoCache::Entry* draw = cache->get(keyFor(0x100u), 0x100u, 0x101u);
    checkResidentOnceDone("draw", draw, res);
    dev.join();

    // An internal pipeline (blit, fill, depth clear).
    const obj_handle_t internal = cache->getSync(keyFor(0x200u), 0x200u, 0x201u, "blitVsMain", "blitFsMain");
    CHECK(internal != 0u && staged(internal));
    res.beforeSubmit();
    CHECK(committed(internal));
    dev.join();

    // A failed build: nothing joins the set.
    g_failCookies.insert(0x300u);
    const size_t before = [] { std::lock_guard l(g_model); return g_staged.size(); }();
    CHECK(cache->getSync(keyFor(0x300u), 0x300u, 0x301u) == 0u);
    CHECK([] { std::lock_guard l(g_model); return g_staged.size(); }() == before);
    dev.join();

    // The state cache prewarm.
    Rc<RenderPsoCache::Entry> warm = makePrewarm(&res, keyFor(0x400u));
    dev.submitCompile(warm, false);
    checkResidentOnceDone("prewarm", warm.ptr(), res);
    dev.join();

    // A prewarm build a draw adopts while it is still pending, and which then fails: the draw's
    // replacement build is resident.
    dev.setStateCache(stateCache);
    g_failCookies.insert(0x500u);
    Rc<RenderPsoCache::Entry> doomed = makePrewarm(&res, keyFor(0x500u));
    CHECK(registry.insertIfAbsent(doomed));
    RenderPsoCache::Entry* adopted = cache->get(keyFor(0x500u), 0x500u, 0x501u);
    CHECK(adopted == doomed.ptr());
    dev.join();
    CHECK(adopted->isDone() && adopted->handle() == 0u);
    g_failCookies.erase(0x500u);
    RenderPsoCache::Entry* fresh = cache->replaceFailedPrewarm(adopted);
    CHECK(fresh != nullptr && fresh != adopted);
    if (fresh)
      checkResidentOnceDone("failed-prewarm replacement", fresh, res);
    dev.join();
    dev.setStateCache(nullptr);
    stateCache->shutdown();

    std::lock_guard l(g_model);
    for (obj_handle_t h : g_built)
      CHECK(g_staged.count(h) == 1u);        // every PSO built is in the set
  }
  unlink((dir + "/test.d9mtstate").c_str());
  rmdir(dir.c_str());
  std::printf("%s (%zu PSOs built)\n", g_fail ? "FAIL" : "ok", g_built.size());
  return g_fail ? 1 : 0;
}
