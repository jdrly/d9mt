// Native unit test for the D9MT_PROFILE hitch attribution (no Wine, no Metal):
//   clang++ -std=c++17 -I driver/d3d9 test/native/hitch_test.cpp -o /tmp/ht && /tmp/ht
// Scopes only record with D9MT_PROFILE set (read during static
// initialisation), so the test re-runs itself with it set.
#include <cstdlib>
#include <unistd.h>

#include "d3d9_hitch.h"

#include <cstdio>
#include <cstring>
#include <thread>

using namespace dxvk;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static void spinMs(double ms) {
  const uint64_t end = hitch::nowNs() + uint64_t(ms * 1e6);
  while (hitch::nowNs() < end) { }
}

int main(int argc, char** argv) {
  (void) argc;
  if (!hitch::g_enabled) {
    setenv("D9MT_PROFILE", "1", 1);
    execv(argv[0], argv);
    std::printf("FAIL: could not re-run with D9MT_PROFILE=1\n");
    return 1;
  }

  { // Formatting: sorted by ms, bytes and bufLock waits shown, cap respected.
    hitch::FrameStats s;
    s.dtMs = 512.0; s.inMs = 480.0; s.csBehind = 3u;
    s.cat[hitch::CsSync]    = { 2u, 450.0, 0u, 0u };
    s.cat[hitch::TexUpload] = { 40u, 12.5, 30u << 20, 0u };
    s.cat[hitch::TexCreate] = { 40u, 3.0, 22u << 20, 0u };
    s.cat[hitch::BufLock]   = { 9u, 0.4, 0u, 2u };
    s.renderWaits = 1u; s.renderWaitMs = 5.0;
    std::string line = hitch::formatHitch(77u, s);
    std::printf("%s\n", line.c_str());
    CHECK(line.rfind("HITCH frame=77 dtMs=512.0 inD9mtMs=480.0 (94%)", 0) == 0);
    CHECK(line.find("texMiB created=22.0 uploaded=30.0") != std::string::npos);
    CHECK(line.find("csBehind=3") != std::string::npos);
    const size_t a = line.find("csSync=2/450.0ms"), b = line.find("texUpload=40/12.5ms/30.0MiB"),
                 c = line.find("texCreate=40/3.0ms/22.0MiB"), d = line.find("bufLock=9/0.4ms(waits 2)");
    CHECK(a != std::string::npos && b != std::string::npos && c != std::string::npos && d != std::string::npos);
    CHECK(a < b && b < c && c < d);
    CHECK(line.find("present=") == std::string::npos);  // zero-count categories omitted
    CHECK(line.find("cs: renderWait=1/5.0ms") != std::string::npos);
    CHECK(line.find("skipped=") == std::string::npos);    // no skips: not shown
    s.skipped = 7u; s.budgetHits = 2u;
    CHECK(hitch::formatHitch(77u, s).find("psos=0/0.0ms skipped=7 budgetHit=2") != std::string::npos);
    s.skipped = 0u; s.budgetHits = 0u;

    std::string top2 = hitch::formatHitch(77u, s, 2u);
    CHECK(top2.find("texCreate=") == std::string::npos && top2.find("texUpload=") != std::string::npos);

    hitch::FrameStats empty;
    empty.dtMs = 60.0;
    CHECK(hitch::formatHitch(1u, empty).find("| none |") != std::string::npos);
  }

  { // Profiler: nested entries count once towards inD9mt, categories add up,
    // a frame under the threshold logs nothing, one over it does.
    hitch::Profiler p;
    hitch::Profiler::CompileTotals compile;
    uint64_t frame = 0u;
    hitch::FrameStats s;

    CHECK(!p.endFrame(hitch::ticks(), hitch::nowNs(), compile, 0u, 50.0, frame, s));  // first frame
    spinMs(25.0);                     // calibration window (>= 20 ms)
    CHECK(!p.endFrame(hitch::ticks(), hitch::nowNs(), compile, 0u, 50.0, frame, s));  // short frame

    { hitch::Scope outer(p, hitch::Present, true);           // entry
      { hitch::Scope inner(p, hitch::TexCreate, true);       // nested entry: not outer
        inner.addBytes(5u << 20);
        spinMs(20.0);
      }
      { hitch::Scope lockLike(p, hitch::BufLock);
        lockLike.addAux();
        spinMs(5.0);
      }
      { hitch::Scope unlockLike(p, hitch::BufLock);
        unlockLike.noCount();
        spinMs(1.0);
      }
    }
    spinMs(40.0);                     // the game's own time
    compile.waits = 2u; compile.waitsNs = 3000000u;
    compile.skipped = 5u; compile.budgetHits = 1u;

    CHECK(p.endFrame(hitch::ticks(), hitch::nowNs(), compile, 4u, 50.0, frame, s));
    std::printf("%s\n", hitch::formatHitch(frame, s).c_str());
    CHECK(frame == 3u);
    CHECK(s.dtMs >= 66.0);
    CHECK(s.inMs >= 25.0 && s.inMs < 45.0);                // 26 ms inside, not doubled by nesting
    CHECK(s.cat[hitch::Present].n == 1u && s.cat[hitch::TexCreate].n == 1u);
    CHECK(s.cat[hitch::TexCreate].bytes == (5u << 20));
    CHECK(s.cat[hitch::TexCreate].ms >= 19.0 && s.cat[hitch::TexCreate].ms < 35.0);
    CHECK(s.cat[hitch::BufLock].n == 1u && s.cat[hitch::BufLock].aux == 1u);
    CHECK(s.cat[hitch::BufLock].ms >= 5.5);
    CHECK(s.renderWaits == 2u && s.renderWaitMs > 2.9 && s.renderWaitMs < 3.1);
    CHECK(s.skipped == 5u && s.budgetHits == 1u);
    CHECK(s.csBehind == 4u);

    // Counters were consumed: an idle long frame shows nothing inside d9mt,
    // and compile deltas are per frame.
    spinMs(55.0);
    CHECK(p.endFrame(hitch::ticks(), hitch::nowNs(), compile, 0u, 50.0, frame, s));
    CHECK(s.inMs == 0.0 && s.cat[hitch::Present].n == 0u && s.renderWaits == 0u);
    CHECK(s.skipped == 0u && s.budgetHits == 0u);
  }

  { // Entries from two threads: depth never goes negative or sticks.
    hitch::Profiler p;
    auto work = [&p] {
      for (int i = 0; i < 20000; i++) {
        hitch::Scope e(p, hitch::Query, true);
        hitch::Scope n(p, hitch::CsSync);
      }
    };
    std::thread t1(work), t2(work);
    t1.join(); t2.join();
    hitch::Profiler::CompileTotals compile;
    uint64_t frame = 0u;
    hitch::FrameStats s;
    p.endFrame(hitch::ticks(), hitch::nowNs(), compile, 0u, 1e9, frame, s);
    { hitch::Scope e(p, hitch::Query, true);     // must be outer again
      spinMs(1.0);
    }
    spinMs(25.0);
    CHECK(p.endFrame(hitch::ticks(), hitch::nowNs(), compile, 0u, 0.001, frame, s));
    CHECK(s.cat[hitch::Query].n == 1u);
    CHECK(s.inMs >= 0.9);
  }

  if (g_fail) {
    std::printf("%d check(s) failed\n", g_fail);
    return 1;
  }
  std::printf("hitch_test: all checks passed\n");
  return 0;
}
