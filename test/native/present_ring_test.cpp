// Native test of the bridge's presented-time ring (bridge/unix/present_ring.h, the production
// code; the SCREEN line, d9mt #153). No Wine, no Metal.
//   clang++ -std=c++17 -I bridge/unix test/native/present_ring_test.cpp -o build/pr && build/pr
//   ThreadSanitizer: scripts/test-present-ring.sh (the same file with -fsanitize=thread)
// Deterministic: in-order records, a writer delayed after taking its number while the ring wraps,
// a writer delayed while owning its slot, an overwrite while the reader copies, a reader more than
// a ring behind, loss accounting (accepted + lost == taken), two streams, a stream released with
// handlers pending, a recreated stream. Stress: writer threads against a draining reader; every
// accepted record must be whole (its fields derive from its seq).
#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

static struct d9mt_present_ring* g_hookRing = nullptr;
static int g_hookWrites = 0;
static void afterCopy(struct d9mt_present_ring* r, struct d9mt_present_slot* s);
#define D9MT_PRESENT_RING_AFTER_COPY(ring, slot) afterCopy(ring, slot)
#include "present_ring.h"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static const uint32_t N = D9MT_PRESENT_RING;

// fields derived from seq, so a mixed record is detectable
static double subOf(uint64_t seq) { return double(seq) * 1.0; }
static double preOf(uint64_t seq) { return double(seq) * 2.0 + 0.5; }
static bool whole(const d9mt_present_rec& r) { return r.submit_s == subOf(r.seq) && r.presented_s == preOf(r.seq); }

static void present(d9mt_present_ring* r, int n) {
  for (int i = 0; i < n; i++) {
    const uint64_t seq = d9mt_present_ring_issue(r);
    d9mt_present_ring_record(r, seq, subOf(seq), preOf(seq));
  }
}

// a writer (seq must already be issued) that ran to completion inside the reader's copy
static void afterCopy(struct d9mt_present_ring* r, struct d9mt_present_slot* s) {
  (void)s;
  if (r != g_hookRing || g_hookWrites <= 0)
    return;
  while (g_hookWrites > 0) {
    g_hookWrites--;
    const uint64_t seq = d9mt_present_ring_issue(r);
    d9mt_present_ring_record(r, seq, subOf(seq), preOf(seq));
  }
}

static uint32_t drainAll(d9mt_present_ring* r, std::vector<d9mt_present_rec>& out, uint64_t* issued, uint64_t* lost) {
  d9mt_present_rec buf[256];
  uint32_t total = 0, n;
  do {
    n = d9mt_present_ring_drain(r, buf, 256u, issued, lost);
    for (uint32_t i = 0; i < n; i++) out.push_back(buf[i]);
    total += n;
  } while (n == 256u);
  return total;
}

