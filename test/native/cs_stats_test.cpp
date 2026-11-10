// Native unit test for the D9MT_PROFILE PROF-CS line (the CS handoff counters, d9mt #127):
//   clang++ -std=c++17 -I driver/cs test/native/cs_stats_test.cpp -o /tmp/cst && /tmp/cst
//
//  1. Counting, per-frame values, the busy/park split and its clamp, the window's reset and
//     restart, a window that never started, a non-unit tick length.
//  2. The line: keys unique, no PROF key besides frame/win, no prefix an existing parser keys on
//     (PROF frame=, STATS t=, PACE t=).
//  3. Producers from several threads, and producers racing the window's reset: every window holds
//     whole events (csWake <= csChunk, sync ticks match their count), nothing is lost.
//  4. The frame boundary, through the production command classes (DxvkCsTypedCmd, DxvkCsDataCmd)
//     and the production loop (cs_stats::runCounted), plain and single-use: the command that ends
//     the window counts in it, the next command of the same chunk in the next window.
//  5. The sync wait (cs_stats::timedWait): already done counts nothing and doesn't wait; a real
//     wait counts once, with its time, and holds the waiter count while it waits.
// The Stats objects are driven directly, so nothing depends on D9MT_PROFILE.
#include "dxvk_cs_cmd.h"
#include "dxvk_cs_stats.h"

#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <set>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

using namespace dxvk;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

// The value of ` key=` in the line; NAN if missing.
static double val(const std::string& line, const char* key) {
  const std::string k = std::string(" ") + key + "=";
  const size_t i = line.find(k);
  if (i == std::string::npos)
    return NAN;
  return std::strtod(line.c_str() + i + k.size(), nullptr);
}

static bool near(double a, double b) {
  return std::fabs(a - b) <= 1e-3 * std::fmax(1.0, std::fabs(b));
}

static std::vector<std::string> keys(const std::string& line) {
  std::vector<std::string> out;
  for (size_t i = line.find('='); i != std::string::npos; i = line.find('=', i + 1)) {
    size_t b = i;
    while (b > 0 && (std::isalnum((unsigned char) line[b - 1]) || line[b - 1] == '_'))
      b--;
    out.push_back(line.substr(b, i - b));
  }
  return out;
}

static const char* kKindKeys[cs_stats::kKinds] = {
  "cmdOther", "cmdDraw", "cmdVb", "cmdIb", "cmdConst", "cmdTex", "cmdShader", "cmdState", "cmdProf"
};

static void commands(cs_stats::Stats& s, DxvkCsCmdKind kind, int n) {
  for (int i = 0; i < n; i++)
    s.onCommand(kind);
}

// A bump arena for command objects, as a chunk holds them.
struct Arena {
  alignas(64) char data[4096];
  size_t used = 0;
  DxvkCsCmd* head = nullptr;
  DxvkCsCmd** next = &head;

  template<DxvkCsCmdKind K, typename F>
  void push(F&& f) {
    using C = DxvkCsTypedCmd<std::decay_t<F>, K>;
    used = (used + alignof(C) - 1) & ~(alignof(C) - 1);
    C* c = new (&data[used]) C(std::move(f));
    used += sizeof(C);
    *next = c;
    next = c->chain();
  }

  template<DxvkCsCmdKind K, typename F>
  void pushData(F&& f) {  // a data command with no data, as a draw with its args elsewhere
    using C = DxvkCsDataCmd<std::decay_t<F>, int, K>;
    used = (used + alignof(C) - 1) & ~(alignof(C) - 1);
    C* c = new (&data[used]) C(std::move(f));
    used += sizeof(C);
    *next = c;
    next = c->chain();
  }

  void destroy() {
    for (DxvkCsCmd* c = head; c != nullptr; ) {
      DxvkCsCmd* n = c->next();
      c->~DxvkCsCmd();
      c = n;
    }
    head = nullptr;
  }
};

struct FakeClock {
  static inline uint64_t t = 0u;
  static uint64_t now() { return t; }
};

