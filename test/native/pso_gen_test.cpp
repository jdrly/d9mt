// Native unit test for PsoGenRing (driver/backend/mtl_pso_gen.h), the content-addressed
// generations of the PSO MRU snapshot:
//  - one generation never stands for two different contents (the MRU stays exact), over a
//    long random sequence that keeps evicting;
//  - content among the last N distinct ones gets its generation back (A->B->A hits);
//  - generation 0 (the state at construction) is never handed out.
//   clang++ -std=c++17 -I driver/backend test/native/pso_gen_test.cpp -o /tmp/pgt && /tmp/pgt
#include "mtl_pso_gen.h"

#include <cstdio>
#include <deque>
#include <map>
#include <random>
#include <vector>

using namespace dxvk::backend;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static auto eq = [] (const std::vector<int>& a, const std::vector<int>& b) { return a == b; };
static auto as = [] (std::vector<int>& a, const std::vector<int>& b) { a = b; };

int main() {
  constexpr uint32_t N = 8u;
  PsoGenRing<std::vector<int>, N> ring;

  // A -> B -> A: A's generation comes back.
  std::vector<int> a = { 1, 2 }, b = { 3 };
  uint32_t ga = ring.intern(a, eq, as), gb = ring.intern(b, eq, as);
  CHECK(ga != 0u && gb != 0u && ga != gb);
  CHECK(ring.intern(a, eq, as) == ga);
  CHECK(ring.intern(b, eq, as) == gb);

  // Random contents from an alphabet larger than N: model the last N distinct contents.
  std::mt19937 rng(11u);
  std::map<uint32_t, std::vector<int>> owner = { { ga, a }, { gb, b } };
  std::deque<std::vector<int>> recent = { a, b };   // insertion order of what the ring holds
  for (int i = 0; i < 200000; i++) {
    std::vector<int> c = { int(rng() % 24u) };
    if (rng() % 4u == 0u)
      c.push_back(int(rng() % 3u));
    bool remembered = false;
    for (auto& r : recent)
      remembered |= r == c;
    uint32_t g = ring.intern(c, eq, as);
    CHECK(g != 0u);
    auto it = owner.find(g);
    if (it != owner.end())
      CHECK(it->second == c);      // same generation, same content
    else
      owner[g] = c;
    if (remembered)
      CHECK(it != owner.end());    // remembered content keeps its generation
    else {
      recent.push_back(c);
      if (recent.size() > N)
        recent.pop_front();
    }
    if (g_fail > 10)
      break;
  }
  std::printf("%s pso_gen_test (%d failures, %zu generations)\n", g_fail ? "FAIL" : "PASS", g_fail, owner.size());
  return g_fail ? 1 : 0;
}
