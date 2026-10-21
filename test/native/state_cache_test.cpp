// Native (macOS, no Wine or GPU) tests of the pipeline state cache
// (driver/mtl/mtl_state_cache_core.* and mtl_state_cache.*, docs/state-cache.md):
//
//  - file format round trip, corrupt entries/header, truncation, garbage;
//  - format version older/newer, key signature change;
//  - the 4096-entry cap (file and database), merge and use() counters;
//  - the prewarm index: readiness on shader announcement, priority order,
//    stale entries, links only against D3D9 vertex shaders;
//  - the record ring (single producer/consumer, never allocates);
//  - the whole cache with a fake backend: record -> write -> load ->
//    announce -> prewarm -> registry hits, a corrupt file, a newer file,
//    concurrent announce/record/prewarm (run under ThreadSanitizer).
//
// Build and run (scripts/test-state-cache.sh does both, plain and TSan):
//   clang++ -std=c++17 -g -O1 [-fsanitize=thread] -Idriver -I<dxvk>/include/native/windows \
//     -I<dxvk>/include/native/directx test/native/state_cache_test.cpp \
//     driver/mtl/mtl_state_cache_core.cpp driver/mtl/mtl_state_cache.cpp -o /tmp/sct

#include <atomic>
#include <unordered_set>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include "mtl/mtl_state_cache.h"
#include "mtl/mtl_context.h"
#include "backend/mtl_pso_key.h"

namespace dxvk {
  void Logger::trace(const std::string&) { }
  void Logger::debug(const std::string&) { }
  void Logger::info (const std::string& m) { if (std::getenv("SCT_VERBOSE")) std::fprintf(stderr, "info: %s\n", m.c_str()); }
  void Logger::warn (const std::string& m) { std::fprintf(stderr, "warn: %s\n", m.c_str()); }
  void Logger::err  (const std::string& m) { std::fprintf(stderr, "err: %s\n",  m.c_str()); }
}

using namespace dxvk;
using namespace dxvk::statecache;

static int g_failures = 0;

#define CHECK(cond) do { if (!(cond)) { g_failures++; \
  std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

constexpr uint64_t Sig = 0x1234567890abcdefull;

static Entry makeEntry(uint32_t id, uint8_t vsOrigin = 1u, uint8_t psOrigin = 1u, uint8_t linked = 1u) {
  Entry e;
  e.vsOrigin = vsOrigin;
  e.psOrigin = psOrigin;
  e.psLinked = linked;
  e.lastSession = 1u;
  e.sessionsUsed = 1u;
  e.firstUseMs = id;
  std::memcpy(e.vsSha, &id, sizeof(id));
  const uint32_t ps = id ^ 0x5a5a5a5au;
  std::memcpy(e.psSha, &ps, sizeof(ps));
  for (uint32_t i = 0u; i < KeyBytes; i++)
    e.key[i] = uint8_t(id * 31u + i);
  e.key[0] = uint8_t((id * 31u) % 200u);   // never the fake backend's "invalid" byte (0xee)
  return e;
}

static void testFormat() {
  std::vector<Entry> in;
  for (uint32_t i = 0u; i < 100u; i++)
    in.push_back(makeEntry(i));

  std::vector<uint8_t> file = serialize(in, Sig, 7u);
  CHECK(file.size() == HeaderSize + 100u * EntrySize);

  std::vector<Entry> out;
  ParseResult r = parse(file.data(), file.size(), Sig, out);
  CHECK(r.status == ParseStatus::Ok);
  CHECK(r.session == 7u && r.corrupt == 0u && r.truncated == 0u);
  CHECK(out.size() == 100u);
  for (size_t i = 0u; i < out.size() && i < in.size(); i++)
    CHECK(!std::memcmp(&out[i], &in[i], sizeof(Entry)));

  // Empty file / no file.
  r = parse(nullptr, 0u, Sig, out);
  CHECK(r.status == ParseStatus::Empty && out.empty());
}

static void testCorruption() {
  std::vector<Entry> in;
  for (uint32_t i = 0u; i < 10u; i++)
    in.push_back(makeEntry(i));
  const std::vector<uint8_t> good = serialize(in, Sig, 3u);
  std::vector<Entry> out;

  // One flipped byte inside entry 4's key: only that entry is lost.
  std::vector<uint8_t> f = good;
  f[HeaderSize + 4u * EntrySize + 100u] ^= 0x40u;
  ParseResult r = parse(f.data(), f.size(), Sig, out);
  CHECK(r.status == ParseStatus::Ok && r.corrupt == 1u && out.size() == 9u);

  // A flipped check word.
  f = good;
  f[HeaderSize + 7u * EntrySize] ^= 0x01u;
  r = parse(f.data(), f.size(), Sig, out);
  CHECK(r.status == ParseStatus::Ok && r.corrupt == 1u && out.size() == 9u);

  // Structurally invalid entry with a valid check (unknown origin).
  std::vector<Entry> bad = in;
  bad[2].vsOrigin = 9u;
  f = serialize(bad, Sig, 3u);
  r = parse(f.data(), f.size(), Sig, out);
  CHECK(r.status == ParseStatus::Ok && r.corrupt == 1u && out.size() == 9u);

  // Linked PS behind a fixed-function VS is invalid.
  bad = in;
  bad[3].vsOrigin = uint8_t(Origin::FixedFunction);
  f = serialize(bad, Sig, 3u);
  r = parse(f.data(), f.size(), Sig, out);
  CHECK(r.corrupt == 1u && out.size() == 9u);

  // Truncated in the middle of entry 6: entries 0..5 survive.
  f = good;
  f.resize(HeaderSize + 6u * EntrySize + 100u);
  r = parse(f.data(), f.size(), Sig, out);
  CHECK(r.status == ParseStatus::Ok && r.truncated == 1u && out.size() == 6u);

  // Truncated exactly at an entry boundary (fewer entries than the header says).
  f = good;
  f.resize(HeaderSize + 4u * EntrySize);
  r = parse(f.data(), f.size(), Sig, out);
  CHECK(r.status == ParseStatus::Ok && r.truncated == 1u && out.size() == 4u);

  // Header damage: entry count changed without fixing the check.
  f = good;
  f[20] ^= 0x01u;
  r = parse(f.data(), f.size(), Sig, out);
  CHECK(r.status == ParseStatus::BadHeader && out.empty());

  // Truncated header, wrong magic, random garbage.
  f = good;
  f.resize(40u);
  r = parse(f.data(), f.size(), Sig, out);
  CHECK(r.status == ParseStatus::BadHeader);

  f = good;
  f[0] = 'X';
  r = parse(f.data(), f.size(), Sig, out);
  CHECK(r.status == ParseStatus::BadHeader);

  std::mt19937 rng(42u);
  for (int i = 0; i < 200; i++) {
    std::vector<uint8_t> g(size_t(rng() % 3000u));
    for (auto& b : g)
      b = uint8_t(rng());
    r = parse(g.data(), g.size(), Sig, out);
    CHECK(r.status != ParseStatus::Ok || out.empty());
  }

  // Random single-byte damage anywhere never yields a damaged entry.
  for (int i = 0; i < 2000; i++) {
    f = good;
    const size_t at = rng() % f.size();
    f[at] ^= uint8_t(1u + rng() % 255u);
    r = parse(f.data(), f.size(), Sig, out);
    for (const Entry& e : out) {
      bool known = false;
      for (const Entry& g : in)
        known |= !std::memcmp(&e, &g, sizeof(Entry));
      CHECK(known);
    }
  }
}

static void testVersions() {
  std::vector<Entry> in = { makeEntry(1u), makeEntry(2u) };
  std::vector<uint8_t> f = serialize(in, Sig, 5u);
  std::vector<Entry> out;

  // Different key layout/mapping: entries dropped, session kept.
  ParseResult r = parse(f.data(), f.size(), Sig + 1u, out);
  CHECK(r.status == ParseStatus::KeyMismatch && out.empty() && r.session == 5u);

  // Newer format: neither read nor (by the cache) overwritten.
  std::vector<uint8_t> g = f;
  const uint32_t newer = FormatVersion + 1u;
  std::memcpy(g.data() + 8u, &newer, 4u);
  r = parse(g.data(), g.size(), Sig, out);
  CHECK(r.status == ParseStatus::NewerVersion && out.empty());

  // A newer format may change the header entirely: still recognized.
  g.resize(12u);
  r = parse(g.data(), g.size(), Sig, out);
  CHECK(r.status == ParseStatus::NewerVersion);

  // Older format: dropped (no converter for version 0).
  g = f;
  const uint32_t older = FormatVersion - 1u;
  std::memcpy(g.data() + 8u, &older, 4u);
  r = parse(g.data(), g.size(), Sig, out);
  CHECK(r.status == ParseStatus::OlderVersion && out.empty());
}

static void testDatabase() {
  Database db;
  Entry a = makeEntry(10u);
  a.firstUseMs = 500u;

  CHECK(db.use(a, 4u));           // new: session 4, used once
  CHECK(db.entries()[0].lastSession == 4u && db.entries()[0].sessionsUsed == 1u);

  Entry a2 = a;
  a2.firstUseMs = 300u;
  CHECK(!db.use(a2, 4u));         // same session: counted once, earlier first use kept
  CHECK(db.entries()[0].sessionsUsed == 1u && db.entries()[0].firstUseMs == 300u);

  a2.firstUseMs = 900u;
  CHECK(!db.use(a2, 5u));
  CHECK(db.entries()[0].sessionsUsed == 2u && db.entries()[0].lastSession == 5u
     && db.entries()[0].firstUseMs == 300u);

  // Counters are not identity: merge keeps one entry with max/min.
  Entry m = a;
  m.lastSession = 9u;
  m.sessionsUsed = 1u;
  m.firstUseMs = 100u;
  CHECK(!db.merge(m));
  CHECK(db.size() == 1u && db.entries()[0].lastSession == 9u
     && db.entries()[0].sessionsUsed == 2u && db.entries()[0].firstUseMs == 100u);

  // A different key byte, link flag or shader is a different entry.
  Entry b = a;
  b.key[KeyBytes - 1u] ^= 1u;
  CHECK(db.merge(b));
  Entry c = a;
  c.psLinked = 0u;
  CHECK(db.merge(c));
  Entry d = a;
  d.psSha[19] ^= 1u;
  CHECK(db.merge(d));
  CHECK(db.size() == 4u);

  CHECK(db.erase(c) && db.size() == 3u && !db.erase(c));

  // A failure doesn't erase what a draw used in this session.
  Entry u = makeEntry(99u);
  CHECK(db.use(u, 12u));
  CHECK(!db.eraseUnlessUsedIn(u, 12u) && db.size() == 4u);
  CHECK(db.eraseUnlessUsedIn(u, 13u) && db.size() == 3u);
  CHECK(!db.merge(b) && db.size() == 3u);   // index still right after erase
}

static void testCap() {
  // The database cap is generic; the cache passes its configured maximum.
  const uint32_t Cap = 4096u;
  Database db;
  for (uint32_t i = 0u; i < Cap + 1000u; i++) {
    Entry e = makeEntry(i);
    e.lastSession  = 1u + i / 100u;   // later entries are newer
    e.sessionsUsed = 1u + (i % 7u);
    db.merge(e);
  }

  CHECK(db.applyCap(Cap) == 1000u);
  CHECK(db.size() == Cap);

  uint32_t minSession = ~0u;
  for (const Entry& e : db.entries())
    minSession = std::min(minSession, e.lastSession);
  // The 1000 oldest (sessions 1..10) went; session 11 is partly kept by sessionsUsed.
  CHECK(minSession >= 10u);

  // Lookups still work after the cap rebuilt the index.
  Entry newest = makeEntry(Cap + 999u);
  newest.lastSession = 1u + (Cap + 999u) / 100u;
  CHECK(!db.merge(newest));
  CHECK(db.size() == Cap);
  CHECK(db.applyCap(Cap) == 0u);
}

static ShaderRef ref(uint8_t stage, uint8_t origin, uint32_t id) {
  ShaderRef r;
  r.stage = stage;
  r.origin = origin;
  std::memcpy(r.sha, &id, sizeof(id));
  return r;
}

