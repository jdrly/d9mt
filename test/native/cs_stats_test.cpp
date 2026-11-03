// Native unit test for the D9MT_PROFILE PROF-CS line (the CS handoff counters, d9mt #127):
//   clang++ -std=c++17 -I driver/cs test/native/cs_stats_test.cpp -o /tmp/cst && /tmp/cst
//
// Counting, per-frame normalisation, the busy/park split and its clamp, the window's reset and
// restart, a non-unit tick length, keys unique on the line and no prefix an existing parser
// matches (PROF frame=, STATS t=, PACE t=), and producer counters from several threads at once.
// The Stats object is driven directly, so nothing depends on D9MT_PROFILE.
#include "dxvk_cs_stats.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <thread>
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

int main() {
  cheap_clock::Calibration ns;  // a tick is 1 ns

  // An empty window: all zeros, the frame and window as given.
  {
    cs_stats::Stats s;
    s.startOnce(1000000u);
    const std::string line = s.take(256u, 256u, ns, 1000000u + 256u * 1000000u);
    CHECK(line.rfind("PROF-CS frame=256 win=256 per-frame:", 0) == 0);
    CHECK(val(line, "csChunk") == 0.0 && val(line, "cmd") == 0.0 && val(line, "csParkMs") == 0.0);
    CHECK(near(val(line, "csBusyMs"), 1.0));  // 256 ms of wall time, never parked: 1 ms/frame
  }

  // Counting and per-frame values over a 4-frame window of 40 ms.
  {
    cs_stats::Stats s;
    s.startOnce(10000000u);
    s.startOnce(99000000u);               // only the first start counts

    // producer: 8 handovers, 3 of which found the CS thread parked; 2 sync waits, 3 ms in all
    for (int i = 0; i < 8; i++)
      s.onHandover(i < 3);
    s.onSyncWait(1000000u);
    s.onSyncWait(2000000u);

    // consumer: 2 parks, 24 ms in all; 1 sync wake; 2 chunks
    s.onPark(20000000u);
    s.onPark(4000000u);
    s.onSyncWake();
    uint32_t a[cs_stats::kKinds] = { };
    a[uint32_t(DxvkCsCmdKind::Draw)]         = 10;
    a[uint32_t(DxvkCsCmdKind::VertexBuffer)] = 15;
    a[uint32_t(DxvkCsCmdKind::Constants)]    = 10;
    a[uint32_t(DxvkCsCmdKind::Prof)]         = 2;
    uint32_t b[cs_stats::kKinds] = { };
    b[uint32_t(DxvkCsCmdKind::Draw)]         = 6;
    b[uint32_t(DxvkCsCmdKind::IndexBuffer)]  = 4;
    b[uint32_t(DxvkCsCmdKind::Texture)]      = 8;
    b[uint32_t(DxvkCsCmdKind::Shader)]       = 4;
    b[uint32_t(DxvkCsCmdKind::State)]        = 12;
    b[uint32_t(DxvkCsCmdKind::Other)]        = 1;
    s.onRun(16384u, a);
    s.onRun(4096u, b);

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
    CHECK(near(val(line, "cmdVb"), 3.75));
    CHECK(near(val(line, "cmdIb"), 1.0));
    CHECK(near(val(line, "cmdConst"), 2.5));
    CHECK(near(val(line, "cmdTex"), 2.0));
    CHECK(near(val(line, "cmdShader"), 1.0));
    CHECK(near(val(line, "cmdState"), 3.0));
    CHECK(near(val(line, "cmdProf"), 0.5));
    CHECK(near(val(line, "cmdOther"), 0.25));

    // The kinds sum to cmd.
    double sum = 0.0;
    for (const char* k : kKindKeys)
      sum += val(line, k);
    CHECK(near(sum, val(line, "cmd")));

    // Keys unique on the line, and none an existing parser keys on.
    std::set<std::string> seen;
    for (const std::string& k : keys(line)) {
      CHECK(!k.empty());
      CHECK(seen.insert(k).second);
    }
    CHECK(seen.size() == 22u);
    CHECK(line.find("PROF frame=") == std::string::npos);
    CHECK(line.find("STATS t=") == std::string::npos);
    CHECK(line.find("PACE t=") == std::string::npos);
    // No key of the PROF line besides the window's frame and win.
    for (const char* k : { "draws", "chunkFlush", "submit", "psoMiss", "abRebuild", "evRead",
                           "unsubWait", "rt", "clear", "xfer", "barrier", "blit" })
      CHECK(!seen.count(k));

    // Reset: the next window starts where this one ended and counts from zero.
    const std::string next = s.take(1028u, 4u, ns, 58000000u);
    CHECK(val(next, "csChunk") == 0.0 && val(next, "cmd") == 0.0 && val(next, "csPark") == 0.0);
    CHECK(near(val(next, "csBusyMs"), 2.0));  // 8 ms / 4, nothing parked
  }

  // Park time over the wall time (several CS threads): busy clamps at 0, never negative.
  {
    cs_stats::Stats s;
    s.startOnce(1u);
    s.onPark(30000000u);
    s.onPark(30000000u);
    const std::string line = s.take(256u, 1u, ns, 1u + 40000000u);
    CHECK(val(line, "csBusyMs") == 0.0);
    CHECK(near(val(line, "csParkMs"), 60.0));
  }

  // A window that never started (no CS thread yet): no wall time, busy 0.
  {
    cs_stats::Stats s;
    const std::string line = s.take(256u, 256u, ns, 123456789u);
    CHECK(val(line, "csBusyMs") == 0.0);
  }

  // Ticks of 0.5 ns: times halve.
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

  // Producer counters from several threads at once lose nothing.
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
    const std::string line = s.take(256u, 1u, ns, 2u);
    CHECK(near(val(line, "csChunk"), 400000.0));
    CHECK(near(val(line, "csWake"), 50000.0));
  }

  if (g_fail) {
    std::printf("%d check(s) failed\n", g_fail);
    return 1;
  }
  std::printf("cs_stats_test: OK\n");
  return 0;
}
