// Native unit test for the render-thread sampler's pure part (driver/util/util_rt_sample.h; no
// Wine, no Metal):
//   clang++ -std=c++17 -I driver/util test/native/rt_sample_test.cpp -o build/rst && build/rst
// D9MT_RT_SAMPLE's parse, the module table's lookup by address (with a control: an off-by-one
// lookup must fail the same check), its limits, a window's tally and its lines. The sampler under
// Wine: scripts/test-rt-sample.sh.
#include "util_rt_sample.h"

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

using namespace dxvk;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

using Find = std::function<int(const rtsample::ModuleTable&, uintptr_t)>;

// The lookup's contract on three modules, two of them adjacent: the number of addresses it gets
// wrong (0: right)
static int mappingErrors(const Find& find) {
  rtsample::ModuleTable t;
  t.add(0x10000u, 0x1000u, "a.dll");   // [0x10000, 0x11000)
  t.add(0x11000u, 0x2000u, "b.dll");   // [0x11000, 0x13000), right after a.dll
  t.add(0x20000u, 0x10u, "c.exe");     // [0x20000, 0x20010)
  const struct { uintptr_t addr; int want; } cases[] = {
    { 0x0u, -1 }, { 0xffffu, -1 },
    { 0x10000u, 0 }, { 0x10800u, 0 }, { 0x10fffu, 0 },
    { 0x11000u, 1 }, { 0x12fffu, 1 },
    { 0x13000u, -1 }, { 0x1ffffu, -1 },
    { 0x20000u, 2 }, { 0x2000fu, 2 },
    { 0x20010u, -1 }, { ~uintptr_t(0), -1 },
  };
  int errors = 0;
  for (const auto& c : cases) {
    if (find(t, c.addr) != c.want)
      errors++;
  }
  return errors;
}

static bool has(const std::string& line, const char* part) {
  return line.find(part) != std::string::npos;
}

