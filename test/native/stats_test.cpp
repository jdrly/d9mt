// Native unit test for the D9MT_PROFILE STATS line (no Wine, no Metal):
//   clang++ -std=c++17 -I driver/d3d9 test/native/stats_test.cpp -o /tmp/st && /tmp/st
#include "d3d9_stats.h"

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

  if (g_fail) {
    std::printf("%d check(s) failed\n", g_fail);
    return 1;
  }
  std::printf("stats_test: OK\n");
  return 0;
}