int main() {
  cheap_clock::Calibration ns;  // a tick is 1 ns

  // 1. An empty window: all zeros, the frame and window as given.
  {
    cs_stats::Stats s;
    s.startOnce(1000000u);
    const std::string line = s.take(256u, 256u, ns, 1000000u + 256u * 1000000u);
    CHECK(line.rfind("PROF-CS frame=256 win=256 per-frame:", 0) == 0);
    CHECK(val(line, "csChunk") == 0.0 && val(line, "cmd") == 0.0 && val(line, "csParkMs") == 0.0);
    CHECK(near(val(line, "csBusyMs"), 1.0));  // 256 ms of wall time, never parked: 1 ms/frame
  }

  // 1+2. Counting and per-frame values over a 4-frame window of 40 ms; the line's keys.
  {
    cs_stats::Stats s;
    s.startOnce(10000000u);
    s.startOnce(99000000u);               // only the first start counts

    // producer: 8 handovers, 3 of which found the CS thread parked; 2 sync waits, 3 ms in all
    for (int i = 0; i < 8; i++)
      s.onHandover(i < 3);
    s.onSyncWait(1000000u);
    s.onSyncWait(2000000u);

    // consumer: 2 parks, 24 ms in all; 1 sync wake; 2 chunks, 20 KiB; 72 commands (each kind a
    // multiple of 2, so every per-frame count is exact at the line's one decimal)
    s.onPark(20000000u);
    s.onPark(4000000u);
    s.onSyncWake();
    s.onRun(16384u);
    s.onRun(4096u);
    commands(s, DxvkCsCmdKind::Draw, 16);
    commands(s, DxvkCsCmdKind::VertexBuffer, 14);
    commands(s, DxvkCsCmdKind::IndexBuffer, 4);
    commands(s, DxvkCsCmdKind::Constants, 10);
    commands(s, DxvkCsCmdKind::Texture, 8);
    commands(s, DxvkCsCmdKind::Shader, 4);
    commands(s, DxvkCsCmdKind::State, 12);
    commands(s, DxvkCsCmdKind::Prof, 2);
    commands(s, DxvkCsCmdKind::Other, 2);

    const std::string line = s.take(1024u, 4u, ns, 50000000u);
    std::printf("%s\n", line.c_str());
    CHECK(line.rfind("PROF-CS frame=1024 win=4 per-frame:", 0) == 0);
    CHECK(near(val(line, "csChunk"), 2.0));
    CHECK(near(val(line, "csWake"), 0.75));
    CHECK(near(val(line, "csSyncWait"), 0.5));
    CHECK(near(val(line, "csSyncWaitMs"), 0.75));
    CHECK(near(val(line, "csRun"), 0.5));
    CHECK(near(val(line, "csKiB"), 5.0));     // 20 KiB / 4
    CHECK(near(val(line, "csPark"), 0.5));
    CHECK(near(val(line, "csParkMs"), 6.0));  // 24 ms / 4
    CHECK(near(val(line, "csBusyMs"), 4.0));  // (40 - 24) ms / 4
    CHECK(near(val(line, "csSyncWake"), 0.25));
    CHECK(near(val(line, "cmd"), 18.0));      // 72 / 4
    CHECK(near(val(line, "cmdDraw"), 4.0));
    CHECK(near(val(line, "cmdVb"), 3.5));
    CHECK(near(val(line, "cmdIb"), 1.0));
    CHECK(near(val(line, "cmdConst"), 2.5));
    CHECK(near(val(line, "cmdTex"), 2.0));
    CHECK(near(val(line, "cmdShader"), 1.0));
    CHECK(near(val(line, "cmdState"), 3.0));
    CHECK(near(val(line, "cmdProf"), 0.5));
    CHECK(near(val(line, "cmdOther"), 0.5));

    double sum = 0.0;
    for (const char* k : kKindKeys)
      sum += val(line, k);
    CHECK(near(sum, val(line, "cmd")));

    std::set<std::string> seen;
    for (const std::string& k : keys(line)) {
      CHECK(!k.empty());
      CHECK(seen.insert(k).second);
    }
    CHECK(seen.size() == 22u);
    CHECK(line.find("PROF frame=") == std::string::npos);
    CHECK(line.find("STATS t=") == std::string::npos);
    CHECK(line.find("PACE t=") == std::string::npos);
    for (const char* k : { "draws", "chunkFlush", "submit", "psoMiss", "abRebuild", "evRead",
                           "unsubWait", "rt", "clear", "xfer", "barrier", "blit" })
      CHECK(!seen.count(k));

    // Reset: the next window starts where this one ended and counts from zero.
    const std::string next = s.take(1028u, 4u, ns, 58000000u);
    CHECK(val(next, "csChunk") == 0.0 && val(next, "cmd") == 0.0 && val(next, "csPark") == 0.0);
    CHECK(val(next, "csSyncWait") == 0.0 && val(next, "csRun") == 0.0);
    CHECK(near(val(next, "csBusyMs"), 2.0));  // 8 ms / 4, nothing parked
  }

  // 1. Park time over the wall time (several CS threads): busy clamps at 0, never negative.
  {
    cs_stats::Stats s;
    s.startOnce(1u);
    s.onPark(30000000u);
    s.onPark(30000000u);
    const std::string line = s.take(256u, 1u, ns, 1u + 40000000u);
    CHECK(val(line, "csBusyMs") == 0.0);
    CHECK(near(val(line, "csParkMs"), 60.0));
  }

  // 1. A window that never started (no CS thread yet): no wall time, busy 0.
  {
    cs_stats::Stats s;
    const std::string line = s.take(256u, 256u, ns, 123456789u);
    CHECK(val(line, "csBusyMs") == 0.0);
  }

  // 1. Ticks of 0.5 ns: times halve.
  {
    cheap_clock::Calibration half;
    half.tsc = true;
    half.nsPerTick = 0.5;
    cs_stats::Stats s;
    s.startOnce(1000u);
    s.onPark(8000000u);                       // 4 ms
    s.onSyncWait(2000000u);                   // 1 ms
    const std::string line = s.take(256u, 1u, half, 1000u + 20000000u);  // 10 ms of wall time
    CHECK(near(val(line, "csParkMs"), 4.0));
    CHECK(near(val(line, "csSyncWaitMs"), 1.0));
    CHECK(near(val(line, "csBusyMs"), 6.0));
  }

  // 3. Producers from several threads at once lose nothing.
  {
    cs_stats::Stats s;
    s.startOnce(1u);
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; t++) {
      threads.emplace_back([&s, t] {
        for (int i = 0; i < 100000; i++)
          s.onHandover((i & 1) && t == 0);
      });
    }
    for (auto& t : threads)
      t.join();
    const cs_stats::Snapshot snap = s.takeSnapshot(2u);
    CHECK(snap.producer.chunks == 400000u);
    CHECK(snap.producer.wakes == 50000u);
  }

  // 3. Producers racing the window's reset: each window holds whole events.
  {
    constexpr int kThreads = 3;
    constexpr int kEvents = 200000;
    constexpr uint64_t kTicks = 7u;
    cs_stats::Stats s;
    s.startOnce(1u);
    std::atomic<int> running = { kThreads };
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; t++) {
      threads.emplace_back([&s, &running] {
        for (int i = 0; i < kEvents; i++) {
          s.onHandover(true);                 // every handover a wake: csWake == csChunk always
          if (!(i % 4))
            s.onSyncWait(kTicks);
        }
        running.fetch_sub(1);
      });
    }
    uint64_t chunks = 0u, wakes = 0u, syncs = 0u, syncTicks = 0u, windows = 0u;
    bool whole = true;
    auto account = [&] (const cs_stats::Snapshot& snap) {
      whole &= snap.producer.wakes == snap.producer.chunks;
      whole &= snap.producer.syncWaitTicks == snap.producer.syncWaits * kTicks;
      chunks += snap.producer.chunks;
      wakes += snap.producer.wakes;
      syncs += snap.producer.syncWaits;
      syncTicks += snap.producer.syncWaitTicks;
      windows++;
    };
    uint64_t now = 2u;
    while (running.load() > 0)
      account(s.takeSnapshot(now++));
    for (auto& t : threads)
      t.join();
    account(s.takeSnapshot(now++));
    CHECK(whole);
    CHECK(chunks == uint64_t(kThreads) * kEvents);
    CHECK(wakes == chunks);
    CHECK(syncs == uint64_t(kThreads) * (kEvents / 4));
    CHECK(syncTicks == syncs * kTicks);
    std::printf("concurrent reset: %llu windows\n", (unsigned long long) windows);
  }

  // 4. The frame boundary inside one chunk: Draw, VB, EndFrame (ends the window), Flush, Draw.
  for (int singleUse = 0; singleUse < 2; singleUse++) {
    cs_stats::Stats s;
    s.startOnce(1u);
    cs_stats::Snapshot atBoundary;
    std::vector<int> order;
    Arena chunk;
    chunk.push<DxvkCsCmdKind::Draw>([&order] (MtlContext*) { order.push_back(1); });
    chunk.push<DxvkCsCmdKind::VertexBuffer>([&order] (MtlContext*) { order.push_back(2); });
    chunk.push<DxvkCsCmdKind::Other>([&order, &s, &atBoundary] (MtlContext*) {
      order.push_back(3);
      atBoundary = s.takeSnapshot(100u);    // what prof::onFrameEnd does inside EndFrame
    });
    chunk.push<DxvkCsCmdKind::Other>([&order] (MtlContext*) { order.push_back(4); });
    chunk.pushData<DxvkCsCmdKind::Draw>([&order] (MtlContext*, int*, size_t) { order.push_back(5); });

    cs_stats::runCounted(s, chunk.head, static_cast<MtlContext*>(nullptr), chunk.used, singleUse != 0);
    const cs_stats::Snapshot after = s.takeSnapshot(200u);

    CHECK((order == std::vector<int>{ 1, 2, 3, 4, 5 }));
    // the window that ends at EndFrame: the chunk (it started there), Draw, VB and EndFrame itself
    CHECK(atBoundary.runs == 1u && atBoundary.bytes == chunk.used);
    CHECK(atBoundary.cmds[uint32_t(DxvkCsCmdKind::Draw)] == 1u);
    CHECK(atBoundary.cmds[uint32_t(DxvkCsCmdKind::VertexBuffer)] == 1u);
    CHECK(atBoundary.cmds[uint32_t(DxvkCsCmdKind::Other)] == 1u);
    // the next window: the flush and the data-command draw, not the chunk again
    CHECK(after.runs == 0u && after.bytes == 0u);
    CHECK(after.cmds[uint32_t(DxvkCsCmdKind::Other)] == 1u);
    CHECK(after.cmds[uint32_t(DxvkCsCmdKind::Draw)] == 1u);
    CHECK(after.cmds[uint32_t(DxvkCsCmdKind::VertexBuffer)] == 0u);

    if (!singleUse)
      chunk.destroy();                        // single-use: runCounted destroyed them
  }

  // 4. Kinds of the production command classes; the tag adds no size.
  {
    auto l = [] (MtlContext*) { };
    CHECK((sizeof(DxvkCsTypedCmd<decltype(l), DxvkCsCmdKind::Draw>) == sizeof(DxvkCsTypedCmd<decltype(l)>)));
    Arena a;
    a.push<DxvkCsCmdKind::Constants>([] (MtlContext*) { });
    a.push<DxvkCsCmdKind::Other>([] (MtlContext*) { });
    a.pushData<DxvkCsCmdKind::Shader>([] (MtlContext*, int*, size_t) { });
    CHECK(a.head->kind() == DxvkCsCmdKind::Constants);
    CHECK(a.head->next()->kind() == DxvkCsCmdKind::Other);
    CHECK(a.head->next()->next()->kind() == DxvkCsCmdKind::Shader);
    a.destroy();
  }

  // 5. The sync wait.
  {
    cs_stats::Stats s;
    uint32_t waiters = 0u;
    bool waited = false;

    // already done: no wait, nothing counted
    FakeClock::t = 1000u;
    cs_stats::timedWait<FakeClock>(s, waiters, [] { return true; }, [&] { waited = true; });
    CHECK(!waited);
    CHECK(waiters == 0u);
    cs_stats::Snapshot snap = s.takeSnapshot(1u);
    CHECK(snap.producer.syncWaits == 0u && snap.producer.syncWaitTicks == 0u);

    // not done: waits once, counts once with its time, holds the waiter count meanwhile
    uint32_t waitersInside = 0u;
    cs_stats::timedWait<FakeClock>(s, waiters, [] { return false; }, [&] {
      waited = true;
      waitersInside = waiters;
      FakeClock::t += 5000u;
    });
    CHECK(waited);
    CHECK(waitersInside == 1u);
    CHECK(waiters == 0u);
    snap = s.takeSnapshot(2u);
    CHECK(snap.producer.syncWaits == 1u && snap.producer.syncWaitTicks == 5000u);
  }

  if (g_fail) {
    std::printf("%d check(s) failed\n", g_fail);
    return 1;
  }
  std::printf("cs_stats_test: OK\n");
  return 0;
}
