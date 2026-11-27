// Native unit test for the render-thread sampler's pure part (driver/util/util_rt_sample.h; no
// Wine, no Metal):
//   clang++ -std=c++17 -I driver/util test/native/rt_sample_test.cpp -o build/rst && build/rst
// D9MT_RT_SAMPLE's parse, the module table's lookup by address (with a control: an off-by-one
// lookup must fail the same check), its limits, the caller search (with a control: a search that
// takes any address inside a module fails the same check), a window's tally and its lines, the RVA
// lines. The sampler under Wine: scripts/test-rt-sample.sh.
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
      CHECK(t.add(0x100000u * uintptr_t(i + 1u), 0x100u, "m.dll") == int(i));
    CHECK(t.count() == rtsample::kMaxModules);
    CHECK(t.add(0x7ff00000u, 0x100u, "one-too-many.dll") == -1);
    CHECK(t.count() == rtsample::kMaxModules);
  }

  // 3b. names: case-insensitive; which modules are system DLLs, and their leaf index (the 17th
  //     and later are other modules); a code range outside the image is dropped
  {
    CHECK(rtsample::sameName("NTDLL.dll", "ntdll.DLL"));
    CHECK(!rtsample::sameName("ntdll.dll", "ntdll.dll2"));
    CHECK(!rtsample::sameName("ntdl", "ntdll"));
    CHECK(rtsample::isLeafName("UCRTBASE.DLL") && rtsample::isLeafName("kernelbase.dll"));
    CHECK(!rtsample::isLeafName("d3d9.dll") && !rtsample::isLeafName("d3dx9_43.dll"));

    rtsample::ModuleTable t;
    const rtsample::Range exeCode[] = { { 0x1010u, 0x1080u } };
    CHECK(t.add(0x1000u, 0x100u, "game.exe", exeCode, 1u) == 0);
    CHECK(t.at(0).leaf == -1 && t.at(0).codeRanges == 1u && t.at(0).inCode(0x1010u) && !t.at(0).inCode(0x1080u));
    const rtsample::Range past[] = { { 0x2010u, 0x2200u } };   // code past the image
    CHECK(t.add(0x2000u, 0x100u, "ntdll.dll", past, 1u) == 1);
    CHECK(t.at(1).leaf == 0 && t.at(1).codeRanges == 0u && !t.at(1).inCode(0x2010u));
    const rtsample::Range five[] = { { 0x3000u, 0x3010u }, { 0x3020u, 0x3030u }, { 0x3040u, 0x3050u },
                                     { 0x3060u, 0x3070u }, { 0x3080u, 0x3090u } };
    CHECK(t.add(0x3000u, 0x100u, "five.dll", five, 5u) == 2);   // the first four are kept
    CHECK(t.at(2).codeRanges == rtsample::kMaxCodeRanges && t.at(2).inCode(0x3065u) && !t.at(2).inCode(0x3085u));
    for (uint32_t i = 1u; i < rtsample::kMaxLeaves; i++)
      CHECK(t.at(uint32_t(t.add(0x10000u * uintptr_t(i + 1u), 0x100u, "msvcrt.dll"))).leaf == int(i));
    CHECK(t.at(uint32_t(t.add(0x900000u, 0x100u, "msvcrt.dll"))).leaf == -1);
  }

  // 3c. the caller search, and its control: a search that takes an address anywhere in a module
  //     (its data too) or in a system DLL fails the same check
  {
    rtsample::ModuleTable t;
    const rtsample::Range crtCode[] = { { 0x11000u, 0x12000u } };
    const rtsample::Range fxCode[]  = { { 0x21000u, 0x21800u }, { 0x22000u, 0x22800u } };   // a data gap between
    const rtsample::Range exeCode[] = { { 0x31000u, 0x33000u } };
    t.add(0x10000u, 0x4000u, "ucrtbase.dll", crtCode, 1u);   // 0: system DLL
    t.add(0x20000u, 0x4000u, "d3dx9_43.dll", fxCode, 2u);    // 1
    t.add(0x30000u, 0x4000u, "game.exe", exeCode, 1u);       // 2
    const uintptr_t slots[] = {
      0x7fff1234u,   // a stack value in no module
      0x11800u,      // a return address inside ucrtbase itself (system DLL): skipped
      0x20100u,      // a pointer to d3dx9's headers/data, not its code: skipped
      0x21900u,      // between d3dx9's two executable sections (data): skipped
      0x22234u,      // d3dx9's second executable section: the caller
      0x31000u,      // game.exe code, further up: not reached
    };
    uint32_t slot = 99u;
    CHECK(t.findCaller(slots, 6u, &slot) == 1 && slot == 4u);
    CHECK(t.findCaller(slots, 4u) == -1);    // only the first four slots: none
    CHECK(t.findCaller(slots + 5, 1u) == 2);
    CHECK(t.findCaller(slots, 0u) == -1);

    const auto anyAddress = [] (const rtsample::ModuleTable& tt, const uintptr_t* v, uint32_t n) {
      for (uint32_t i = 0u; i < n; i++) {
        if (tt.find(v[i]) >= 0)
          return tt.find(v[i]);
      }
      return -1;
    };
    const int control = anyAddress(t, slots, 6u);
    std::printf("caller: real search finds module 1 (d3dx9_43.dll), the control finds %d\n", control);
    CHECK(control != 1);

    // the second control: a search over the span from the first executable section to the last
    // takes the data between them, slot 3, not the return address in slot 4
    uint32_t spanSlot = 99u;
    for (uint32_t i = 0u; i < 6u && spanSlot == 99u; i++) {
      const int m = t.find(slots[i]);
      if (m >= 0 && t.at(uint32_t(m)).leaf < 0 && t.at(uint32_t(m)).codeRanges != 0u
       && slots[i] >= t.at(uint32_t(m)).code[0].begin
       && slots[i] < t.at(uint32_t(m)).code[t.at(uint32_t(m)).codeRanges - 1u].end)
        spanSlot = i;
    }
    std::printf("caller: real search takes slot %u, the span control slot %u\n", slot, spanSlot);
    CHECK(spanSlot != slot);
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

  // 6. a system DLL's line: its callers, the most first, then those with none found and with the
  //    stack not read; a caller index past the table counts as none found
  {
    rtsample::ModuleTable t;
    t.add(0x10000u, 0x1000u, "ntdll.dll");
    t.add(0x20000u, 0x1000u, "d3d9.dll");
    t.add(0x30000u, 0x1000u, "game.exe");
    rtsample::Window w;
    for (int i = 0; i < 10; i++)
      w.add(0, 1000u);
    w.add(2, 1000u);
    const int callers[] = { 1, 2, 1, 1, -1, -2, -2, 1, 2, int(rtsample::kMaxModules) + 5 };
    for (int c : callers)
      w.addCaller(t.at(0).leaf, c);
    w.addCaller(-1, 1);   // not a system DLL: ignored
    const auto lines = rtsample::windowLines(t, w, 0x10u, 1u, 1.0, 200u, "every 10 s");
    for (const auto& l : lines)
      std::printf("%s\n", l.c_str());
    CHECK(lines.size() == 3u);
    CHECK(lines.size() > 2u && lines[1] == "D9MT-RT-SAMPLE:    90.9 % ntdll.dll (10), called from d3d9.dll 4, game.exe 2, no caller found 2, stack not read 2");
    CHECK(lines.size() > 2u && lines[2] == "D9MT-RT-SAMPLE:     9.1 % game.exe (1)");

    rtsample::Window none;
    none.add(0, 1u);
    none.addCaller(0, -1);
    const auto l2 = rtsample::windowLines(t, none, 0x10u, 2u, 1.0, 200u, "every 10 s");
    CHECK(l2.size() == 2u && l2[1] == "D9MT-RT-SAMPLE:   100.0 % ntdll.dll (1), no caller found 1, stack not read 0");
  }

  // 7. the RVA lines: 16-byte buckets by RVA, 24 a line, nothing outside the image; too large an
  //    image has none
  {
    rtsample::RvaHistogram h;
    CHECK(!h.init(0u) && h.empty());
    CHECK(!h.init(rtsample::kMaxRvaImage + 1u) && h.empty());
    CHECK(h.init(0x1001u) && !h.empty());
    CHECK(h.lines("x.dll", "rva", 1u).empty());
    h.add(0x0u); h.add(0xfu); h.add(0x10u); h.add(0x1000u); h.add(0x1010u);   // the last: outside
    auto l = h.lines("x.dll", "rva", 7u);
    CHECK(l.size() == 1u && l[0] == "D9MT-RT-SAMPLE: x.dll rva window 7: 0=2 10=1 1000=1");
    for (uint32_t b = 0u; b < 30u; b++)
      h.add(0x100u + b * 16u);
    l = h.lines("x.dll", "caller rva", 8u);
    CHECK(l.size() == 2u);
    CHECK(l.size() == 2u && has(l[1], "D9MT-RT-SAMPLE: x.dll caller rva window 8: ") && has(l[1], " 1000=1"));
    h.clear();
    CHECK(h.lines("x.dll", "rva", 9u).empty());
  }

  if (g_fail == 0)
    std::printf("rt_sample_test: ok\n");
  return g_fail == 0 ? 0 : 1;
}
