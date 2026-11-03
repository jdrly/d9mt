// Native test of the bridge's presented-time ring (bridge/unix/present_ring.h, the production
// code; the SCREEN line, d9mt #153), and of the SCREEN window (driver/d3d9/d3d9_screen.h) fed
// from it. No Wine, no Metal.
//   clang++ -std=c++17 -I bridge/unix -I driver/d3d9 test/native/present_ring_test.cpp -o build/pr && build/pr
//   ThreadSanitizer: scripts/test-present-ring.sh (the same file with -fsanitize=thread)
// Deterministic: in-order records; a handler delayed across two drains (no false 2-refresh
// interval, no late); a handler unresolved past the hold bound (lost, the interval chain breaks);
// a writer delayed while owning its slot during a wrap; an overwrite while the reader copies; a
// reader more than a ring behind; pending accounting; break markers; streams A -> B -> A; a
// stream released with handlers pending, and a recreated one; a writer paused mid-write across
// timed drains (review 3). Stress: writer threads against a
// draining reader: every accepted record whole, seqs strictly increasing, accepted + lost ==
// issued. Each thread runs its own presents' handlers late and newest first, so handlers finish
// out of global seq order; there is no separate handoff queue.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

static struct d9mt_present_ring* g_hookRing = nullptr;
static int g_hookWrites = 0;
static void afterCopy(struct d9mt_present_ring* r, struct d9mt_present_slot* s);
#define D9MT_PRESENT_RING_AFTER_COPY(ring, slot) afterCopy(ring, slot)
#include "present_ring.h"
#include "d3d9_screen.h"

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

// writers that run to completion inside the reader's copy
static void afterCopy(struct d9mt_present_ring* r, struct d9mt_present_slot* s) {
  (void)s;
  if (r != g_hookRing || g_hookWrites <= 0)
    return;
  while (g_hookWrites > 0) {
    g_hookWrites--;
    present(r, 1);
  }
}

static uint32_t drainAll(d9mt_present_ring* r, std::vector<d9mt_present_rec>& out, double now,
                         uint64_t* pending, uint64_t* lostTotal) {
  d9mt_present_rec buf[256];
  uint32_t total = 0, n;
  do {
    uint64_t lostNew = 0;
    n = d9mt_present_ring_drain(r, buf, 256u, now, pending, &lostNew);
    if (lostTotal) *lostTotal += lostNew;
    for (uint32_t i = 0; i < n; i++) out.push_back(buf[i]);
    total += n;
  } while (n == 256u);
  return total;
}

// one SCREEN second: drain the stream into the window, take the line
static std::string second(d9mt_present_ring* r, dxvk::screen::Window& w, double now, double t) {
  d9mt_present_rec buf[256];
  uint64_t pending = 0, lostNew = 0;
  uint32_t n;
  do {
    n = d9mt_present_ring_drain(r, buf, 256u, now, &pending, &lostNew);
    w.addLost(lostNew);
    for (uint32_t i = 0; i < n; i++) w.add({ buf[i].seq, buf[i].submit_s, buf[i].presented_s });
  } while (n == 256u);
  return w.take(t, 60.0, 1000.0 / 60.0, pending);
}

static bool has(const std::string& line, const char* part) {
  return line.find(part) != std::string::npos;
}