static void testIndex() {
  // e0: D3D9 VS 1 + linked D3D9 PS 2, first used at 900 ms
  // e1: D3D9 VS 1 + FF PS, first used at 100 ms
  // e2: FF VS + standalone D3D9 PS 2, first used at 500 ms
  // e3: D3D9 VS 3 only (no PS)
  // e4: stale (last used 40 sessions ago)
  auto mk = [] (uint8_t vo, uint32_t vid, uint8_t po, uint32_t pid, uint8_t linked, uint32_t ms) {
    Entry e = makeEntry(ms, vo, po, linked);
    std::memset(e.vsSha, 0, 20);
    std::memset(e.psSha, 0, 20);
    if (vo == 1u) std::memcpy(e.vsSha, &vid, 4);
    if (po == 1u) std::memcpy(e.psSha, &pid, 4);
    e.lastSession = 50u;
    return e;
  };

  std::vector<Entry> entries = {
    mk(1, 1, 1, 2, 1, 900u),
    mk(1, 1, 2, 0, 0, 100u),
    mk(2, 0, 1, 2, 0, 500u),
    mk(1, 3, 0, 0, 0, 50u),
    mk(1, 1, 1, 2, 1, 10u),
  };
  entries[4].key[0] ^= 0xffu;
  entries[4].lastSession = 10u;

  PrewarmIndex idx;
  CHECK(idx.announce(ref(0, 2, 0)) == 0u);   // FF VS announced before the index exists
  idx.build(entries, 51u);
  CHECK(idx.indexedCount() == 4u);           // the stale one is not prewarmed
  CHECK(idx.readyCount() == 0u);

  CHECK(idx.announce(ref(1, 1, 2)) == 1u);   // PS 2: e2 (FF VS + PS 2) is ready
  CHECK(idx.announce(ref(1, 1, 2)) == 0u);   // announced twice: nothing new
  CHECK(idx.announce(ref(1, 1, 99)) == 0u);  // unknown shader
  CHECK(idx.announce(ref(0, 1, 1)) == 1u);   // VS 1: e0 (VS 1 + PS 2) ready; e1 still needs the FF PS
  CHECK(idx.announce(ref(1, 2, 0)) == 1u);   // FF PS: e1
  CHECK(idx.announce(ref(0, 1, 3)) == 1u);   // VS 3: e3

  // Out in first-use order: e3 (50), e1 (100), e2 (500), e0 (900).
  Entry e;
  const uint32_t order[] = { 50u, 100u, 500u, 900u };
  for (uint32_t ms : order) {
    CHECK(idx.pop(e));
    CHECK(e.firstUseMs == ms);
  }
  CHECK(!idx.pop(e));

  // Rebuilding keeps what was announced: everything is ready at once.
  idx.build(entries, 51u);
  CHECK(idx.readyCount() == 4u);
}

static void testRing() {
  struct Item {
    std::shared_ptr<int> p;
    uint64_t seq = 0u;
  };

  // Full ring: push fails, nothing stored.
  {
    SpscRing<Item, 8u> ring;
    for (int i = 0; i < 8; i++)
      CHECK(ring.push({ std::make_shared<int>(i), uint64_t(i) }));
    CHECK(!ring.push({ nullptr, 99u }));
    Item it;
    for (uint64_t i = 0u; i < 8u; i++) {
      CHECK(ring.pop(it) && it.seq == i && it.p && *it.p == int(i));
    }
    CHECK(!ring.pop(it));
  }

  // Concurrent: every pushed item arrives once, in order; references are
  // released (use_count back to 1 at the end).
  auto shared = std::make_shared<int>(7);
  SpscRing<Item, 64u> ring;
  constexpr uint64_t N = 200000u;
  uint64_t dropped = 0u;
  std::atomic<bool> done = { false };

  std::thread producer([&] {
    for (uint64_t i = 0u; i < N; i++) {
      while (!ring.push({ shared, i }))
        std::this_thread::yield();
    }
    done = true;
  });

  uint64_t next = 0u;
  Item it;
  while (next < N) {
    if (!ring.pop(it)) {
      std::this_thread::yield();
      continue;
    }
    if (it.seq != next)
      dropped++;
    next = it.seq + 1u;
    it = Item();
  }
  producer.join();
  CHECK(!done.load() || !ring.pop(it));
  CHECK(dropped == 0u && next == N);
  CHECK(shared.use_count() == 1);
}

// ---------------------------------------------------------------------------
// The whole cache with a fake backend and fake shaders.

struct FakePso : public MtlCompileTask {
  bool     fail = false;
  bool     ok = false;
  bool     prewarmed = false;
  uint64_t ns = 0u;
  // Optional: the build blocks until *gate is true, and records whether
  // this file existed meanwhile (the crash-loop guard's marker).
  std::atomic<bool>* gate = nullptr;
  std::atomic<bool>* started = nullptr;
  std::string        watchFile;
  std::atomic<bool>  sawFile = { false };
  void run() override {
    if (started)
      started->store(true);
    while (gate && !gate->load())
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (!watchFile.empty())
      sawFile = access(watchFile.c_str(), F_OK) == 0;
    std::this_thread::sleep_for(std::chrono::microseconds(300));
    ok = !fail;
    ns = 300000u;
  }
};

struct FakeBackend : public MtlStateCacheBackend {
  std::mutex mutex;
  std::map<std::vector<uint8_t>, Rc<FakePso>> registry;
  std::atomic<uint32_t> urgent = { 0u };
  std::atomic<uint32_t> created = { 0u };
  uint8_t failKeyByte = 0xffu;     // key[0] value whose builds fail
  uint8_t invalidKeyByte = 0xeeu;  // key[0] value psoStateValid() rejects
  std::atomic<bool>* gate = nullptr;     // passed to prewarm builds
  std::atomic<bool>* started = nullptr;
  std::string watchFile;
  Rc<FakePso> lastPrewarm;

  static std::vector<uint8_t> keyOf(const uint8_t* state, const MtlShaderFunction* vs, const MtlShaderFunction* fs) {
    std::vector<uint8_t> k(state, state + KeyBytes);
    const uint64_t c[2] = { uint64_t(vs->library), fs ? uint64_t(fs->library) : 0u };
    k.insert(k.end(), reinterpret_cast<const uint8_t*>(c), reinterpret_cast<const uint8_t*>(c) + sizeof(c));
    return k;
  }

  // What RenderPsoCache::get + Registry::adoptOrInsert do on a miss,
  // minus Metal: a failed prewarm build is replaced by a fresh one.
  Rc<FakePso> renderMiss(const uint8_t* state, const MtlShaderFunction* vs, const MtlShaderFunction* fs, MtlPsoUse& use) {
    Rc<FakePso> fresh = new FakePso();
    std::lock_guard lock(mutex);
    auto r = registry.emplace(keyOf(state, vs, fs), fresh);
    Rc<FakePso>& e = r.first->second;
    if (!r.second && e->prewarmed && e->isDone() && !e->ok)
      e = fresh;
    use = !e->prewarmed ? MtlPsoUse::Miss : (e->isDone() ? MtlPsoUse::Hit : MtlPsoUse::Late);
    return e;
  }

  Rc<MtlCompileTask> createPrewarmPso(const uint8_t* state, const MtlShaderFunction* vs, const MtlShaderFunction* fs) override {
    if (!psoStateValid(state, nullptr))
      return nullptr;
    Rc<FakePso> e = new FakePso();
    e->fail = state[0] == failKeyByte;
    e->prewarmed = true;
    e->gate = gate;
    e->started = started;
    e->watchFile = watchFile;
    std::lock_guard lock(mutex);
    if (!registry.emplace(keyOf(state, vs, fs), e).second)
      return nullptr;
    created++;
    lastPrewarm = e;
    return e;
  }

  bool psoStateValid(const uint8_t* state, const char** why) override {
    if (state[0] != invalidKeyByte)
      return true;
    if (why)
      *why = "fake invalid";
    return false;
  }

  bool psoSucceeded(MtlCompileTask* p) override {
    return p->isDone() && static_cast<FakePso*>(p)->ok;
  }

  uint64_t psoBuildNs(MtlCompileTask* p) override {
    return p->isDone() ? static_cast<FakePso*>(p)->ns : 0u;
  }

  uint32_t urgentCompilesQueued() override {
    return urgent.load();
  }
};

struct FakeFnTask : public MtlCompileTask {
  MtlShaderFunction* fn = nullptr;
  uint64_t lib = 0u;
  void run() override {
    std::this_thread::sleep_for(std::chrono::microseconds(100));
    fn->library = lib;
  }
};

struct FakeShader final : public RcObject, public MtlStateCacheShader {
  MtlShaderStage   stage = MtlShaderStage::Vertex;
  MtlShaderStateId id;
  uint64_t         libBase = 0u;
  std::mutex       mutex;
  std::map<MtlStateCacheShader*, Rc<MtlShaderFunction>> fns;

  Rc<MtlShaderFunction> prewarmFunction(MtlStateCacheShader* prev) override {
    std::lock_guard lock(mutex);
    auto& f = fns[prev];
    if (f == nullptr) {
      f = new MtlShaderFunction();
      f->stage = stage;
      f->stateId = id;
      f->stateId.linked = prev ? 1u : 0u;
      Rc<FakeFnTask> t = new FakeFnTask();
      t->fn  = f.ptr();
      t->lib = libBase + uint64_t(prev ? 1u : 0u);
      f->task = t;
    }
    return f;
  }

  // The draw path: the same function, compiled on demand.
  Rc<MtlShaderFunction> drawFunction(MtlStateCacheShader* prev) {
    Rc<MtlShaderFunction> f = prewarmFunction(prev);
    f->task->wait();
    return f;
  }
};

static Rc<FakeShader> makeShader(MtlShaderStage stage, uint32_t id) {
  Rc<FakeShader> s = new FakeShader();
  s->stage = stage;
  s->id.origin = uint8_t(Origin::D3D9);
  std::memcpy(s->id.sha, &id, sizeof(id));
  s->libBase = (uint64_t(id) << 8) | (stage == MtlShaderStage::Vertex ? 0x10u : 0x20u);
  return s;
}

struct Workload {
  std::vector<Rc<FakeShader>> vs, ps;
  struct Draw { uint32_t vs, ps; uint8_t state[KeyBytes]; };
  std::vector<Draw> draws;

  explicit Workload(uint32_t nVs, uint32_t nPs, uint32_t nDraws, uint32_t seed) {
    std::mt19937 rng(seed);
    for (uint32_t i = 0u; i < nVs; i++)
      vs.push_back(makeShader(MtlShaderStage::Vertex, 1000u + i));
    for (uint32_t i = 0u; i < nPs; i++)
      ps.push_back(makeShader(MtlShaderStage::Fragment, 5000u + i));
    for (uint32_t i = 0u; i < nDraws; i++) {
      Draw d;
      d.vs = rng() % nVs;
      d.ps = i % nPs;
      for (auto& b : d.state)
        b = uint8_t(rng() % 4u);
      d.state[1] = uint8_t(i);         // distinct keys
      d.state[2] = uint8_t(i >> 8);
      draws.push_back(d);
    }
  }

  void announceAll(MtlStateCache* sc) {
    for (auto& s : vs) sc->announce(s->stage, s->id, s.ptr());
    for (auto& s : ps) sc->announce(s->stage, s->id, s.ptr());
  }

  // The render thread: one miss per draw, recorded like RenderPsoCache::get.
  void render(MtlStateCache* sc, FakeBackend& be, uint64_t* pHits = nullptr) {
    uint64_t hits = 0u;
    for (const Draw& d : draws) {
      Rc<MtlShaderFunction> v = vs[d.vs]->drawFunction(nullptr);
      Rc<MtlShaderFunction> p = ps[d.ps]->drawFunction(vs[d.vs].ptr());
      MtlPsoUse use;
      Rc<FakePso> pso = be.renderMiss(d.state, v.ptr(), p.ptr(), use);
      if (use == MtlPsoUse::Late)
        sc->boostPrewarm(pso.ptr());
      pso->wait();   // runs it here if nobody has started it
      hits += use == MtlPsoUse::Hit;
      sc->record(d.state, v.ptr(), p.ptr(), pso.ptr(), use);
    }
    if (pHits)
      *pHits = hits;
  }
};

static std::string tempDir() {
  char buf[] = "/tmp/d9mt-sct.XXXXXX";
  const char* d = mkdtemp(buf);
  return d ? std::string(d) : std::string("/tmp");
}

static MtlStateCacheConfig configFor(const std::string& dir, uint32_t threads) {
  MtlStateCacheConfig c;
  c.file = dir + "/test.d9mtstate";
  c.displayPath = c.file;
  c.keySignature = Sig;
  c.threads = threads;
  return c;
}

static std::vector<uint8_t> readFile(const std::string& path) {
  std::vector<uint8_t> d;
  if (FILE* f = std::fopen(path.c_str(), "rb")) {
    std::fseek(f, 0, SEEK_END);
    d.resize(size_t(std::ftell(f)));
    std::fseek(f, 0, SEEK_SET);
    if (std::fread(d.data(), 1u, d.size(), f) != d.size())
      d.clear();
    std::fclose(f);
  }
  return d;
}

static void writeFile(const std::string& path, const std::vector<uint8_t>& d) {
  if (FILE* f = std::fopen(path.c_str(), "wb")) {
    std::fwrite(d.data(), 1u, d.size(), f);
    std::fclose(f);
  }
}

