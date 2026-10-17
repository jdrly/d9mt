// Native unit test for the D9MT_PROFILE PACE line (no Wine, no Metal):
//   clang++ -std=c++17 -I driver/d3d9 test/native/pace_test.cpp -o /tmp/pt && /tmp/pt
#include "d3d9_pace.h"

#include <cstdio>
#include <string>

using namespace dxvk;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static bool has(const std::string& line, const char* part) {
  return line.find(part) != std::string::npos;
}

int main() {
  constexpr uint64_t ms = 1000000u;
  pace::Window w;
  uint64_t t = 1000u * ms;

  // the first Present only starts the clocks
  w.presentEntry(t);
  CHECK(!w.frameEnd(t + 2u * ms, 0.0, 2.0, 0.0));

  // 59 more frames: Present entered every 16 ms, except one 26 ms gap and one
  // 6 ms one (sd > 0); 5 ms inside d9mt per frame, 1 ms of it waiting
  bool due = false;
  std::string line;
  for (int i = 0; i < 64 && !due; i++) {
    const uint64_t gap = (i == 10 ? 26u : i == 11 ? 6u : 16u) * ms;
    t += gap;
    w.presentEntry(t);
    due = w.frameEnd(t + 2u * ms, double(gap) * 1e-6, 5.0, 1.0);
    if (due)
      line = w.take(t + 2u * ms, i == 0 ? 0u : 500u * ms);
  }
  std::printf("%s\n", line.c_str());
  CHECK(due);
  CHECK(has(line, "PACE t=1 "));
  CHECK(has(line, " present=6.0/16.0/26.0 "));
  CHECK(!has(line, " sd=0.00 "));
  CHECK(has(line, "| d9mt=5.0/5.0/5.0 wait=1.0 "));
  CHECK(has(line, " game=1.0/11.0/21.0 |"));
  CHECK(has(line, " cpu=n/a d9mtBusy="));     // no CPU time before this window

  // second window: the thread's CPU time moved by 600 ms in about a second
  due = false;
  for (int i = 0; i < 64 && !due; i++) {
    t += 16u * ms;
    w.presentEntry(t);
    due = w.frameEnd(t + 2u * ms, 16.0, 4.0, 0.0);
    if (due)
      line = w.take(t + 2u * ms, 1100u * ms);
  }
  std::printf("%s\n", line.c_str());
  CHECK(due);
  CHECK(has(line, " present=16.0/16.0/16.0 sd=0.00 "));
  CHECK(has(line, " wait=0.0 game=12.0/12.0/12.0 |"));
  CHECK(has(line, " cpu=59"));                 // 600 ms over ~1.008 s
  CHECK(has(line, " gameCpu="));

  if (g_fail)
    std::printf("%d check(s) failed\n", g_fail);
  else
    std::printf("pace_test: all checks passed\n");
  return g_fail ? 1 : 0;
}