int main() {
  const double R = 1.0 / 60.0;

  // 1. in order, nothing lost, nothing pending
  {
    d9mt_present_ring* r = d9mt_present_ring_create();
    present(r, 100);
    std::vector<d9mt_present_rec> v; uint64_t pending = 9, lost = 0;
    CHECK(drainAll(r, v, 0.0, &pending, &lost) == 100u);
    CHECK(pending == 0u && lost == 0u);
    bool ok = true;
    for (size_t i = 0; i < v.size(); i++) ok = ok && v[i].seq == i + 1 && whole(v[i]);
    CHECK(ok);
    CHECK(d9mt_present_ring_unref(r) == 1);
  }

  // 2. review 2 #2: the handler of p1 runs late, after p2's, across two drains. The reader holds
  //    at p1, so p0 -> p2 is never reported as a 2-refresh interval; p1 arrives in the next drain
  //    and both intervals are 1 refresh, none late.
  {
    d9mt_present_ring* r = d9mt_present_ring_create();
    dxvk::screen::Window w;
    const double T = 100.0;
    const uint64_t s0 = d9mt_present_ring_issue(r), s1 = d9mt_present_ring_issue(r), s2 = d9mt_present_ring_issue(r);
    d9mt_present_ring_record(r, s0, T - 0.02, T);
    d9mt_present_ring_record(r, s2, T + 2 * R - 0.02, T + 2 * R);   // p2's handler first
    std::string a = second(r, w, 10.0, 1.0);                        // p1 unresolved: held
    std::printf("%s\n", a.c_str());
    CHECK(has(a, "shown=1 dropped=0 pend=2 lost=0 onscreen=n/a refreshes=0/0/0 late=0"));
    d9mt_present_ring_record(r, s1, T + R - 0.02, T + R);           // p1's handler, late
    std::string b = second(r, w, 11.0, 2.0);
    std::printf("%s\n", b.c_str());
    CHECK(has(b, "shown=2 dropped=0 pend=0 lost=0 onscreen=16.7/0.00/16.7 refreshes=2/0/0 late=0"));
    CHECK(d9mt_present_ring_unref(r) == 1);
  }

  // 3. the same, but p1's handler is still running at the next drain (past the hold bound): p1
  //    is lost, and no interval spans the gap (p0 -> p2 is not a 2-refresh interval)
  {
    d9mt_present_ring* r = d9mt_present_ring_create();
    dxvk::screen::Window w;
    const double T = 200.0;
    const uint64_t s0 = d9mt_present_ring_issue(r), s1 = d9mt_present_ring_issue(r), s2 = d9mt_present_ring_issue(r);
    const uint64_t s3 = d9mt_present_ring_issue(r);
    d9mt_present_ring_record(r, s0, T - 0.02, T);
    d9mt_present_ring_record(r, s2, T + 2 * R - 0.02, T + 2 * R);
    d9mt_present_ring_record(r, s3, T + 3 * R - 0.02, T + 3 * R);
    std::string a = second(r, w, 10.0, 1.0);
    CHECK(has(a, "shown=1 dropped=0 pend=3 lost=0"));
    std::string b = second(r, w, 11.0, 2.0);                        // 1 s later: p1 given up
    std::printf("%s\n", b.c_str());
    CHECK(has(b, "shown=2 dropped=0 pend=0 lost=1 onscreen=16.7/0.00/16.7 refreshes=1/0/0 late=0"));
    d9mt_present_ring_record(r, s1, T + R - 0.02, T + R);           // p1's handler, much too late
    std::string c = second(r, w, 12.0, 3.0);
    CHECK(has(c, "shown=0 dropped=0 pend=0 lost=0"));               // not read again, not counted twice
    CHECK(d9mt_present_ring_unref(r) == 1);
  }

  // 4. a writer delayed while it owns its slot (busy); the ring wraps: the writer of the same
  //    slot one lap later gives up instead of overlapping; once the slow writer has published
  //    and every handler has run, that later position is lost; every position accounted once
  {
    d9mt_present_ring* r = d9mt_present_ring_create();
    const uint64_t s0 = d9mt_present_ring_issue(r);
    d9mt_present_slot* slot0 = nullptr;
    CHECK(d9mt_present_ring_own(r, s0, &slot0) == 1);              // busy(0)
    present(r, int(N));                                             // seq N+1 finds slot 0 busy: gives up
    std::vector<d9mt_present_rec> v; uint64_t pending = 0, lost = 0;
    drainAll(r, v, 0.0, &pending, &lost);
    // N + 1 issued: position 0 (more than a ring behind) is lost at once; 1..N-1 accepted;
    // position N (seq N+1): its slot is busy with position 0 and a handler is still running: held
    CHECK(v.size() == N - 1u && lost == 1u && pending == 1u);
    d9mt_present_ring_fill(slot0, s0, subOf(s0), preOf(s0));
    d9mt_present_ring_publish(slot0, s0);
    d9mt_present_ring_finish(r);
    drainAll(r, v, 0.05, &pending, &lost);                         // now quiet: seq N+1 is lost
    CHECK(v.size() == N - 1u && lost == 2u && pending == 0u);
    bool ok = true;
    for (auto& x : v) ok = ok && whole(x);
    CHECK(ok);
    present(r, 5);                                                  // the cursor moves on afterwards
    size_t before = v.size();
    drainAll(r, v, 0.1, &pending, &lost);
    CHECK(v.size() == before + 5u && lost == 2u && pending == 0u);
    CHECK(d9mt_present_ring_unref(r) == 1);
  }

  // 5. a writer that takes the slot while the reader copies it: the copy is discarded as lost
  //    (never accepted with mixed fields); the newer record is read in order afterwards
  {
    d9mt_present_ring* r = d9mt_present_ring_create();
    present(r, int(N));                                             // slots full, seq 1..N
    g_hookRing = r;
    g_hookWrites = 1;                                               // inside the first copy: seq N+1 -> slot 0
    std::vector<d9mt_present_rec> v; uint64_t pending = 0, lost = 0;
    drainAll(r, v, 0.0, &pending, &lost);
    g_hookRing = nullptr;
    drainAll(r, v, 0.0, &pending, &lost);
    bool ok = true, ordered = true;
    for (size_t i = 0; i < v.size(); i++) { ok = ok && whole(v[i]); if (i) ordered = ordered && v[i].seq > v[i - 1].seq; }
    CHECK(ok && ordered);
    CHECK(lost == 1u);                                              // seq 1, overwritten during its copy
    CHECK(v.size() == N && v.back().seq == N + 1u);
    CHECK(pending == 0u);
    CHECK(d9mt_present_ring_unref(r) == 1);
  }

  // 6. a reader far behind: 3 laps written, one drain: the last N accepted, the rest lost
  {
    d9mt_present_ring* r = d9mt_present_ring_create();
    present(r, int(3 * N));
    std::vector<d9mt_present_rec> v; uint64_t pending = 0, lost = 0;
    drainAll(r, v, 0.0, &pending, &lost);
    CHECK(v.size() == N && lost == 2u * N && pending == 0u);
    CHECK(!v.empty() && v.front().seq == 2u * N + 1u && v.back().seq == 3u * N);
    CHECK(d9mt_present_ring_unref(r) == 1);
  }

  // 7. break markers (times -1) are retired in order like records; the window breaks its chain
  //    there: no interval from before the marker to after it
  {
    d9mt_present_ring* r = d9mt_present_ring_create();
    dxvk::screen::Window w;
    const double T = 300.0;
    for (int i = 0; i < 3; i++) { uint64_t s = d9mt_present_ring_issue(r); d9mt_present_ring_record(r, s, T + i * R - 0.02, T + i * R); }
    uint64_t m = d9mt_present_ring_issue(r); d9mt_present_ring_record(r, m, -1.0, -1.0);
    for (int i = 0; i < 3; i++) { uint64_t s = d9mt_present_ring_issue(r); d9mt_present_ring_record(r, s, T + 5.0 + i * R - 0.02, T + 5.0 + i * R); }
    std::string a = second(r, w, 1.0, 1.0);
    std::printf("%s\n", a.c_str());
    CHECK(has(a, "shown=6 dropped=0 pend=0 lost=0 onscreen=16.7/0.00/16.7 refreshes=4/0/0 late=0"));
    CHECK(d9mt_present_ring_unref(r) == 1);
  }

  // 8. review 2 #1: streams A -> B -> A (the swapchain's presenters are cached per window):
  //    each stream keeps its own accounting; back on A nothing is pending and nothing reported
  //    twice; A's frames before and after the switch are separated by its break marker, and the
  //    window's rebind breaks the chain between streams
  {
    d9mt_present_ring* A = d9mt_present_ring_create();
    d9mt_present_ring* B = d9mt_present_ring_create();
    dxvk::screen::Window w;
    double T = 400.0;
    w.bindStream(1u);
    for (int i = 0; i < 100; i++) { uint64_t s = d9mt_present_ring_issue(A); d9mt_present_ring_record(A, s, T + i * R - 0.02, T + i * R); }
    std::string a = second(A, w, 1.0, 1.0);
    CHECK(has(a, "shown=100 dropped=0 pend=0 lost=0"));
    w.bindStream(2u);
    for (int i = 0; i < 60; i++) { uint64_t s = d9mt_present_ring_issue(B); d9mt_present_ring_record(B, s, T + 10.0 + i * R - 0.02, T + 10.0 + i * R); }
    std::string b = second(B, w, 2.0, 2.0);
    CHECK(has(b, "shown=60 dropped=0 pend=0 lost=0 onscreen=16.7/0.00/16.7 refreshes=59/0/0"));
    w.bindStream(1u);
    uint64_t m = d9mt_present_ring_issue(A); d9mt_present_ring_record(A, m, -1.0, -1.0);   // the presenter's marker
    for (int i = 0; i < 60; i++) { uint64_t s = d9mt_present_ring_issue(A); d9mt_present_ring_record(A, s, T + 20.0 + i * R - 0.02, T + 20.0 + i * R); }
    std::string c = second(A, w, 3.0, 3.0);
    std::printf("%s\n", c.c_str());
    CHECK(has(c, "shown=60 dropped=0 pend=0 lost=0 onscreen=16.7/0.00/16.7 refreshes=59/0/0 late=0"));
    CHECK(d9mt_present_ring_unref(A) == 1);
    CHECK(d9mt_present_ring_unref(B) == 1);
  }

  // 9. a stream released by its owner with handlers pending stays valid until the last handler
  //    ran; a recreated stream starts at zero
  {
    d9mt_present_ring* a = d9mt_present_ring_create();
    present(a, 10);
    const uint64_t s1 = d9mt_present_ring_issue(a), s2 = d9mt_present_ring_issue(a);   // pending
    CHECK(d9mt_present_ring_unref(a) == 0);                         // the owner (device) goes away
    d9mt_present_ring_record(a, s1, subOf(s1), preOf(s1));          // a late handler: still valid memory
    d9mt_present_slot* s = nullptr;
    CHECK(d9mt_present_ring_own(a, s2, &s) == 1);
    d9mt_present_ring_fill(s, s2, subOf(s2), preOf(s2));
    d9mt_present_ring_publish(s, s2);
    CHECK(d9mt_present_ring_finish(a) == 1);                        // the last reference frees it
    d9mt_present_ring* c = d9mt_present_ring_create();              // the recreated device's stream
    std::vector<d9mt_present_rec> vc; uint64_t pc = 9, lc = 0;
    CHECK(drainAll(c, vc, 0.0, &pc, &lc) == 0u && pc == 0u && lc == 0u);
    present(c, 2);
    drainAll(c, vc, 0.0, &pc, &lc);
    CHECK(vc.size() == 2u && vc[0].seq == 1u && pc == 0u);
    CHECK(d9mt_present_ring_unref(c) == 1);
  }

  // 10. review 3: a writer paused mid-write (it owns its slot: busy) across timed drains, with
  //     fewer than a ring of presents. The busy position waits under the same hold bound as an
  //     unwritten one: held at the first drain, lost at the next (a second later), so the later
  //     presents don't stay pending. The writer keeps its slot and publishes later; that record
  //     is never read, nothing is counted twice, and no interval spans the lost position.
  {
    d9mt_present_ring* r = d9mt_present_ring_create();
    dxvk::screen::Window w;
    const double T = 500.0;
    const uint64_t s0 = d9mt_present_ring_issue(r), s1 = d9mt_present_ring_issue(r), s2 = d9mt_present_ring_issue(r);
    d9mt_present_ring_record(r, s0, T - 0.02, T);
    d9mt_present_slot* slot1 = nullptr;
    CHECK(d9mt_present_ring_own(r, s1, &slot1) == 1);              // busy(1): paused mid-write
    d9mt_present_ring_record(r, s2, T + 2 * R - 0.02, T + 2 * R);
    std::string a = second(r, w, 10.0, 1.0);                        // held at the busy position
    std::printf("%s\n", a.c_str());
    CHECK(has(a, "shown=1 dropped=0 pend=2 lost=0"));
    std::string b = second(r, w, 11.0, 2.0);                        // past the bound: lost
    std::printf("%s\n", b.c_str());
    CHECK(has(b, "shown=1 dropped=0 pend=0 lost=1 onscreen=n/a refreshes=0/0/0 late=0"));
    d9mt_present_ring_fill(slot1, s1, T + R - 0.02, T + R);        // the writer resumes
    d9mt_present_ring_publish(slot1, s1);
    d9mt_present_ring_finish(r);
    const uint64_t s3 = d9mt_present_ring_issue(r);
    d9mt_present_ring_record(r, s3, T + 3 * R - 0.02, T + 3 * R);
    std::string c = second(r, w, 12.0, 3.0);
    std::printf("%s\n", c.c_str());
    CHECK(has(c, "shown=1 dropped=0 pend=0 lost=0 onscreen=16.7/0.00/16.7 refreshes=1/0/0 late=0"));
    CHECK(d9mt_present_ring_unref(r) == 1);
  }

  // 11. stress: 4 threads each issue presents and run their handlers late, in batches of 1-5,
  //     newest first, so handlers complete out of global seq order (within a thread and across
  //     threads; no separate handoff queue), against a draining reader; afterwards every issued
  //     present is accepted or lost exactly once, accepted records are whole and in seq order
  {
    d9mt_present_ring* r = d9mt_present_ring_create();
    const int threads = 4, per = 20000;
    std::atomic<bool> stop{ false };
    std::vector<d9mt_present_rec> v;
    uint64_t pending = 0, lost = 0;
    auto clock = [] { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); };
    std::thread reader([&] {
      while (!stop.load()) {
        drainAll(r, v, clock(), &pending, &lost);
        std::this_thread::yield();
      }
    });
    std::vector<std::thread> ws;
    for (int w = 0; w < threads; w++)
      ws.emplace_back([&, w] {
        std::vector<uint64_t> held;                                 // issued, handled later
        for (int k = 0; k < per; k++) {
          held.push_back(d9mt_present_ring_issue(r));
          if (held.size() >= size_t(1 + ((k * 7 + w) % 5))) {         // run 1-5 held handlers, newest first
            while (!held.empty()) {
              const uint64_t seq = held.back(); held.pop_back();
              d9mt_present_slot* s = nullptr;
              if (d9mt_present_ring_own(r, seq, &s)) {
                atomic_store_explicit(&s->seq, seq, memory_order_relaxed);
                if (((k + w) & 63) == 0) std::this_thread::yield();   // paused mid-record
                atomic_store_explicit(&s->submit_s, subOf(seq), memory_order_relaxed);
                atomic_store_explicit(&s->presented_s, preOf(seq), memory_order_relaxed);
                d9mt_present_ring_publish(s, seq);
              }
              d9mt_present_ring_finish(r);
            }
          }
        }
        for (uint64_t seq : held) d9mt_present_ring_record(r, seq, subOf(seq), preOf(seq));
      });
    for (auto& t : ws) t.join();
    stop.store(true);
    reader.join();
    drainAll(r, v, clock() + 10.0, &pending, &lost);              // quiet now: everything resolved
    bool ok = true, ordered = true;
    for (size_t i = 0; i < v.size(); i++) { ok = ok && whole(v[i]); if (i) ordered = ordered && v[i].seq > v[i - 1].seq; }
    CHECK(ok);
    CHECK(ordered);
    CHECK(pending == 0u);
    CHECK(v.size() + lost == uint64_t(threads) * per);
    std::printf("stress: %zu accepted, %llu lost of %d\n", v.size(), (unsigned long long)lost, threads * per);
    CHECK(d9mt_present_ring_unref(r) == 1);
  }

  std::printf("%s (%d failures)\n", g_fail ? "FAILED" : "ok", g_fail);
  return g_fail ? 1 : 0;
}