static void waitLoaded(MtlStateCache* sc) {
  for (int i = 0; i < 2000 && !sc->loaded(); i++)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  CHECK(sc->loaded());
}

static void testRecordLoadPrewarm() {
  const std::string dir = tempDir();
  const MtlStateCacheConfig cfg = configFor(dir, 2u);

  // Session 1: nothing on disk; 300 draws are recorded.
  {
    Workload w(8u, 60u, 300u, 1u);
    FakeBackend be;
    Rc<MtlStateCache> sc = new MtlStateCache(&be, cfg);
    waitLoaded(sc.ptr());
    w.announceAll(sc.ptr());
    w.render(sc.ptr(), be);
    sc->shutdown();
    const auto s = sc->stats();
    CHECK(s.misses == 300u && s.hits == 0u && s.recorded == 300u && s.ringDrops == 0u);
    CHECK(s.built == 0u && s.writes >= 1u && s.entries == 300u);
  }

  std::vector<Entry> onDisk;
  ParseResult r = parse(readFile(cfg.file).data(), readFile(cfg.file).size(), Sig, onDisk);
  CHECK(r.status == ParseStatus::Ok && onDisk.size() == 300u && r.session == 1u);

  // Session 2: same shaders (new library handles would be fine too); the
  // prewarm builds everything before the draws, which then all hit.
  {
    Workload w(8u, 60u, 300u, 1u);
    FakeBackend be;
    Rc<MtlStateCache> sc = new MtlStateCache(&be, cfg);
    w.announceAll(sc.ptr());       // announced before or after load: both work
    waitLoaded(sc.ptr());
    CHECK(sc->waitPrewarmIdle(10000u));
    uint64_t hits = 0u;
    w.render(sc.ptr(), be, &hits);
    sc->shutdown();
    const auto s = sc->stats();
    CHECK(s.built == 300u && s.failed == 0u && s.dup == 0u);
    CHECK(hits == 300u && s.hits == 300u && s.misses == 0u && s.recorded == 0u);
  }

  r = parse(readFile(cfg.file).data(), readFile(cfg.file).size(), Sig, onDisk);
  CHECK(r.status == ParseStatus::Ok && onDisk.size() == 300u && r.session == 2u);
  for (const Entry& e : onDisk)
    CHECK(e.sessionsUsed == 2u && e.lastSession == 2u);

  // Session 3: prewarm off (threads = 0) still loads and records.
  {
    Workload w(8u, 60u, 300u, 1u);
    FakeBackend be;
    Rc<MtlStateCache> sc = new MtlStateCache(&be, configFor(dir, 0u));
    waitLoaded(sc.ptr());
    w.announceAll(sc.ptr());
    w.render(sc.ptr(), be);
    sc->shutdown();
    const auto s = sc->stats();
    CHECK(s.built == 0u && s.misses == 300u && s.recorded == 0u && s.entries == 300u);
  }

  // Session 4: only some shaders exist; only their pipelines are built.
  {
    Workload w(8u, 60u, 300u, 1u);
    FakeBackend be;
    Rc<MtlStateCache> sc = new MtlStateCache(&be, cfg);
    waitLoaded(sc.ptr());
    for (auto& s : w.vs) sc->announce(s->stage, s->id, s.ptr());
    for (uint32_t i = 0u; i < 10u; i++) sc->announce(w.ps[i]->stage, w.ps[i]->id, w.ps[i].ptr());
    CHECK(sc->waitPrewarmIdle(10000u));
    uint32_t expected = 0u;
    for (const auto& d : w.draws)
      expected += d.ps < 10u;
    CHECK(sc->stats().built == expected);
    sc->shutdown();
  }
}

static void testFailedAndCorrupt() {
  const std::string dir = tempDir();
  const MtlStateCacheConfig cfg = configFor(dir, 1u);

  {
    Workload w(4u, 20u, 100u, 2u);
    FakeBackend be;
    Rc<MtlStateCache> sc = new MtlStateCache(&be, cfg);
    waitLoaded(sc.ptr());
    w.announceAll(sc.ptr());
    w.render(sc.ptr(), be);
    sc->shutdown();
  }

  // Pipelines whose key starts with 3 no longer build: they are dropped.
  uint32_t kept = 0u;
  {
    Workload w(4u, 20u, 100u, 2u);
    uint32_t failing = 0u;
    for (const auto& d : w.draws)
      failing += d.state[0] == 3u;
    CHECK(failing > 0u);

    FakeBackend be;
    be.failKeyByte = 3u;
    Rc<MtlStateCache> sc = new MtlStateCache(&be, cfg);
    w.announceAll(sc.ptr());
    waitLoaded(sc.ptr());
    CHECK(sc->waitPrewarmIdle(10000u));
    sc->shutdown();
    const auto s = sc->stats();
    CHECK(s.failed == failing && s.built == 100u - failing);
    CHECK(s.entries == 100u - failing);
    kept = 100u - failing;

    std::vector<Entry> onDisk;
    auto d = readFile(cfg.file);
    parse(d.data(), d.size(), Sig, onDisk);
    CHECK(onDisk.size() == 100u - failing);
    for (const Entry& e : onDisk)
      CHECK(e.key[0] != 3u);
  }

  // A damaged entry in the file: skipped and the file is rewritten.
  {
    auto d = readFile(cfg.file);
    d[HeaderSize + 5u * EntrySize + 200u] ^= 0x10u;
    writeFile(cfg.file, d);

    FakeBackend be;
    Rc<MtlStateCache> sc = new MtlStateCache(&be, cfg);
    waitLoaded(sc.ptr());
    sc->shutdown();

    std::vector<Entry> onDisk;
    auto d2 = readFile(cfg.file);
    ParseResult r = parse(d2.data(), d2.size(), Sig, onDisk);
    CHECK(r.status == ParseStatus::Ok && r.corrupt == 0u);
    CHECK(onDisk.size() == kept - 1u && sc->stats().entries == kept - 1u);
  }

  // Garbage file: replaced by a valid (empty) one.
  {
    writeFile(cfg.file, std::vector<uint8_t>(1000u, uint8_t(0x7fu)));
    FakeBackend be;
    Rc<MtlStateCache> sc = new MtlStateCache(&be, cfg);
    waitLoaded(sc.ptr());
    sc->shutdown();
    std::vector<Entry> onDisk;
    auto d = readFile(cfg.file);
    CHECK(parse(d.data(), d.size(), Sig, onDisk).status == ParseStatus::Ok);
  }

  // A newer d9mt's file is never overwritten, even with new records.
  {
    std::vector<uint8_t> newer = serialize({ makeEntry(1u) }, Sig, 9u);
    const uint32_t v = FormatVersion + 1u;
    std::memcpy(newer.data() + 8u, &v, 4u);
    writeFile(cfg.file, newer);

    Workload w(2u, 4u, 10u, 3u);
    FakeBackend be;
    Rc<MtlStateCache> sc = new MtlStateCache(&be, cfg);
    waitLoaded(sc.ptr());
    w.announceAll(sc.ptr());
    w.render(sc.ptr(), be);
    sc->shutdown();
    CHECK(readFile(cfg.file) == newer);
  }

  // Key signature change: old entries dropped, file rewritten with the new one.
  {
    writeFile(cfg.file, serialize({ makeEntry(1u), makeEntry(2u) }, Sig + 7u, 4u));
    FakeBackend be;
    Rc<MtlStateCache> sc = new MtlStateCache(&be, cfg);
    waitLoaded(sc.ptr());
    sc->shutdown();
    std::vector<Entry> onDisk;
    auto d = readFile(cfg.file);
    ParseResult r = parse(d.data(), d.size(), Sig, onDisk);
    CHECK(r.status == ParseStatus::Ok && onDisk.empty() && r.session == 5u);
  }

  // Cap: a file with more than the default maximum (1024) loads capped and
  // is rewritten capped, keeping the most recently used entries.
  {
    std::vector<Entry> many;
    for (uint32_t i = 0u; i < DefaultMaxEntries + 300u; i++) {
      Entry e = makeEntry(i);
      e.lastSession = 1u + i / 1000u;
      many.push_back(e);
    }
    writeFile(cfg.file, serialize(many, Sig, 6u));
    FakeBackend be;
    Rc<MtlStateCache> sc = new MtlStateCache(&be, configFor(dir, 0u));
    waitLoaded(sc.ptr());
    sc->shutdown();
    CHECK(sc->stats().entries == DefaultMaxEntries);
    std::vector<Entry> onDisk;
    auto d = readFile(cfg.file);
    parse(d.data(), d.size(), Sig, onDisk);
    CHECK(onDisk.size() == DefaultMaxEntries);
    CHECK(d.size() == HeaderSize + size_t(DefaultMaxEntries) * EntrySize);
    // Sessions: entries 0..999 last used in session 1, 1000..1323 in
    // session 2. LRU keeps all 324 of session 2 and 700 of session 1.
    uint32_t newest = 0u;
    for (const Entry& e : onDisk)
      newest += e.lastSession == 2u;
    CHECK(newest == DefaultMaxEntries + 300u - 1000u);
  }

  // A larger configured maximum (D9MT_STATE_CACHE_MAX) keeps them all.
  {
    std::vector<Entry> many;
    for (uint32_t i = 0u; i < DefaultMaxEntries + 300u; i++) {
      Entry e = makeEntry(i);
      e.lastSession = 1u + i / 1000u;
      many.push_back(e);
    }
    writeFile(cfg.file, serialize(many, Sig, 6u));
    FakeBackend be;
    MtlStateCacheConfig big = configFor(dir, 0u);
    big.maxEntries = 2000u;
    Rc<MtlStateCache> sc = new MtlStateCache(&be, big);
    waitLoaded(sc.ptr());
    sc->shutdown();
    CHECK(sc->stats().entries == DefaultMaxEntries + 300u);
  }
}

static void testTwoProcessesMerge() {
  const std::string dir = tempDir();
  const MtlStateCacheConfig cfg = configFor(dir, 0u);

  // Two caches on one file (two game processes): neither loses the
  // other's entries.
  Workload a(2u, 10u, 50u, 10u), b(2u, 10u, 50u, 20u);
  FakeBackend beA, beB;
  Rc<MtlStateCache> scA = new MtlStateCache(&beA, cfg);
  Rc<MtlStateCache> scB = new MtlStateCache(&beB, cfg);
  waitLoaded(scA.ptr());
  waitLoaded(scB.ptr());
  a.announceAll(scA.ptr());
  b.announceAll(scB.ptr());
  a.render(scA.ptr(), beA);
  b.render(scB.ptr(), beB);
  scA->shutdown();
  scB->shutdown();

  std::vector<Entry> onDisk;
  auto d = readFile(cfg.file);
  parse(d.data(), d.size(), Sig, onDisk);
  CHECK(onDisk.size() == 100u);
}

static void testConcurrency() {
  // Announce on one thread, render (record) on another, prewarm on two,
  // the urgent-queue pause toggling: TSan checks the hand-offs; the end
  // state checks nothing was lost.
  const std::string dir = tempDir();
  const MtlStateCacheConfig cfg = configFor(dir, 2u);

  {
    Workload w(16u, 200u, 1000u, 5u);
    FakeBackend be;
    Rc<MtlStateCache> sc = new MtlStateCache(&be, cfg);
    w.announceAll(sc.ptr());
    w.render(sc.ptr(), be);
    sc->shutdown();
    CHECK(sc->stats().entries == 1000u);
  }

  for (int round = 0; round < 3; round++) {
    Workload w(16u, 200u, 1000u, 5u);
    FakeBackend be;
    Rc<MtlStateCache> sc = new MtlStateCache(&be, cfg);

    std::atomic<bool> stop = { false };
    std::thread pauser([&] {
      std::mt19937 rng(round);
      while (!stop.load()) {
        be.urgent.store(rng() % 3u == 0u ? 1u : 0u);
        std::this_thread::sleep_for(std::chrono::microseconds(500));
      }
      be.urgent.store(0u);
    });

    std::thread announcer([&] { w.announceAll(sc.ptr()); });
    uint64_t hits = 0u;
    std::thread renderer([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(round * 20));
      w.render(sc.ptr(), be, &hits);
    });

    announcer.join();
    renderer.join();
    stop = true;
    pauser.join();

    // Shut down while the prewarm may still be working (round 2: at once).
    if (round < 2)
      sc->waitPrewarmIdle(10000u);
    sc->shutdown();

    const auto s = sc->stats();
    CHECK(s.hits + s.late + s.misses == 1000u);
    CHECK(s.ringDrops == 0u && s.failed == 0u);
    CHECK(s.built <= be.created.load());
    CHECK(s.entries == 1000u);
  }
}

