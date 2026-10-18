// Native unit test for the D9MT_PROFILE XFER line (StretchRect / ColorFill paths, #14):
//   clang++ -std=c++17 -I driver/d3d9 test/native/xfer_stats_test.cpp -o /tmp/xst && /tmp/xst
#include "d3d9_xfer_stats.h"

#include <cstdio>

using namespace dxvk;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

int main() {
  xfer::Stats s;
  CHECK(s.take(1.0).empty());                 // nothing happened: no line
  s.add(xfer::StretchBlit);
  s.add(xfer::BlitScaled);
  s.add(xfer::BlitLinear);
  s.add(xfer::StretchCopy);
  s.add(xfer::FillBc);
  s.add(xfer::FillRect);
  std::string line = s.take(2.4);
  CHECK(line == "XFER t=2 | stretch copy=1 resolve=0 blit=1 (scaled=1 linear=1 convert=0 msaa=0) skip=0"
                " | fill rt=0 rect=1 bc=1 skip=0");
  CHECK(s.take(3.0).empty());                 // reported once
  s.add(xfer::StretchBlit);
  s.add(xfer::FillSkip);
  s.add(xfer::StretchSkip);
  line = s.take(4.0);
  CHECK(line == "XFER t=4 | stretch copy=0 resolve=0 blit=1 (scaled=0 linear=0 convert=0 msaa=0) skip=1"
                " | fill rt=0 rect=0 bc=0 skip=1");
  CHECK(s.summary() == "XFER-SUMMARY | stretch copy=1 resolve=0 blit=2 (scaled=1 linear=1 convert=0 msaa=0) skip=1"
                       " | fill rt=0 rect=1 bc=1 skip=1");
  if (g_fail) {
    std::printf("%d check(s) failed\n", g_fail);
    return 1;
  }
  std::printf("xfer_stats_test: OK\n");
  return 0;
}