int main() {
  // 1. the variable: unset and empty are off and valid; 1 to 1000 whole Hz; anything else invalid
  {
    bool bad = true;
    CHECK(rtsample::hzFromEnv(nullptr, &bad) == 0u && !bad);
    CHECK(rtsample::hzFromEnv("", &bad) == 0u && !bad);
    CHECK(rtsample::hzFromEnv("200", &bad) == 200u && !bad);
    CHECK(rtsample::hzFromEnv("1", &bad) == 1u && !bad);
    CHECK(rtsample::hzFromEnv("1000", &bad) == 1000u && !bad);
    CHECK(rtsample::hzFromEnv("0100", &bad) == 100u && !bad);
    for (const char* v : { "0", "000", "1001", "-5", "+5", "2.5", "200Hz", " 200", "200 ", "x",
                           "99999999999999999999" }) {
      bad = false;
      CHECK(rtsample::hzFromEnv(v, &bad) == 0u && bad);
    }
  }

  // 2. lookup by address, and the control: a lookup that takes a module's end as its own (<=
  //    instead of <) fails the same check, so the check tells the two apart
  {
    const Find real = [] (const rtsample::ModuleTable& t, uintptr_t a) { return t.find(a); };
    const Find inclusiveEnd = [] (const rtsample::ModuleTable& t, uintptr_t a) {
      for (uint32_t i = 0u; i < t.count(); i++) {
        if (a >= t.at(i).base && a - t.at(i).base <= t.at(i).size)
          return int(i);
      }
      return -1;
    };
    CHECK(mappingErrors(real) == 0);
    const int controlErrors = mappingErrors(inclusiveEnd);
    std::printf("mapping: real lookup 0 errors expected, got %d; off-by-one control %d errors\n",
      mappingErrors(real), controlErrors);
    CHECK(controlErrors > 0);
  }

  // 3. the table's limits: an empty or wrapping range isn't added, a long name is cut, the 65th
  //    module doesn't fit
  {
    rtsample::ModuleTable t;
    CHECK(t.add(0x1000u, 0u, "empty.dll") == -1);
    CHECK(t.add(~uintptr_t(0) - 0xfu, 0x100u, "wraps.dll") == -1);
    CHECK(t.add(0x1000u, 0x10u, "a-name-longer-than-the-forty-bytes-a-module-keeps.dll") == 0);
    CHECK(std::string(t.at(0).name).size() == sizeof(t.at(0).name) - 1u);
    for (uint32_t i = 1u; i < rtsample::kMaxModules; i++)
      CHECK(t.add(0x100000u * (i + 1u), 0x100u, "m.dll") == int(i));
    CHECK(t.count() == rtsample::kMaxModules);
    CHECK(t.add(0x7f000000u, 0x100u, "one-too-many.dll") == -1);
    CHECK(t.count() == rtsample::kMaxModules);
  }

  // 4. the leaf of a path
  {
    CHECK(std::string(rtsample::leafName("C:\\windows\\system32\\ntdll.dll")) == "ntdll.dll");
    CHECK(std::string(rtsample::leafName("/a/b/winemetal.dll")) == "winemetal.dll");
    CHECK(std::string(rtsample::leafName("plain.exe")) == "plain.exe");
    CHECK(std::string(rtsample::leafName("dir\\")).empty());
  }

  // 5. a window: its tally and its lines. b.dll 5, a.dll 3, c.exe 3 (after a.dll in the table,
  //    so after it in the lines), 1 sample outside every module and 1 with an index past the
  //    table: "(no module)" 2. 13 samples, held 1..13 us
  {
    rtsample::ModuleTable t;
    t.add(0x10000u, 0x1000u, "a.dll");
    t.add(0x11000u, 0x2000u, "b.dll");
    t.add(0x20000u, 0x10u, "c.exe");
    rtsample::Window w;
    const int idx[] = { 1, 0, 1, 2, 0, 1, -1, 2, 1, 0, 1, int(rtsample::kMaxModules), 2 };
    uint64_t ns = 0u;
    for (int i : idx)
      w.add(i, ns += 1000u);
    w.missed = 4u;
    CHECK(w.samples() == 13u);
    CHECK(w.noModule == 2u);
    CHECK(w.heldMaxNs == 13000u);

    const auto lines = rtsample::windowLines(t, w, 0x1f4u, 3u, 10.02, 200u, "every 10 s");
    for (const auto& l : lines)
      std::printf("%s\n", l.c_str());
    CHECK(lines.size() == 5u);
    CHECK(has(lines[0], "D9MT-RT-SAMPLE: thread 0x01f4, window 3 (every 10 s), 10.0 s, 13 samples at 200 Hz asked, 4 missed"));
    CHECK(has(lines[0], "held suspended 0.007 ms mean, 0.013 ms longest"));
    CHECK(lines.size() > 4u && lines[1] == "D9MT-RT-SAMPLE:    38.5 % b.dll (5)");
    CHECK(lines.size() > 4u && lines[2] == "D9MT-RT-SAMPLE:    23.1 % a.dll (3)");
    CHECK(lines.size() > 4u && lines[3] == "D9MT-RT-SAMPLE:    23.1 % c.exe (3)");
    CHECK(lines.size() > 4u && lines[4] == "D9MT-RT-SAMPLE:    15.4 % (no module) (2)");

    // an empty window: the first line only, no division by zero
    const auto empty = rtsample::windowLines(t, rtsample::Window(), 0x1f4u, 4u, 0.3, 200u, "last, device released");
    CHECK(empty.size() == 1u);
    CHECK(has(empty[0], "window 4 (last, device released), 0.3 s, 0 samples at 200 Hz asked, 0 missed, held suspended 0.000 ms mean"));
  }

  if (g_fail == 0)
    std::printf("rt_sample_test: ok\n");
  return g_fail == 0 ? 0 : 1;
}