// Location and limits from the environment (native build: HOME stands in
// for Wine's WINEHOMEDIR). Real TMPDIR/HOME are replaced by fake dirs, so
// nothing outside /tmp is looked at.
static void testResolveConfig() {
  struct SavedEnv {
    std::string name; bool set; std::string value;
    explicit SavedEnv(const char* n) : name(n) {
      const char* v = std::getenv(n);
      set = v != nullptr;
      value = v ? v : "";
    }
    ~SavedEnv() { set ? setenv(name.c_str(), value.c_str(), 1) : unsetenv(name.c_str()); }
  };
  SavedEnv e1("D9MT_STATE_CACHE"), e2("TMPDIR"), e3("HOME"), e4("D9MT_STATE_CACHE_MAX"),
           e5("D9MT_STATE_CACHE_THREADS"), e6("D9MT_STATE_CACHE_EXE");

  const std::string root = tempDir();
  const std::string home = root + "/home", tmpT = root + "/var/folders/ab/cdef/T";
  std::string cmd = "mkdir -p '" + home + "' '" + tmpT + "'";
  CHECK(!std::system(cmd.c_str()));
  setenv("D9MT_STATE_CACHE_EXE", "swtor.exe", 1);
  setenv("TMPDIR", (tmpT + "/").c_str(), 1);
  setenv("HOME", home.c_str(), 1);
  unsetenv("D9MT_STATE_CACHE_MAX");
  unsetenv("D9MT_STATE_CACHE_THREADS");

  auto resolve = [] (MtlStateCacheConfig& c, std::string& reason) {
    c = MtlStateCacheConfig();
    reason.clear();
    return MtlStateCache::resolveConfig(c, reason);
  };
  MtlStateCacheConfig c;
  std::string reason;

  // Neither TMPDIR's sibling C/d9mt nor ~/Library/Caches exists: off, with a reason.
  unsetenv("D9MT_STATE_CACHE");
  CHECK(!resolve(c, reason) && reason.find("set D9MT_STATE_CACHE") != std::string::npos);

  // ~/Library/Caches exists: the fallback.
  cmd = "mkdir -p '" + home + "/Library/Caches'";
  CHECK(!std::system(cmd.c_str()));
  CHECK(resolve(c, reason) && c.file == home + "/Library/Caches/d9mt/state/swtor.exe.d9mtstate");

  // The user cache dir next to the AIR cache wins over ~/Library/Caches.
  const std::string cDir = root + "/var/folders/ab/cdef/C";
  cmd = "mkdir -p '" + cDir + "/d9mt'";
  CHECK(!std::system(cmd.c_str()));
  CHECK(resolve(c, reason) && c.file == cDir + "/d9mt/state/swtor.exe.d9mtstate");

  // The explicit variable wins over both: a directory, or a file.
  setenv("D9MT_STATE_CACHE", (root + "//explicit/").c_str(), 1);
  CHECK(resolve(c, reason) && c.file == root + "/explicit/swtor.exe.d9mtstate");
  setenv("D9MT_STATE_CACHE", (root + "/x/my.d9mtstate").c_str(), 1);
  CHECK(resolve(c, reason) && c.file == root + "/x/my.d9mtstate");

  // =0 turns it off whatever else exists; an empty value counts as unset.
  setenv("D9MT_STATE_CACHE", "0", 1);
  CHECK(!resolve(c, reason) && reason.find("D9MT_STATE_CACHE=0") != std::string::npos);
  setenv("D9MT_STATE_CACHE", "", 1);
  CHECK(resolve(c, reason) && c.file == cDir + "/d9mt/state/swtor.exe.d9mtstate");

  // A HOME that isn't a usable absolute path doesn't count.
  unsetenv("D9MT_STATE_CACHE");
  setenv("TMPDIR", "/nonexistent/T/", 1);
  setenv("HOME", "relative/home", 1);
  CHECK(!resolve(c, reason));
  setenv("HOME", home.c_str(), 1);
  CHECK(resolve(c, reason) && c.file == home + "/Library/Caches/d9mt/state/swtor.exe.d9mtstate");

  // The entry cap: default 1024, D9MT_STATE_CACHE_MAX within [64, 16384].
  CHECK(c.maxEntries == DefaultMaxEntries && DefaultMaxEntries == 1024u);
  setenv("D9MT_STATE_CACHE_MAX", "3000", 1);
  CHECK(resolve(c, reason) && c.maxEntries == 3000u);
  setenv("D9MT_STATE_CACHE_MAX", "10", 1);
  CHECK(resolve(c, reason) && c.maxEntries == MinMaxEntries);
  setenv("D9MT_STATE_CACHE_MAX", "999999", 1);
  CHECK(resolve(c, reason) && c.maxEntries == MaxMaxEntries);
  setenv("D9MT_STATE_CACHE_MAX", "junk", 1);
  CHECK(resolve(c, reason) && c.maxEntries == DefaultMaxEntries);

  setenv("D9MT_STATE_CACHE_THREADS", "9", 1);
  CHECK(resolve(c, reason) && c.threads == 4u);
  setenv("D9MT_STATE_CACHE_THREADS", "0", 1);
  CHECK(resolve(c, reason) && c.threads == 0u);
  setenv("D9MT_STATE_CACHE_THREADS", "junk", 1);
  CHECK(resolve(c, reason) && c.threads == 2u);       // not a number: the default
  setenv("D9MT_STATE_CACHE_THREADS", "1x", 1);
  CHECK(resolve(c, reason) && c.threads == 2u);
  unsetenv("D9MT_STATE_CACHE_THREADS");

  // A relative D9MT_STATE_CACHE is ignored (logged): the default location.
  setenv("D9MT_STATE_CACHE", "relative/dir", 1);
  CHECK(resolve(c, reason) && c.file == home + "/Library/Caches/d9mt/state/swtor.exe.d9mtstate");
  setenv("D9MT_STATE_CACHE", "..\\up", 1);
  CHECK(resolve(c, reason) && c.file == home + "/Library/Caches/d9mt/state/swtor.exe.d9mtstate");
  // Windows absolute paths are taken as they are.
  setenv("D9MT_STATE_CACHE", "C:\\cache", 1);
  CHECK(resolve(c, reason) && c.file == "C:\\cache\\swtor.exe.d9mtstate");
  setenv("D9MT_STATE_CACHE", "\\\\server\\share\\x.d9mtstate", 1);
  CHECK(resolve(c, reason) && c.file == "\\\\server\\share\\x.d9mtstate");
  unsetenv("D9MT_STATE_CACHE");
}

// The key validator (backend/mtl_pso_key.h): a key shaped like resolvePso()'s
// passes; each forged variant (one invalid field) is rejected. These are the
// descriptors Metal terminates the process on, or reads past arrays with.
static backend::RenderPsoKey drawShapedKey() {
  backend::RenderPsoKey k = {};
  k.attrs[0] = { 30u /* Float3 */, 0u, 3u, 0u };
  k.attrs[1] = { 9u /* UChar4Normalized */, 12u, 3u, 0u };
  k.attrs[2] = { 31u /* Float4 */, 0u, 19u, 0u };   // the null stream
  k.attrCount = 3u;
  k.layouts[0] = { 16u, 1u, 3u };
  k.layouts[1] = { 16u, 1u, 19u };
  k.layoutCount = 2u;
  k.colors[0].pixelFormat  = uint32_t(MtlFormat::BGRA8Unorm);
  k.colors[0].blendEnabled = 1u;
  k.colors[0].rgbSrcFactor = 4u;
  k.colors[0].rgbDstFactor = 5u;
  k.colorCount = 1u;
  k.sampleCount = 1u;
  k.topologyClass = 3u;
  return k;
}

static void testKeyValidator() {
  using backend::RenderPsoKey;
  const char* why = nullptr;

  RenderPsoKey ok = drawShapedKey();
  CHECK(backend::psoStateValid(ok.stateBytes(), &why));

  // Also valid: an empty slot inside the count, garbage past the counts,
  // MSAA, blending on 16- and 32-bit float targets, 4 targets.
  RenderPsoKey v = ok;
  v.colors[1] = { };
  v.colors[1].pixelFormat = 0u;
  v.colors[2].pixelFormat = uint32_t(MtlFormat::RGBA16Float);
  v.colors[2].blendEnabled = 1u;
  v.colors[3].pixelFormat = uint32_t(MtlFormat::R32Uint);
  v.colorCount = 4u;
  v.colors[0].pixelFormat = uint32_t(MtlFormat::RGBA32Float);   // blending on 32-bit float: fine
  v.attrs[10].format = 9999u;
  v.layouts[15].stride = 0u;
  v.sampleCount = 4u;
  CHECK(backend::psoStateValid(v.stateBytes(), &why));
  for (MtlFormat f : { MtlFormat::R32Float, MtlFormat::RG32Float, MtlFormat::RGBA32Float }) {
    RenderPsoKey b = ok;
    b.colors[0].pixelFormat = uint32_t(f);
    CHECK(backend::psoStateValid(b.stateBytes(), &why));
  }

  struct Case { const char* name; void (*mutate)(RenderPsoKey&); };
  const Case cases[] = {
    { "attrCount 21",          [] (RenderPsoKey& k) { k.attrCount = 21u; } },
    { "layoutCount 21",        [] (RenderPsoKey& k) { k.layoutCount = 21u; } },
    { "colorCount 5",          [] (RenderPsoKey& k) { k.colorCount = 5u; } },
    { "colorCount 255",        [] (RenderPsoKey& k) { k.colorCount = 255u; } },
    { "zero stride",           [] (RenderPsoKey& k) { k.layouts[0].stride = 0u; } },
    { "buffer index 2",        [] (RenderPsoKey& k) { k.layouts[0].pad = 2u; k.attrs[0].bufferIndex = 2u; k.attrs[1].bufferIndex = 2u; } },
    { "buffer index 20",       [] (RenderPsoKey& k) { k.layouts[1].pad = 20u; k.attrs[2].bufferIndex = 20u; } },
    { "layout twice",          [] (RenderPsoKey& k) { k.layouts[1].pad = 3u; } },
    { "step function 0",       [] (RenderPsoKey& k) { k.layouts[0].stepFunction = 0u; } },
    { "vertex format 0",       [] (RenderPsoKey& k) { k.attrs[0].format = 0u; } },
    { "vertex format UChar2",  [] (RenderPsoKey& k) { k.attrs[0].format = 1u; } },
    { "vertex format 999",     [] (RenderPsoKey& k) { k.attrs[1].format = 999u; } },
    { "buffer without layout", [] (RenderPsoKey& k) { k.attrs[1].bufferIndex = 5u; } },
    { "pixel format 999",      [] (RenderPsoKey& k) { k.colors[0].pixelFormat = 999u; } },
    { "pixel format depth",    [] (RenderPsoKey& k) { k.colors[0].pixelFormat = uint32_t(MtlFormat::Depth32Float); } },
    { "pixel format BC1",      [] (RenderPsoKey& k) { k.colors[0].pixelFormat = uint32_t(MtlFormat::BC1_RGBA); } },
    { "pixel format vtx-only", [] (RenderPsoKey& k) { k.colors[0].pixelFormat = uint32_t(MtlFormat::RGB32Float); } },
    { "blend on R32Uint",      [] (RenderPsoKey& k) { k.colors[0].pixelFormat = uint32_t(MtlFormat::R32Uint); } },
    { "blend on R8Uint",       [] (RenderPsoKey& k) { k.colors[0].pixelFormat = uint32_t(MtlFormat::R8Uint); } },
    { "blend on R16Uint",      [] (RenderPsoKey& k) { k.colors[0].pixelFormat = uint32_t(MtlFormat::R16Uint); } },
    { "blend on empty slot",   [] (RenderPsoKey& k) { k.colors[0].pixelFormat = 0u; } },
    { "blendEnabled 2",        [] (RenderPsoKey& k) { k.colors[0].blendEnabled = 2u; } },
    { "write mask 0x1f",       [] (RenderPsoKey& k) { k.colors[0].writeMask = 0x1fu; } },
    { "blend factor 19",       [] (RenderPsoKey& k) { k.colors[0].alphaDstFactor = 19u; } },
    { "blend op 5",            [] (RenderPsoKey& k) { k.colors[0].rgbBlendOp = 5u; } },
    { "sample count 0",        [] (RenderPsoKey& k) { k.sampleCount = 0u; } },
    { "sample count 3",        [] (RenderPsoKey& k) { k.sampleCount = 3u; } },
    { "sample count 16",       [] (RenderPsoKey& k) { k.sampleCount = 16u; } },
    { "topology 4",            [] (RenderPsoKey& k) { k.topologyClass = 4u; } },
    { "alpha to coverage 2",   [] (RenderPsoKey& k) { k.alphaToCoverage = 2u; } },
    { "depth format",          [] (RenderPsoKey& k) { k.depthFormat = uint32_t(MtlFormat::Depth32Float); } },
    { "stencil format",        [] (RenderPsoKey& k) { k.stencilFormat = uint32_t(MtlFormat::Stencil8); } },
  };

  for (const Case& c : cases) {
    RenderPsoKey k = ok;
    c.mutate(k);
    why = nullptr;
    const bool valid = backend::psoStateValid(k.stateBytes(), &why);
    if (valid)
      std::fprintf(stderr, "validator accepted: %s\n", c.name);
    CHECK(!valid && why != nullptr);
  }

  // 100 000 random keys: never accepted unless every invariant holds, and
  // never a read past the arrays (ASan/TSan builds would see it).
  std::mt19937 rng(7u);
  uint32_t accepted = 0u;
  for (uint32_t i = 0u; i < 100000u; i++) {
    uint8_t state[statecache::KeyBytes];
    for (auto& b : state)
      b = uint8_t(rng());
    accepted += backend::psoStateValid(state, nullptr);
  }
  CHECK(accepted == 0u);
}

