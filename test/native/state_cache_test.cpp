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

#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <unistd.h>

#include "mtl/mtl_state_cache.h"
#include "mtl/mtl_context.h"

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
  CHECK(!db.merge(b) && db.size() == 3u);   // index still right after erase
}

static void testCap() {
  Database db;
  for (uint32_t i = 0u; i < MaxEntries + 1000u; i++) {
    Entry e = makeEntry(i);
    e.lastSession  = 1u + i / 100u;   // later entries are newer
    e.sessionsUsed = 1u + (i % 7u);
    db.merge(e);
  }

  CHECK(db.applyCap(MaxEntries) == 1000u);
  CHECK(db.size() == MaxEntries);

  uint32_t minSession = ~0u;
  for (const Entry& e : db.entries())
    minSession = std::min(minSession, e.lastSession);
  // The 1000 oldest (sessions 1..10) went; session 11 is partly kept by sessionsUsed.
  CHECK(minSession >= 10u);

  // Lookups still work after the cap rebuilt the index.
  Entry newest = makeEntry(MaxEntries + 999u);
  newest.lastSession = 1u + (MaxEntries + 999u) / 100u;
  CHECK(!db.merge(newest));
  CHECK(db.size() == MaxEntries);
  CHECK(db.applyCap(MaxEntries) == 0u);
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
  uint64_t ns = 0u;
  void run() override {
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
  uint8_t failKeyByte = 0xffu;   // key[0] value whose builds fail

  static std::vector<uint8_t> keyOf(const uint8_t* state, const MtlShaderFunction* vs, const MtlShaderFunction* fs) {
    std::vector<uint8_t> k(state, state + KeyBytes);
    const uint64_t c[2] = { uint64_t(vs->library), fs ? uint64_t(fs->library) : 0u };
    k.insert(k.end(), reinterpret_cast<const uint8_t*>(c), reinterpret_cast<const uint8_t*>(c) + sizeof(c));
    return k;
  }

  // What RenderPsoCache::get does on a miss, minus Metal.
  Rc<FakePso> renderMiss(const uint8_t* state, const MtlShaderFunction* vs, const MtlShaderFunction* fs, MtlPsoUse& use) {
    Rc<FakePso> fresh = new FakePso();
    std::lock_guard lock(mutex);
    auto r = registry.emplace(keyOf(state, vs, fs), fresh);
    use = r.second ? MtlPsoUse::Miss : (r.first->second->isDone() ? MtlPsoUse::Hit : MtlPsoUse::Late);
    return r.first->second;
  }

  Rc<MtlCompileTask> createPrewarmPso(const uint8_t* state, const MtlShaderFunction* vs, const MtlShaderFunction* fs) override {
    Rc<FakePso> e = new FakePso();
    e->fail = state[0] == failKeyByte;
    std::lock_guard lock(mutex);
    if (!registry.emplace(keyOf(state, vs, fs), e).second)
      return nullptr;
    created++;
    return e;
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

  // Cap: a file with more than MaxEntries loads capped and is rewritten capped.
  {
    std::vector<Entry> many;
    for (uint32_t i = 0u; i < MaxEntries + 300u; i++) {
      Entry e = makeEntry(i);
      e.lastSession = 1u + i / 1000u;
      many.push_back(e);
    }
    writeFile(cfg.file, serialize(many, Sig, 6u));
    FakeBackend be;
    Rc<MtlStateCache> sc = new MtlStateCache(&be, configFor(dir, 0u));
    waitLoaded(sc.ptr());
    sc->shutdown();
    CHECK(sc->stats().entries == MaxEntries);
    std::vector<Entry> onDisk;
    auto d = readFile(cfg.file);
    parse(d.data(), d.size(), Sig, onDisk);
    CHECK(onDisk.size() == MaxEntries);
    CHECK(d.size() == HeaderSize + size_t(MaxEntries) * EntrySize);
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

// Render-thread cost of recording one PSO miss (the only state cache work
// on the render thread), against the same loop without it. `bench` mode.
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

  if (g_failures) {
    std::printf("state_cache_test: %d FAILED\n", g_failures);
    return 1;
  }

  std::printf("state_cache_test: OK\n");
  return 0;
}
