// Native unit test for the D9MT_PROFILE STATS line (no Wine, no Metal):
//   clang++ -std=c++17 -I driver/d3d9 test/native/stats_test.cpp -o /tmp/st && /tmp/st
#include "d3d9_stats.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

using namespace dxvk;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static bool has(const std::string& line, const char* part) {
  return line.find(part) != std::string::npos;
}

int main() {
  constexpr uint64_t ms = 1000000u;
  stats::Window w;
  uint64_t t = 5000u * ms;

  CHECK(!w.addFrame(t));  // the first Present only starts the clock

  // 57 frames of 16 ms, one of 40 ms and one of 60 ms: 1.012 s, due at
  // the frame that crosses the second.
  bool due = false;
  for (int i = 0; i < 59; i++) {
    t += (i == 20 ? 40u : i == 40 ? 60u : 16u) * ms;
    due = w.addFrame(t);
    CHECK(due == (i == 58));
  }

  rstats::Snapshot rs;
  rs.v[rstats::Frames]        = 60u;
  rs.v[rstats::RenderPasses]  = 60u * 30u;
  rs.v[rstats::PassesSkipped] = 60u * 2u;
  rs.v[rstats::CmdBufs]       = 60u * 13u;
  rs.v[rstats::Draws]         = 60u * 1500u;
  rs.v[rstats::LoadLoad]      = 60u * 12u;
  rs.v[rstats::StoreDontCare] = 60u * 6u;
  rs.v[rstats::BytesSaved]    = 60u * (32u << 20);
  stats::CompileTotals ct;
  ct.shaders = 3u; ct.psos = 2u; ct.waits = 1u; ct.waitsNs = 12500000u;
  stats::Extra extra;
  extra.allocBytes = 5000ull << 20;

  std::string line = w.take(t, rs, ct, extra);
  std::printf("%s\n", line.c_str());
  CHECK(has(line, "STATS t=1 fps=58.3 "));
  CHECK(has(line, " ms=17.2/60.0/60.0 slow33=2 slow50=1 "));
  CHECK(has(line, "| rp=30.0 skip=2.0 cb=13.0 draws=1500 "));
  CHECK(has(line, "| ld=12.0 clr=0.0 ldDc=0.0 st=0.0 stDc=6.0 rsv=0.0 restore=0.0 savedMiB=32.0 "));
  CHECK(has(line, "| sh=3 pso=2 wait=1/12.5ms | allocMiB=5000 att=on"));

  // The next window reports deltas only, and a negative saving (restores).
  for (int i = 0; i < 63; i++) {
    due = w.addFrame(t += 16u * ms);
    CHECK(due == (i == 62));
  }
  rs.v[rstats::Frames] += 61u;
  rs.v[rstats::Restores] += 61u;
  rs.v[rstats::BytesSaved] -= 61u * (1u << 20);
  extra.att = "live-off";
  line = w.take(t, rs, ct, extra);
  std::printf("%s\n", line.c_str());
  CHECK(has(line, " ms=16.0/16.0/16.0 slow33=0 slow50=0 "));
  CHECK(has(line, " rp=0.0 "));
  CHECK(has(line, " restore=1.0 savedMiB=-1.0 "));
  CHECK(has(line, "| sh=0 pso=0 wait=0/0.0ms "));
  CHECK(has(line, " att=live-off"));

  // RESTORE: what the restores were for, per second and for the session.
  {
    CHECK(std::abs(w.seconds(t) - 2.02) < 0.01);

    rstore::Log log;
    rstore::Key shadow;
    shadow.format = 260u; shadow.width = shadow.height = 2048u;
    shadow.aspects = rstore::AspectStencil;
    shadow.reason = rstore::Reason::Sample; shadow.api = rstore::Api::Draw;
    rstore::Key scene;
    scene.format = 80u; scene.width = 3840u; scene.height = 2160u;
    scene.aspects = rstore::AspectColor;
    scene.reason = rstore::Reason::Copy; scene.api = rstore::Api::StretchRect;
    rstore::Key part = shadow;
    part.aspects = rstore::AspectDepth | rstore::AspectStencil;
    part.reason = rstore::Reason::PartialClear; part.api = rstore::Api::Clear;

    rstore::Window rw;
    CHECK(rw.take(1.0, 60u, log.snapshot()).empty());           // nothing restored: no line

    for (int i = 0; i < 120; i++) log.add(shadow);
    for (int i = 0; i < 6; i++)   log.add(scene);
    log.add(part);
    std::string r = rw.take(2.0, 120u, log.snapshot());
    std::printf("%s\n", r.c_str());
    CHECK(r == "RESTORE t=2 frames=60 n=127 perFrame=2.12"
               " | 120x 2048x2048 Depth32Float_Stencil8 s sample@Draw"
               " | 6x 3840x2160 BGRA8Unorm c copy@StretchRect"
               " | 1x 2048x2048 Depth32Float_Stencil8 ds partial-clear@Clear");

    CHECK(rw.take(3.0, 180u, log.snapshot()).empty());         // a second without any
    log.add(scene);
    r = rw.take(4.0, 240u, log.snapshot(), 0u);
    CHECK(r == "RESTORE t=4 frames=60 n=1 perFrame=0.02 | +1 other");

    r = rstore::summary(240u, log.snapshot(), 1u);
    std::printf("%s\n", r.c_str());
    CHECK(r == "RESTORE-SUMMARY frames=240 n=128 perFrame=0.53"
               " | 120x 2048x2048 Depth32Float_Stencil8 s sample@Draw | +8 other");
    CHECK(rstore::summary(0u, rstore::Entries()) == "RESTORE-SUMMARY frames=0 n=0 perFrame=0.00");

    // Bounded: keys beyond MaxKeys count as one default key.
    rstore::Log big;
    for (uint32_t i = 0u; i < rstore::Log::MaxKeys + 10u; i++) {
      rstore::Key k;
      k.width = i + 1u;
      big.add(k);
    }
    rstore::Entries e = big.snapshot();
    CHECK(e.size() == rstore::Log::MaxKeys);
    CHECK(e.back().key == rstore::Key() && e.back().count == 11u);

    CHECK(rstore::formatName(123456u) == "fmt123456");
    CHECK(std::string(rstore::apiName(rstore::Api(200))) == "Unknown");
    CHECK(std::string(rstore::reasonName(rstore::Reason::RtView)) == "rt-view");
  }

  if (g_fail) {
    std::printf("%d check(s) failed\n", g_fail);
    return 1;
  }
  std::printf("stats_test: OK\n");
  return 0;
}