// Entries the backend rejects are dropped at load, never prewarmed, and the
// file is rewritten without them.
static void testLoadValidation() {
  const std::string dir = tempDir();
  const MtlStateCacheConfig cfg = configFor(dir, 2u);
  uint32_t forged = 0u;

  {
    Workload w(4u, 20u, 100u, 9u);
    for (uint32_t i = 0u; i < w.draws.size(); i += 7u) {
      w.draws[i].state[0] = 0xeeu;
      forged++;
    }
    FakeBackend be;
    Rc<MtlStateCache> sc = new MtlStateCache(&be, cfg);
    waitLoaded(sc.ptr());
    w.announceAll(sc.ptr());
    w.render(sc.ptr(), be);   // the draw path doesn't validate: all recorded
    sc->shutdown();
    CHECK(sc->stats().entries == 100u);
  }

  {
    Workload w(4u, 20u, 100u, 9u);
    FakeBackend be;
    Rc<MtlStateCache> sc = new MtlStateCache(&be, cfg);
    w.announceAll(sc.ptr());
    waitLoaded(sc.ptr());
    CHECK(sc->waitPrewarmIdle(10000u));
    sc->shutdown();
    const auto s = sc->stats();
    CHECK(s.invalid == forged && s.built == 100u - forged && s.entries == 100u - forged);

    std::vector<Entry> onDisk;
    auto d = readFile(cfg.file);
    parse(d.data(), d.size(), Sig, onDisk);
    CHECK(onDisk.size() == 100u - forged);
    for (const Entry& e : onDisk)
      CHECK(e.key[0] != 0xeeu);
  }
}

static bool exists(const std::string& path) {
  return access(path.c_str(), F_OK) == 0;
}

// The crash-loop guard: <file>.prewarm exists while prewarm builds run, is
// gone after the queue drains and after a clean shutdown, and if found at
// load (the last session died during the prewarm) the file is dropped and
// nothing is prewarmed.
static void testCrashGuard() {
  const std::string dir = tempDir();
  const MtlStateCacheConfig cfg = configFor(dir, 1u);
  const std::string marker = cfg.file + ".prewarm";

  {
    Workload w(4u, 20u, 60u, 11u);
    FakeBackend be;
    Rc<MtlStateCache> sc = new MtlStateCache(&be, cfg);
    waitLoaded(sc.ptr());
    w.announceAll(sc.ptr());
    w.render(sc.ptr(), be);
    sc->shutdown();
    CHECK(!exists(marker));   // no prewarm ran: no marker ever
  }

  // A session whose first prewarm build blocks: the marker is on disk
  // during the build (seen from inside it), gone once the queue drains.
  {
    Workload w(4u, 20u, 60u, 11u);
    std::atomic<bool> gate = { false }, started = { false };
    FakeBackend be;
    be.gate = &gate;
    be.started = &started;
    be.watchFile = marker;
    Rc<MtlStateCache> sc = new MtlStateCache(&be, cfg);
    w.announceAll(sc.ptr());
    waitLoaded(sc.ptr());
    for (int i = 0; i < 5000 && !started.load(); i++)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(started.load() && exists(marker));
    gate = true;
    CHECK(sc->waitPrewarmIdle(10000u));
    for (int i = 0; i < 1000 && exists(marker); i++)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(!exists(marker));
    CHECK(be.lastPrewarm != nullptr && be.lastPrewarm->sawFile.load());
    CHECK(sc->stats().built == 60u);
    sc->shutdown();
  }

  // Shut down while the prewarm is busy (queue not drained): a clean
  // shutdown removes the marker too.
  {
    Workload w(4u, 20u, 60u, 11u);
    std::atomic<bool> gate = { false }, started = { false };
    FakeBackend be;
    be.gate = &gate;
    be.started = &started;
    Rc<MtlStateCache> sc = new MtlStateCache(&be, cfg);
    w.announceAll(sc.ptr());
    waitLoaded(sc.ptr());
    for (int i = 0; i < 5000 && !started.load(); i++)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(exists(marker));
    std::thread releaser([&gate] {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      gate = true;
    });
    sc->shutdown();
    releaser.join();
    CHECK(!exists(marker) && sc->stats().built < 60u);
  }

  // The entries on disk now, and a marker text naming some of them.
  auto diskEntries = [&cfg] {
    std::vector<Entry> out;
    auto d = readFile(cfg.file);
    parse(d.data(), d.size(), Sig, out);
    return out;
  };
  auto markerText = [] (uint32_t trips, const std::vector<Entry>& inflight) {
    std::string t = "d9mt-prewarm 1\ntrips " + std::to_string(trips) + "\n";
    for (const Entry& e : inflight) {
      char hex[20];
      std::snprintf(hex, sizeof(hex), "%016llx\n", (unsigned long long)identityHash(e));
      t += hex;
    }
    return std::vector<uint8_t>(t.begin(), t.end());
  };

  // While a job runs, the marker names it.
  {
    Workload w(4u, 20u, 60u, 11u);
    std::atomic<bool> gate = { false }, started = { false };
    FakeBackend be;
    be.gate = &gate;
    be.started = &started;
    Rc<MtlStateCache> sc = new MtlStateCache(&be, cfg);
    w.announceAll(sc.ptr());
    waitLoaded(sc.ptr());
    for (int i = 0; i < 5000 && !started.load(); i++)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const auto m = readFile(marker);
    const std::string text(m.begin(), m.end());
    uint32_t named = 0u;
    for (const Entry& e : diskEntries()) {
      char hex[20];
      std::snprintf(hex, sizeof(hex), "%016llx", (unsigned long long)identityHash(e));
      named += text.find(hex) != std::string::npos;
    }
    CHECK(!text.compare(0u, 23u, "d9mt-prewarm 1\ntrips 0\n") && named == 1u);   // 1 thread: 1 job
    gate = true;
    CHECK(sc->waitPrewarmIdle(10000u));
    sc->shutdown();
    CHECK(!exists(marker));
  }

  // The last session ended during the prewarm (crash, kill, or an exit that
  // never released the device): only the two entries that were in flight
  // are dropped; the rest is kept and prewarmed.
  {
    const std::vector<Entry> before = diskEntries();
    CHECK(before.size() == 60u);
    writeFile(marker, markerText(0u, { before[7], before[31] }));

    Workload w(4u, 20u, 60u, 11u);
    FakeBackend be;
    Rc<MtlStateCache> sc = new MtlStateCache(&be, cfg);
    w.announceAll(sc.ptr());
    waitLoaded(sc.ptr());
    CHECK(sc->waitPrewarmIdle(10000u));
    auto s = sc->stats();
    CHECK(s.guardTripped && s.suspects == 2u && s.trips == 1u && !s.prewarmOff);
    CHECK(s.built == 58u && s.entries == 58u);
    for (int i = 0; i < 1000 && exists(marker); i++)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(!exists(marker));

    // A draw that needs them again records them again: nothing is lost
    // for good.
    w.render(sc.ptr(), be);
    sc->shutdown();
    s = sc->stats();
    CHECK(s.recorded == 2u && s.entries == 60u && diskEntries().size() == 60u);
  }

  // Unreadable marker: a trip without suspects; nothing is dropped.
  {
    writeFile(marker, std::vector<uint8_t>(4u, uint8_t('x')));
    Workload w(4u, 20u, 60u, 11u);
    FakeBackend be;
    Rc<MtlStateCache> sc = new MtlStateCache(&be, cfg);
    w.announceAll(sc.ptr());
    waitLoaded(sc.ptr());
    CHECK(sc->waitPrewarmIdle(10000u));
    sc->shutdown();
    const auto s = sc->stats();
    CHECK(s.guardTripped && s.suspects == 0u && s.built == 60u && s.entries == 60u);
  }

  // Three sessions in a row ended during the prewarm: this one runs
  // without prewarm (so it can't trip) and keeps the file; the next one
  // (no marker left) prewarms again with the count still at 3, and its
  // drained queue resets the count.
  {
    writeFile(marker, markerText(2u, { }));
    Workload w(4u, 20u, 60u, 11u);
    FakeBackend be;
    Rc<MtlStateCache> sc = new MtlStateCache(&be, cfg);
    w.announceAll(sc.ptr());
    waitLoaded(sc.ptr());
    CHECK(sc->waitPrewarmIdle(5000u));
    const auto s = sc->stats();
    CHECK(s.guardTripped && s.trips == 3u && s.prewarmOff && s.built == 0u && s.entries == 60u);
    CHECK(!exists(marker));
    w.render(sc.ptr(), be);
    sc->shutdown();
    CHECK(diskEntries().size() == 60u);
  }
  {
    Workload w(4u, 20u, 60u, 11u);
    FakeBackend be;
    Rc<MtlStateCache> sc = new MtlStateCache(&be, cfg);
    w.announceAll(sc.ptr());
    waitLoaded(sc.ptr());
    CHECK(sc->waitPrewarmIdle(10000u));
    sc->shutdown();
    const auto s = sc->stats();
    CHECK(!s.guardTripped && s.trips == 3u && s.built == 60u);
    CHECK(!exists(cfg.file + ".trips"));
  }

  // A trip counter carries over while sessions keep ending mid-prewarm:
  // a marker written by a session that had tripped once says "trips 1".
  {
    writeFile(marker, markerText(0u, { }));
    Workload w(4u, 20u, 60u, 11u);
    std::atomic<bool> gate = { false }, started = { false };
    FakeBackend be;
    be.gate = &gate;
    be.started = &started;
    Rc<MtlStateCache> sc = new MtlStateCache(&be, cfg);
    w.announceAll(sc.ptr());
    waitLoaded(sc.ptr());
    for (int i = 0; i < 5000 && !started.load(); i++)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const auto m = readFile(marker);
    CHECK(std::string(m.begin(), m.end()).find("trips 1\n") != std::string::npos);
    gate = true;
    CHECK(sc->waitPrewarmIdle(10000u));
    sc->shutdown();
  }

  // Two processes on one file: B takes A's marker as a trip and removes
  // it; A's next job writes it again, so A's builds stay guarded.
  {
    Workload wa(4u, 20u, 60u, 11u), wb(4u, 20u, 60u, 11u);
    std::atomic<bool> gate = { false }, started = { false };
    FakeBackend beA, beB;
    beA.gate = &gate;
    beA.started = &started;
    beA.watchFile = marker;
    Rc<MtlStateCache> a = new MtlStateCache(&beA, cfg);
    wa.announceAll(a.ptr());
    waitLoaded(a.ptr());
    for (int i = 0; i < 5000 && !started.load(); i++)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(exists(marker));

    Rc<MtlStateCache> b = new MtlStateCache(&beB, configFor(dir, 0u));
    waitLoaded(b.ptr());
    CHECK(b->stats().guardTripped && b->stats().suspects == 1u && !exists(marker));

    gate = true;   // A goes on: its next jobs write the marker again
    CHECK(a->waitPrewarmIdle(10000u));
    CHECK(beA.lastPrewarm != nullptr && beA.lastPrewarm->sawFile.load());
    a->shutdown();
    b->shutdown();
  }
}

