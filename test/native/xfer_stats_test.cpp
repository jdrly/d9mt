// Native unit test for the D9MT_PROFILE XFER line (StretchRect / ColorFill paths, #14), and the
// window taken under the lock the adds use (#194), under ThreadSanitizer:
//   clang++ -std=c++17 -g -O1 -fsanitize=thread -I driver/d3d9 test/native/xfer_stats_test.cpp -o /tmp/xst && /tmp/xst
#include "d3d9_xfer_stats.h"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <thread>

using namespace dxvk;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

int main() {
  // The device lock's part: add() and the take both hold it.
  std::mutex m;
  auto acquire = [&m] { return std::unique_lock<std::mutex>(m); };

  xfer::Stats s;
  CHECK(s.take(1.0, acquire).empty());                 // nothing happened: no line
  s.add(xfer::StretchBlit);
  s.add(xfer::BlitScaled);
  s.add(xfer::BlitLinear);
  s.add(xfer::StretchCopy);
  s.add(xfer::FillBc);
  s.add(xfer::FillRect);
  std::string line = s.take(2.4, acquire);
  CHECK(line == "XFER t=2 | stretch copy=1 resolve=0 blit=1 (scaled=1 linear=1 convert=0 msaa=0) skip=0"
                " | fill rt=0 rect=1 bc=1 skip=0");
  CHECK(s.take(3.0, acquire).empty());                 // reported once
  s.add(xfer::StretchBlit);
  s.add(xfer::FillSkip);
  s.add(xfer::StretchSkip);
  line = s.take(4.0, acquire);
  CHECK(line == "XFER t=4 | stretch copy=0 resolve=0 blit=1 (scaled=0 linear=0 convert=0 msaa=0) skip=1"
                " | fill rt=0 rect=0 bc=0 skip=1");
  CHECK(s.summary() == "XFER-SUMMARY | stretch copy=1 resolve=0 blit=2 (scaled=1 linear=1 convert=0 msaa=0) skip=1"
                       " | fill rt=0 rect=1 bc=1 skip=1");
  { // Two threads add (each under the lock) while a third takes windows: every add lands in
    // exactly one window, and nothing races (ThreadSanitizer).
    xfer::Stats r;
    constexpr uint64_t kAdds = 20000u;
    std::atomic<int> running = { 2 };
    auto adder = [&] (xfer::Counter a, xfer::Counter b) {
      for (uint64_t i = 0u; i < kAdds; i++) {
        { auto lock = acquire(); r.add(a); }
        { auto lock = acquire(); r.add(b); }
      }
      running--;
    };
    xfer::Counts sum;
    auto collect = [&] {
      const xfer::Counts d = r.takeCounts(acquire);
      for (uint32_t i = 0u; i < xfer::Count; i++) sum.v[i] += d.v[i];
    };
    uint64_t windows = 0u;
    std::thread t1(adder, xfer::StretchBlit, xfer::FillRt);
    std::thread t2(adder, xfer::StretchBlit, xfer::FillBc);
    while (running.load() > 0) { collect(); windows++; }
    t1.join();
    t2.join();
    collect();
    CHECK(sum.v[xfer::StretchBlit] == 2u * kAdds);
    CHECK(sum.v[xfer::FillRt] == kAdds && sum.v[xfer::FillBc] == kAdds);
    CHECK(sum.v[xfer::StretchCopy] == 0u);
    CHECK(r.takeCounts(acquire).v[xfer::StretchBlit] == 0u);   // all reported
    std::printf("race: %llu windows\n", (unsigned long long)windows);
  }

  if (g_fail) {
    std::printf("%d check(s) failed\n", g_fail);
    return 1;
  }
  std::printf("xfer_stats_test: OK\n");
  return 0;
}