int main() {
  // 1. in order, nothing lost
  {
    d9mt_present_ring* r = d9mt_present_ring_create();
    present(r, 100);
    std::vector<d9mt_present_rec> v; uint64_t issued = 0, lost = 0;
    CHECK(drainAll(r, v, &issued, &lost) == 100u);
    CHECK(issued == 100u && lost == 0u);
    bool ok = true;
    for (size_t i = 0; i < v.size(); i++) ok = ok && v[i].seq == i + 1 && whole(v[i]);
    CHECK(ok);
    CHECK(d9mt_present_ring_unref(r) == 1);
  }

  // 2. a writer delayed after taking its number: the ring wraps past it; when it resumes, its
  //    slot holds a newer present, so it gives up and writes nothing; every number is accounted
  {
    d9mt_present_ring* r = d9mt_present_ring_create();
    const uint64_t seq0 = d9mt_present_ring_issue(r);
    const uint64_t i0 = d9mt_present_ring_take(r);           // the delayed writer: number 0
    present(r, int(N));                                       // numbers 1..N; N lands on slot 0
    d9mt_present_slot* s = nullptr;
    CHECK(d9mt_present_ring_own(r, i0, &s) == 0);             // slot 0 has pub(N): gives up
    d9mt_present_ring_finish(r);
    (void)seq0;
    std::vector<d9mt_present_rec> v; uint64_t issued = 0, lost = 0;
    drainAll(r, v, &issued, &lost);
    bool ok = true;
    for (auto& x : v) ok = ok && whole(x) && x.seq != seq0;
    CHECK(ok);
    CHECK(v.size() + lost == N + 1u);                        // every taken number: accepted or lost
    CHECK(lost == 1u);
    CHECK(d9mt_present_ring_unref(r) == 1);
  }

  // 3. a writer delayed while it owns its slot (busy); the ring wraps: the writer of the same
  //    slot one lap later gives up instead of overlapping; the reader waits on the busy slot,
  //    and once the slow writer publishes, its record is whole; the late lap's number is lost
  {
    d9mt_present_ring* r = d9mt_present_ring_create();
    const uint64_t seq0 = d9mt_present_ring_issue(r);
    const uint64_t i0 = d9mt_present_ring_take(r);
    d9mt_present_slot* s0 = nullptr;
    CHECK(d9mt_present_ring_own(r, i0, &s0) == 1);            // busy(0)
    present(r, int(N));                                       // number N finds slot 0 busy: gives up
    std::vector<d9mt_present_rec> v; uint64_t issued = 0, lost = 0;
    drainAll(r, v, &issued, &lost);
    // the reader is more than a ring behind (N + 1 taken): number 0 is counted lost at once;
    // 1..N-1 accepted; number N's slot is busy with an older number and a writer is running: wait
    CHECK(v.size() == N - 1u && lost == 1u);
    d9mt_present_ring_fill(s0, seq0, subOf(seq0), preOf(seq0));
    d9mt_present_ring_publish(s0, i0);
    d9mt_present_ring_finish(r);
    drainAll(r, v, &issued, &lost);                          // now quiet: number N (gave up) is lost
    CHECK(v.size() == N - 1u && lost == 2u);
    bool ok = true;
    for (auto& x : v) ok = ok && whole(x);
    CHECK(ok);
    present(r, 5);                                            // the cursor moves on afterwards
    size_t before = v.size();
    drainAll(r, v, &issued, &lost);
    CHECK(v.size() == before + 5u && lost == 2u);
    CHECK(d9mt_present_ring_unref(r) == 1);
  }

  // 4. a writer that takes the slot while the reader copies it: the copy is discarded as lost
  //    (never accepted with mixed fields)
  {
    d9mt_present_ring* r = d9mt_present_ring_create();
    present(r, int(N));                                       // slots full, numbers 0..N-1
    g_hookRing = r;
    g_hookWrites = 1;                                         // inside the first copy: number N -> slot 0
    std::vector<d9mt_present_rec> v; uint64_t issued = 0, lost = 0;
    drainAll(r, v, &issued, &lost);
    g_hookRing = nullptr;
    bool ok = true;
    for (auto& x : v) ok = ok && whole(x);
    CHECK(ok);
    CHECK(lost == 1u);                                        // number 0, overwritten during its copy
    CHECK(v.size() == N);                                     // 1..N-1 and N
    CHECK(v.size() + lost == N + 1u);
    CHECK(d9mt_present_ring_unref(r) == 1);
  }

  // 5. a reader far behind: 3 laps written, one drain: the last N accepted, the rest lost
  {
    d9mt_present_ring* r = d9mt_present_ring_create();
    present(r, int(3 * N));
    std::vector<d9mt_present_rec> v; uint64_t issued = 0, lost = 0;
    drainAll(r, v, &issued, &lost);
    CHECK(v.size() == N && lost == 2u * N && issued == 3u * N);
    CHECK(!v.empty() && v.front().seq == 2u * N + 1u && v.back().seq == 3u * N);
    CHECK(d9mt_present_ring_unref(r) == 1);
  }

  // 6. a taken number whose writer hasn't reached its slot yet: the reader waits (not lost)
  {
    d9mt_present_ring* r = d9mt_present_ring_create();
    present(r, 3);
    const uint64_t seq = d9mt_present_ring_issue(r);
    const uint64_t i = d9mt_present_ring_take(r);
    std::vector<d9mt_present_rec> v; uint64_t issued = 0, lost = 0;
    drainAll(r, v, &issued, &lost);
    CHECK(v.size() == 3u && lost == 0u);
    d9mt_present_slot* s = nullptr;
    CHECK(d9mt_present_ring_own(r, i, &s) == 1);
    d9mt_present_ring_fill(s, seq, subOf(seq), preOf(seq));
    d9mt_present_ring_publish(s, i);
    d9mt_present_ring_finish(r);
    drainAll(r, v, &issued, &lost);
    CHECK(v.size() == 4u && lost == 0u && whole(v.back()));
    CHECK(d9mt_present_ring_unref(r) == 1);
  }

  // 7. two streams don't see each other's records; a stream released by its owner with handlers
  //    pending stays valid until the last handler ran; a recreated stream starts at zero
  {
    d9mt_present_ring* a = d9mt_present_ring_create();
    d9mt_present_ring* b = d9mt_present_ring_create();
    present(a, 10);
    present(b, 4);
    std::vector<d9mt_present_rec> va, vb; uint64_t ia = 0, la = 0, ib = 0, lb = 0;
    drainAll(a, va, &ia, &la);
    drainAll(b, vb, &ib, &lb);
    CHECK(va.size() == 10u && ia == 10u && vb.size() == 4u && ib == 4u && la == 0u && lb == 0u);
    const uint64_t s1 = d9mt_present_ring_issue(a), s2 = d9mt_present_ring_issue(a);  // pending
    CHECK(d9mt_present_ring_unref(a) == 0);                   // the owner (device) goes away
    d9mt_present_ring_record(a, s1, subOf(s1), preOf(s1));    // a late handler: still valid memory
    uint64_t i2 = d9mt_present_ring_take(a);
    d9mt_present_slot* s = nullptr;
    CHECK(d9mt_present_ring_own(a, i2, &s) == 1);
    d9mt_present_ring_fill(s, s2, subOf(s2), preOf(s2));
    d9mt_present_ring_publish(s, i2);
    CHECK(d9mt_present_ring_finish(a) == 1);                  // the last reference frees it
    d9mt_present_ring* c = d9mt_present_ring_create();        // the recreated device's stream
    std::vector<d9mt_present_rec> vc; uint64_t ic = 9, lc = 9;
    CHECK(drainAll(c, vc, &ic, &lc) == 0u && ic == 0u && lc == 0u);
    present(c, 2);
    drainAll(c, vc, &ic, &lc);
    CHECK(vc.size() == 2u && vc[0].seq == 1u && ic == 2u);
    CHECK(d9mt_present_ring_unref(b) == 1);
    CHECK(d9mt_present_ring_unref(c) == 1);
  }

  // 8. stress: 4 writer threads (each with pauses inside its slot) against a draining reader;
  //    afterwards every taken number is accepted or lost exactly once, every record whole
  {
    d9mt_present_ring* r = d9mt_present_ring_create();
    const int writers = 4, per = 20000;
    std::atomic<bool> stop{ false };
    std::vector<d9mt_present_rec> v;
    uint64_t issued = 0, lost = 0;
    std::thread reader([&] {
      while (!stop.load()) {
        drainAll(r, v, &issued, &lost);
        std::this_thread::yield();
      }
    });
    std::vector<std::thread> ws;
    for (int w = 0; w < writers; w++)
      ws.emplace_back([&, w] {
        for (int k = 0; k < per; k++) {
          const uint64_t seq = d9mt_present_ring_issue(r);
          const uint64_t i = d9mt_present_ring_take(r);
          d9mt_present_slot* s = nullptr;
          if (d9mt_present_ring_own(r, i, &s)) {
            atomic_store_explicit(&s->seq, seq, memory_order_relaxed);
            if (((k + w) & 63) == 0) std::this_thread::yield();   // a writer paused mid-record
            atomic_store_explicit(&s->submit_s, subOf(seq), memory_order_relaxed);
            atomic_store_explicit(&s->presented_s, preOf(seq), memory_order_relaxed);
            d9mt_present_ring_publish(s, i);
          }
          d9mt_present_ring_finish(r);
        }
      });
    for (auto& t : ws) t.join();
    stop.store(true);
    reader.join();
    drainAll(r, v, &issued, &lost);                          // quiet now: everything resolved
    bool ok = true;
    for (auto& x : v) ok = ok && whole(x);
    CHECK(ok);
    CHECK(issued == uint64_t(writers) * per);
    CHECK(v.size() + lost == uint64_t(writers) * per);
    std::printf("stress: %zu accepted, %llu lost of %d\n", v.size(), (unsigned long long)lost, writers * per);
    CHECK(d9mt_present_ring_unref(r) == 1);
  }

  std::printf("%s (%d failures)\n", g_fail ? "FAILED" : "ok", g_fail);
  return g_fail ? 1 : 0;
}