// A crash the in-flight list doesn't catch (another thread, a torn marker)
// that happens in every session with a prewarm: the trip count survives
// the session without prewarm, and at 6 the file is dropped; after that
// things are normal again. Also: a drain resets the count, and a crash in
// the marker-write window leaves the old marker intact.
static void testCrashChain() {
  const std::string dir = tempDir();
  const MtlStateCacheConfig cfg = configFor(dir, 1u);
  const std::string marker = cfg.file + ".prewarm", trips = cfg.file + ".trips";
  const auto noSuspects = [] (uint32_t t) {
    const std::string s = "d9mt-prewarm 1\ntrips " + std::to_string(t) + "\n";
    return std::vector<uint8_t>(s.begin(), s.end());
  };

  { Workload w(4u, 20u, 60u, 21u);
    FakeBackend be;
    Rc<MtlStateCache> sc = new MtlStateCache(&be, cfg);
    waitLoaded(sc.ptr());
    w.announceAll(sc.ptr());
    w.render(sc.ptr(), be);
    sc->shutdown();
  }

  // One session: its prewarm "crashes" (blocked, then the process goes
  // without a drain: a clean shutdown here, then the marker it would have
  // left is planted). Returns the stats as loaded.
  auto crashingSession = [&] (bool expectPrewarm) {
    Workload w(4u, 20u, 60u, 21u);
    std::atomic<bool> gate = { false }, started = { false };
    FakeBackend be;
    be.gate = &gate;
    be.started = &started;
    Rc<MtlStateCache> sc = new MtlStateCache(&be, cfg);
    w.announceAll(sc.ptr());
    waitLoaded(sc.ptr());
    if (expectPrewarm) {
      for (int i = 0; i < 5000 && !started.load(); i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      CHECK(started.load());
    }
    const auto s = sc->stats();
    std::thread releaser([&gate] {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      gate = true;
    });
    sc->shutdown();
    releaser.join();
    if (expectPrewarm)
      writeFile(marker, noSuspects(0u));   // the crash left its marker
    return s;
  };

  writeFile(marker, noSuspects(0u));       // session 0 crashed during its prewarm
  auto s = crashingSession(true);           // 1
  CHECK(s.guardTripped && s.trips == 1u && !s.prewarmOff && s.entries == 60u);
  s = crashingSession(true);                // 2
  CHECK(s.trips == 2u && !s.prewarmOff);
  s = crashingSession(false);               // 3: no prewarm, so no crash, no marker left
  CHECK(s.trips == 3u && s.prewarmOff && s.entries == 60u && !exists(marker) && exists(trips));
  s = crashingSession(true);                // count kept across the skip session
  CHECK(!s.guardTripped && s.trips == 3u && !s.prewarmOff && s.entries == 60u);
  s = crashingSession(true);                // 4
  CHECK(s.trips == 4u && !s.prewarmOff && !s.fileDropped);
  s = crashingSession(true);                // 5
  CHECK(s.trips == 5u && !s.fileDropped);

  { // 6: the file is dropped; nothing to prewarm, so nothing to trip on.
    Workload w(4u, 20u, 60u, 21u);
    FakeBackend be;
    Rc<MtlStateCache> sc = new MtlStateCache(&be, cfg);
    w.announceAll(sc.ptr());
    waitLoaded(sc.ptr());
    CHECK(sc->waitPrewarmIdle(5000u));
    s = sc->stats();
    CHECK(s.guardTripped && s.trips == 6u && s.fileDropped && s.entries == 0u && s.built == 0u);
    CHECK(!exists(trips) && !exists(marker));
    w.render(sc.ptr(), be);                 // records afresh
    sc->shutdown();
    CHECK(sc->stats().entries == 60u);
  }

  { // Normal again: prewarm, drain, no trips.
    Workload w(4u, 20u, 60u, 21u);
    FakeBackend be;
    Rc<MtlStateCache> sc = new MtlStateCache(&be, cfg);
    w.announceAll(sc.ptr());
    waitLoaded(sc.ptr());
    CHECK(sc->waitPrewarmIdle(10000u));
    sc->shutdown();
    s = sc->stats();
    CHECK(!s.guardTripped && s.trips == 0u && s.built == 60u);
  }

  { // A drained queue resets a count left by earlier sessions.
    writeFile(trips, noSuspects(4u));
    Workload w(4u, 20u, 60u, 21u);
    FakeBackend be;
    Rc<MtlStateCache> sc = new MtlStateCache(&be, cfg);
    w.announceAll(sc.ptr());
    waitLoaded(sc.ptr());
    CHECK(sc->waitPrewarmIdle(10000u));
    for (int i = 0; i < 1000 && exists(trips); i++)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(sc->stats().trips == 4u && !exists(trips));
    sc->shutdown();
  }

  { // A crash while the marker was being rewritten: the temp file is torn,
    // the marker itself is the previous, complete one.
    std::vector<Entry> onDisk;
    auto d = readFile(cfg.file);
    parse(d.data(), d.size(), Sig, onDisk);
    std::string m = "d9mt-prewarm 1\ntrips 0\n";
    char hex[20];
    std::snprintf(hex, sizeof(hex), "%016llx\n", (unsigned long long)identityHash(onDisk[5]));
    m += hex;
    writeFile(marker, std::vector<uint8_t>(m.begin(), m.end()));
    writeFile(marker + ".tmp-999-1", std::vector<uint8_t>(m.begin(), m.begin() + 9));

    Workload w(4u, 20u, 60u, 21u);
    FakeBackend be;
    Rc<MtlStateCache> sc = new MtlStateCache(&be, cfg);
    w.announceAll(sc.ptr());
    waitLoaded(sc.ptr());
    CHECK(sc->waitPrewarmIdle(10000u));
    sc->shutdown();
    s = sc->stats();
    CHECK(s.guardTripped && s.suspects == 1u && s.trips == 1u && s.built == 59u);
    unlink((marker + ".tmp-999-1").c_str());
  }

  { // The marker is replaced atomically: at no point is it empty or torn
    // while prewarm jobs keep rewriting it.
    Workload w(4u, 20u, 60u, 21u);
    FakeBackend be;
    Rc<MtlStateCache> sc = new MtlStateCache(&be, configFor(dir, 2u));
    std::atomic<bool> stop = { false };
    std::atomic<uint32_t> torn = { 0u }, seen = { 0u };
    std::thread reader([&] {
      while (!stop.load()) {
        auto d = readFile(marker);
        if (!d.empty()) {
          seen++;
          const std::string t(d.begin(), d.end());
          if (t.compare(0u, 15u, "d9mt-prewarm 1\n") || t.find("trips ") == std::string::npos || t.back() != '\n')
            torn++;
        }
      }
    });
    w.announceAll(sc.ptr());
    waitLoaded(sc.ptr());
    CHECK(sc->waitPrewarmIdle(10000u));
    stop = true;
    reader.join();
    sc->shutdown();
    CHECK(torn.load() == 0u);
  }
}

// A draw that would wait for a prewarm build in progress raises the
// building thread (counted; the priority call itself is Win32 only).
static void testBoost() {
  const std::string dir = tempDir();
  const MtlStateCacheConfig cfg = configFor(dir, 1u);

  {
    Workload w(1u, 1u, 1u, 13u);
    FakeBackend be;
    Rc<MtlStateCache> sc = new MtlStateCache(&be, cfg);
    waitLoaded(sc.ptr());
    w.announceAll(sc.ptr());
    w.render(sc.ptr(), be);
    sc->shutdown();
  }

  Workload w(1u, 1u, 1u, 13u);
  std::atomic<bool> gate = { false }, started = { false };
  FakeBackend be;
  be.gate = &gate;
  be.started = &started;
  Rc<MtlStateCache> sc = new MtlStateCache(&be, cfg);
  w.announceAll(sc.ptr());
  waitLoaded(sc.ptr());
  for (int i = 0; i < 5000 && !started.load(); i++)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  CHECK(started.load());

  std::thread renderer([&] { w.render(sc.ptr(), be); });   // Late: boosts, then waits
  for (int i = 0; i < 2000 && !sc->stats().boosts; i++)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  gate = true;
  renderer.join();
  sc->shutdown();
  const auto s = sc->stats();
  CHECK(s.boosts == 1u && s.late == 1u && s.built == 1u);

  // A boost for a PSO no prewarm thread is building does nothing.
  sc->boostPrewarm(nullptr);
  CHECK(sc->stats().boosts == 1u);
}

// A failed prewarm build is not adopted: the draw builds its own.
static void testFailedPrewarmNotAdopted() {
  const std::string dir = tempDir();
  const MtlStateCacheConfig cfg = configFor(dir, 1u);
  {
    Workload w(2u, 4u, 20u, 15u);
    FakeBackend be;
    Rc<MtlStateCache> sc = new MtlStateCache(&be, cfg);
    waitLoaded(sc.ptr());
    w.announceAll(sc.ptr());
    w.render(sc.ptr(), be);
    sc->shutdown();
  }

  Workload w(2u, 4u, 20u, 15u);
  uint32_t failing = 0u;
  for (const auto& d : w.draws)
    failing += d.state[0] == 2u;
  CHECK(failing > 0u);

  FakeBackend be;
  be.failKeyByte = 2u;
  Rc<MtlStateCache> sc = new MtlStateCache(&be, cfg);
  w.announceAll(sc.ptr());
  waitLoaded(sc.ptr());
  CHECK(sc->waitPrewarmIdle(10000u));
  uint64_t hits = 0u;
  w.render(sc.ptr(), be, &hits);
  sc->shutdown();
  const auto s = sc->stats();
  CHECK(s.failed == failing && hits == 20u - failing && s.misses == failing);
  CHECK(s.entries == 20u);   // the draws built them: they stay in the file
}

// Render-thread cost of recording one PSO miss (the only state cache work
// on the render thread), against the same loop without it. `bench` mode.

// ---------------------------------------------------------------------------
// The journal (docs/state-cache-journal.md).

static void testJournalFormat() {
  std::vector<Entry> in;
  for (uint32_t i = 0u; i < 10u; i++) {
    Entry e = makeEntry(100u + i);
    e.lastSession = 3u + (i == 4u ? 4u : 0u);
    in.push_back(e);
  }
  std::vector<uint8_t> j = journalHeader(Sig);
  CHECK(j.size() == JournalHeaderSize);
  appendRecords(j, in);
  CHECK(j.size() == JournalHeaderSize + 10u * EntrySize);

  std::vector<Entry> out;
  JournalParseResult r = parseJournal(j.data(), j.size(), Sig, out);
  CHECK(r.status == ParseStatus::Ok && r.records == 10u && r.corrupt == 0u && r.tornBytes == 0u);
  CHECK(out.size() == 10u && r.maxSession == 7u);
  for (uint32_t i = 0u; i < 10u; i++)
    CHECK(!std::memcmp(&out[i], &in[i], sizeof(Entry)));

  // A torn tail (the last append cut short at any byte): every whole record kept.
  for (size_t cut = 1u; cut < EntrySize; cut += 37u) {
    std::vector<uint8_t> t(j.begin(), j.end() - std::ptrdiff_t(cut));
    r = parseJournal(t.data(), t.size(), Sig, out);
    CHECK(r.status == ParseStatus::Ok && out.size() == 9u && r.corrupt == 0u && r.tornBytes == EntrySize - cut);
  }

  // A bit flip in the middle record: only that one dropped.
  std::vector<uint8_t> f = j;
  f[JournalHeaderSize + 5u * EntrySize + 100u] ^= 0x10u;
  r = parseJournal(f.data(), f.size(), Sig, out);
  CHECK(r.status == ParseStatus::Ok && r.records == 9u && r.corrupt == 1u && out.size() == 9u && r.tornBytes == 0u);
  for (const Entry& e : out)
    CHECK(!sameIdentity(e, in[5]));
  // ... also in its check.
  f = j;
  f[JournalHeaderSize + 2u * EntrySize + 3u] ^= 0x01u;
  r = parseJournal(f.data(), f.size(), Sig, out);
  CHECK(r.corrupt == 1u && out.size() == 9u);

  // Resync: the file is never cut, so a torn append can be followed by whole ones (a kill, then
  // the next session's appends), and two writers' records can overlap. Every good record after
  // such a stretch is found again.
  auto rec = [&](uint32_t i) { return std::vector<uint8_t>(j.begin() + JournalHeaderSize + i * EntrySize,
                                                           j.begin() + JournalHeaderSize + (i + 1u) * EntrySize); };
  auto cat = [](std::vector<uint8_t>& to, const std::vector<uint8_t>& from, size_t a = 0u, size_t b = SIZE_MAX) {
    to.insert(to.end(), from.begin() + a, from.begin() + std::min(b, from.size()));
  };
  {
    std::vector<uint8_t> t = journalHeader(Sig);
    for (uint32_t i = 0u; i < 4u; i++) cat(t, rec(i));
    cat(t, rec(4u), 0u, 200u);                     // torn
    for (uint32_t i = 5u; i < 10u; i++) cat(t, rec(i));
    r = parseJournal(t.data(), t.size(), Sig, out);
    CHECK(r.status == ParseStatus::Ok && r.records == 9u && out.size() == 9u && r.corrupt == 1u && r.tornBytes == 0u);
    for (const Entry& e : out)
      CHECK(!sameIdentity(e, in[4]));
  }
  {
    std::vector<uint8_t> t = journalHeader(Sig);   // record 1 split by record 2 (an overlap)
    cat(t, rec(0u)); cat(t, rec(1u), 0u, 150u); cat(t, rec(2u)); cat(t, rec(1u), 150u); cat(t, rec(3u));
    r = parseJournal(t.data(), t.size(), Sig, out);
    CHECK(r.records == 3u && r.corrupt == 2u && r.tornBytes == 0u);
    CHECK(out.size() == 3u && !std::memcmp(&out[0], &in[0], sizeof(Entry))
       && !std::memcmp(&out[1], &in[2], sizeof(Entry)) && !std::memcmp(&out[2], &in[3], sizeof(Entry)));
  }
  {
    // Damage of any kind (overwritten, cut out, inserted, repeated bytes): a record that is
    // kept is always one that was written, and records away from the damage stay.
    std::mt19937 rng(1234u);
    uint32_t kept = 0u;
    for (int it = 0; it < 400; it++) {
      std::vector<uint8_t> t = j;
      const int ops = 1 + int(rng() % 3u);
      for (int o = 0; o < ops; o++) {
        const size_t at = JournalHeaderSize + rng() % (t.size() - JournalHeaderSize);
        const size_t len = 1u + rng() % 500u;
        switch (rng() % 4u) {
          case 0: for (size_t k = at; k < std::min(t.size(), at + len); k++) t[k] = uint8_t(rng()); break;
          case 1: t.erase(t.begin() + at, t.begin() + std::min(t.size(), at + len)); break;
          case 2: { std::vector<uint8_t> g(len); for (auto& b : g) b = uint8_t(rng()); t.insert(t.begin() + at, g.begin(), g.end()); break; }
          case 3: { const size_t from = JournalHeaderSize + rng() % (j.size() - JournalHeaderSize);
                    t.insert(t.begin() + at, j.begin() + from, j.begin() + std::min(j.size(), from + len)); break; }
        }
      }
      r = parseJournal(t.data(), t.size(), Sig, out);
      CHECK(r.status == ParseStatus::Ok);
      for (const Entry& e : out) {
        bool written = false;
        for (const Entry& w : in)
          written |= !std::memcmp(&e, &w, sizeof(Entry));
        CHECK(written);
      }
      kept += uint32_t(out.size());
    }
    CHECK(kept > 400u * 4u);   // most records survive 1-3 damaged spots
  }

  // Header cases.
  r = parseJournal(nullptr, 0u, Sig, out);
  CHECK(r.status == ParseStatus::Empty);
  r = parseJournal(j.data(), j.size(), Sig + 1u, out);
  CHECK(r.status == ParseStatus::KeyMismatch && out.empty());
  f = j; f[20] ^= 0x01u;   // the key signature, without fixing the check
  r = parseJournal(f.data(), f.size(), Sig, out);
  CHECK(r.status == ParseStatus::BadHeader);
  f = j; f[8] = uint8_t(JournalVersion + 1u);
  r = parseJournal(f.data(), f.size(), Sig, out);
  CHECK(r.status == ParseStatus::NewerVersion);
  std::vector<uint8_t> mainFile = serialize(in, Sig, 1u);   // the main file is no journal
  r = parseJournal(mainFile.data(), mainFile.size(), Sig, out);
  CHECK(r.status == ParseStatus::BadHeader);
  r = parseJournal(j.data(), JournalHeaderSize - 1u, Sig, out);
  CHECK(r.status == ParseStatus::BadHeader);
}

static void copyFileIfExists(const std::string& from, const std::string& to) {
  if (exists(from))
    writeFile(to, readFile(from));
  else
    unlink(to.c_str());
}

static void testJournalSessions() {
  const std::string dir = tempDir();
  const MtlStateCacheConfig cfg = configFor(dir, 1u);
  const std::string journal = cfg.file + ".journal";
  Workload w(4u, 20u, 60u, 21u);

  // A session that dies before its first write (the writer waits 20 s after the last change):
  // a copy of the directory once the journal holds the 60 records is the state a kill leaves.
  const std::string killed = tempDir();
  const MtlStateCacheConfig kcfg = configFor(killed, 1u);
  {
    FakeBackend be;
    Rc<MtlStateCache> sc = new MtlStateCache(&be, cfg);
    waitLoaded(sc.ptr());
    CHECK(!exists(journal));   // created by the first append, not at load
    w.announceAll(sc.ptr());
    w.render(sc.ptr(), be);
    for (int i = 0; i < 5000 && sc->stats().journaled < 60u; i++)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(sc->stats().journaled == 60u && sc->stats().writes == 0u);
    copyFileIfExists(cfg.file, kcfg.file);
    copyFileIfExists(journal, kcfg.file + ".journal");
    CHECK(!exists(kcfg.file) && exists(kcfg.file + ".journal"));
    sc->shutdown();
    CHECK(!exists(journal) && exists(cfg.file));   // folded in by the final write
  }
  CHECK(readFile(kcfg.file + ".journal").size() == JournalHeaderSize + 60u * EntrySize);

  // Next session after the kill: the journal's records are back and prewarmed.
  const std::vector<uint8_t> killedJournal = readFile(kcfg.file + ".journal");
  auto loadKilled = [&](uint64_t wantReplayed, uint64_t wantBuilt) {
    FakeBackend be;
    Rc<MtlStateCache> sc = new MtlStateCache(&be, kcfg);
    w.announceAll(sc.ptr());
    waitLoaded(sc.ptr());
    CHECK(sc->waitPrewarmIdle(10000u));
    const auto s = sc->stats();
    CHECK(s.journalReplayed == wantReplayed && s.entries == wantReplayed && s.built == wantBuilt);
    sc->shutdown();
    std::vector<Entry> onDisk;
    const ParseResult r = parse(readFile(kcfg.file).data(), readFile(kcfg.file).size(), Sig, onDisk);
    CHECK(r.status == ParseStatus::Ok && onDisk.size() == wantReplayed);
    // The killed session never wrote the file: its records are session 1, this one is 2.
    CHECK(r.session == 2u);
    for (const Entry& e : onDisk)
      CHECK(e.lastSession == 1u);
    CHECK(!exists(kcfg.file + ".journal"));
  };
  loadKilled(60u, 60u);

  // A torn tail (the kill hit an append): every whole record survives.
  unlink(kcfg.file.c_str());
  {
    std::vector<uint8_t> t = killedJournal;
    t.resize(t.size() - 100u);   // the last record cut short
    writeFile(kcfg.file + ".journal", t);
  }
  loadKilled(59u, 59u);

  // A bit flip in a record in the middle: that one dropped, the rest stays.
  unlink(kcfg.file.c_str());
  {
    std::vector<uint8_t> f = killedJournal;
    f[JournalHeaderSize + 30u * EntrySize + 200u] ^= 0x04u;
    writeFile(kcfg.file + ".journal", f);
  }
  loadKilled(59u, 59u);

  // The next appends after a torn tail: the file is not cut (another client may be appending),
  // they follow the fragment, and the parser finds every whole record on both sides of it.
  unlink(kcfg.file.c_str());
  {
    std::vector<uint8_t> t = killedJournal;
    t.resize(t.size() - 7u);
    writeFile(kcfg.file + ".journal", t);
    Workload w2(4u, 20u, 70u, 22u);   // other draws: new records to journal
    FakeBackend be;
    Rc<MtlStateCache> sc = new MtlStateCache(&be, kcfg);
    waitLoaded(sc.ptr());
    w2.announceAll(sc.ptr());
    w2.render(sc.ptr(), be);
    for (int i = 0; i < 5000 && sc->stats().journaled < 70u; i++)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const std::vector<uint8_t> after = readFile(kcfg.file + ".journal");
    CHECK(after.size() == t.size() + 70u * EntrySize);
    std::vector<Entry> recs;
    const JournalParseResult jr = parseJournal(after.data(), after.size(), Sig, recs);
    CHECK(jr.status == ParseStatus::Ok && jr.corrupt == 1u && jr.tornBytes == 0u && recs.size() == 59u + 70u);
    sc->shutdown();
  }

  // An old state file and no journal (every file before the journal): loads as before, and
  // nothing is created at load.
  {
    const std::string old = tempDir();
    const MtlStateCacheConfig ocfg = configFor(old, 0u);
    std::vector<Entry> es;
    for (uint32_t i = 0u; i < 20u; i++) { Entry e = makeEntry(300u + i); e.lastSession = 4u; e.sessionsUsed = 2u; es.push_back(e); }
    writeFile(ocfg.file, serialize(es, Sig, 4u));
    FakeBackend be;
    Rc<MtlStateCache> sc = new MtlStateCache(&be, ocfg);
    waitLoaded(sc.ptr());
    const auto s = sc->stats();
    CHECK(s.entries == 20u && s.journalReplayed == 0u && s.journaled == 0u);
    CHECK(!exists(ocfg.file + ".journal"));
    sc->shutdown();
    std::vector<Entry> onDisk;
    const ParseResult r = parse(readFile(ocfg.file).data(), readFile(ocfg.file).size(), Sig, onDisk);
    CHECK(r.status == ParseStatus::Ok && onDisk.size() == 20u);
  }

  // The crash-loop guard applies to the journal too: a suspect only the journal has is dropped.
  unlink(kcfg.file.c_str());
  writeFile(kcfg.file + ".journal", killedJournal);
  {
    std::vector<Entry> recs;
    parseJournal(killedJournal.data(), killedJournal.size(), Sig, recs);
    CHECK(recs.size() == 60u);
    if (recs.size() < 8u)
      return;
    char hex[24];
    std::snprintf(hex, sizeof(hex), "%016llx\n", (unsigned long long)identityHash(recs[7]));
    const std::string marker = std::string("d9mt-prewarm 1\ntrips 0\n") + hex;
    writeFile(kcfg.file + ".prewarm", std::vector<uint8_t>(marker.begin(), marker.end()));
    FakeBackend be;
    Rc<MtlStateCache> sc = new MtlStateCache(&be, kcfg);
    waitLoaded(sc.ptr());
    const auto s = sc->stats();
    CHECK(s.guardTripped && s.journalReplayed == 59u && s.entries == 59u);
    sc->shutdown();
  }

  // The journal's records as another set (forged, another key signature), on a fresh file.
  std::vector<Entry> recs;
  parseJournal(killedJournal.data(), killedJournal.size(), Sig, recs);
  auto fresh = [&](const std::vector<uint8_t>& j) {
    for (const char* ext : { "", ".prewarm", ".trips", ".journal" })
      unlink((kcfg.file + ext).c_str());
    writeFile(kcfg.file + ".journal", j);
  };

  // A forged record (a valid check, a key psoStateValid rejects): dropped, never prewarmed.
  {
    std::vector<Entry> forged = recs;
    forged[11].key[0] = 0xeeu;
    std::vector<uint8_t> j = journalHeader(Sig);
    appendRecords(j, forged);
    fresh(j);
    FakeBackend be;
    Rc<MtlStateCache> sc = new MtlStateCache(&be, kcfg);
    w.announceAll(sc.ptr());
    waitLoaded(sc.ptr());
    CHECK(sc->waitPrewarmIdle(10000u));
    const auto s = sc->stats();
    CHECK(s.journalReplayed == 59u && s.invalid == 1u && s.entries == 59u && s.built == 59u);
    sc->shutdown();
  }

  // A journal of another key signature (another d9mt's key layout): ignored and removed, the
  // main file's rules apply.
  {
    std::vector<uint8_t> j = journalHeader(Sig ^ 1u);
    appendRecords(j, recs);
    fresh(j);
    FakeBackend be;
    Rc<MtlStateCache> sc = new MtlStateCache(&be, kcfg);
    waitLoaded(sc.ptr());
    const auto s = sc->stats();
    CHECK(s.journalReplayed == 0u && s.entries == 0u);
    CHECK(!exists(kcfg.file + ".journal"));
    sc->shutdown();
  }

  // A write that fails (the directory read-only by then) leaves the journal whole.
  {
    const std::string ro = tempDir();
    const MtlStateCacheConfig rcfg = configFor(ro, 0u);
    FakeBackend be;
    Rc<MtlStateCache> sc = new MtlStateCache(&be, rcfg);
    waitLoaded(sc.ptr());
    w.announceAll(sc.ptr());
    w.render(sc.ptr(), be);
    for (int i = 0; i < 5000 && sc->stats().journaled < 60u; i++)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(sc->stats().journaled == 60u);
    chmod(ro.c_str(), 0555);
    sc->shutdown();
    chmod(ro.c_str(), 0755);
    CHECK(!exists(rcfg.file) && sc->stats().writes == 0u);
    CHECK(readFile(rcfg.file + ".journal").size() == JournalHeaderSize + 60u * EntrySize);
  }
}

// Opening the journal: missing, ours (never cut), a creator's short file, foreign, newer.
static void testJournalOpen() {
  const std::string dir = tempDir();
  const std::string path = dir + "/o.d9mtstate.journal";
  CHECK(MtlStateCache::openJournal(path, Sig) == JournalOpen::Ready);
  CHECK(readFile(path) == journalHeader(Sig));

  std::vector<uint8_t> t = journalHeader(Sig);
  appendRecords(t, { makeEntry(1u), makeEntry(2u) });
  t.resize(t.size() - 50u);   // a torn tail: kept as it is
  writeFile(path, t);
  CHECK(MtlStateCache::openJournal(path, Sig) == JournalOpen::Ready);
  CHECK(readFile(path) == t);

  writeFile(path, std::vector<uint8_t>(10u, uint8_t(0u)));   // a creator killed after the create
  CHECK(MtlStateCache::openJournal(path, Sig) == JournalOpen::Ready);
  CHECK(readFile(path) == journalHeader(Sig));

  writeFile(path, journalHeader(Sig ^ 1u));   // another key signature
  CHECK(MtlStateCache::openJournal(path, Sig) == JournalOpen::Ready);
  CHECK(readFile(path) == journalHeader(Sig));

  std::vector<uint8_t> newer = journalHeader(Sig);
  newer[8] = uint8_t(JournalVersion + 1u);
  writeFile(path, newer);
  CHECK(MtlStateCache::openJournal(path, Sig) == JournalOpen::Newer);
  CHECK(readFile(path) == newer);

  unlink(path.c_str());
  CHECK(MtlStateCache::appendJournalRecords(path, t) == JournalAppend::Gone);   // appends never create it
  CHECK(!exists(path));
}

// Two clients appending to one journal at once, a third opening it over and over (another
// client's first append of a session): nothing accepted is wrong, and natively (O_APPEND)
// nothing is lost. n013-perf/scripts/journal-wine.sh runs the same under Wine.
static void testJournalConcurrentAppend() {
  const std::string dir = tempDir();
  const std::string path = dir + "/c.d9mtstate.journal";
  CHECK(MtlStateCache::openJournal(path, Sig) == JournalOpen::Ready);

  constexpr uint32_t Writers = 2u, Batches = 300u;
  static const uint32_t sizes[3] = { 1u, 3u, 17u };
  std::atomic<bool> stop = { false };
  std::atomic<uint32_t> written = { 0u }, failed = { 0u };

  std::vector<std::thread> threads;
  for (uint32_t w = 0u; w < Writers; w++) {
    threads.emplace_back([&, w] {
      uint32_t seq = 0u;
      for (uint32_t b = 0u; b < Batches; b++) {
        std::vector<Entry> es;
        for (uint32_t k = 0u; k < sizes[(b + w) % 3u]; k++)
          es.push_back(makeEntry((w + 1u) * 100000u + seq++));
        std::vector<uint8_t> bytes;
        appendRecords(bytes, es);
        if (MtlStateCache::appendJournalRecords(path, bytes) == JournalAppend::Ok)
          written += uint32_t(es.size());
        else
          failed++;
      }
    });
  }
  std::thread opener([&] {
    while (!stop.load())
      if (MtlStateCache::openJournal(path, Sig) != JournalOpen::Ready)
        failed++;
  });
  for (auto& t : threads)
    t.join();
  stop = true;
  opener.join();

  std::vector<Entry> recs;
  const std::vector<uint8_t> data = readFile(path);
  const JournalParseResult r = parseJournal(data.data(), data.size(), Sig, recs);
  CHECK(failed.load() == 0u && r.status == ParseStatus::Ok);
  CHECK(r.records == written.load() && r.corrupt == 0u && r.tornBytes == 0u);
  std::unordered_set<uint64_t> seen;
  for (const Entry& e : recs) {
    const Entry want = makeEntry(e.firstUseMs);
    CHECK(!std::memcmp(&e, &want, sizeof(Entry)));   // exactly a written record
    CHECK(seen.insert(identityHash(e)).second);               // once
  }
}

// The journal lock: another client holding it (or frozen while holding it, SIGSTOP style)
// makes an append Busy within ~JournalLockWaitMs, never a long wait; the cache keeps those
// records pending and journals them once the lock is free, none lost.
static void testJournalLock() {
  const std::string dir = tempDir();
  const MtlStateCacheConfig cfg = configFor(dir, 0u);
  const std::string path = cfg.file + ".journal";
  CHECK(MtlStateCache::openJournal(path, Sig) == JournalOpen::Ready);

  const int holder = ::open(path.c_str(), O_RDONLY);
  CHECK(holder >= 0 && !::flock(holder, LOCK_EX));

  std::vector<uint8_t> bytes;
  appendRecords(bytes, { makeEntry(7u) });
  auto t0 = std::chrono::steady_clock::now();
  CHECK(MtlStateCache::appendJournalRecords(path, bytes) == JournalAppend::Busy);
  const auto waitedMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
  std::printf("journal lock: Busy after %lld ms (bound %u ms)\n", (long long)waitedMs, JournalLockWaitMs);
  // At least the lock's time bound; the upper bound allows for timer coalescing (a background-QoS
  // process, as scripts/test-state-cache.sh under taskpolicy -b, sleeps 1 ms as ~3 ms: ~60 ms
  // measured, ~21 ms at normal priority). The writer thread's real wait is measured under Wine
  // (n013-perf scripts/journal-wine.sh FROZEN=1, at THREAD_PRIORITY_LOWEST).
  CHECK(waitedMs >= JournalLockWaitMs && waitedMs < JournalLockWaitMs + 100);

  // A cache whose intakes all find the lock held: the records wait, nothing is dropped.
  Workload w(4u, 20u, 30u, 31u);
  {
    FakeBackend be;
    Rc<MtlStateCache> sc = new MtlStateCache(&be, cfg);
    waitLoaded(sc.ptr());
    w.announceAll(sc.ptr());
    w.render(sc.ptr(), be);
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));   // ~4 intakes, all Busy
    CHECK(sc->stats().journaled == 0u);
    ::flock(holder, LOCK_UN);
    ::close(holder);
    for (int i = 0; i < 5000 && sc->stats().journaled < 30u; i++)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(sc->stats().journaled == 30u);
    std::vector<Entry> recs;
    const std::vector<uint8_t> data = readFile(path);
    const JournalParseResult r = parseJournal(data.data(), data.size(), Sig, recs);
    CHECK(r.status == ParseStatus::Ok && r.records == 30u && r.corrupt == 0u);
    sc->shutdown();
  }

  // write() while another client holds the lock: the file is written and the journal kept
  // (only removed under the lock, so never with an append in flight).
  {
    Workload w2(4u, 20u, 10u, 32u);
    FakeBackend be;
    Rc<MtlStateCache> sc = new MtlStateCache(&be, cfg);
    waitLoaded(sc.ptr());
    w2.announceAll(sc.ptr());
    w2.render(sc.ptr(), be);
    for (int i = 0; i < 5000 && sc->stats().journaled < 10u; i++)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(sc->stats().journaled == 10u);
    const int h = ::open(path.c_str(), O_RDONLY);
    CHECK(h >= 0 && !::flock(h, LOCK_EX));
    sc->shutdown();
    CHECK(sc->stats().writes >= 1u && exists(path));
    ::flock(h, LOCK_UN);
    ::close(h);
  }

  // An append that opened the journal before a write() removed it, waiting for the lock: it
  // must not land in the removed file (Gone; Busy if its wait ran out first).
  {
    const std::string p2 = dir + "/r.d9mtstate.journal";
    CHECK(MtlStateCache::openJournal(p2, Sig) == JournalOpen::Ready);
    const int h = ::open(p2.c_str(), O_RDONLY);
    CHECK(h >= 0 && !::flock(h, LOCK_EX));
    std::atomic<int> res = { -1 };
    std::thread t([&] { res = int(MtlStateCache::appendJournalRecords(p2, bytes)); });
    // A spin, not a sleep: under taskpolicy -b a 5 ms sleep can outlast the append's 20 ms.
    for (auto t0 = std::chrono::steady_clock::now(); std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(3); ) { }
    ::unlink(p2.c_str());
    ::flock(h, LOCK_UN);
    t.join();
    struct stat st;
    CHECK(!::fstat(h, &st) && st.st_size == off_t(JournalHeaderSize));
    CHECK(res.load() == int(JournalAppend::Gone));
    std::printf("journal lock: append into a removed journal -> %s\n", res.load() == int(JournalAppend::Gone) ? "Gone" : "Busy");
    ::close(h);
  }
}

// Two processes on one cache: one's journal survives the other's write (it merges it first).
static void testJournalTwoProcesses() {
  const std::string dir = tempDir();
  const MtlStateCacheConfig cfg = configFor(dir, 0u);
  Workload wa(4u, 10u, 30u, 31u), wb(4u, 10u, 40u, 32u);
  FakeBackend bea, beb;
  Rc<MtlStateCache> a = new MtlStateCache(&bea, cfg);
  Rc<MtlStateCache> b = new MtlStateCache(&beb, cfg);
  waitLoaded(a.ptr());
  waitLoaded(b.ptr());
  wa.announceAll(a.ptr());
  wa.render(a.ptr(), bea);
  for (int i = 0; i < 5000 && a->stats().journaled < 30u; i++)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  wb.announceAll(b.ptr());
  wb.render(b.ptr(), beb);
  for (int i = 0; i < 5000 && b->stats().journaled < 40u; i++)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  b->shutdown();   // writes: its records and a's journaled ones, removes the journal
  std::vector<Entry> onDisk;
  parse(readFile(cfg.file).data(), readFile(cfg.file).size(), Sig, onDisk);
  CHECK(onDisk.size() == 70u);
  a->shutdown();
  parse(readFile(cfg.file).data(), readFile(cfg.file).size(), Sig, onDisk);
  CHECK(onDisk.size() == 70u);
}

static void benchRecord() {
  const std::string dir = tempDir();
  FakeBackend be;
  Rc<MtlStateCache> sc = new MtlStateCache(&be, configFor(dir, 0u));
  waitLoaded(sc.ptr());

  Rc<FakeShader> v = makeShader(MtlShaderStage::Vertex, 1u), p = makeShader(MtlShaderStage::Fragment, 2u);
  Rc<MtlShaderFunction> vf = v->drawFunction(nullptr), pf = p->drawFunction(v.ptr());
  Rc<FakePso> pso = new FakePso();
  pso->tryRun();
  uint8_t state[KeyBytes] = { };

  constexpr uint32_t Batch = 512u;   // below the ring size: the writer drains between batches
  constexpr uint32_t Rounds = 200u;
  uint64_t recordNs = 0u, emptyNs = 0u;
  volatile uint64_t sink = 0u;

  for (uint32_t r = 0u; r < Rounds; r++) {
    auto t0 = std::chrono::steady_clock::now();
    for (uint32_t i = 0u; i < Batch; i++) {
      state[0] = uint8_t(i);
      sc->record(state, vf.ptr(), pf.ptr(), pso.ptr(), MtlPsoUse::Miss);
    }
    auto t1 = std::chrono::steady_clock::now();
    for (uint32_t i = 0u; i < Batch; i++) {
      state[0] = uint8_t(i);
      sink = sink + state[0];
    }
    auto t2 = std::chrono::steady_clock::now();
    recordNs += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    emptyNs  += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count());
    std::this_thread::sleep_for(std::chrono::milliseconds(300));   // let the writer drain
  }

  const auto s = sc->stats();
  sc->shutdown();
  std::printf("state_cache_bench: record() %.1f ns per PSO miss (loop alone %.1f ns), %u records, ring drops %llu\n",
    double(recordNs) / double(Batch * Rounds), double(emptyNs) / double(Batch * Rounds),
    Batch * Rounds, (unsigned long long)s.ringDrops);
}

int main(int argc, char** argv) {
  if (argc > 1 && !std::strcmp(argv[1], "bench")) {
    benchRecord();
    return 0;
  }

  testFormat();
  testCorruption();
  testVersions();
  testDatabase();
  testCap();
  testIndex();
  testRing();
  testRecordLoadPrewarm();
  testFailedAndCorrupt();
  testTwoProcessesMerge();
  testConcurrency();
  testResolveConfig();
  testKeyValidator();
  testLoadValidation();
  testCrashGuard();
  testCrashChain();
  testBoost();
  testFailedPrewarmNotAdopted();
  testJournalFormat();
  testJournalSessions();
  testJournalOpen();
  testJournalConcurrentAppend();
  testJournalLock();
  testJournalTwoProcesses();

  if (g_failures) {
    std::printf("state_cache_test: %d FAILED\n", g_failures);
    return 1;
  }

  std::printf("state_cache_test: OK\n");
  return 0;
}
